#version 450
#extension GL_GOOGLE_include_directive : require

// Imperfect shadow maps -- step 5 of the Dreams-gap work.
//
// The idea (Ritschel et al. 2008, and what Dreams shipped: "now we are in
// loads-of-points land, we did the obvious thing and moved to imperfect
// shadow maps") is that a point cloud is already a perfectly good shadow
// caster. Instead of marching a ray per pixel per light through the distance
// field, each light gets a small depth map that the SAME cluster points are
// splatted into, once per frame, with the same atomicMin trick the primary
// visibility pass uses. Shading then costs one texture-style lookup per
// light instead of a whole march.
//
// The maps are deliberately low quality -- 128x128, undersampled, splatted
// with single points and no filtering. That is what "imperfect" means, and
// what makes 64 of them affordable where 64 marched shadows are not. The
// noise this leaves is exactly what the TAA resolve is for.
//
// ONE WORKGROUP PER (CLUSTER, LIGHT) PAIR, indirectly dispatched over the
// list Builtin.ChunkClusterCull.comp.glsl built this frame -- so a cluster
// near no light costs nothing, and a cluster near three lights is splatted
// three times, which is exactly the work that exists.
//
// OCTAHEDRAL, NOT PARABOLOID. Dreams used paraboloid maps, which need two
// hemispheres (and a seam between them) to cover a point light's whole
// sphere of directions. An octahedral map covers the full sphere in one
// square tile with a cheap, seam-free-in-the-interior mapping, and its
// distortion is lower. Same idea, better projection.

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

#include "Builtin.SdfFieldConfig.inc.glsl"

// Must match the engine-side constants of the same name.
const int MAX_CLUSTERS = 131072;
const int ISM_RESOLUTION = 128;  // edge of one light's map, in texels
const int ISM_COUNT = 64;        // how many lights can have one
const int ISM_ATLAS_DIM = 8;     // tiles per row in the atlas (8 * 8 == 64)
const int MAX_SHADOW_PAIRS = 262144; // must match the cull pass's own cap
const int SPLAT_DISPATCH_STRIDE_X = 1024;

layout(binding = 0) readonly buffer ChunkClusterPointBuffer {
    uvec2 cluster_points[];
};
layout(binding = 1) readonly buffer ChunkClusterBuffer {
    ChunkCluster chunk_clusters[];
};

struct Light {
    vec4 vector_type;
    vec4 colour_intensity;
    vec4 source_primitive;
};
layout(binding = 2) readonly buffer LightBuffer {
    Light lights[];
};

// The shadow atlas: ISM_COUNT tiles of ISM_RESOLUTION^2 texels, each holding
// the distance from that light to the nearest surface in that direction, as
// raw float bits so atomicMin resolves nearest-wins (the same "positive
// floats order like their bit patterns" trick the visibility buffer uses).
// Cleared to ~0 ("nothing here") before every build.
layout(binding = 3) buffer ShadowAtlasBuffer {
    uint shadow_atlas[];
};

// This frame's (cluster, light) pairs and their dispatch arguments -- see
// Builtin.ChunkClusterCull.comp.glsl. [3] is the pair count.
layout(binding = 4) readonly buffer ShadowPairBuffer {
    uint shadow_pairs[];
};
layout(binding = 5) readonly buffer ShadowArgsBuffer {
    uint shadow_args[];
};

// Per-brick primitive provenance (see Builtin.ChunkVoxelize.comp.glsl's
// nearest_primitive store) -- needed to keep a light's own source primitive
// from shadow-casting into its own map, below.
layout(binding = 6) readonly buffer ChunkBrickPrimitiveBuffer {
    int chunk_brick_primitive[];
};

layout(push_constant) uniform PushConstants {
    int frame_index; // Seeds the stochastic thinning below.
    int pad0;
    int pad1;
    int pad2;
} push;

// How much of a point's own footprint to cover in the map, in texels. A
// single texel per point leaves pinholes that read as light leaking through
// solid geometry; a small square closes them at a cost that is linear in its
// area. 1 gives a 3x3 stamp.
const int ISM_SPLAT_RADIUS = 1;

// Direction -> point in the unit square, octahedral mapping (the same one
// pack_point_normal() uses for normals, kept separate here because this one
// wants full float precision and no packing).
vec2 direction_to_octahedral(vec3 d) {
    vec3 a = d / max(abs(d.x) + abs(d.y) + abs(d.z), 1e-8);
    vec2 oct = a.z >= 0.0
        ? a.xy
        : (1.0 - abs(a.yx)) * vec2(a.x >= 0.0 ? 1.0 : -1.0,
                                   a.y >= 0.0 ? 1.0 : -1.0);
    return clamp(oct * 0.5 + 0.5, 0.0, 1.0);
}

