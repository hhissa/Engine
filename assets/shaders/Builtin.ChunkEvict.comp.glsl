#version 450
#extension GL_GOOGLE_include_directive : require

// Frees a resident chunk's bricks back onto the shared free-list stack and
// clears its indirection sub-block to empty -- the GPU-side half of
// ChunkStreamingManager (engine/src/systems/chunk_streaming_manager.h)
// evicting a chunk. This shader has no awareness of *when* it's safe to run
// -- that's entirely ChunkStreamingManager's ring-delay (see its header
// comment): the caller must not record this dispatch until the chunk's own
// voxelize_chunk() bake is confirmed complete, and must not hand gpu_slot
// to a new chunk's voxelize_chunk() until THIS dispatch is itself confirmed
// complete.
//
// One invocation per chunk-local coarse cell (CHUNK_COARSE_DIM^3 total) --
// same dispatch shape as Builtin.ChunkVoxelize.comp.glsl.
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

layout(push_constant) uniform PushConstants {
    int chunk_slot;
} push;

void main() {
    ivec3 local_cell = ivec3(gl_GlobalInvocationID);
    if (local_cell.x >= CHUNK_COARSE_DIM || local_cell.y >= CHUNK_COARSE_DIM ||
        local_cell.z >= CHUNK_COARSE_DIM) {
        return;
    }

    int local_cell_index = local_cell.x + local_cell.y * CHUNK_COARSE_DIM +
        local_cell.z * CHUNK_COARSE_DIM * CHUNK_COARSE_DIM;
    int cell_index = push.chunk_slot * CHUNK_CELL_COUNT + local_cell_index;

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
