#version 450
#extension GL_GOOGLE_include_directive : require

// Binary voxel cascades -- the world-space half of step 6's ambient
// occlusion, and about as cheap as an occlusion structure can be: one BIT
// per voxel, "is there surface here or not", rebuilt from the point cloud
// every frame by atomic-OR.
//
// Dreams built exactly this ("I tried voxelizing the scene - since we have
// point clouds, it was fairly easy to generate a work list with LOD adapted
// to 4 world cascades, and atomic OR each voxel into a 1 bit per voxel dense
// cascaded volume texture"), and used it the same way: the ambient-occlusion
// ray starts in the depth buffer for contact-scale detail, then switches to
// these for everything beyond it, dropping to coarser cascades as it gets
// further from its origin. The result is cone-trace-like without any of
// prefiltering's memory cost -- all the softness comes from stochastic
// sampling plus TAA.
//
// Four cascades, each VOXEL_CASCADE_DIM^3 bits, centred on the camera and
// snapped to their own voxel size so the contents don't swim as it moves.
// The whole structure is 128KB, which is why rebuilding it from scratch
// every frame is cheaper than trying to keep it incrementally correct.
//
// ONE WORKGROUP PER RESIDENT CLUSTER, indirectly dispatched over the list
// Builtin.ChunkClusterCull.comp.glsl builds -- resident, not visible:
// geometry behind the camera occludes just as much as geometry in front.

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

#include "Builtin.SdfFieldConfig.inc.glsl"

const int MAX_CLUSTERS = 131072;
const int SPLAT_DISPATCH_STRIDE_X = 1024;

// Must match kVoxelCascadeDim/kVoxelCascadeCount engine-side, and the copies
// in Builtin.StochasticAo.comp.glsl.
const int VOXEL_CASCADE_DIM = 64;
const int VOXEL_CASCADE_COUNT = 4;
const int VOXEL_CASCADE_WORDS =
    (VOXEL_CASCADE_DIM * VOXEL_CASCADE_DIM * VOXEL_CASCADE_DIM) / 32;

layout(binding = 0) readonly buffer ChunkClusterPointBuffer {
    uvec2 cluster_points[];
};
layout(binding = 1) readonly buffer ChunkClusterBuffer {
    ChunkCluster chunk_clusters[];
};
layout(binding = 2) readonly buffer ResidentClusterBuffer {
    uint resident_clusters[];
};
layout(binding = 3) readonly buffer ResidentArgsBuffer {
    uint resident_args[];
};
// VOXEL_CASCADE_COUNT * VOXEL_CASCADE_WORDS uints, cascade-major. Cleared to
// zero before this pass.
layout(binding = 4) buffer VoxelCascadeBuffer {
    uint voxel_cascades[];
};

layout(push_constant) uniform PushConstants {
    // Per cascade: xyz = the min corner of its volume in RENDER space, w =
    // its voxel size. Computed engine-side (snapped to the voxel grid) so
    // this pass and the trace pass cannot disagree about where a voxel is.
    vec4 cascade_origin[VOXEL_CASCADE_COUNT];
} push;

void main() {
    uint list_index = uint(gl_WorkGroupID.y) * uint(SPLAT_DISPATCH_STRIDE_X) +
        uint(gl_WorkGroupID.x);
    if (list_index >= resident_args[3]) {
        return;
    }
    int cluster_index = int(resident_clusters[list_index]);
    if (cluster_index >= MAX_CLUSTERS) {
        return;
    }

    ChunkCluster cluster = chunk_clusters[cluster_index];
    uint used = cluster.meta.y;
    uint local_index = gl_LocalInvocationID.x;
    if (local_index >= used) {
        return;
    }

    // Only the coarsest LOD level's points are voxelized. Even cascade 0's
    // voxels are far larger than the finest point spacing, so the extra
    // points would set bits already set -- pure atomic traffic for no
    // information. The coarse level is a strict prefix of the fine one, so
    // this loses no coverage.
    uint brick_point_index = cluster.meta.x + local_index;
    if (brick_point_index >= max(cluster.lod_counts[0], 1u)) {
        return;
    }

    uvec2 packed = cluster_points[uint(cluster_index) *
                                  uint(CHUNK_CLUSTER_POINTS) + local_index];
    vec3 frac = vec3(float(packed.x & 1023u), float((packed.x >> 10) & 1023u),
                     float((packed.x >> 20) & 1023u)) * (1.0 / 1023.0);
    vec3 p = cluster.bbox_min_cell.xyz + frac * cluster.bbox_min_cell.w;

    // A point lands in every cascade whose volume contains it, not just the
    // finest: the trace walks outward through coarser cascades and has to
    // find the same geometry there.
    for (int c = 0; c < VOXEL_CASCADE_COUNT; ++c) {
        vec3 origin = push.cascade_origin[c].xyz;
        float voxel = push.cascade_origin[c].w;
        ivec3 v = ivec3(floor((p - origin) / voxel));
        if (any(lessThan(v, ivec3(0))) ||
            any(greaterThanEqual(v, ivec3(VOXEL_CASCADE_DIM)))) {
            continue;
        }
        int bit_index = v.x + v.y * VOXEL_CASCADE_DIM +
            v.z * VOXEL_CASCADE_DIM * VOXEL_CASCADE_DIM;
        atomicOr(voxel_cascades[c * VOXEL_CASCADE_WORDS + (bit_index >> 5)],
                 1u << (bit_index & 31));
    }
}
