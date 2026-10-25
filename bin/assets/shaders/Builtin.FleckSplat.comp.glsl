#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_ARB_gpu_shader_int64 : require
#extension GL_EXT_shader_atomic_int64 : require

// Paint mode's free-floating flecks, splatted straight into the splat
// visibility buffer alongside the surface points (Builtin.ChunkPointSplat.
// comp.glsl) -- a fleck has no field, so it is drawn only here. ONE
// WORKGROUP PER FLECK: its threads share the pixels of its footprint.
//
// Each fleck is a flat card (local +Z its facing, +X its direction) of
// one of the 16 fleck styles (Builtin.FleckShape.inc.glsl), raised into a
// little dab of paint by its relief, two-sided. Every covered pixel stores
// its exact ray distance to the card, so the render pass places and lights
// it from that alone.
//
// PAYLOAD. Same packing as the surface points' (see that shader), marked
// by a combination they never write: looseness bits (28-29) 0 with ruffle
// bits (30-31) 3. The id bits (0-24) are the fleck's index into
// FreeFleckBuffer.

#include "Builtin.FleckShape.inc.glsl"

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// One per fleck (VulkanRaymarchShader::upload_free_flecks()).
struct FreeFleck {
    vec4 position_radius; // xyz, w = radius (world)
    vec4 normal_slot;     // xyz = facing, w = material slot
    vec4 direction_style; // xyz = direction along the fleck, w = style 0-15
    vec4 object_seed;     // x = object id, y = random seed 0..1
};
layout(set = 0, binding = 7) readonly buffer FreeFleckBuffer {
    FreeFleck free_flecks[];
};
layout(set = 0, binding = 8) buffer SplatVisibilityBuffer {
    uint64_t splat_visibility[];
};

// Workgroups are laid out in rows of this many (the per-dimension limit).
const uint FLECK_DISPATCH_STRIDE_X = 32768u;
const int FLECK_SPLAT_MAX_RADIUS_PX = 96;

layout(push_constant) uniform PushConstants {
    vec4 camera_position; // xyz; w = this frame's TAA jitter x (pixels)
    vec4 camera_forward;  // xyz; w = this frame's TAA jitter y (pixels)
    vec4 camera_right;
    vec4 camera_up;
    int width;
    int height;
    int fleck_count;
    int pad;
} push;

void main() {
    uint index = gl_WorkGroupID.y * FLECK_DISPATCH_STRIDE_X + gl_WorkGroupID.x;
    if (index >= uint(push.fleck_count)) {
        return;
    }
    FreeFleck fleck = free_flecks[index];
    vec3 p = fleck.position_radius.xyz;
    float radius = fleck.position_radius.w;
    vec3 n = normalize(fleck.normal_slot.xyz);
    vec3 t = fleck.direction_style.xyz;
    t = normalize(t - n * dot(t, n));
    vec3 b = cross(n, t);
    int style = clamp(int(round(fleck.direction_style.w)), 0, 15);
    uint seed = uint(fleck.object_seed.y * 16777215.0) * 2654435761u + index;
    seed ^= seed >> 15;
    seed *= 2246822519u;

    vec3 cam = push.camera_position.xyz;
    vec3 v = p - cam;
    float z = dot(v, push.camera_forward.xyz);
    if (z < 0.05 || radius <= 0.0) {
        return;
    }
    float inv_z = 1.0 / z;
    vec2 uv = vec2(dot(v, push.camera_right.xyz), dot(v, push.camera_up.xyz)) * inv_z;
    vec2 size = vec2(float(push.width), float(push.height));
    vec2 pixel = uv * size.y + 0.5 * size;
    ivec2 centre = ivec2(round(pixel));

    // The card's axes on screen (pixels per unit of fleck square).
    vec2 st = (vec2(dot(t, push.camera_right.xyz), dot(t, push.camera_up.xyz)) -
               uv * dot(t, push.camera_forward.xyz)) * inv_z * size.y * radius;
    vec2 sb = (vec2(dot(b, push.camera_right.xyz), dot(b, push.camera_up.xyz)) -
               uv * dot(b, push.camera_forward.xyz)) * inv_z * size.y * radius;
    vec2 extent = abs(st) + abs(sb);
    float reach = max(extent.x, extent.y);
    if (reach > float(FLECK_SPLAT_MAX_RADIUS_PX)) {
        float shrink = float(FLECK_SPLAT_MAX_RADIUS_PX) / reach;
        st *= shrink;
        sb *= shrink;
        reach = float(FLECK_SPLAT_MAX_RADIUS_PX);
    }
    float det = st.x * sb.y - st.y * sb.x;
    bool edge_on = abs(det) < 0.25;
    mat2 to_fleck = edge_on ? mat2(1.0) : mat2(sb.y, -st.y, -sb.x, st.x) / det;
    int r = edge_on ? 0 : max(int(ceil(reach)), 1);

    uint payload = (index & 0x01FFFFFFu) | (3u << 30);
    float point_dist = length(v);
    vec2 jitter = vec2(push.camera_position.w, push.camera_forward.w);
    vec2 sample_offset = vec2(centre) + jitter - pixel;
    int side = 2 * r + 1;
    for (int k = int(gl_LocalInvocationID.x); k < side * side; k += 64) {
        ivec2 d = ivec2(k % side, k / side) - ivec2(r);
        ivec2 q = centre + d;
        if (q.x < 0 || q.y < 0 || q.x >= push.width || q.y >= push.height) {
            continue;
        }
        float dome = 0.0;
        if (!edge_on &&
            !fleck_covers(style, to_fleck * (vec2(d) + sample_offset), seed,
                          fleck_detail(float(r)), dome)) {
            continue;
        }
        vec2 ray_uv = (vec2(q) + jitter - 0.5 * size) / size.y;
        vec3 ray = normalize(ray_uv.x * push.camera_right.xyz +
                             ray_uv.y * push.camera_up.xyz + push.camera_forward.xyz);
        float denom = dot(ray, n);
        float dist = abs(denom) > 1e-3 ? dot(p - cam, n) / denom : point_dist;
        // The dab rises toward the viewer, whichever side it is seen from.
        dist -= FLECK_DOME * radius * dome / max(abs(denom), 0.3);
        uint64_t entry = (uint64_t(floatBitsToUint(max(dist, 1e-3))) << 32) |
                         uint64_t(payload);
        atomicMin(splat_visibility[q.y * push.width + q.x], entry);
    }
}
