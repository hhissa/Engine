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
// The stamps that can matter to this cell are gathered once (see
// gather_cell_stamps()), then every voxel folds them in list order
// (fold_voxel()):
//   - Union / Subtraction act on the whole running field: smooth_union()
//     onto it, smooth_subtraction() out of it (a carve of nothing is
//     nothing), exactly as the live incremental path (Builtin.SculptStamp.
//     comp.glsl) blends them.
//   - The object-scoped ops (OP_OBJECT_*) act on their own object's field:
//     a run of one object's stamps is built up on its own (add, carve,
//     crop = smooth max, paint) and merged into the running field with a
//     hard min when the run ends -- so they never reach anything else.
// Each voxel also gets the material slot of the stamp owning the surface
// there (an add owns the voxels where it is the nearer surface; a paint
// stamp takes over its object's voxels inside it), written to the paint
// buffer for the shading pass. The brick's single material (what the
// splat pass reads fleck settings from) is the slot owning most of its
// near-surface voxels.
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
// One workgroup per cell: its 256 threads share the brick's 18^3 voxels.
// (It used to be one thread per cell, which left most of the GPU idle --
// an edit touches ~100 cells -- and each thread walking 5832 voxels alone.)
layout(local_size_x = 256) in;
const int WG_SIZE = 256;

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
// Object-scoped ops (SculptBrushOp::Object*, renderer_types.inl): a run of
// one object's stamps is built up on its own, then merged into the rest
// with a hard union -- so its carves/crops/paint never reach anything else.
const int OP_OBJECT_UNION = 2;
const int OP_OBJECT_SUBTRACTION = 3;
const int OP_OBJECT_INTERSECTION = 4;
const int OP_OBJECT_PAINT = 5;

// Identical dispatch table to Builtin.SculptStamp.comp.glsl's own
// eval_brush_shape() -- see its header comment for the per-shape params
// layout table (duplicated there and in VulkanRaymarchShader::stamp_shape()'s
// own header comment; this shader doesn't repeat the table a third time,
// just the function).
float eval_brush_shape(vec3 p, int shape_type, vec4 params) {
    // Shared with every sculpt shader, modifiers included -- see
    // brush_shape_sdf(), Builtin.BrushShapes.inc.glsl.
    return brush_shape_sdf(p, shape_type, params);
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

// The stamps that can matter to this dispatch's cells, as indices into
// StampListBuffer, built on the CPU (VulkanRaymarchShader::
// build_recompute_candidates()): those whose bounds reach the batch's
// cells, plus the crops of the objects among them. Ordered for the fold:
// list order, except that each object's scoped stamps are moved together
// (to where its last one is), so an object is always folded as one piece
// even when another object's stamps were made in between. Each entry is
// stamp index | (weld block << 18): a weld block's stamps (the strokes of
// one sculpture and its carves) come in stamp order and fold as one run
// (see fold_voxel()); block 0 = not welded.
layout(std430, binding = 13) readonly buffer CandidateBuffer {
    int candidates[];
};
int candidate_stamp(int c) {
    return c & 0x3FFFF;
}
int candidate_weld(int c) {
    return c >> 18;
}

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
    int candidate_count; // How many of CandidateBuffer's entries are valid.
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
    // How far past its own bounds a welded stamp still counts (the widest
    // weld smoothness -- VulkanRaymarchShader::weld_pad()).
    float weld_pad;
    // 1 = DRAG PREVIEW: fold only every DRAG_STRIDE-th voxel along each
    // axis and fill the rest in by interpolation -- for a big sculpture
    // being dragged; it is refolded in full when the drag ends
    // (VulkanRendererBackend::set_drag_preview()).
    int coarse;
} pc;

// Whether any Union stamp's bounding cube overlaps the cell -- exactly the
// condition under which main()'s replay leaves the cell with a brick.

