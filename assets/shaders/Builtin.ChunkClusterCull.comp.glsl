#version 450
#extension GL_GOOGLE_include_directive : require

// Cluster culling -- step 3 of the Dreams-gap work. One thread per cluster
// page; the survivors are appended to a compact list that Builtin.
// ChunkPointSplat.comp.glsl is then dispatched over INDIRECTLY, so the splat
// pass costs what the screen contains rather than what the pool can hold.
//
// Before this existed the splat pass dispatched one workgroup per pool slot,
// every frame, whether or not the slot held a resident cluster and whether
// or not that cluster was anywhere near the view -- 131,072 workgroups of
// which the overwhelming majority did nothing but read a record and exit.
// Dreams culls a BVH over its clusters for the same reason; this is the flat
// version of that (one level, no tree), which is enough while the pool is
// small enough to touch once per frame and is the natural place to hang a
// hierarchy off later.
//
// The same sweep also builds the SHADOW work list (step 5): for every
// resident cluster, every local light whose influence radius reaches it
// produces one (cluster, light) pair, and Builtin.ChunkShadowSplat.comp.glsl
// is dispatched once per pair. Pairing here rather than looping over lights
// inside the shadow pass is what keeps that pass's cost proportional to the
// work that actually exists -- a cluster near no light produces no pairs at
// all, and a 256-thread workgroup never spends 255 of its threads
// re-deciding the same light-range test. Shadow pairs deliberately ignore
// the camera frustum: an occluder behind the camera still casts.
//
// Everything rejected from the VISIBLE list is rejected because it CANNOT
// contribute a pixel:
// a free page, a box entirely outside the frustum, or a cluster whose normal
// cone faces entirely away. Clusters that are visible but too coarse to
// splat are deliberately KEPT -- they still have to post their conservative
// near bound (see Builtin.ChunkPointSplat.comp.glsl's DECLINED CLUSTERS
// STILL OCCLUDE note), and dropping them here would punch holes in exactly
// the geometry that bound protects.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#include "Builtin.SdfFieldConfig.inc.glsl"

// Must match kMaxChunkClusters engine-side.
const int MAX_CLUSTERS = 131072;
// Must match kNumLevels * kMaxResidentChunks engine-side -- the size of
// chunk_slot_published[] below.
const int CHUNK_MAX_SLOTS = NUM_CHUNK_LEVELS * 512;

// Must match kMaxShadowPairs engine-side -- the cap on how many (cluster,
// light) pairs one frame can cast. Overflowing it drops shadow casters
// rather than corrupting anything, which is why it is a plain cap and not a
// hard error.
const int MAX_SHADOW_PAIRS = 262144;

// Must match SPLAT_DISPATCH_STRIDE_X in Builtin.ChunkPointSplat.comp.glsl:
// the splat dispatch is folded 2D because a visible-cluster count can exceed
// Vulkan's guaranteed 65535 workgroups in one dimension.
const int SPLAT_DISPATCH_STRIDE_X = 1024;

layout(binding = 0) readonly buffer ChunkClusterBuffer {
    ChunkCluster chunk_clusters[];
};

// The survivors, densely packed. Indices into chunk_clusters, in whatever
// order the atomics happened to produce -- the splat pass has no ordering
// requirement (it resolves visibility with atomicMin, not with draw order).
layout(binding = 1) buffer VisibleClusterBuffer {
    uint visible_clusters[];
};

// Four uints: [0..2] are the vkCmdDispatchIndirect arguments the splat pass
// is launched with, [3] is the visible count itself (the splat pass needs it
// to bounds-check, since the dispatch is rounded up to whole rows of
// SPLAT_DISPATCH_STRIDE_X workgroups). Cleared to zero before this pass.
layout(binding = 2) buffer ClusterCullArgsBuffer {
    uint cull_args[];
};

// Lights, in the same layout every other pass sees them (see LightBuffer in
// Builtin.DeferredShade.comp.glsl): vector_type.xyz is a position for a
// point light (type 1) and colour_intensity.a its intensity.
struct Light {
    vec4 vector_type;
    vec4 colour_intensity;
    vec4 source_primitive;
};
layout(binding = 3) readonly buffer LightBuffer {
    Light lights[];
};

// The shadow work list: one entry per (cluster, light) pair, packed as
// cluster_index * ISM_COUNT + ism_slot. Its own four-uint argument block
// follows the same convention as cull_args.
layout(binding = 4) buffer ShadowPairBuffer {
    uint shadow_pairs[];
};
layout(binding = 5) buffer ShadowArgsBuffer {
    uint shadow_args[];
};

