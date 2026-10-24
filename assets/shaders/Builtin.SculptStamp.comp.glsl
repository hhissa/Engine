#version 450
#extension GL_GOOGLE_include_directive : require

// M2a: stamps ONE analytic brush shape (any of the 15 in Builtin.
// BrushShapes.inc.glsl, selected by pc.shape_type) against whatever a
// touched cell already holds, blended via either a smooth union or a smooth
// subtraction (pc.op), with blend radius pc.smoothness -- generalized from
// M1a's sphere-only/hard-union-only version (see /home/h-i/.claude/plans/
// joyful-skipping-giraffe.md's M2a section) directly into the chunked
// field's EXISTING GPU buffers (chunk_indirection_buffer_/chunk_brick_pool_
// buffer_/chunk_brick_primitive_buffer_/the brick free list) -- see
// VulkanRaymarchShader::stamp_shape() (vulkan_raymarch_shader.cpp), which
// records this dispatch. Brush ROTATION is explicitly out of scope for this
// pass -- every shape below stays in its canonical/axis-aligned local
// orientation (world_pos - center is evaluated directly, with no basis
// change).
//
// M2b: also captures, for every touched cell, exactly what this invocation
// is about to overwrite -- into UndoCaptureMetaBuffer/UndoCaptureVoxelBuffer
// (bindings 6/7, below) -- so VulkanRaymarchShader::undo_stamp() can revert
// this stamp later without needing to invert smooth_union()/smooth_
// subtraction() (not possible by formula; see the plan file's M2b section).
// This is written at exactly the point each quantity is about to change --
// no separate capture dispatch, no re-evaluation of the brush shape. Capped
// at UNDO_CAPTURE_CAPACITY cells (independent of, and smaller than,
// StampCellBuffer's own kMaxSculptStampCellsPerCall capacity -- see
// VulkanRaymarchShader::stamp_shape()'s own comment on why undo capture is
// allowed to be lossy for a stamp that touches an unusually large number of
// cells, while the stamp itself never is).
//
// One invocation per TOUCHED COARSE CELL (not per voxel): the CPU
// (stamp_shape()) has already enumerated every level-0 coarse cell the
// brush's world-space AABB overlaps (using a conservative per-shape
// bounding radius -- see sculpt_shape_bounding_radius() engine-side, since
// an un-rotated non-sphere shape's true AABB isn't a sphere, but a cube
// built from the largest conservative radius is still a safe superset),
// resolved each one's chunk-table slot (allocating a fresh one on first
// touch -- see acquire_sculpt_chunk_slot()), and uploaded the resulting
// list into StampCellBuffer below. Each invocation here allocates (or
// reuses) that cell's brick from the shared free-list stack, then loops its
// own 18^3 apron voxels evaluating the selected shape directly -- a serial
// loop per invocation, but one that's trivially cheap (a few thousand
// iterations) against the whole dispatch running in parallel across every
// touched cell, which is why this doesn't need the heavier per-voxel-
// parallel dispatch shape Builtin.ChunkVoxelize.comp.glsl (M0-deleted) used
// for a primitive-scanning bake: that one had to amortize evaluating many
// primitives against many chunks; this one brush shape is O(1) to evaluate,
// so per-cell serial is the simplest design that is nowhere near a
// bottleneck for an authoring-time, one-off stamp.
layout(local_size_x = 64) in;

#include "Builtin.SdfFieldConfig.inc.glsl"
#include "Builtin.BrushShapes.inc.glsl"

// Shape selector -- pc.shape_type's values. Must match VulkanRaymarchShader
// ::BrushShape (vulkan_raymarch_shader.h) and engine/src/renderer/
// renderer_types.inl's SculptBrushShape exactly; nothing in C++ checks this
// half of the lockstep (the two C++-side enums DO check each other via a
// static_assert in vulkan_backend.cpp), so a value added to any of the
// three must be added to all three in the same order.
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

// Blend op selector -- pc.op's values. Must match VulkanRaymarchShader::
// BrushOp and SculptBrushOp (renderer_types.inl) exactly, same lockstep
// note as the shape constants above.
const int OP_UNION = 0;
const int OP_SUBTRACTION = 1;

