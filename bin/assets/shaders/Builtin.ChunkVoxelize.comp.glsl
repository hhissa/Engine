#version 450
#extension GL_GOOGLE_include_directive : require

// Chunked-field voxelizer -- Phase 3a of the clipmap streaming work (see
// ChunkKey/ChunkRecord, engine/src/systems/chunk_types.h). One dispatch of
// this bakes exactly ONE chunk's CHUNK_COARSE_DIM^3 coarse cells into the
// SEPARATE chunked-field buffers (ChunkIndirectionBuffer/ChunkBrickPool
// Buffer/ChunkBrickPrimitiveBuffer below), at whichever gpu_slot the caller
// (VulkanRaymarchShader::voxelize_chunk()) assigned it -- unlike Builtin.
// RaymarchVoxelize.comp.glsl's single dispatch over the whole fixed
// COARSE_DIM^3 grid, this is meant to be dispatched once per resident chunk,
// letting a caller (re)bake just the chunks that actually changed.
//
// Deliberately a full parallel copy of RaymarchVoxelize.comp.glsl's
// structure rather than a shared/parameterized function: sharing would mean
// this new, less-battle-tested path and the existing one both depend on the
// same code, so a bug introduced here could reach back and affect
// sdf_editor/games -- see Builtin.SdfFieldConfig.inc.glsl's comment on why
// CHUNK_COARSE_DIM etc. are a separate field entirely. The actual analytic
// SDF evaluation (scene_map()) IS still shared, via Builtin.SdfSceneCommon.
// inc.glsl below, exactly like the old voxelizer -- that file has no
// COARSE_DIM/chunk awareness at all, it just evaluates the scene at a given
// world point, so sharing it carries none of that risk.
//
// One invocation per chunk-local coarse cell (CHUNK_COARSE_DIM^3 total).
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

#include "Builtin.SdfFieldConfig.inc.glsl"

// Must match kMaxChunkBricks in vulkan_raymarch_shader.cpp exactly -- this
// is the SAME brick pool capacity concept as Builtin.RaymarchVoxelize.comp.
// glsl's MAX_BRICKS, just governing the chunked field's own, separate,
// smaller pool (see ChunkBrickPoolBuffer below), SHARED across every clip
// level (see NUM_CHUNK_LEVELS, Builtin.SdfFieldConfig.inc.glsl). 4096 is
// the per-level share -- must match the literal 4096 in vulkan_raymarch_
// shader.cpp's kMaxChunkBricks = 4096 * kNumLevels exactly (NUM_CHUNK_
// LEVELS itself already has to match kNumLevels there too, so multiplying
// by it here rather than hardcoding the product keeps this in sync
// automatically if kNumLevels/NUM_CHUNK_LEVELS ever changes together --
// only the per-level 4096 needs updating by hand on both sides).
const int MAX_BRICKS = 4096 * NUM_CHUNK_LEVELS;

// Sized kMaxResidentChunks * CHUNK_CELL_COUNT engine-side -- one
// CHUNK_CELL_COUNT-sized dense sub-block per resident chunk slot, indexed by
// push.chunk_slot below. Slot assignment (which world chunk owns which
// slot) is a CPU-side scheduling decision (see ChunkStreamingManager, a
// later phase) this shader has no awareness of -- it just writes wherever
// push.chunk_slot points.
layout(binding = 0) buffer ChunkIndirectionBuffer {
    int chunk_indirection[];
};

layout(binding = 1) buffer ChunkBrickPoolBuffer {
    float chunk_bricks[];
};

// Unconditionally incremented every time a cell decides it needs a brick,
// regardless of whether the free-list pop below actually succeeds -- mirrors
// Builtin.RaymarchVoxelize.comp.glsl's own BrickCounterBuffer exactly (see
// its comment): lets the CPU side detect "the chunked pool's demand exceeded
// its capacity" after a bake by comparing this against MAX_BRICKS, the same
// diagnostic check check_brick_overflow() already does for the old field.
layout(binding = 2) buffer ChunkBrickDemandBuffer {
    uint chunk_brick_demand;
};

// A stack of free brick indices (see BrickFreeListTopBuffer below for the
// stack pointer) -- CPU-initialized to [0, 1, 2, ..., MAX_BRICKS-1] with the
// top pointer at MAX_BRICKS every time the chunked field starts fresh (see
// VulkanRaymarchShader's chunked-field init), then popped from here instead
// of Builtin.RaymarchVoxelize.comp.glsl's monotonic atomicAdd -- this is
// what makes an evicted chunk's bricks reusable by a future chunk bake (a
// later phase pushes freed indices back onto this same stack on eviction;
// this shader only ever pops).
layout(binding = 3) buffer BrickFreeListBuffer {
    int free_list[];
};

// How many entries free_list currently holds, from the top. Popping is
// atomicAdd(free_list_top, -1): the returned (pre-decrement) value is how
// many entries existed before this pop, so a fetched value of N means the
// entry this invocation claimed is free_list[N-1], and popping when N was
// already 0 wraps to a huge unsigned value -- both handled in main() below,
// exactly mirroring how the old path's brick_index >= MAX_BRICKS overflow
// check works today.
layout(binding = 4) buffer BrickFreeListTopBuffer {
    uint free_list_top;
};

// The analytic scene SDF -- identical shared include to Builtin.
// RaymarchVoxelize.comp.glsl (see this file's own header comment for why
// this one dependency IS shared safely).
#define SDF_PRIMITIVE_BUFFER_BINDING 5
#define SDF_LAYER_BUFFER_BINDING 7
#define SDF_PARAM_EXPR_BUFFER_BINDING 8
#include "Builtin.SdfSceneCommon.inc.glsl"