// --- Stamp-list entry decoding ---------------------------------------------
// center_shape.w = shape_type + 32 * object id (build_stamp_list_entry()).
int stamp_op(int s) {
    return int(round(stamps[s].smoothness_op_material_looseness.y));
}
int stamp_object(int s) {
    return int(round(stamps[s].center_shape.w)) / 32;
}
int stamp_shape(int s) {
    int w = int(round(stamps[s].center_shape.w));
    return w - 32 * (w / 32);
}
bool op_adds(int op) {
    return op == OP_UNION || op == OP_OBJECT_UNION;
}
// The conservative cube-vs-cell reject test VulkanRaymarchShader::
// stamp_shape()'s CPU-side cell enumeration also uses -- see
// collect_recompute_cells()'s own .cpp comment for the algebra.
bool stamp_overlaps(int s, vec3 cell_min, vec3 cell_max) {
    vec3 center = stamps[s].center_shape.xyz;
    float bounding_radius = stamps[s].bounding_radius_pad.x;
    return all(lessThanEqual(center - vec3(bounding_radius), cell_max)) &&
           all(greaterThanEqual(center + vec3(bounding_radius), cell_min));
}

// --- Shared state ---------------------------------------------------------
// The cell's stamps, decoded once into shared memory by the gather (up to
// CELL_CAP; past that each voxel streams the rest of the candidate list).
const int CELL_CAP = 256;
shared vec4 s_center_smooth[CELL_CAP]; // xyz center, w smoothness
shared vec4 s_params[CELL_CAP];
shared vec4 s_rotation[CELL_CAP];
shared ivec4 s_meta[CELL_CAP];         // op, material slot, object, shape
shared float s_loose[CELL_CAP];
shared int s_weld[CELL_CAP];
shared float s_radius[CELL_CAP];
// How far past its bounds a welded adding stamp still counts in this cell:
// the widest smoothness of the welded adding stamps that reach the cell
// (only they can blend a fillet into it). Never more than pc.weld_pad.
shared uint s_pad_bits;
// Drag preview (pc.coarse): the folded samples, at apron voxels 0, 4, 8,
// 12, 16 and 17 along each axis. Meta = (owner + 1) << 11 | (slot + 1); a
// sample's looseness goes straight into s_vote_loose[slot]. Kept small:
// past ~32KB of shared memory a GPU core fits only one cell at a time.
const int DRAG_STRIDE = 4;
const int DRAG_DIM = CHUNK_BRICK_DIM / DRAG_STRIDE + 2;
const int DRAG_COUNT = DRAG_DIM * DRAG_DIM * DRAG_DIM; // < WG_SIZE: one per thread
shared float s_cdist[DRAG_COUNT];
shared int s_cmeta[DRAG_COUNT];
shared uint s_scan[WG_SIZE];
shared int s_count;
// When more than CELL_CAP stamps are kept: the candidate index of the
// first one that didn't fit. The fold takes the first CELL_CAP from shared
// memory and streams only the rest, from here on.
shared int s_overflow;
shared bool s_any_add;
shared bool s_keep;
shared int s_brick;
// Brick material vote: surface voxels per material slot.
const int SLOT_COUNT = 1024;
shared uint s_votes[SLOT_COUNT];
shared float s_vote_loose[SLOT_COUNT];
shared uint s_best_vote;    // (count << 10) | slot
shared uint s_nearest_bits; // smallest |dist| of any voxel, as bits
shared int s_nearest_slot;
shared float s_nearest_loose;

struct Stamp {
    vec3 center;
    float smoothness;
    vec4 params;
    vec4 rotation;
    int op;
    int slot;
    int object;
    int shape;
    float looseness;
    int weld; // weld block (0 = none)
    float radius; // bounding cube half-size, smoothness included
};

Stamp decode_stamp(int s) {
    Stamp st;
    st.center = stamps[s].center_shape.xyz;
    vec4 smop = stamps[s].smoothness_op_material_looseness;
    st.smoothness = smop.x;
    st.op = int(round(smop.y));
    st.slot = int(round(smop.z));
    st.looseness = smop.w;
    st.params = stamps[s].params;
    // yzw = the rotation quaternion's vector part, stored with w >= 0 so
    // the scalar part can be rebuilt here (build_stamp_list_entry()).
    vec3 rot_xyz = stamps[s].bounding_radius_pad.yzw;
    st.rotation = vec4(rot_xyz, sqrt(max(0.0, 1.0 - dot(rot_xyz, rot_xyz))));
    st.object = stamp_object(s);
    st.shape = stamp_shape(s);
    st.weld = 0;
    st.radius = stamps[s].bounding_radius_pad.x;
    return st;
}

