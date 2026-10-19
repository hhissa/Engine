#version 450
#extension GL_GOOGLE_include_directive : require

// M5 (checkpoint-removal redesign -- see /home/h-i/.claude/plans/
// joyful-skipping-giraffe.md's M4 section for the ORIGINAL checkpoint/
// replay design this REPLACES, and VulkanRendererBackend::delete_edit()/
// move_edit() for why): re-evaluates ONE coarse cell's ENTIRE brick FROM
// SCRATCH against the CURRENT ordered GPU stamp-list mirror (StampListBuffer,
// binding 12 -- VulkanRaymarchShader::chunk_sculpt_stamp_list_buffer_, kept
// in lockstep with SculptVolume::stamps() by sculpt_stamp_list_append()/
// _update()/_erase()), instead of blending ONE new shape into whatever a
// cell already holds the way Builtin.SculptStamp.comp.glsl does.
//
// This is the whole point of the redesign: deleting or moving an arbitrary
// past stamp can no longer be undone/reconstructed by formula (smooth_union/
// smooth_subtraction aren't invertible), so instead of snapshotting the
// whole volume periodically and replaying every stamp after a restore point
// (the old, deleted checkpoint system -- expensive, and the hitch scaled
// with total sculpture size, not with the edit), the CPU mutates the
// authoritative stamp list FIRST (both SculptVolume and this shader's own
// StampListBuffer mirror), then dispatches THIS shader once per cell that
// could possibly have changed -- only the cells the edited stamp's own AABB
// (for a Move: the UNION of its old AND new AABB) touches, since nothing
// outside that region was ever influenced by that one stamp in the first
// place. One invocation per TOUCHED CELL, same dispatch shape as Builtin.
// SculptStamp.comp.glsl/Builtin.SculptUndoRestore.comp.glsl (StampCellBuffer,
// binding 0, enumerated CPU-side -- see VulkanRaymarchShader::
// collect_recompute_cells()).
//
// --- Per-cell algorithm (one invocation) ---
// Walk StampListBuffer in order, index 0..pc.stamp_count-1. For every stamp
// whose own conservative AABB (center +/- its precomputed bounding_radius --
// see GpuSculptStampListEntry's own comment, engine-side, for why that's
// stored per-entry rather than re-derived here) overlaps THIS cell's bounds
// (the identical reject test VulkanRaymarchShader::stamp_shape()'s CPU-side
// cell enumeration already uses to decide which cells a stamp touches AT
// ALL -- see collect_recompute_cells()'s own .cpp comment for the algebra
// showing the two are equivalent):
//   - UNION, and no earlier overlapping stamp in this cell was a UNION yet
//     (brick_exists == false): this is the FIRST real content this cell
//     ever gets, exactly mirroring Builtin.SculptStamp.comp.glsl's own
//     `is_new` case -- the brick (freshly allocated if the cell had none
//     before, reused verbatim if it did) is written RAW (no blend: unioning
//     against "nothing" is just the shape itself).
//   - UNION, brick_exists == true: smooth_union() against the running
//     per-voxel value already written, exactly like the live incremental
//     path.
//   - SUBTRACTION, brick_exists == false: a no-op (carving a hole out of
//     nothing is still nothing) -- EXACTLY Builtin.SculptStamp.comp.glsl's
//     own `is_new && OP_SUBTRACTION` early return, just expressed as "skip
//     this one stamp and keep walking the list" instead of "return out of
//     the whole dispatch", since a LATER stamp in the list might still be
//     the one that first creates this cell's brick.
//   - SUBTRACTION, brick_exists == true: smooth_subtraction() against the
//     running value, same as the live path.
// Every UNION stamp that reaches the "blend" or "raw write" case above
// (i.e. every one that actually touched this cell) OVERWRITES
// last_union_material/last_union_looseness -- so after the full walk, those
// two hold whichever UNION stamp was LAST, in list order, among every stamp
// overlapping this cell. That is exactly the semantics Builtin.SculptStamp.
// comp.glsl's own per-brick (not per-voxel) material/looseness write already
// has today (`chunk_brick_primitive[brick_index] = pc.material_slot;`, a
// flat UNION-only overwrite with no voxel-level blending) -- this shader
// reproduces the exact same end state by constructing it incrementally as
// if every historical UNION stamp's own write had actually landed in order,
// which is what "last UNION stamp wins" always meant.
//
// --- Brick allocate/reclaim ---
// If the cell ends up with brick_exists == true: a brick index is resolved
// (reusing whatever brick_index the cell already pointed at, if any -- no
// need to free-then-reallocate when the same physical slot can just be
// overwritten -- or popping a fresh one from the shared free list,
// IDENTICAL atomicAdd(brick_free_top, 0xFFFFFFFFu) pattern to Builtin.
// SculptStamp.comp.glsl, if the cell had none before) and chunk_indirection
// is written to point at it. If the cell ends up with brick_exists ==
// false but USED TO have a brick (every stamp that ever created content
// here was itself deleted/moved away): that orphaned brick is freed back
// onto the pool (atomicAdd(brick_free_top, 1u), IDENTICAL push to Builtin.
// SculptUndoRestore.comp.glsl's own NEW_BRICK-undo branch), its owned splat-
// point cluster pages are freed the same way (walk brick_clusters[], free
// each non -1 page back onto cluster_free_list -- same loop, same idiom),
// and chunk_indirection is cleared to -1. Both directions matter: without
// the free-on-orphan branch, a long editing session full of deletes/moves
// would slowly leak bricks (and their clusters) that nothing can ever see
// again, until the shared pool was exhausted.
//
// --- What this does NOT do ---
// Splat-point regeneration for whatever this shader just wrote (or freed)
// is a SEPARATE dispatch (Builtin.SculptSplatRegen.comp.glsl, re-run over
// the exact same StampCellBuffer list right after this one, with a buffer
// barrier in between -- see VulkanRaymarchShader::recompute_cells()) --
// its own existing `if (brick_index < 0) return;` early-out already does
// exactly the right thing for a cell this shader just freed, no special-
// casing needed on either side, same precedent Builtin.SculptUndoRestore.
// comp.glsl's own NEW_BRICK branch already established.
layout(local_size_x = 64) in;

