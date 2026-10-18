#version 450
#extension GL_GOOGLE_include_directive : require

// M4b (Delete-of-an-arbitrary-edit): the GPU half of
// VulkanRaymarchShader::restore_sculpt_checkpoint() -- repopulates the
// chunked field from a SculptCheckpoint's captured per-brick snapshot after
// reset_chunked_field() has just wiped everything to empty. Unlike Builtin.
// SculptStamp.comp.glsl (which blends a brush shape against whatever a cell
// already holds) or Builtin.SculptUndoRestore.comp.glsl (which restores a
// brick that is still resident right now), this shader always runs against
// a field that was JUST reset -- so every cell it touches is, by
// construction, unoccupied (chunk_indirection reads -1) and this dispatch's
// only job is: allocate a fresh brick (the exact free-list pop idiom
// Builtin.SculptStamp.comp.glsl's own is_new branch uses) and copy the
// checkpoint's captured material + CHUNK_BRICK_VOXEL_COUNT voxel snapshot
// into it verbatim -- no shape evaluation, no blending, a plain restore,
// the same "no blending, no shape evaluation" contract Builtin.
// SculptUndoRestore.comp.glsl's own MODIFIED branch already documents, just
// combined with that shader's NEW_BRICK branch's allocation step instead of
// assuming the brick already exists.
//
// One invocation per occupied cell in the checkpoint's own cell list (built
// CPU-side by VulkanRaymarchShader::restore_sculpt_checkpoint(), batched at
// VulkanRaymarchShader::kMaxSculptUndoCellsPerStamp entries per dispatch --
// see that method's own comment for why it reuses UndoCaptureMetaBuffer/
// UndoCaptureVoxelBuffer's existing capacity rather than allocating new,
// larger buffers just for this). The caller re-runs Builtin.
// SculptSplatRegen.comp.glsl over the SAME cell list right after this,
// completely unmodified, exactly like every other stamp/undo/redo path
// already does -- a restored brick with no splat points would render
// incorrectly even though its SDF data is correct.
layout(local_size_x = 64) in;

#include "Builtin.SdfFieldConfig.inc.glsl"

// Mirrors Builtin.SculptStamp.comp.glsl's identical StampCell struct.
struct StampCell {
    ivec4 slot_cell;    // x = chunk-table slot, y = local cell index.
    vec4 cell_min_size; // Unused here (no shape evaluation needs a world
                       // position) but kept for identical layout with every
                       // other StampCellBuffer consumer -- VulkanRaymarchShader
                       // ::restore_sculpt_checkpoint() reuses chunk_sculpt_
                       // cell_buffer_ as-is, so this struct must match
                       // exactly, used fields or not.
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

// Reuses Builtin.SculptStamp.comp.glsl's UndoCaptureMeta layout exactly
// (same buffer, chunk_sculpt_undo_capture_meta_buffer_, is bound here) --
// but the FIELDS MEAN SOMETHING DIFFERENT for a checkpoint restore:
// `old_material` is repurposed to carry "the material to restore", and
// `change_type`/pad0/pad1 are unused (every cell this shader sees always
// needs a fresh brick, so there is no NOOP/NEW_BRICK/MODIFIED distinction
// to make). Kept as the same struct/binding rather than a new one so this
// pass can share chunk_sculpt_stamp_set_'s already-existing descriptor set
// wholesale -- see VulkanRaymarchShader::restore_sculpt_checkpoint()'s own
// comment.
struct RestoreMeta {
    int change_type; // Unused.
    int material;    // The brick's material id to restore.
    int pad0;
    int pad1;
};

layout(std430, binding = 6) readonly buffer RestoreMetaBuffer {
    RestoreMeta restore_meta[];
};

// Reuses Builtin.SculptStamp.comp.glsl's UndoCaptureVoxelBuffer layout
// exactly (same buffer, chunk_sculpt_undo_capture_voxel_buffer_) -- stride
// CHUNK_BRICK_VOXEL_COUNT per StampCellBuffer slot, this cell's full
// captured voxel snapshot.
layout(std430, binding = 7) readonly buffer RestoreVoxelBuffer {
    float restore_voxels[];
};

// Reused from Builtin.SculptStamp.comp.glsl's identical binding -- a freshly
// allocated brick's paint voxels must be explicitly zeroed (checkpoint
// capture does not snapshot the Spraypaint paint buffer -- see
// VulkanRaymarchShader::capture_sculpt_checkpoint()'s own comment for why --
// so without this, a restored brick could carry over whatever stale paint
// data a long-gone earlier occupant of this same brick index last left
// behind).
layout(std430, binding = 8) buffer ChunkBrickPaintBuffer {
    float chunk_brick_paint[];
};

// Must match kMaxChunkBricks engine-side exactly -- same cross-check as
// every other sculpt-family shader's identical constant.
const int MAX_CHUNK_BRICKS = 16384 * NUM_CHUNK_LEVELS;

layout(push_constant) uniform PushConstants {
    int cell_count; // How many of StampCellBuffer/RestoreMetaBuffer/
                   // RestoreVoxelBuffer's entries are valid this dispatch --
                   // matches Builtin.SculptUndoRestore.comp.glsl's identical
                   // single-member push constant block (VulkanRaymarchShader
                   // reuses that same C++ struct, SculptUndoRestorePushConstants,
                   // for this pipeline too).
} pc;

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= uint(pc.cell_count)) {
        return;
    }

    int chunk_slot = cells[idx].slot_cell.x;
    int local_cell_index = cells[idx].slot_cell.y;
    int indirection_index = chunk_slot * CHUNK_CELL_COUNT + local_cell_index;

    // Should always be -1 here (reset_chunked_field() just wiped the table,
    // and VulkanRaymarchShader::restore_sculpt_checkpoint() vkCmdFillBuffer()s
    // every freshly-claimed slot's indirection sub-block to -1 before this
    // dispatch runs, exactly like stamp_shape() does for ITS new slots) --
    // defensive fallback (overwrite the existing brick in place rather than
    // leaking a second allocation) if it somehow isn't.
    int existing_brick = chunk_indirection[indirection_index];
    int brick_index;
    if (existing_brick < 0) {
        // Exact same free-list pop idiom as Builtin.SculptStamp.comp.glsl's
        // own is_new branch -- see its comment for the exact atomic idiom
        // this mirrors.
        uint free_before = atomicAdd(brick_free_top, 0xFFFFFFFFu);
        if (free_before == 0u || free_before > uint(MAX_CHUNK_BRICKS)) {
            // Pool exhausted -- undo the decrement and drop this cell. Same
            // degrade every other sculpt shader uses; shouldn't happen in
            // practice since a checkpoint can never need more bricks than
            // were ever resident when it was captured, which already fit.
            atomicAdd(brick_free_top, 1u);
            return;
        }
        brick_index = brick_free_list[free_before - 1u];
        chunk_indirection[indirection_index] = brick_index;
    } else {
        brick_index = existing_brick;
    }

    chunk_brick_primitive[brick_index] = restore_meta[idx].material;

    int base = brick_index * CHUNK_BRICK_VOXEL_COUNT;
    int voxel_base = int(idx) * CHUNK_BRICK_VOXEL_COUNT;
    for (int i = 0; i < CHUNK_BRICK_VOXEL_COUNT; ++i) {
        chunk_bricks[base + i] = restore_voxels[voxel_base + i];
        chunk_brick_paint[base + i] = 0.0;
    }
}