// Every resident cluster, and its own dispatch arguments -- see this
// shader's header. Consumed by Builtin.ChunkVoxelCascade.comp.glsl.
layout(binding = 6) buffer ResidentClusterBuffer {
    uint resident_clusters[];
};
layout(binding = 7) buffer ResidentArgsBuffer {
    uint resident_args[];
};

// NUM_CHUNK_LEVELS * kMaxResidentChunks entries -- one per chunk gpu_slot
// across every clip level, matching CHUNK_MAX_SLOTS below. Non-zero means
// "this slot's bake has completed AND been published by the CPU"; zero
// means either nothing lives there or a bake is filling it in right now.
//
// This is what lets the graphics queue stop waiting on the async bake.
// Before, every frame that queued chunk work made its own submission wait
// on that bake's semaphore, so a 2-second bake was a 2-second frame -- the
// bake ran on the async queue but the schedule was still fully serial. The
// chunk TABLE alone is not enough to decouple them: it is consulted by the
// field sampler, but the cull/splat/cascade passes sweep the cluster pool
// FLAT (meta.y != 0 is the only liveness marker), so they would happily
// read pages a running bake is still writing. Gating on the owning chunk's
// publication here closes that, and the bake's own record write orders
// meta last behind a memoryBarrierBuffer() so a published page is never
// half-described.
layout(binding = 8) readonly buffer ChunkSlotPublishedBuffer {
    uint chunk_slot_published[];
};
// The drag instance (Builtin.SculptInstance.inc.glsl), after the flags.
#define SCULPT_INSTANCE_WORD(i) int(chunk_slot_published[CHUNK_MAX_SLOTS + (i)])
#include "Builtin.SculptInstance.inc.glsl"

layout(push_constant) uniform PushConstants {
    vec4 camera_position; // xyz + pad
    vec4 camera_forward;
    vec4 camera_right;
    vec4 camera_up;
    // Half-extents of the view rectangle at unit forward distance, in the
    // same uv units Builtin.RaymarchShader.comp.glsl builds rays in: uv.x
    // spans +-width/(2*height), uv.y spans +-0.5. Passing them rather than a
    // projection matrix keeps this pass using the exact same camera model as
    // every other pass here.
    float half_extent_x;
    float half_extent_y;
    int cluster_count; // How many pool slots to test (kMaxChunkClusters).
    int light_count;   // How many of lights[] to pair clusters against.
    int ism_count;     // How many shadow-map slots exist (kIsmCount). Only
                      // the first this-many lights get one; the rest fall
                      // back to the SDF shadow march.
    int shadow_enabled; // Zero skips shadow pairing entirely (the imperfect
                       // shadow maps are off), leaving the marched shadows
                       // exactly as they were.
    int pad0;
    int pad1;
    // Occlusion culling: the camera and render size of the last frame,
    // whose farthest depth per 16x16 tile is in occlusion_tiles.
    // occlusion_size.z = 0 = off (no valid tiles).
    vec4 occlusion_position;
    vec4 occlusion_forward;
    vec4 occlusion_right;
    vec4 occlusion_up;
    ivec4 occlusion_size;
} push;

layout(binding = 9) readonly buffer OcclusionTileBuffer {
    uint occlusion_tiles[];
};

// True when the box was entirely hidden behind last frame's surfaces: its
// nearest point is farther from that frame's camera than the farthest
// surface in every tile its projection covers (one tile of margin all
// round, for TAA jitter and a frame's camera motion). A tile with any sky
// in it reads infinitely far, so silhouettes never hide anything, and a box
// reaching past the edge of that frame's screen is never culled (it may be
// in view now). Moved or deleted occluders cost one frame of lag at most:
// the next frame's tiles show what was uncovered.
bool occluded(vec3 box_min, vec3 box_max) {
    if (push.occlusion_size.z == 0) {
        return false;
    }
    vec3 cam = push.occlusion_position.xyz;
    vec3 fwd = push.occlusion_forward.xyz;
    vec3 right = push.occlusion_right.xyz;
    vec3 up = push.occlusion_up.xyz;
    vec2 size = vec2(push.occlusion_size.xy);
    vec2 lo = vec2(1e30);
    vec2 hi = vec2(-1e30);
    for (int i = 0; i < 8; ++i) {
        vec3 corner = vec3((i & 1) != 0 ? box_max.x : box_min.x,
                           (i & 2) != 0 ? box_max.y : box_min.y,
                           (i & 4) != 0 ? box_max.z : box_min.z);
        vec3 v = corner - cam;
        float z = dot(v, fwd);
        if (z < 0.05) {
            return false; // reaches the camera plane
        }
        // Same mapping as the splat pass (pixel = uv * height + size / 2).
        vec2 pixel = vec2(dot(v, right), dot(v, up)) / z * size.y + 0.5 * size;
        lo = min(lo, pixel);
        hi = max(hi, pixel);
    }
    if (any(lessThan(lo, vec2(0.0))) || any(greaterThan(hi, size))) {
        return false;
    }
    ivec2 tiles = (push.occlusion_size.xy + 15) / 16;
    ivec2 t0 = max(ivec2(floor(lo / 16.0)) - 1, ivec2(0));
    ivec2 t1 = min(ivec2(floor(hi / 16.0)) + 1, tiles - 1);
    ivec2 span = t1 - t0 + 1;
    if (span.x * span.y > 64) {
        return false; // too big on screen to be worth the reads
    }
    vec3 outside = max(max(box_min - cam, cam - box_max), vec3(0.0));
    float nearest = length(outside);
    float threshold = nearest / 1.01 - 0.02;
    for (int ty = t0.y; ty <= t1.y; ++ty) {
        for (int tx = t0.x; tx <= t1.x; ++tx) {
            if (uintBitsToFloat(occlusion_tiles[ty * tiles.x + tx]) >= threshold) {
                return false; // something in this tile is as far as the box
            }
        }
    }
    return true;
}