void main() {
    uint pair_index = uint(gl_WorkGroupID.y) * uint(SPLAT_DISPATCH_STRIDE_X) +
        uint(gl_WorkGroupID.x);
    // The count is incremented before the cull pass checks the cap, and a
    // single cluster can emit several pairs, so it CAN exceed the list's
    // capacity -- clamp, or the overflow reads past the buffer and splats
    // garbage pairs into the atlas.
    if (pair_index >= min(shadow_args[3], uint(MAX_SHADOW_PAIRS))) {
        return; // last row of the dispatch overshoots the real count
    }

    uint pair = shadow_pairs[pair_index];
    int cluster_index = int(pair / uint(ISM_COUNT));
    int ism_slot = int(pair % uint(ISM_COUNT));
    if (cluster_index >= MAX_CLUSTERS) {
        return;
    }

    ChunkCluster cluster = chunk_clusters[cluster_index];
    uint used = cluster.meta.y;
    uint local_index = gl_LocalInvocationID.x;
    if (local_index >= used) {
        return; // partially filled page, or a page freed since the cull
    }

    // Only the coarsest LOD level's points are splatted. A shadow map this
    // small cannot resolve more, and the coarse level is a strict subset of
    // the fine one (see the point generator's pyramid), so this is a free
    // 4-16x reduction in shadow work with no gaps -- the footprint below is
    // sized to the spacing the coarse level actually has.
    uint brick_point_index = cluster.meta.x + local_index;
    uint coarse_count = max(cluster.lod_counts[0], 1u);
    if (brick_point_index >= coarse_count) {
        return;
    }

    Light light = lights[ism_slot];
    if (int(light.vector_type.w) != 1) {
        return; // not a point light after all (light set changed this frame)
    }

    // A light synthesized from an emissive primitive sits INSIDE that
    // primitive's own surface, so the primitive's baked points surround the
    // light at near-zero distance in every direction. Splatting them into
    // this light's own map stamps tiny occluder distances across most of the
    // sphere, and every surface in the room then fails the depth test except
    // where the emitter's point cloud happens to have angular gaps -- which
    // reads as large mottled shadow blotches radiating from the light. The
    // marched shadow paths already exclude the source primitive (see
    // shadow_march()'s ignore_primitive); this is the same exclusion for the
    // splatted path, at brick-provenance granularity because that is what a
    // cluster carries.
    int source_primitive = int(light.source_primitive.x);
    if (source_primitive >= 0 &&
        chunk_brick_primitive[int(cluster.meta.z)] == source_primitive) {
        return;
    }

    uvec2 packed = cluster_points[uint(cluster_index) *
                                  uint(CHUNK_CLUSTER_POINTS) + local_index];
    vec3 frac = vec3(float(packed.x & 1023u), float((packed.x >> 10) & 1023u),
                     float((packed.x >> 20) & 1023u)) * (1.0 / 1023.0);
    vec3 p = cluster.bbox_min_cell.xyz + frac * cluster.bbox_min_cell.w;

    vec3 to_point = p - light.vector_type.xyz;
    float dist = length(to_point);
    if (dist < 1e-4) {
        return;
    }

    vec2 oct = direction_to_octahedral(to_point / dist);
    ivec2 tile = ivec2(ism_slot % ISM_ATLAS_DIM, ism_slot / ISM_ATLAS_DIM);
    ivec2 texel = ivec2(oct * float(ISM_RESOLUTION - 1) + 0.5);

    uint depth_bits = floatBitsToUint(dist);
    for (int dy = -ISM_SPLAT_RADIUS; dy <= ISM_SPLAT_RADIUS; ++dy) {
        for (int dx = -ISM_SPLAT_RADIUS; dx <= ISM_SPLAT_RADIUS; ++dx) {
            ivec2 q = texel + ivec2(dx, dy);
            // Clamped, not wrapped: the octahedral map's edges fold onto
            // each other, so a wrapped neighbour is a genuinely different
            // direction and stamping into it would cast a shadow from the
            // wrong side of the light.
            if (q.x < 0 || q.y < 0 || q.x >= ISM_RESOLUTION ||
                q.y >= ISM_RESOLUTION) {
                continue;
            }
            ivec2 atlas = tile * ISM_RESOLUTION + q;
            atomicMin(shadow_atlas[atlas.y * (ISM_ATLAS_DIM * ISM_RESOLUTION) +
                                   atlas.x],
                      depth_bits);
        }
    }
}
