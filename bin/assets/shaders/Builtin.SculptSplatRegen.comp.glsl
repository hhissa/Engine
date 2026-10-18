#version 450
#extension GL_GOOGLE_include_directive : require

// M1a: regenerates the surface-point splats (see CHUNK_CLUSTER_POINTS,
// Builtin.SdfFieldConfig.inc.glsl) for every brick Builtin.SculptStamp.
// comp.glsl just (re)wrote, so the splat pipeline -- what final rendering
// actually samples, NOT the raw brick SDF values alone -- has something to
// draw for a freshly stamped sphere. Dispatched immediately after
// SculptStamp.comp.glsl, same command buffer, with a buffer barrier between
// them (see VulkanRaymarchShader::stamp_shape()).
//
// main() below is a thin adapter: it resolves each touched cell's brick
// index (written by the stamp dispatch moments ago) and calls
// generate_splat_points(), whose BODY is lifted near-verbatim from the now-
// deleted Builtin.ChunkVoxelize.comp.glsl -- preserved at Builtin.
// ChunkVoxelize.comp.glsl.SPLAT_REGEN_REFERENCE (see that file's header for
// the exact original line range and its full dependency list, reproduced
// here). One invocation per touched cell, same StampCellBuffer
// SculptStamp.comp.glsl read -- generate_splat_points() is pure scalar code
// (no parallelism within one brick), so parallelism here comes from one
// invocation per BRICK, exactly as it always ran.
layout(local_size_x = 64) in;

#include "Builtin.SdfFieldConfig.inc.glsl"

struct StampCell {
    ivec4 slot_cell;
    vec4 cell_min_size;
};

layout(std430, binding = 0) readonly buffer StampCellBuffer {
    StampCell cells[];
};

layout(std430, binding = 1) readonly buffer ChunkIndirectionBuffer {
    int chunk_indirection[];
};

layout(std430, binding = 2) readonly buffer ChunkBrickPoolBuffer {
    float chunk_bricks[];
};

// --- generate_splat_points()'s own dependencies (SPLAT_REGEN_REFERENCE). ---

layout(std430, binding = 3) buffer ChunkClusterFreeListBuffer {
    int cluster_free_list[];
};

layout(std430, binding = 4) buffer ChunkClusterFreeTopBuffer {
    uint cluster_free_top;
};

layout(std430, binding = 5) buffer ChunkClusterPointBuffer {
    uvec2 cluster_points[];
};

layout(std430, binding = 6) buffer ChunkClusterBuffer {
    ChunkCluster chunk_clusters[];
};

layout(std430, binding = 7) buffer ChunkBrickClusterBuffer {
    int brick_clusters[];
};

// Stats-only (see the `stats` parameter below) -- bound regardless since
// generate_splat_points() references it unconditionally even though the
// atomics inside only fire when stats is true; M1a always passes false.
layout(std430, binding = 8) buffer ChunkBrickDemandBuffer {
    uint chunk_brick_demand[];
};

// Must match kMaxChunkClusters engine-side (same cross-check as Builtin.
// ChunkEvict.comp.glsl's identical MAX_CLUSTERS).
const int MAX_CLUSTERS = 131072;
// M1a has no other concurrent stamp consuming the shared cluster pool, so
// there is nothing yet to reserve a page against -- see generate_splat_
// points()'s own comment for what these guard in the general case. Revisit
// once multiple in-flight sculptures/brushes can contend for the pool.
const int CLUSTER_FIRST_PAGE_RESERVE = 0;
const int CLUSTER_SECOND_PAGE_RESERVE = 0;

layout(push_constant) uniform PushConstants {
    int cell_count;
    int stats;           // bool, as an int (GLSL push-constant bools are
                         // disallowed) -- always 0 in M1a.
    float chunk_cell_size; // COARSE_CELL_SIZE -- shared by every cell.
    float voxel_size;      // chunk_cell_size / CHUNK_BRICK_DIM.
} pc;

#include "Builtin.ChunkVoxelize.comp.glsl.SPLAT_REGEN_REFERENCE"

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= uint(pc.cell_count)) {
        return;
    }

    int chunk_slot = cells[idx].slot_cell.x;
    int local_cell_index = cells[idx].slot_cell.y;
    vec3 cell_min = cells[idx].cell_min_size.xyz;

    int brick_index = chunk_indirection[chunk_slot * CHUNK_CELL_COUNT + local_cell_index];
    if (brick_index < 0) {
        // Shouldn't happen -- SculptStamp.comp.glsl must have run (with a
        // barrier) before this dispatch for every cell in this same list.
        // Defensive only.
        return;
    }

    generate_splat_points(brick_index, chunk_slot, cell_min, pc.chunk_cell_size,
                          pc.voxel_size, pc.stats != 0);
}