// Distance past which a point light of this intensity contributes less than
// roughly 1/256 of a unit -- its inverse-square falloff means there is
// always such a distance, and beyond it a caster cannot darken anything
// measurably. This is the same cutoff the shadow splat pass uses.
float light_radius(float intensity) {
    return sqrt(max(intensity, 0.0) * 256.0);
}

// True when the axis-aligned box is entirely outside the view frustum.
//
// All five planes pass through the camera position (the four sides of a
// perspective frustum meet at the eye, and the near plane is treated as
// passing through it too), so each one is just an outward normal and a
// point-relative-to-camera dot product. The normals fall straight out of
// this renderer's ray construction: a point is inside the right plane when
// dot(v, right) <= half_extent_x * dot(v, forward), i.e. when
// dot(v, right - half_extent_x * forward) <= 0.
//
// A box is entirely outside a plane exactly when its LEAST-positive corner
// along that normal is still outside -- picking that corner per component is
// what makes this one dot product instead of eight.
bool frustum_cull(vec3 box_min, vec3 box_max) {
    vec3 cam = push.camera_position.xyz;
    vec3 fwd = push.camera_forward.xyz;
    vec3 right = push.camera_right.xyz;
    vec3 up = push.camera_up.xyz;

    vec3 normals[5] = vec3[5](
        right - push.half_extent_x * fwd,
        -right - push.half_extent_x * fwd,
        up - push.half_extent_y * fwd,
        -up - push.half_extent_y * fwd,
        -fwd); // near plane: everything with dot(v, fwd) < 0 is behind us
    for (int i = 0; i < 5; ++i) {
        vec3 n = normals[i];
        vec3 nearest = mix(box_max, box_min, step(vec3(0.0), n));
        if (dot(nearest - cam, n) > 0.0) {
            return true; // every corner is outside this plane
        }
    }
    return false;
}