#include "Builtin.SdfFieldConfig.inc.glsl"
#include "Builtin.BrushShapes.inc.glsl"

// Mirrors Builtin.SculptStamp.comp.glsl's shape/op selector constants
// exactly -- same manual-lockstep note as that shader's own copy (this
// codebase's established convention is to duplicate small per-shader eval/
// dispatch tables rather than share them across compute shaders, which
// can't link against each other).
const int SHAPE_SPHERE = 0;
const int SHAPE_BOX = 1;
const int SHAPE_PLANE = 2;
const int SHAPE_TORUS = 3;
const int SHAPE_CAPPED_CYLINDER = 4;
const int SHAPE_CAPPED_CONE = 5;
const int SHAPE_ROUND_BOX = 6;
const int SHAPE_BOX_FRAME = 7;
const int SHAPE_OCTAHEDRON = 8;
const int SHAPE_PYRAMID = 9;
const int SHAPE_HEX_PRISM = 10;
const int SHAPE_ROUND_CONE = 11;
const int SHAPE_CAPSULE = 12;
const int SHAPE_LINK = 13;
const int SHAPE_ELLIPSOID = 14;

const int OP_UNION = 0;
const int OP_SUBTRACTION = 1;

// Identical dispatch table to Builtin.SculptStamp.comp.glsl's own
// eval_brush_shape() -- see its header comment for the per-shape params
// layout table (duplicated there and in VulkanRaymarchShader::stamp_shape()'s
// own header comment; this shader doesn't repeat the table a third time,
// just the function).
float eval_brush_shape(vec3 p, int shape_type, vec4 params) {
    switch (shape_type) {
        case SHAPE_SPHERE:         return sphere_sdf(p, params.x);
        case SHAPE_BOX:            return box_sdf(p, params.xyz);
        case SHAPE_PLANE:          return plane_sdf(p, params.x);
        case SHAPE_TORUS:          return torus_sdf(p, params.x, params.y);
        case SHAPE_CAPPED_CYLINDER:
            return capped_cylinder_sdf(p, params.x, params.y);
        case SHAPE_CAPPED_CONE:
            return capped_cone_sdf(p, params.x, params.y, params.z);
        case SHAPE_ROUND_BOX:
            return round_box_sdf(p, params.xyz, params.w);
        case SHAPE_BOX_FRAME:
            return box_frame_sdf(p, params.xyz, params.w);
        case SHAPE_OCTAHEDRON:     return octahedron_sdf(p, params.x);
        case SHAPE_PYRAMID:        return pyramid_sdf(p, params.x, params.y);
        case SHAPE_HEX_PRISM:      return hex_prism_sdf(p, params.x, params.y);
        case SHAPE_ROUND_CONE:
            return round_cone_sdf(p, params.x, params.y, params.z);
        case SHAPE_CAPSULE:        return capsule_sdf(p, params.x, params.y);
        case SHAPE_LINK:
            return link_sdf(p, params.x, params.y, params.z);
        case SHAPE_ELLIPSOID:      return ellipsoid_sdf(p, params.xyz);
        default:
            return sphere_sdf(p, params.x);
    }
}

