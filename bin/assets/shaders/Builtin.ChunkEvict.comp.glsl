#version 450
#extension GL_GOOGLE_include_directive : require

// Frees a BATCH of resident chunks' bricks back onto the shared free-list
// stack and clears each one's indirection sub-block to empty -- the GPU-
// side half of ChunkStreamingManager (engine/src/systems/chunk_streaming_
// manager.h) evicting a chunk. This shader has no awareness of *when* it's
// safe to run -- that's entirely ChunkStreamingManager's ring-delay (see
// its header comment): the caller must not record this dispatch until the
// chunk's own voxelize_chunk() bake is confirmed complete, and must not
// hand gpu_slot to a new chunk's voxelize_chunk() until THIS dispatch is
// itself confirmed complete.
//
// One invocation per chunk-local coarse cell (CHUNK_COARSE_DIM^3), times
// however many chunks this dispatch's batch covers -- Phase 6: one
// dispatch now handles every chunk a frame needs evicted (boundary AND
// dirty-triggered forced re-bakes alike -- see VulkanRaymarchShader::
// update_streaming()'s own comment) instead of one dispatch per chunk.
// Same Z-encoding scheme as Builtin.ChunkVoxelize.comp.glsl's main() --
// see its own comment.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

#include "Builtin.SdfFieldConfig.inc.glsl"

// Must match kMaxChunkBricks engine-side (and Builtin.ChunkVoxelize.comp.
// glsl's identical MAX_BRICKS -- see its own comment for the NUM_CHUNK_
// LEVELS multiplication) -- the shared brick pool this chunk's bricks are
// being returned to.
const int MAX_BRICKS = 16384 * NUM_CHUNK_LEVELS;

layout(binding = 0) buffer ChunkIndirectionBuffer {
    int chunk_indirection[];
};

layout(binding = 1) buffer BrickFreeListBuffer {
    int free_list[];
};

layout(binding = 2) buffer BrickFreeListTopBuffer {
    uint free_list_top;
};

// Only chunk_slot is needed here (unlike Builtin.ChunkVoxelize.comp.glsl's
// richer ChunkBatchEntry) -- freeing a chunk's bricks needs no world-space
// info at all, just which slot to clear. A separate buffer (not shared with
// the voxelize batch) since the two dispatches' batches can hold entirely
// different chunks, written and read at different points in the same
// frame's command buffer.
layout(binding = 3) buffer ChunkEvictBatchBuffer {
    int evict_batch_slots[];
};

layout(push_constant) uniform PushConstants {
    int batch_count;
} push;

void main() {
    int batch_index = int(gl_GlobalInvocationID.z) / CHUNK_COARSE_DIM;
    if (batch_index >= push.batch_count) {
        return;
    }
    ivec3 local_cell = ivec3(gl_GlobalInvocationID.xy,
                             int(gl_GlobalInvocationID.z) % CHUNK_COARSE_DIM);
    if (local_cell.x >= CHUNK_COARSE_DIM || local_cell.y >= CHUNK_COARSE_DIM) {
        return;
    }

    int chunk_slot = evict_batch_slots[batch_index];
    int local_cell_index = local_cell.x + local_cell.y * CHUNK_COARSE_DIM +
        local_cell.z * CHUNK_COARSE_DIM * CHUNK_COARSE_DIM;
    int cell_index = chunk_slot * CHUNK_CELL_COUNT + local_cell_index;

    int brick_index = chunk_indirection[cell_index];
    if (brick_index >= 0) {
        // Push back onto the free-list stack -- the exact inverse of
        // Builtin.ChunkVoxelize.comp.glsl's pop (atomicAdd(-1) there,
        // atomicAdd(+1) here). free_list_top's value right before this
        // atomicAdd is where this index gets written; the total count of
        // outstanding allocations can never exceed MAX_BRICKS (every
        // successful pop there is matched by at most one push here, per
        // brick), so this can't legitimately overflow free_list[]'s own
        // capacity -- the bounds check is defensive only.
        uint write_index = atomicAdd(free_list_top, 1u);
        if (write_index < uint(MAX_BRICKS)) {
            free_list[write_index] = brick_index;
        }
        chunk_indirection[cell_index] = -1;
    }
}