Stamp shared_stamp(int k) {
    Stamp st;
    st.center = s_center_smooth[k].xyz;
    st.smoothness = s_center_smooth[k].w;
    st.params = s_params[k];
    st.rotation = s_rotation[k];
    st.op = s_meta[k].x;
    st.slot = s_meta[k].y;
    st.object = s_meta[k].z;
    st.shape = s_meta[k].w;
    st.looseness = s_loose[k];
    st.weld = s_weld[k];
    st.radius = s_radius[k];
    return st;
}

// Whether candidate stamp s takes part in this cell's fold: anything whose
// bounds reach the cell, and every crop (a crop removes all of its object
// outside it, near or not -- the CPU only sends crops of objects that
// matter here). Carves and paint with nothing to act on are harmless: the
// fold ignores them.
// A welded adding stamp reaches s_pad_bits further: a neighbour in its
// block can blend into its surface that far out. A carve or paint acts only
// inside its own bounds (which include its smoothness).
bool stamp_kept(int c, vec3 cell_min, vec3 cell_max) {
    int s = candidate_stamp(c);
    int op = stamp_op(s);
    if (op == OP_OBJECT_INTERSECTION) {
        return true;
    }
    if (candidate_weld(c) == 0 || op != OP_OBJECT_UNION) {
        return stamp_overlaps(s, cell_min, cell_max);
    }
    vec3 center = stamps[s].center_shape.xyz;
    float radius = stamps[s].bounding_radius_pad.x + uintBitsToFloat(s_pad_bits);
    return all(lessThanEqual(center - vec3(radius), cell_max)) &&
           all(greaterThanEqual(center + vec3(radius), cell_min));
}

// --- Gather (all threads) --------------------------------------------------
// Walks the candidates WG_SIZE at a time; a prefix sum over each block keeps
// the kept stamps in candidate order. Leaves s_count (the number kept, which
// may exceed CELL_CAP -- then the fold streams) and s_any_add.
void gather_cell_stamps(uint tid, vec3 cell_min, vec3 cell_max) {
    if (tid == 0u) {
        s_count = 0;
        s_any_add = false;
        s_pad_bits = 0u;
        s_overflow = pc.candidate_count;
    }
    barrier();
    // This cell's weld pad (see s_pad_bits). Smoothness is >= 0, so its
    // float bits order like the floats.
    if (pc.weld_pad > 0.0) {
        uint pad_bits = 0u;
        for (int i = int(tid); i < pc.candidate_count; i += WG_SIZE) {
            int c = candidates[i];
            int s = candidate_stamp(c);
            if (candidate_weld(c) != 0 && stamp_op(s) == OP_OBJECT_UNION &&
                stamp_overlaps(s, cell_min, cell_max)) {
                float k = clamp(stamps[s].smoothness_op_material_looseness.x,
                                0.0, pc.weld_pad);
                pad_bits = max(pad_bits, floatBitsToUint(k));
            }
        }
        atomicMax(s_pad_bits, pad_bits);
        barrier();
    }
    for (int start = 0; start < pc.candidate_count; start += WG_SIZE) {
        int i = start + int(tid);
        int s = -1;
        int c = 0;
        bool keep = false;
        if (i < pc.candidate_count) {
            c = candidates[i];
            s = candidate_stamp(c);
            keep = stamp_kept(c, cell_min, cell_max);
            if (keep && op_adds(stamp_op(s)) &&
                stamp_overlaps(s, cell_min, cell_max)) {
                s_any_add = true;
            }
        }
        // Inclusive prefix sum of `keep` over the block.
        s_scan[tid] = keep ? 1u : 0u;
        barrier();
        for (uint offset = 1u; offset < uint(WG_SIZE); offset <<= 1) {
            uint add = tid >= offset ? s_scan[tid - offset] : 0u;
            barrier();
            s_scan[tid] += add;
            barrier();
        }
        int base = s_count;
        int pos = base + int(s_scan[tid]) - 1;
        if (keep && pos == CELL_CAP) {
            s_overflow = i;
        }
        if (keep && pos < CELL_CAP) {
            Stamp st = decode_stamp(s);
            s_center_smooth[pos] = vec4(st.center, st.smoothness);
            s_params[pos] = st.params;
            s_rotation[pos] = st.rotation;
            s_meta[pos] = ivec4(st.op, st.slot, st.object, st.shape);
            s_loose[pos] = st.looseness;
            s_weld[pos] = candidate_weld(c);
            s_radius[pos] = st.radius;
        }
        barrier();
        if (tid == 0u) {
            s_count = base + int(s_scan[WG_SIZE - 1]);
        }
        barrier();
    }
}