// Mirrors Builtin.SculptStamp.comp.glsl's/Builtin.SculptSplatRegen.comp.
// glsl's identical StampCell struct exactly -- this shader's cell list is
// the SAME buffer (VulkanRaymarchShader::chunk_sculpt_cell_buffer_) and the
// SAME struct, just re-enumerated by VulkanRaymarchShader::
// collect_recompute_cells() instead of stamp_shape()'s own AABB walk.
struct StampCell {
    ivec4 slot_cell;
    vec4 cell_min_size;
};

layout(std430, binding = 0) readonly buffer StampCellBuffer {
    StampCell cells[];
};

layout(std430, binding = 1) buffer ChunkIndirectionBuffer {
    int chunk_indirection[];
};

layout(std430, binding = 2) buffer ChunkBrickPoolBuffer {
    float chunk_bricks[];
};

layout(std430, binding = 3) buffer ChunkBrickPrimitiveBuffer {
    int chunk_brick_primitive[];
};

layout(std430, binding = 4) buffer ChunkBrickFreeListBuffer {
    int brick_free_list[];
};

layout(std430, binding = 5) buffer ChunkBrickFreeListTopBuffer {
    uint brick_free_top;
};

// Splat-point cluster bookkeeping -- only touched by the "free an orphaned
// brick" branch below (see this file's own header comment). Same four
// bindings/idiom as Builtin.SculptUndoRestore.comp.glsl's own 6-9.
layout(std430, binding = 6) buffer ChunkBrickClusterBuffer {
    int brick_clusters[];
};
layout(std430, binding = 7) buffer ChunkClusterBuffer {
    ChunkCluster chunk_clusters[];
};
layout(std430, binding = 8) buffer ChunkClusterFreeListBuffer {
    int cluster_free_list[];
};
layout(std430, binding = 9) buffer ChunkClusterFreeTopBuffer {
    uint cluster_free_top;
};

// M3 (Looseness): same per-brick buffer/UNION-only-overwrites rule as
// Builtin.SculptStamp.comp.glsl's own binding 9.
layout(std430, binding = 10) buffer ChunkBrickLoosenessBuffer {
    float chunk_brick_looseness[];
};

// M3c (Spraypaint): zeroed for a FRESHLY allocated brick only, same spot/
// reasoning as Builtin.SculptStamp.comp.glsl's own binding 8 -- never
// touched for a brick this shader merely overwrites in place (re-stamping a
// painted surface must not erase its paint, same rule the live path
// already follows).
layout(std430, binding = 11) buffer ChunkBrickPaintBuffer {
    float chunk_brick_paint[];
};

// M5: one entry per CPU SculptStamp, in list order, kept in lockstep by
// VulkanRaymarchShader::sculpt_stamp_list_append()/_update()/_erase() --
// see GpuSculptStampListEntry's own comment, engine-side, for the exact
// byte layout this must match field-for-field. Read-only here: this shader
// never mutates the stamp list itself, only the voxel/brick data it
// describes.
struct StampEntry {
    vec4 center_shape;    // xyz = center, w = shape_type (as float).
    vec4 params;
    vec4 smoothness_op_material_looseness; // x=smoothness, y=op, z=
                                           // material_slot, w=looseness
                                           // (y/z as float, same int(round())
                                           // convention as w above).
    vec4 bounding_radius_pad; // x = precomputed bounding radius; yzw
                             // reserved (0).
};

layout(std430, binding = 12) readonly buffer StampListBuffer {
    StampEntry stamps[];
};