layout(binding = 6) buffer ChunkBrickPrimitiveBuffer {
    int chunk_brick_primitive[];
};

layout(push_constant) uniform PushConstants {
    int layer_count;
    float max_smoothness; // See CULL_RADIUS_CELLS below -- same role as the
                         // old voxelizer's identical push constant.
    int chunk_slot;       // Which ChunkIndirectionBuffer sub-block (and
                         // implicitly, since this whole dispatch bakes one
                         // chunk, which chunk) this invocation writes into.
    float chunk_world_min_x;
    float chunk_world_min_y;
    float chunk_world_min_z; // This chunk's min corner in RENDER space (see
                            // VulkanRendererBackend::world_origin_offset_) --
                            // split into three scalars rather than a vec3 to
                            // avoid std430 push-constant vec3 alignment
                            // padding surprises, matching this codebase's
                            // existing convention (see e.g. GpuPrimitive's
                            // float[4] fields engine-side) of preferring
                            // explicit scalars/float[N] over vec3 wherever
                            // layout matters.
    float chunk_cell_size; // This chunk's own fine-voxel cell size (Phase 4:
                          // chunk_level_world_size(level) / CHUNK_COARSE_DIM,
                          // computed engine-side -- NOT the shared
                          // COARSE_CELL_SIZE constant, which is only ever
                          // correct for level 0). A coarser (higher-level)
                          // chunk covers more world space per cell, which is
                          // exactly what makes it cheaper to bake/sample --
                          // see Builtin.SdfFieldConfig.inc.glsl's comment on
                          // chunk_level_world_size().
} push;

// Same rationale/tuning as Builtin.RaymarchVoxelize.comp.glsl's identical
// constant -- see its own comment.
const float CULL_RADIUS_CELLS = 6.0;

void main() {
    ivec3 local_cell = ivec3(gl_GlobalInvocationID);
    if (local_cell.x >= CHUNK_COARSE_DIM || local_cell.y >= CHUNK_COARSE_DIM ||
        local_cell.z >= CHUNK_COARSE_DIM) {
        return;
    }

    int local_cell_index = local_cell.x + local_cell.y * CHUNK_COARSE_DIM +
        local_cell.z * CHUNK_COARSE_DIM * CHUNK_COARSE_DIM;
    int cell_index = push.chunk_slot * CHUNK_CELL_COUNT + local_cell_index;

    vec3 chunk_world_min = vec3(push.chunk_world_min_x, push.chunk_world_min_y,
                                push.chunk_world_min_z);
    float chunk_cell_size = push.chunk_cell_size;
    vec3 cell_min = chunk_world_min + vec3(local_cell) * chunk_cell_size;
    vec3 cell_center = cell_min + vec3(chunk_cell_size * 0.5);

    // No cull here -- same reasoning as the old voxelizer's identical call:
    // this is the one evaluation that decides whether a brick exists at all
    // for this cell, so it has to see the whole scene regardless of
    // distance.
    int nearest_primitive;
    float center_dist = scene_map(cell_center, push.layer_count,
                                  UNBOUNDED_BOUNDING_RADIUS, nearest_primitive);

    float half_diagonal = chunk_cell_size * 0.8660254;

    chunk_indirection[cell_index] = -1;

    if (nearest_primitive >= 0 && abs(center_dist) <= half_diagonal) {
        atomicAdd(chunk_brick_demand, 1u);

        // Pop one index off free_list (see BrickFreeListBuffer's own
        // comment for exactly why this arithmetic is correct, including
        // the empty-stack case).
        uint free_count_before = atomicAdd(free_list_top, 0xFFFFFFFFu);
        if (free_count_before > 0u && free_count_before <= uint(MAX_BRICKS)) {
            int brick_index = free_list[free_count_before - 1u];
            chunk_indirection[cell_index] = brick_index;
            chunk_brick_primitive[brick_index] = nearest_primitive;

            float voxel_size = chunk_cell_size / float(BRICK_DIM);
            float cull_radius = chunk_cell_size * CULL_RADIUS_CELLS + push.max_smoothness;
            // Local index range is -1..BRICK_DIM inclusive (the apron),
            // mapped to storage index range 0..BRICK_APRON_DIM-1 via +1 --
            // identical scheme to the old voxelizer's brick population.
            for (int vz = -1; vz <= BRICK_DIM; ++vz) {
                for (int vy = -1; vy <= BRICK_DIM; ++vy) {
                    for (int vx = -1; vx <= BRICK_DIM; ++vx) {
                        vec3 voxel_pos = cell_min + (vec3(vx, vy, vz) + 0.5) * voxel_size;
                        int unused_nearest;
                        float d = scene_map(voxel_pos, push.layer_count, cull_radius,
                                            unused_nearest);
                        ivec3 store = ivec3(vx, vy, vz) + ivec3(1);
                        int local_index = store.x + store.y * BRICK_APRON_DIM + store.z * BRICK_APRON_DIM * BRICK_APRON_DIM;
                        chunk_bricks[brick_index * BRICK_VOXEL_COUNT + local_index] = d;
                    }
                }
            }
        } else {
            // Free list was already empty -- undo the wraparound decrement
            // so a later successful pop elsewhere doesn't read a bogus
            // (huge, wrapped) free_list_top forever. Leaves chunk_indirection
            // at -1 for this cell (no brick), same visible failure mode as
            // the old path's brick_index >= MAX_BRICKS case: a missing
            // patch of surface, not a crash -- chunk_brick_demand having
            // exceeded MAX_BRICKS is exactly what a caller checks
            // afterward to explain why.
            atomicAdd(free_list_top, 1u);
        }
    }
}