// --- One voxel ---------------------------------------------------------------
// Paint opacity. A paint stamp has no use for looseness, so it carries
// the stroke's opacity there instead: floor() is a per-stroke seed and
// fract() is 1 - opacity (0 = opaque, so older paint is unchanged). The
// material is one slot per voxel, so opacity is coverage: a voxel takes the
// paint when its hash under that seed clears the opacity. One stroke's
// stamps share a seed (overlapping stamps don't build up); another stroke
// draws a new pattern, so going over a patch again builds it up.
// See SculptBrushOp::ObjectPaint (renderer_types.inl).
bool paint_covers(float looseness, vec3 world_pos) {
    float transparency = fract(looseness);
    if (transparency <= 0.0) {
        return true;
    }
    uvec3 v = uvec3(ivec3(floor(world_pos * 256.0)));
    v += uvec3(uint(floor(looseness)) * 747796405u);
    // pcg3d
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return float(v.x >> 8u) * (1.0 / 16777216.0) >= transparency;
}

// The distance at world_pos with every stamp folded in, in order, plus the
// material slot (and looseness) of whichever stamp owns the surface there:
// an adding stamp owns the voxels where it is the nearer surface, a paint
// stamp takes over its object's voxels inside it. Global ops work on the
// whole running field; object-scoped ones on their object's own running
// field, merged in (hard min) when its run ends.
struct VoxelResult {
    float dist;
    int slot;
    float looseness;
    int owner; // the object whose surface it is (-1: none)
};

// What the paint buffer holds per voxel: (owner + 1) * 1024 + slot, so the
// shading pass knows both the material and the object a surface belongs
// to (focus/scope-in tests by object). Always >= 1024, which tells it apart
// from the Qt tool's spray opacities (0..1). 0 = nothing owns it.
float owner_code(int owner, int slot) {
    return owner < 0 || slot < 0 ? 0.0
                                 : float((owner + 1) * 1024 + min(slot, 1023));
}

// Merges an object's finished field into the scene: a hard min, the
// nearer side keeping its material and owner.
void merge_into(inout float field, inout int slot, inout float loose,
                inout int owner, float obj, int obj_slot, float obj_loose,
                int obj_owner) {
    if (obj < field) {
        field = obj;
        slot = obj_slot;
        loose = obj_loose;
        owner = obj_owner;
    }
}

// A run: one object's scoped stamps, or a whole weld block's (the strokes
// of one sculpture and its carves), folded as one field in stamp order --
// each adding shape blending into everything of the run before it with
// its own smoothness, each carve cutting everything before it. The owner
// of a surface is the object of the adding stamp that won there.
int run_id(Stamp st) {
    return st.weld != 0 ? -st.weld : st.object;
}