// Must match MAX_CHUNK_BRICKS/MAX_CLUSTERS_UNDO in Builtin.SculptStamp.
// comp.glsl/Builtin.SculptUndoRestore.comp.glsl exactly -- same cross-check,
// same constants engine-side (kMaxChunkBricks/kMaxChunkClusters).
// Specialization constant 0: the engine passes its VRAM-sized
// max_chunk_bricks_ here at pipeline creation; 16384 per level is only the default.
layout(constant_id = 0) const int MAX_CHUNK_BRICKS = 16384 * NUM_CHUNK_LEVELS;
const int MAX_CLUSTERS_RECOMPUTE = 131072;

// Plain scalars only, same order/convention as every other sculpt-family
// push-constant block -- see Builtin.SculptStamp.comp.glsl's own comment
// for why (byte-exact match to SculptChunkRecomputePushConstants
// engine-side, no std430 vec4-alignment surprises).
layout(push_constant) uniform PushConstants {
    int cell_count;    // How many of StampCellBuffer's entries are valid
                       // this dispatch.
    int stamp_count;   // How many of StampListBuffer's entries are valid --
                       // VulkanRaymarchShader::gpu_stamp_mirror_count() at
                       // dispatch time, AFTER the CPU-side mutation that
                       // triggered this recompute has already landed.
    float cell_size;   // COARSE_CELL_SIZE.
    float voxel_size;  // cell_size / CHUNK_BRICK_DIM.
    // 0 = RELEASE: free the brick of every cell that will end up empty (no
    //     Union stamp overlaps it) and do nothing else.
    // 1 = REBUILD: the full replay below, which only ever ALLOCATES.
    // Run as two dispatches with a barrier between (recompute_cells()).
    // Freeing pushes onto the same free-list stack allocating pops from; in
    // one dispatch a pop could read a slot a concurrent push had claimed
    // but not yet written, handing out a brick still in use -- two cells
    // sharing one brick, i.e. holes and stray fragments after objects had
    // been moved around for a while.
    int mode;
} pc;

// Whether any Union stamp's bounding cube overlaps the cell -- exactly the
// condition under which main()'s replay leaves the cell with a brick.
bool cell_has_union_stamp(vec3 cell_min, vec3 cell_max) {
    for (int s = 0; s < pc.stamp_count; ++s) {
        if (int(round(stamps[s].smoothness_op_material_looseness.y)) != OP_UNION) {
            continue;
        }
        vec3 center = stamps[s].center_shape.xyz;
        float bounding_radius = stamps[s].bounding_radius_pad.x;
        if (all(lessThanEqual(center - vec3(bounding_radius), cell_max)) &&
            all(greaterThanEqual(center + vec3(bounding_radius), cell_min))) {
            return true;
        }
    }
    return false;
}