// Evaluates the selected brush shape at local point p (already translated
// by the brush center -- see main() below), picking which subset of the
// 4-float params block each *_sdf() needs. See VulkanRaymarchShader::
// stamp_shape()'s header comment for the authoritative param-layout table
// per shape (duplicated there for the CPU-side bounding-radius computation
// to stay next to it) -- summarized here:
//   Sphere:          params.x = radius
//   Box:             params.xyz = half_extents
//   Plane:           params.x = height
//   Torus:           params.x = major_radius, params.y = minor_radius
//   CappedCylinder:  params.x = radius, params.y = half_height
//   CappedCone:      params.x = half_height, params.y = r1, params.z = r2
//   RoundBox:        params.xyz = half_extents, params.w = corner_radius
//   BoxFrame:        params.xyz = half_extents, params.w = edge_thickness
//   Octahedron:      params.x = s
//   Pyramid:         params.x = h, params.y = base
//   HexPrism:        params.x = inradius, params.y = half_height
//   RoundCone:       params.x = r1, params.y = r2, params.z = half_height
//   Capsule:         params.x = radius, params.y = half_height
//   Link:            params.x = half_length, params.y = r1, params.z = r2
//   Ellipsoid:       params.xyz = radii
float eval_brush_shape(vec3 p, int shape_type, vec4 params) {
    // Shared with every sculpt shader, modifiers included -- see
    // brush_shape_sdf(), Builtin.BrushShapes.inc.glsl.
    return brush_shape_sdf(p, shape_type, params);
}