VoxelResult fold_voxel(vec3 world_pos, bool streaming, vec3 cell_min,
                       vec3 cell_max) {
    float scene = 1e10;
    int scene_slot = -1;
    float scene_loose = 0.0;
    int scene_owner = -1;
    bool in_run = false;
    int run = 0;
    float obj = 1e10;
    int obj_slot = -1;
    float obj_loose = 0.0;
    int obj_owner = -1;
    int n = streaming ? CELL_CAP + (pc.candidate_count - s_overflow) : s_count;
    for (int k = 0; k < n; ++k) {
        Stamp st;
        if (k >= CELL_CAP) {
            int c = candidates[s_overflow + (k - CELL_CAP)];
            if (!stamp_kept(c, cell_min, cell_max)) {
                continue;
            }
            st = decode_stamp(candidate_stamp(c));
            st.weld = candidate_weld(c);
        } else {
            st = shared_stamp(k);
        }
        int op = st.op;
        bool scoped = op >= OP_OBJECT_UNION;
        if (in_run && (!scoped || run_id(st) != run)) {
            merge_into(scene, scene_slot, scene_loose, scene_owner, obj,
                       obj_slot, obj_loose, obj_owner);
            in_run = false;
        }
        if (scoped && !in_run) {
            in_run = true;
            run = run_id(st);
            obj = 1e10;
            obj_slot = -1;
            obj_loose = 0.0;
            obj_owner = st.object;
        }

        // Skip a stamp too far away to change anything here. Its shape lies
        // inside its bounding cube, so the distance to the cube is a lower
        // bound on its own: a union that can't come within its blend band
        // of the field, a carve that can't reach the field's surface plus
        // its band, or paint outside its shape leaves everything as it is.
        // (Not a plane, whose field runs on past its cube.)
        if (st.shape != SHAPE_PLANE && op != OP_OBJECT_INTERSECTION) {
            vec3 q = abs(world_pos - st.center) - vec3(st.radius);
            float box = length(max(q, vec3(0.0)));
            if (box > 0.0) {
                float k = max(st.smoothness, 0.0);
                if (op == OP_OBJECT_PAINT) {
                    continue;
                }
                float field = op == OP_UNION || op == OP_SUBTRACTION ? scene : obj;
                if (op_adds(op) ? box >= field + k : box >= k - field) {
                    continue;
                }
            }
        }

        float d = eval_brush_shape(
            quat_rotate_inverse(st.rotation, world_pos - st.center), st.shape,
            st.params);
        float h;
        if (op == OP_UNION) {
            float nd = smooth_union(d, scene, st.smoothness, h);
            if (h >= 0.5) {
                scene_slot = st.slot;
                scene_loose = st.looseness;
                scene_owner = st.object;
            }
            scene = nd;
        } else if (op == OP_SUBTRACTION) {
            if (scene < 1e9) {
                scene = smooth_subtraction(d, scene, st.smoothness, h);
            }
        } else if (op == OP_OBJECT_UNION) {
            float nd = smooth_union(d, obj, st.smoothness, h);
            if (h >= 0.5) {
                obj_slot = st.slot;
                obj_loose = st.looseness;
                obj_owner = st.object;
            }
            obj = nd;
        } else if (op == OP_OBJECT_SUBTRACTION) {
            if (obj < 1e9) {
                obj = smooth_subtraction(d, obj, st.smoothness, h);
            }
        } else if (op == OP_OBJECT_INTERSECTION) {
            if (obj < 1e9) {
                obj = -smooth_union(-d, -obj, st.smoothness, h); // smooth max
            }
        } else if (op == OP_OBJECT_PAINT) {
            if (obj < 1e9 && d <= 0.0 && paint_covers(st.looseness, world_pos)) {
                obj_slot = st.slot;
            }
        }
    }
    if (in_run) {
        merge_into(scene, scene_slot, scene_loose, scene_owner, obj, obj_slot,
                   obj_loose, obj_owner);
    }
    return VoxelResult(scene, scene_slot, scene_loose, scene_owner);
}