void free_cell_brick(int indirection_index, int existing_brick) {
    uint write_index = atomicAdd(brick_free_top, 1u);
    if (write_index < uint(MAX_CHUNK_BRICKS)) {
        brick_free_list[write_index] = existing_brick;
    }
    chunk_indirection[indirection_index] = -1;

    for (int i = 0; i < CHUNK_MAX_CLUSTERS_PER_BRICK; ++i) {
        int list_index = existing_brick * CHUNK_MAX_CLUSTERS_PER_BRICK + i;
        int page = brick_clusters[list_index];
        if (page < 0) {
            continue;
        }
        brick_clusters[list_index] = -1;
        chunk_clusters[page].meta = uvec4(0);
        uint slot = atomicAdd(cluster_free_top, 1u);
        if (slot < uint(MAX_CLUSTERS_RECOMPUTE)) {
            cluster_free_list[slot] = page;
        }
    }
}

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= uint(pc.cell_count)) {
        return;
    }

    int chunk_slot = cells[idx].slot_cell.x;
    int local_cell_index = cells[idx].slot_cell.y;
    vec3 cell_min = cells[idx].cell_min_size.xyz;
    vec3 cell_max = cell_min + vec3(pc.cell_size);

    int indirection_index = chunk_slot * CHUNK_CELL_COUNT + local_cell_index;
    int existing_brick = chunk_indirection[indirection_index];
    bool had_brick_before = existing_brick >= 0;

    if (pc.mode == 0) {
        if (had_brick_before && !cell_has_union_stamp(cell_min, cell_max)) {
            free_cell_brick(indirection_index, existing_brick);
        }
        return;
    }

    bool brick_exists = false;   // Whether, after the full replay below,
                                 // this cell ends up with ANY geometry.
    int brick_index = -1;        // Resolved lazily, on the first UNION
                                 // touch -- see this file's own header
                                 // comment.
    bool alloc_failed = false;   // Free-list exhausted when a NEW brick was
                                 // needed -- same "a missing patch of
                                 // surface, not a crash" degrade Builtin.
                                 // SculptStamp.comp.glsl's own pop uses.
    int last_union_material = -1;
    float last_union_looseness = 0.0;
    // How close the winning stamp's surface comes to this brick's voxels --
    // see the material choice after each UNION stamp below.
    float best_surface_dist = 1e30;

    int base = -1; // brick_index * CHUNK_BRICK_VOXEL_COUNT, once resolved.

    for (int s = 0; s < pc.stamp_count; ++s) {
        if (alloc_failed) {
            break;
        }
        vec4 center_shape = stamps[s].center_shape;
        vec3 center = center_shape.xyz;
        int shape_type = int(round(center_shape.w));
        vec4 params = stamps[s].params;
        vec4 smop = stamps[s].smoothness_op_material_looseness;
        float smoothness = smop.x;
        int op = int(round(smop.y));
        int material_slot = int(round(smop.z));
        float looseness = smop.w;
        float bounding_radius = stamps[s].bounding_radius_pad.x;
        // yzw = the rotation quaternion's vector part, stored with w >= 0 so
        // the scalar part can be rebuilt here (see build_stamp_list_entry()).
        vec3 rot_xyz = stamps[s].bounding_radius_pad.yzw;
        vec4 rotation = vec4(rot_xyz, sqrt(max(0.0, 1.0 - dot(rot_xyz, rot_xyz))));

        // Identical conservative cube-vs-cell reject test to the one
        // VulkanRaymarchShader::stamp_shape()'s CPU-side cell enumeration
        // uses to decide which cells a stamp touches in the first place --
        // see collect_recompute_cells()'s own .cpp comment for the algebra.
        vec3 bmin = center - vec3(bounding_radius);
        vec3 bmax = center + vec3(bounding_radius);
        bool overlap = all(lessThanEqual(bmin, cell_max)) &&
                       all(greaterThanEqual(bmax, cell_min));
        if (!overlap) {
            continue;
        }

        if (op == OP_SUBTRACTION) {
            if (!brick_exists) {
                // Carving a hole out of nothing is still nothing -- same
                // no-op Builtin.SculptStamp.comp.glsl's own `is_new &&
                // OP_SUBTRACTION` early return represents, just expressed
                // as "keep walking the list" since a LATER stamp might
                // still create this cell's brick.
                continue;
            }
            for (int pz = 0; pz < CHUNK_BRICK_APRON_DIM; ++pz) {
                for (int py = 0; py < CHUNK_BRICK_APRON_DIM; ++py) {
                    for (int px = 0; px < CHUNK_BRICK_APRON_DIM; ++px) {
                        vec3 world_pos = cell_min +
                            (vec3(px, py, pz) - 0.5) * pc.voxel_size;
                        float new_dist = eval_brush_shape(
                            quat_rotate_inverse(rotation, world_pos - center),
                            shape_type, params);
                        int voxel_index = base + px +
                            py * CHUNK_BRICK_APRON_DIM +
                            pz * CHUNK_BRICK_APRON_DIM * CHUNK_BRICK_APRON_DIM;
                        float old_dist = chunk_bricks[voxel_index];
                        float unused_h;
                        chunk_bricks[voxel_index] =
                            smooth_subtraction(new_dist, old_dist, smoothness,
                                              unused_h);
                    }
                }
            }
            // SUBTRACTION never touches material/looseness -- same rule
            // Builtin.SculptStamp.comp.glsl's own comment documents.
            continue;
        }

        // OP_UNION.
        float stamp_surface_dist = 1e30; // min |distance| over the voxels
        if (!brick_exists) {
            // First real content this cell gets -- resolve a brick index
            // (reuse the existing one if this cell already had a brick
            // from before this recompute, otherwise pop a fresh one from
            // the shared free list, IDENTICAL pattern to Builtin.
            // SculptStamp.comp.glsl's own `is_new` branch).
            if (had_brick_before) {
                brick_index = existing_brick;
            } else {
                uint free_before = atomicAdd(brick_free_top, 0xFFFFFFFFu);
                if (free_before == 0u || free_before > uint(MAX_CHUNK_BRICKS)) {
                    atomicAdd(brick_free_top, 1u);
                    alloc_failed = true;
                    break;
                }
                brick_index = brick_free_list[free_before - 1u];
            }
            base = brick_index * CHUNK_BRICK_VOXEL_COUNT;
            brick_exists = true;
            for (int pz = 0; pz < CHUNK_BRICK_APRON_DIM; ++pz) {
                for (int py = 0; py < CHUNK_BRICK_APRON_DIM; ++py) {
                    for (int px = 0; px < CHUNK_BRICK_APRON_DIM; ++px) {
                        vec3 world_pos = cell_min +
                            (vec3(px, py, pz) - 0.5) * pc.voxel_size;
                        float new_dist = eval_brush_shape(
                            quat_rotate_inverse(rotation, world_pos - center),
                            shape_type, params);
                        int voxel_index = base + px +
                            py * CHUNK_BRICK_APRON_DIM +
                            pz * CHUNK_BRICK_APRON_DIM * CHUNK_BRICK_APRON_DIM;
                        // Raw write, no blend -- unioning against "nothing"
                        // is just the shape itself (same reasoning Builtin.
                        // SculptStamp.comp.glsl's own `is_new` branch uses).
                        chunk_bricks[voxel_index] = new_dist;
                        stamp_surface_dist = min(stamp_surface_dist, abs(new_dist));
                    }
                }
            }
        } else {
            for (int pz = 0; pz < CHUNK_BRICK_APRON_DIM; ++pz) {
                for (int py = 0; py < CHUNK_BRICK_APRON_DIM; ++py) {
                    for (int px = 0; px < CHUNK_BRICK_APRON_DIM; ++px) {
                        vec3 world_pos = cell_min +
                            (vec3(px, py, pz) - 0.5) * pc.voxel_size;
                        float new_dist = eval_brush_shape(
                            quat_rotate_inverse(rotation, world_pos - center),
                            shape_type, params);
                        int voxel_index = base + px +
                            py * CHUNK_BRICK_APRON_DIM +
                            pz * CHUNK_BRICK_APRON_DIM * CHUNK_BRICK_APRON_DIM;
                        float old_dist = chunk_bricks[voxel_index];
                        float unused_h;
                        chunk_bricks[voxel_index] =
                            smooth_union(new_dist, old_dist, smoothness,
                                        unused_h);
                        stamp_surface_dist = min(stamp_surface_dist, abs(new_dist));
                    }
                }
            }
        }
        // The brick's material (it has only one) is the stamp whose SURFACE
        // passes closest to its voxels -- the one the brick actually
        // shows. (It used to be simply the last stamp whose bounding box
        // touched the cell, so an object placed next to another took over
        // the neighbour's nearby bricks -- e.g. a glowing object turned
        // the facing side of the plain box beside it glowing too.) Ties go
        // to the later stamp, as before.
        if (stamp_surface_dist <= best_surface_dist) {
            best_surface_dist = stamp_surface_dist;
            last_union_material = material_slot;
            last_union_looseness = looseness;
        }
    }

    if (brick_exists && !alloc_failed) {
        chunk_indirection[indirection_index] = brick_index;
        chunk_brick_primitive[brick_index] = last_union_material;
        chunk_brick_looseness[brick_index] = last_union_looseness;
        if (!had_brick_before) {
            // Freshly claimed from the free list -- its paint voxels are
            // whatever garbage device memory last held (same reasoning
            // Builtin.SculptStamp.comp.glsl's own `is_new` branch gives),
            // so zero them now, same spot the live path does it.
            for (int i = 0; i < CHUNK_BRICK_VOXEL_COUNT; ++i) {
                chunk_brick_paint[base + i] = 0.0;
            }
        }
        return;
    }

    if (had_brick_before && !brick_exists) {
        // Orphaned: every stamp that ever put content in this cell was
        // itself deleted/moved away. Free the brick and every splat-point
        // cluster page it owns back onto their shared pools -- IDENTICAL
        // idiom to Builtin.SculptUndoRestore.comp.glsl's own NEW_BRICK-undo
        // branch (see this file's own header comment).
        //
        // Unreachable since the RELEASE pass (mode 0) took over freeing --
        // it frees exactly these cells first, so they arrive here with no
        // brick. Kept as a safety net.
        free_cell_brick(indirection_index, existing_brick);
    }
    // Else: had no brick before, has none after -- true no-op, nothing to
    // do (e.g. a cell this stamp's conservative bounding cube reaches but
    // no surviving stamp's own AABB actually overlaps).
}