void main() {
    int cluster_index = int(gl_GlobalInvocationID.x);
    if (cluster_index >= push.cluster_count) {
        return;
    }

    ChunkCluster cluster = chunk_clusters[cluster_index];
    if (cluster.meta.y == 0u) {
        return; // page is on the free list
    }
    // The owning chunk's slot rides in meta.w's high 16 bits (see the
    // voxelizer's own comment where it packs this). A slot that has not
    // been published is either mid-bake or already retired -- either way
    // its pages must not reach the splat, shadow or cascade passes, all of
    // which are dispatched from lists built right here.
    uint owner_slot = cluster.meta.w >> 16;
    if (owner_slot >= uint(CHUNK_MAX_SLOTS) ||
        chunk_slot_published[owner_slot] == 0u) {
        return;
    }

    vec3 box_min = cluster.bbox_min_cell.xyz;
    vec3 box_max = box_min + vec3(cluster.bbox_min_cell.w);
    vec4 normal_cone = cluster.normal_cone;
    // A drag instance's cluster is in its drag-start pose: tested where it
    // is now. It stays out of the resident list (GI) and the shadow maps
    // for the drag -- both would show it where it was; the marched shadows
    // see it where it is.
    bool instance = sculpt_instance_owns_cluster(cluster.meta.w);
    if (instance) {
        sculpt_instance_box_to_world(box_min, box_max, box_min, box_max);
        normal_cone.xyz = sculpt_instance_dir_to_world(normal_cone.xyz);
    }
    vec3 box_centre = (box_min + box_max) * 0.5;
    float box_radius_all = length(box_max - box_min) * 0.5;

    // Resident list: no test at all beyond "this page holds points".
    if (!instance) {
        uint slot = atomicAdd(resident_args[3], 1u);
        if (slot < uint(MAX_CLUSTERS)) {
            resident_clusters[slot] = uint(cluster_index);
            atomicMax(resident_args[1],
                      (slot + uint(SPLAT_DISPATCH_STRIDE_X)) /
                          uint(SPLAT_DISPATCH_STRIDE_X));
            resident_args[0] = uint(SPLAT_DISPATCH_STRIDE_X);
            resident_args[2] = 1u;
        }
    }

    // --- Shadow pairing (step 5). Before any camera-visibility test: a
    // cluster the camera cannot see still casts a shadow into a light's
    // map, and dropping it here is exactly how off-screen geometry stops
    // shadowing on-screen geometry. ---
    if (push.shadow_enabled != 0 && !instance) {
        int pairable = min(push.light_count, push.ism_count);
        for (int li = 0; li < pairable; ++li) {
            Light light = lights[li];
            if (int(light.vector_type.w) != 1) {
                continue; // only point lights get an imperfect shadow map
            }
            float reach = light_radius(light.colour_intensity.a) + box_radius_all;
            vec3 to_light = light.vector_type.xyz - box_centre;
            if (dot(to_light, to_light) > reach * reach) {
                continue;
            }
            uint slot = atomicAdd(shadow_args[3], 1u);
            if (slot >= uint(MAX_SHADOW_PAIRS)) {
                break; // list full -- this frame simply casts fewer shadows
            }
            shadow_pairs[slot] = uint(cluster_index) * uint(push.ism_count) +
                uint(li);
            atomicMax(shadow_args[1],
                      (slot + uint(SPLAT_DISPATCH_STRIDE_X)) /
                          uint(SPLAT_DISPATCH_STRIDE_X));
            shadow_args[0] = uint(SPLAT_DISPATCH_STRIDE_X);
            shadow_args[2] = 1u;
        }
    }

    if (frustum_cull(box_min, box_max)) {
        return;
    }

    // Normal-cone backface rejection.
    //
    // A surface faces the camera when dot(normal, view_dir) < 0, where
    // view_dir points FROM the camera TO the surface. This cluster is
    // entirely backfacing only when that holds with the wrong sign for every
    // normal in its cone AND every point in its box, i.e. when
    //
    //     angle(cone_axis, view_dir) + cone_half_angle + box_half_angle < 90
    //
    // Rearranged into a dot product: cull when
    // dot(axis, view_dir) > cos(90 - slack) == sin(slack). The sin is the
    // whole point -- an earlier version of this test compared against
    // cos(slack), which is the same thing only at slack == 45 degrees and
    // which culls almost everything once the cone is wide (a cluster
    // wrapping a corner has cone_cos near or below zero, giving slack > 90
    // and cos(slack) < 0, so nearly any orientation "passed"). That silently
    // deleted whole clusters of perfectly visible geometry.
    //
    // A slack of 90 degrees or more means the cone alone already spans a
    // hemisphere: some normal always faces the camera, so nothing is ever
    // culled. The distance guard skips the test entirely when the camera is
    // close enough to the box for its subtended angle to dominate.
    vec3 to_cluster = box_centre - push.camera_position.xyz;
    float dist = length(to_cluster);
    float box_radius = box_radius_all;
    if (dist > box_radius * 2.0) {
        vec3 view_dir = to_cluster / dist;
        float cone_cos = clamp(normal_cone.w, -1.0, 1.0);
        float slack = acos(cone_cos) + asin(clamp(box_radius / dist, 0.0, 1.0));
        const float HALF_PI = 1.5707963;
        if (slack < HALF_PI &&
            dot(normal_cone.xyz, view_dir) > sin(slack)) {
            return; // whole cone faces away from the camera
        }
    }

    if (occluded(box_min, box_max)) {
        return; // hidden behind what last frame drew
    }

    uint slot = atomicAdd(cull_args[3], 1u);
    if (slot >= uint(MAX_CLUSTERS)) {
        return; // list full (cannot happen while it is sized to the pool)
    }
    visible_clusters[slot] = uint(cluster_index);

    // Grow the indirect dispatch to cover this slot. Every appending thread
    // does this, so the final value is the largest row index any of them
    // needed -- no separate finalize pass, and no dependence on the order
    // the atomics happened to run in.
    atomicMax(cull_args[1],
              (slot + uint(SPLAT_DISPATCH_STRIDE_X)) /
                  uint(SPLAT_DISPATCH_STRIDE_X));
    cull_args[0] = uint(SPLAT_DISPATCH_STRIDE_X);
    cull_args[2] = 1u;
}