// Drag preview: the apron voxel a coarse sample sits at, and how a voxel
// lies between two samples along one axis.
int coarse_to_apron(int c) {
    return c < DRAG_DIM - 1 ? DRAG_STRIDE * c : CHUNK_BRICK_APRON_DIM - 1;
}
void coarse_axis(int i, out int c0, out int c1, out float t) {
    if (i >= CHUNK_BRICK_APRON_DIM - 1) {
        c0 = DRAG_DIM - 1;
        c1 = c0;
        t = 0.0;
    } else {
        c0 = i / DRAG_STRIDE;
        c1 = min(c0 + 1, DRAG_DIM - 1);
        t = float(i - c0 * DRAG_STRIDE) / float(DRAG_STRIDE);
    }
}
// A voxel's distance interpolated from the coarse samples around it, its
// material and owner from whichever of them is nearest the surface.
VoxelResult coarse_voxel(ivec3 p) {
    ivec3 c0, c1;
    vec3 t;
    coarse_axis(p.x, c0.x, c1.x, t.x);
    coarse_axis(p.y, c0.y, c1.y, t.y);
    coarse_axis(p.z, c0.z, c1.z, t.z);
    float dist = 0.0;
    float nearest = 1e30;
    int meta = 0;
    for (int k = 0; k < 8; ++k) {
        bvec3 hi = bvec3((k & 1) != 0, (k & 2) != 0, (k & 4) != 0);
        vec3 w3 = mix(vec3(1.0) - t, t, hi);
        float w = w3.x * w3.y * w3.z;
        if (w <= 0.0) {
            continue;
        }
        ivec3 c = ivec3(hi.x ? c1.x : c0.x, hi.y ? c1.y : c0.y,
                        hi.z ? c1.z : c0.z);
        int index = c.x + DRAG_DIM * (c.y + DRAG_DIM * c.z);
        float d = s_cdist[index];
        // An empty sample (1e10) would swamp the blend; anything this far
        // out reads the same.
        dist += w * clamp(d, -1e3, 1e3);
        if (abs(d) < nearest) {
            nearest = abs(d);
            meta = s_cmeta[index];
        }
    }
    int slot = (meta & 2047) - 1;
    float loose = slot >= 0 ? s_vote_loose[min(slot, SLOT_COUNT - 1)] : 0.0;
    return VoxelResult(dist, slot, loose, (meta >> 11) - 1);
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
    // One workgroup per cell; every early return below is uniform across
    // the workgroup (it reads shared state after a barrier).
    uint cell = gl_WorkGroupID.x;
    uint tid = gl_LocalInvocationID.x;
    if (cell >= uint(pc.cell_count)) {
        return;
    }

    int chunk_slot = cells[cell].slot_cell.x;
    int local_cell_index = cells[cell].slot_cell.y;
    vec3 cell_min = cells[cell].cell_min_size.xyz;
    vec3 cell_max = cell_min + vec3(pc.cell_size);

    int indirection_index = chunk_slot * CHUNK_CELL_COUNT + local_cell_index;
    int existing_brick = chunk_indirection[indirection_index];
    bool had_brick_before = existing_brick >= 0;

    if (pc.mode == 0 && !had_brick_before) {
        return; // nothing to free
    }

    // Does this cell need a brick? Only if something adds here AND the
    // surface comes near it. Stamps' bounding cubes cover far more than
    // their surfaces -- the inside of a big shape, the empty corners of a
    // round one -- and every brick there was VRAM (46KB each) holding
    // nothing the renderer needs: a cell with no brick reads as empty, and
    // rays and points only ever matter at the surface. The test is one
    // fold at the cell's centre: the field changes by at most 1 per unit
    // of distance, so a centre further from the surface than the brick's
    // half-diagonal (plus a 25% margin for the few shapes whose distance is
    // only approximate, plus two voxels) can't have it anywhere in the
    // brick. Both passes decide the same way, so pass 0 frees exactly the
    // bricks pass 1 won't keep.
    gather_cell_stamps(tid, cell_min, cell_max);
    bool streaming = s_count > CELL_CAP;
    if (tid == 0u) {
        s_keep = false;
        if (s_any_add) {
            float half_extent = 0.5 * pc.cell_size + pc.voxel_size; // + apron
            float reach = 1.25 * half_extent * sqrt(3.0) + 2.0 * pc.voxel_size;
            VoxelResult centre = fold_voxel(cell_min + vec3(0.5 * pc.cell_size),
                                            streaming, cell_min, cell_max);
            s_keep = abs(centre.dist) <= reach;
        }
    }
    barrier();

    if (pc.mode == 0) {
        if (tid == 0u && !s_keep) {
            free_cell_brick(indirection_index, existing_brick);
        }
        return;
    }
    if (!s_keep) {
        // Nothing to store here; pass 0 already freed any brick the cell had.
        return;
    }

    // Reuse the cell's brick, or pop a fresh one from the shared free list
    // (same pattern as Builtin.SculptStamp.comp.glsl's own `is_new` branch).
    if (tid == 0u) {
        if (had_brick_before) {
            s_brick = existing_brick;
        } else {
            uint free_before = atomicAdd(brick_free_top, 0xFFFFFFFFu);
            if (free_before == 0u || free_before > uint(MAX_CHUNK_BRICKS)) {
                // Pool exhausted: a missing patch of surface, not a crash.
                atomicAdd(brick_free_top, 1u);
                s_brick = -1;
            } else {
                s_brick = brick_free_list[free_before - 1u];
            }
        }
        s_best_vote = 0u;
        s_nearest_bits = 0x7F800000u; // +inf
        s_nearest_slot = -1;
        s_nearest_loose = 0.0;
    }
    for (int i = int(tid); i < SLOT_COUNT; i += WG_SIZE) {
        s_votes[i] = 0u;
    }
    barrier();
    int brick_index = s_brick;
    if (brick_index < 0) {
        return;
    }
    int base = brick_index * CHUNK_BRICK_VOXEL_COUNT;

    // The brick has a single material (what the splat pass reads its fleck
    // settings from): the slot owning most of its surface voxels, failing
    // any surface the voxel nearest it.
    float surface_band = 1.5 * pc.voxel_size;
    float my_nearest = 1e30;
    int my_nearest_slot = -1;
    float my_nearest_loose = 0.0;
    const int apron = CHUNK_BRICK_APRON_DIM;
    bool coarse = pc.coarse != 0;
    if (coarse) {
        if (int(tid) < DRAG_COUNT) {
            int v = int(tid);
            ivec3 c = ivec3(v % DRAG_DIM, (v / DRAG_DIM) % DRAG_DIM,
                            v / (DRAG_DIM * DRAG_DIM));
            ivec3 p = ivec3(coarse_to_apron(c.x), coarse_to_apron(c.y),
                            coarse_to_apron(c.z));
            vec3 world_pos = cell_min + (vec3(p) - 0.5) * pc.voxel_size;
            VoxelResult r = fold_voxel(world_pos, streaming, cell_min, cell_max);
            s_cdist[v] = r.dist;
            s_cmeta[v] = ((max(r.owner, -1) + 1) << 11) | (clamp(r.slot, -1, 2046) + 1);
            if (r.slot >= 0) {
                s_vote_loose[min(r.slot, SLOT_COUNT - 1)] = r.looseness;
            }
        }
        barrier();
    }
    for (int v = int(tid); v < apron * apron * apron; v += WG_SIZE) {
        int px = v % apron;
        int py = (v / apron) % apron;
        int pz = v / (apron * apron);
        vec3 world_pos = cell_min + (vec3(px, py, pz) - 0.5) * pc.voxel_size;
        VoxelResult r = coarse ? coarse_voxel(ivec3(px, py, pz))
                               : fold_voxel(world_pos, streaming, cell_min, cell_max);
        chunk_bricks[base + v] = r.dist;
        // Per-voxel material and owner (the deferred pass reads them, see
        // sample_chunk_material_at_point()).
        chunk_brick_paint[base + v] = owner_code(r.owner, r.slot);

        float a = abs(r.dist);
        if (a < my_nearest) {
            my_nearest = a;
            my_nearest_slot = r.slot;
            my_nearest_loose = r.looseness;
        }
        if (a < surface_band && r.slot >= 0) {
            int slot = min(r.slot, SLOT_COUNT - 1);
            atomicAdd(s_votes[slot], 1u);
            s_vote_loose[slot] = r.looseness;
        }
    }
    atomicMin(s_nearest_bits, floatBitsToUint(my_nearest));
    barrier();
    if (floatBitsToUint(my_nearest) == s_nearest_bits) {
        // Ties write the same kind of answer; any of them will do.
        s_nearest_slot = my_nearest_slot;
        s_nearest_loose = my_nearest_loose;
    }
    for (int i = int(tid); i < SLOT_COUNT; i += WG_SIZE) {
        if (s_votes[i] > 0u) {
            atomicMax(s_best_vote, (s_votes[i] << 10) | uint(i));
        }
    }
    barrier();

    if (tid == 0u) {
        int brick_slot = s_nearest_slot;
        float brick_loose = s_nearest_loose;
        if (s_best_vote != 0u) {
            brick_slot = int(s_best_vote & 1023u);
            brick_loose = s_vote_loose[brick_slot];
        }
        chunk_indirection[indirection_index] = brick_index;
        chunk_brick_primitive[brick_index] = max(brick_slot, 0);
        chunk_brick_looseness[brick_index] = brick_loose;
    }
}