// One entry per touched coarse cell, written host-side by
// VulkanRaymarchShader::stamp_shape() into a small (kMaxSculptStampCellsPer
// Call-capacity) host-visible scratch buffer -- see GpuSculptStampCell,
// engine-side, for the identical layout. vec4-only members so std430's
// array stride is the plain 32 bytes with no struct-packing surprises (see
// GpuChunkBatchEntry's own comment, vulkan_raymarch_shader.h, for the class
// of bug this avoids).
struct StampCell {
    ivec4 slot_cell;    // x = chunk-table slot, y = local cell index
                       // (into this chunk's CHUNK_CELL_COUNT sub-block),
                       // z/w unused.
    vec4 cell_min_size; // xyz = this cell's world-space min corner, w = its
                       // world-space size (COARSE_CELL_SIZE -- carried per
                       // entry for symmetry with Builtin.SculptSplatRegen.
                       // comp.glsl, which reads the identical buffer, even
                       // though every M1a/M2a cell shares the same size).
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

// M2b undo capture -- one entry per StampCellBuffer slot (same idx), valid
// only for idx < UNDO_CAPTURE_CAPACITY (see main()'s capture_allowed). Three
// states a cell can end this stamp in, from Undo's point of view:
//   0 (NOOP)     -- nothing changed (the is_new-and-SUBTRACTION early-return,
//                    or the free-list-exhausted degrade below): Undo does
//                    nothing for this cell.
//   1 (NEW_BRICK) -- this invocation allocated a fresh brick: there is
//                    nothing valid to restore IT to (a freshly popped
//                    brick's prior contents are garbage, not geometry), so
//                    Undo just frees it back to the pool (mirroring
//                    Builtin.ChunkEvict.comp.glsl's push) and clears
//                    chunk_indirection. No voxel data needs capturing.
//   2 (MODIFIED) -- an existing brick was blended against: Undo restores
//                    old_material and every voxel captured into
//                    UndoCaptureVoxelBuffer below.
// old_material is only meaningful for MODIFIED (chunk_brick_primitive's
// value immediately before this invocation's own OP_UNION write, if any --
// captured regardless of op, since reading it costs nothing extra once
// brick_index is known).
struct UndoCaptureMeta {
    int change_type;  // 0 = NOOP, 1 = NEW_BRICK, 2 = MODIFIED.
    int old_material;
    // M3 (Looseness): repurposed from a formerly-unused pad0 -- same
    // change_type==2-only meaning as old_material immediately above.
    float old_looseness;
    int pad1;
};

layout(std430, binding = 6) buffer UndoCaptureMetaBuffer {
    UndoCaptureMeta undo_meta[];
};

// Flat array, stride CHUNK_BRICK_VOXEL_COUNT per StampCellBuffer slot (same
// idx as undo_meta[] -- undo_voxels[idx * CHUNK_BRICK_VOXEL_COUNT + i] is
// this cell's pre-stamp voxel i). Only ever written (and later read back by
// VulkanRaymarchShader::undo_stamp()) for MODIFIED cells; garbage for
// NOOP/NEW_BRICK cells, which is harmless since nothing reads it for those.
layout(std430, binding = 7) buffer UndoCaptureVoxelBuffer {
    float undo_voxels[];
};

// M3c (Spraypaint): the per-voxel paint-opacity accumulator Builtin.
// SculptSpraypaint.comp.glsl writes -- bound here ONLY so a freshly
// allocated brick (is_new below) can have its paint voxels explicitly
// zeroed at the exact moment it's claimed, same spot its distance voxels
// get a real (not garbage) value written for the first time. Without this,
// a brick that was painted on, then freed back to the pool by Undo (see
// Builtin.SculptUndoRestore.comp.glsl's NEW_BRICK branch), then handed back
// out to a LATER, unrelated stamp somewhere else, would carry that stale
// paint data over onto brand-new geometry that was never actually sprayed.
// Never written to for an EXISTING brick (the `else` branch below) --
// re-stamping an already-painted surface must never silently erase its
// paint.
layout(std430, binding = 8) buffer ChunkBrickPaintBuffer {
    float chunk_brick_paint[];
};

// M3 (Looseness): one f32 per brick slot, same size/indexing as
// ChunkBrickPrimitiveBuffer above -- written right alongside the material
// write below (same UNION-only-overwrites rule: a Subtraction stamp leaves
// a brick's existing looseness untouched, exactly like material). Nothing
// shades with it at the moment. Also bound (same layout) by Builtin.
// SculptCheckpointRestore.comp.glsl, which restores it the same way it
// restores chunk_brick_primitive.
layout(std430, binding = 9) buffer ChunkBrickLoosenessBuffer {
    float chunk_brick_looseness[];
};

// How many of StampCellBuffer's cells this dispatch can capture undo state
// for, independent of (and smaller than) pc.cell_count/kMaxSculptStampCellsPerCall
// -- see VulkanRaymarchShader::kMaxSculptUndoCellsPerStamp, which this must
// match exactly (same manual-sync-point convention as MAX_CHUNK_BRICKS
// above). Cells beyond this cap still get stamped normally; they just can't
// be undone (see stamp_shape()'s own comment on this tradeoff).
const int UNDO_CAPTURE_CAPACITY = 1024;

// Must match kMaxChunkBricks engine-side exactly (kMaxChunkBricksPerLevel
// (16384) * kNumLevels (5) -- see Builtin.ChunkEvict.comp.glsl's identical
// MAX_BRICKS for the same cross-check).
// Specialization constant 0: the engine passes its VRAM-sized
// max_chunk_bricks_ here at pipeline creation; 16384 per level is only the default.
layout(constant_id = 0) const int MAX_CHUNK_BRICKS = 16384 * NUM_CHUNK_LEVELS;

// Plain scalars throughout (never a vec/ivec, and never reordered) so this
// block's byte layout matches SculptStampPushConstants engine-side exactly,
// member for member, with no std430 vec4-alignment padding to account for
// -- same convention Builtin.ChunkedFieldDebugQuery.comp.glsl's push
// constant block already uses for the same reason. This is NOT cosmetic:
// M1a had a real, hard-to-diagnose bug from a C++/GLSL push-constant struct
// drifting out of byte-sync (see the plan file's M1a postmortem) -- every
// member below is a 4-byte scalar (float or int), in the exact same order
// as SculptStampPushConstants, so there is no possible alignment
// discrepancy between the two as long as the order stays identical.
layout(push_constant) uniform PushConstants {
    float center_x;
    float center_y;
    float center_z;
    float param0;       // See eval_brush_shape()'s comment for what these
    float param1;       // four mean for the currently selected shape_type.
    float param2;
    float param3;
    float smoothness;   // Blend radius k for smooth_union()/
                        // smooth_subtraction() -- 0 collapses to a hard
                        // min()/max() (see their own comments).
    float voxel_size;   // COARSE_CELL_SIZE / CHUNK_BRICK_DIM -- shared by
                        // every cell (level 0 only, see the plan).
    int shape_type;     // One of the SHAPE_* constants above.
    int cell_count;     // How many of StampCellBuffer's entries are valid
                        // this dispatch.
    int material_slot;  // Indexes the per-primitive material arrays (see
                        // ensure_default_sculpt_material() engine-side).
    int op;             // One of the OP_* constants above.
    float looseness;    // M3 (Looseness): written into chunk_brick_
    // Shape orientation as a unit quaternion (x, y, z, w) -- identity for
    // brush stamps; see SculptStamp::rotation.
    float rot_x;
    float rot_y;
    float rot_z;
    float rot_w;
    int object_id;      // the stamp's object: each voxel records its owner
                        // looseness[] alongside material_slot below, same
                        // UNION-only-overwrites rule. Added at the END of
                        // this block -- see SculptStampPushConstants'
                        // engine-side comment for why appending keeps
                        // every earlier member's offset unchanged.
} pc;

// The paint buffer's per-voxel owner code (see Builtin.SculptChunkRecompute.
// comp.glsl's owner_code()): this stamp's material slot and object.
float voxel_owner_code() {
    return float((max(pc.object_id, 0) + 1) * 1024 + clamp(pc.material_slot, 0, 1023));
}

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= uint(pc.cell_count)) {
        return;
    }

    int chunk_slot = cells[idx].slot_cell.x;
    int local_cell_index = cells[idx].slot_cell.y;
    vec3 cell_min = cells[idx].cell_min_size.xyz;

    int indirection_index = chunk_slot * CHUNK_CELL_COUNT + local_cell_index;
    int existing_brick = chunk_indirection[indirection_index];
    bool is_new = existing_brick < 0;

    // M2b: whether this invocation's slot has room in the undo capture
    // buffers at all -- see UNDO_CAPTURE_CAPACITY's own comment.
    bool capture_allowed = idx < uint(UNDO_CAPTURE_CAPACITY);

    // Correctness fix relative to M1a (which was union-only and never had
    // to consider this): subtracting a shape out of a coarse cell that has
    // never had a brick allocated is carving a hole out of nothing, and the
    // correct result of that is still nothing -- by definition, there is no
    // surface to carve. Returning here, before popping the free list or
    // writing chunk_indirection at all, leaves the cell exactly as it was
    // (unallocated/"-1"/no brick), which IS the correct "no-op" result,
    // instead of allocating a brick, filling it with meaningless leftover
    // device-memory contents (a freshly popped brick's prior contents are
    // garbage, not a sentinel "empty" value -- see is_new's use further
    // down), and then writing that garbage in as if it were real geometry.
    // This is simpler than the alternative the plan considered (seeding
    // old_dist with a large "definitely empty" sentinel distance and
    // blending normally) and produces an identical end result for this
    // case with less work: either way nothing should appear, and skipping
    // the allocation entirely means nothing does.
    if (is_new && pc.op == OP_SUBTRACTION) {
        if (capture_allowed) {
            undo_meta[idx].change_type = 0; // NOOP -- nothing to undo.
            undo_meta[idx].old_material = -1;
            undo_meta[idx].old_looseness = 0.0;
        }
        return;
    }

    int brick_index;
    if (is_new) {
        // Exact inverse of Builtin.ChunkEvict.comp.glsl's push (atomicAdd(+1)
        // there) -- pop the top of the stack. 0xFFFFFFFFu added to a uint is
        // the standard "subtract 1 without a separate atomicSub" trick (see
        // the SPLAT_REGEN_REFERENCE's identical cluster-pool pop for the
        // same idiom). free_before is the value BEFORE this decrement, so
        // the index just freed sits at free_before - 1. Only UNION reaches
        // here for a new cell -- SUBTRACTION already returned above.
        uint free_before = atomicAdd(brick_free_top, 0xFFFFFFFFu);
        if (free_before == 0u || free_before > uint(MAX_CHUNK_BRICKS)) {
            // Pool exhausted (or already was, racing another invocation) --
            // undo the decrement and drop this cell's stamp. Same "a missing
            // patch of surface, not a crash" degrade Builtin.ChunkVoxelize.
            // comp.glsl's own pop used (M0-deleted; see its reference copy's
            // header comment).
            atomicAdd(brick_free_top, 1u);
            if (capture_allowed) {
                undo_meta[idx].change_type = 0; // NOOP -- pool was exhausted.
                undo_meta[idx].old_material = -1;
                undo_meta[idx].old_looseness = 0.0;
            }
            return;
        }
        brick_index = brick_free_list[free_before - 1u];
        chunk_indirection[indirection_index] = brick_index;
        if (capture_allowed) {
            // NEW_BRICK -- nothing valid to restore to; Undo just frees it.
            undo_meta[idx].change_type = 1;
            undo_meta[idx].old_material = -1;
            undo_meta[idx].old_looseness = 0.0;
        }
    } else {
        brick_index = existing_brick;
        if (capture_allowed) {
            // MODIFIED -- captured BEFORE either this or the material/
            // looseness write below can overwrite it.
            undo_meta[idx].change_type = 2;
            undo_meta[idx].old_material = chunk_brick_primitive[brick_index];
            undo_meta[idx].old_looseness = chunk_brick_looseness[brick_index];
        }
    }

    // UNION always takes the stamped material (same behaviour M1a had,
    // generalized to the named op constant). SUBTRACTION deliberately
    // leaves the brick's existing material untouched -- carving a hole out
    // of a surface shouldn't repaint what remains with the cutter shape's
    // own material slot; there is nothing conceptually being "added" by a
    // subtraction. This is a per-BRICK (not per-voxel) material, same
    // coarse-grained approximation M1a already made -- smooth_union()/
    // smooth_subtraction()'s own `h` winner output is reserved for a future
    // per-voxel material-blending milestone, not this one (see Builtin.
    // BrushShapes.inc.glsl's comment).
    if (pc.op == OP_UNION) {
        chunk_brick_primitive[brick_index] = pc.material_slot;
        // M3 (Looseness): authored alongside material at stamp time,
        // same UNION-only-overwrites rule as the write immediately above --
        // see ChunkBrickLoosenessBuffer's own comment.
        chunk_brick_looseness[brick_index] = pc.looseness;
    }

    vec3 brush_center = vec3(pc.center_x, pc.center_y, pc.center_z);
    vec4 params = vec4(pc.param0, pc.param1, pc.param2, pc.param3);
    float voxel_size = pc.voxel_size;

    int base = brick_index * CHUNK_BRICK_VOXEL_COUNT;
    for (int pz = 0; pz < CHUNK_BRICK_APRON_DIM; ++pz) {
        for (int py = 0; py < CHUNK_BRICK_APRON_DIM; ++py) {
            for (int px = 0; px < CHUNK_BRICK_APRON_DIM; ++px) {
                // Apron index 0 is the -1 margin voxel, index CHUNK_BRICK_
                // DIM+1 the +1 margin -- see Builtin.ChunkedFieldCommon.
                // inc.glsl's sample_chunked_field_at_level_slot() comment
                // for the exact i0/s0 derivation this must match: a voxel's
                // CENTRE at local offset (apron_index - 1) sits at cell_min
                // + (apron_index - 0.5) * voxel_size.
                vec3 world_pos = cell_min + (vec3(px, py, pz) - 0.5) * voxel_size;
                float new_dist = eval_brush_shape(
                    quat_rotate_inverse(vec4(pc.rot_x, pc.rot_y, pc.rot_z,
                                             pc.rot_w),
                                        world_pos - brush_center),
                    pc.shape_type, params);

                int voxel_index = base + px +
                    py * CHUNK_BRICK_APRON_DIM +
                    pz * CHUNK_BRICK_APRON_DIM * CHUNK_BRICK_APRON_DIM;

                float out_dist;
                if (is_new) {
                    // Nothing valid to blend against -- a freshly allocated
                    // brick's prior contents are whatever device memory
                    // last held, not "empty space" (there is no sentinel
                    // value for that here, unlike chunk_indirection's -1).
                    // Only UNION reaches here (see the early return above),
                    // so writing the raw shape distance directly is always
                    // correct: unioning with nothing valid IS just the
                    // shape itself.
                    out_dist = new_dist;
                    // M3c: this voxel's slot in the paint buffer is equally
                    // garbage device memory the instant this brick is
                    // freshly claimed -- stamp it to "unpainted" now, same
                    // as out_dist above, rather than leaving whatever a
                    // long-gone earlier occupant of this brick index last
                    // sprayed there (see ChunkBrickPaintBuffer's own
                    // comment).
                    // Per-voxel material (see Builtin.SculptChunkRecompute.
                    // comp.glsl): this stamp owns every voxel of a new brick.
                    chunk_brick_paint[voxel_index] = voxel_owner_code();
                } else {
                    float old_dist = chunk_bricks[voxel_index];
                    if (capture_allowed) {
                        // voxel_index - base is this voxel's local index
                        // within the brick (0..CHUNK_BRICK_VOXEL_COUNT-1),
                        // identical to px + py*APRON + pz*APRON*APRON --
                        // reusing the already-computed offset instead of
                        // recomputing it.
                        undo_voxels[int(idx) * CHUNK_BRICK_VOXEL_COUNT +
                                    (voxel_index - base)] = old_dist;
                    }
                    float unused_h;
                    if (pc.op == OP_UNION) {
                        out_dist = smooth_union(new_dist, old_dist,
                                                pc.smoothness, unused_h);
                        // ...and the voxels where it is the nearer surface.
                        if (unused_h >= 0.5) {
                            chunk_brick_paint[voxel_index] = voxel_owner_code();
                        }
                    } else { // OP_SUBTRACTION
                        out_dist = smooth_subtraction(new_dist, old_dist,
                                                      pc.smoothness, unused_h);
                    }
                }
                chunk_bricks[voxel_index] = out_dist;
            }
        }
    }
}
