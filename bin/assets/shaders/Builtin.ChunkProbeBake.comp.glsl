#version 450
#extension GL_GOOGLE_include_directive : require

// Phase 5: camera-centered GI cascade bake -- the chunked-field counterpart
// of Builtin.ProbeBake.comp.glsl (see that file's header comment for the
// gather/bounce technique itself, identical here), gathering against the
// CHUNKED/streamed field (sample_clipmap_field()/chunked_shadow_march(),
// Builtin.ChunkedFieldCommon.inc.glsl) instead of the fixed-cube one, and
// centered on push.cascade_center (recentered engine-side in discrete
// whole-cell steps as the camera moves -- see VulkanRaymarchShader::
// update_gi_cascade()) instead of a compile-time-fixed BOUNDS cube. A
// deliberate full parallel copy rather than a shared/parameterized
// function, same reasoning as Builtin.ChunkVoxelize.comp.glsl vs. Builtin.
// RaymarchVoxelize.comp.glsl: keeps this newer path from being able to
// affect Builtin.ProbeBake.comp.glsl (still used verbatim by the
// fixed-cube field's own GI) at all.
//
// One invocation per probe (PROBE_DIM^3 total), run kProbeBounceCount times
// per cascade bake (see VulkanRaymarchShader::bake_gi_cascade()).
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

#include "Builtin.SdfFieldConfig.inc.glsl"

// This cascade's own probe grid shape -- same PROBE_DIM as the fixed-cube
// field's single grid (Builtin.ProbeBake.comp.glsl), but spanning a much
// smaller, camera-centered volume (GI_CASCADE_CELL_SIZE apart, not spread
// across the whole BOUNDS cube) -- see push.cascade_center below for
// placement. Must match kGiProbeDim/kGiCascadeCellSize in vulkan_raymarch_
// shader.cpp exactly.
const int PROBE_DIM = 16;
const float GI_CASCADE_CELL_SIZE = 2.0;
// Comfortably covers this cascade's own full diagonal ((PROBE_DIM-1) *
// GI_CASCADE_CELL_SIZE * sqrt(3) =~ 52) from any interior point -- same
// role as Builtin.ProbeBake.comp.glsl's identical MAX_DIST, just resized
// for this cascade's own (smaller, camera-relative) extent.
const float MAX_DIST = 54.0;
const float SURF_DIST = 0.01;
const int GATHER_MAX_STEPS = 128;

// Same shadow-ray tuning rationale as Builtin.ProbeBake.comp.glsl's
// identical constants -- see its own comment.
const float SHADOW_NORMAL_BIAS = 0.12;
const float SHADOW_SOFTNESS = 16.0;
const int SHADOW_MAX_STEPS = 64;

#define CHUNKED_FIELD_TABLE_BINDING 0
#define CHUNKED_FIELD_INDIRECTION_BINDING 1
#define CHUNKED_FIELD_BRICKPOOL_BINDING 2
#define CHUNKED_FIELD_BRICKPRIMITIVE_BINDING 3
#include "Builtin.ChunkedFieldCommon.inc.glsl"

layout(binding = 4) readonly buffer ScenePrimitiveColours {
    vec4 scene_diffuse_colours[];
};

struct Light {
    vec4 vector_type;
    vec4 colour_intensity;
    vec4 source_primitive;
};

layout(binding = 5) readonly buffer LightBuffer {
    Light lights[];
};

layout(binding = 6) readonly buffer PrevProbeBuffer {
    vec4 prev_probes[];
};

layout(binding = 7) writeonly buffer CurrProbeBuffer {
    vec4 curr_probes[];
};

layout(push_constant) uniform PushConstants {
    int light_count;
    float ambient;
    float cascade_center_x;
    float cascade_center_y;
    float cascade_center_z;
    float camera_x;
    float camera_y;
    float camera_z;
} push;

vec3 cascade_grid_min() {
    vec3 center = vec3(push.cascade_center_x, push.cascade_center_y,
                       push.cascade_center_z);
    float half_extent = float(PROBE_DIM - 1) * 0.5 * GI_CASCADE_CELL_SIZE;
    return center - vec3(half_extent);
}

vec3 probe_world_position(ivec3 index) {
    return cascade_grid_min() + vec3(index) * GI_CASCADE_CELL_SIZE;
}

// Trilinearly samples the *previous* bounce's probe grid -- see Builtin.
// ProbeBake.comp.glsl's identical sample_prev_probes() for why this is
// duplicated rather than shared.
vec3 sample_prev_probes(vec3 p) {
    vec3 grid_min = cascade_grid_min();
    vec3 local = (p - grid_min) / GI_CASCADE_CELL_SIZE;
    vec3 local_clamped = clamp(local, vec3(0.0), vec3(float(PROBE_DIM - 1)));
    ivec3 i0 = ivec3(floor(local_clamped));
    ivec3 i1 = min(i0 + ivec3(1), ivec3(PROBE_DIM - 1));
    vec3 t = local_clamped - vec3(i0);

    #define PROBE_AT(ix, iy, iz) prev_probes[(ix) + (iy) * PROBE_DIM + (iz) * PROBE_DIM * PROBE_DIM].rgb

    vec3 c000 = PROBE_AT(i0.x, i0.y, i0.z);
    vec3 c100 = PROBE_AT(i1.x, i0.y, i0.z);
    vec3 c010 = PROBE_AT(i0.x, i1.y, i0.z);
    vec3 c110 = PROBE_AT(i1.x, i1.y, i0.z);
    vec3 c001 = PROBE_AT(i0.x, i0.y, i1.z);
    vec3 c101 = PROBE_AT(i1.x, i0.y, i1.z);
    vec3 c011 = PROBE_AT(i0.x, i1.y, i1.z);
    vec3 c111 = PROBE_AT(i1.x, i1.y, i1.z);

    #undef PROBE_AT

    vec3 c00 = mix(c000, c100, t.x);
    vec3 c10 = mix(c010, c110, t.x);
    vec3 c01 = mix(c001, c101, t.x);
    vec3 c11 = mix(c011, c111, t.x);
    vec3 c0 = mix(c00, c10, t.y);
    vec3 c1 = mix(c01, c11, t.y);
    return mix(c0, c1, t.z);
}

vec3 camera_pos_vec() {
    return vec3(push.camera_x, push.camera_y, push.camera_z);
}

// Normal of the CHUNKED field at p via finite differences -- see Builtin.
// ProbeBake.comp.glsl's identical calc_static_normal() for the technique
// and the skip_dist fallback's rationale.
vec3 calc_static_normal(vec3 p) {
    vec3 dummy_dir = vec3(0.0, 0.0, 1.0);
    vec3 camera_pos = camera_pos_vec();
    vec2 e = vec2(0.0025, 0.0);
    float dist_p, skip_p;
    int unused_material;
    sample_clipmap_field(p, dummy_dir, camera_pos, dist_p, skip_p, unused_material);

    float dx0, dx1, dy0, dy1, dz0, dz1, skip;
    sample_clipmap_field(p + e.xyy, dummy_dir, camera_pos, dx1, skip, unused_material);
    if (skip != 0.0) dx1 = dist_p;
    sample_clipmap_field(p - e.xyy, dummy_dir, camera_pos, dx0, skip, unused_material);
    if (skip != 0.0) dx0 = dist_p;
    sample_clipmap_field(p + e.yxy, dummy_dir, camera_pos, dy1, skip, unused_material);
    if (skip != 0.0) dy1 = dist_p;
    sample_clipmap_field(p - e.yxy, dummy_dir, camera_pos, dy0, skip, unused_material);
    if (skip != 0.0) dy0 = dist_p;
    sample_clipmap_field(p + e.yyx, dummy_dir, camera_pos, dz1, skip, unused_material);
    if (skip != 0.0) dz1 = dist_p;
    sample_clipmap_field(p - e.yyx, dummy_dir, camera_pos, dz0, skip, unused_material);
    if (skip != 0.0) dz0 = dist_p;
    return normalize(vec3(dx1 - dx0, dy1 - dy0, dz1 - dz0));
}

// See Builtin.ProbeBake.comp.glsl's identical relax_probe_origin() for why
// this exists (nudging a probe that landed too close to/inside geometry
// out along the surface normal before gathering).
const float PROBE_SAFE_DISTANCE = 0.3;

vec3 relax_probe_origin(vec3 origin) {
    float dist, skip_dist;
    int material;
    sample_clipmap_field(origin, vec3(0.0, 0.0, 1.0), camera_pos_vec(), dist,
                         skip_dist, material);
    bool valid = (skip_dist == 0.0);
    if (valid && dist < PROBE_SAFE_DISTANCE) {
        vec3 normal = calc_static_normal(origin);
        origin += normal * (PROBE_SAFE_DISTANCE - dist);
    }
    return origin;
}

float gather_raymarch(vec3 origin, vec3 dir, out int hit_material) {
    vec3 camera_pos = camera_pos_vec();
    float travelled = 0.0;
    hit_material = -1;
    for (int i = 0; i < GATHER_MAX_STEPS; ++i) {
        vec3 p = origin + dir * travelled;
        float dist, skip_dist;
        int material;
        sample_clipmap_field(p, dir, camera_pos, dist, skip_dist, material);
        bool valid = (skip_dist == 0.0);

        if (valid && abs(dist) < SURF_DIST) {
            hit_material = material;
            break;
        }

        float step = valid ? abs(dist) : skip_dist;
        travelled += max(step, SURF_DIST);

        if (travelled > MAX_DIST) {
            break;
        }
    }
    return travelled;
}

vec3 direct_diffuse_at(vec3 p, vec3 normal, vec3 albedo) {
    vec3 camera_pos = camera_pos_vec();
    vec3 result = vec3(0.0);
    for (int i = 0; i < push.light_count; ++i) {
        Light light = lights[i];
        int light_type = int(light.vector_type.w);
        vec3 light_colour = light.colour_intensity.rgb;
        float intensity = light.colour_intensity.a;

        vec3 light_dir;
        float attenuation;
        float shadow_max_dist;
        if (light_type == 1) {
            vec3 to_light = light.vector_type.xyz - p;
            float dist = length(to_light);
            light_dir = to_light / max(dist, 0.0001);
            attenuation = intensity / max(dist * dist, 0.0001);
            shadow_max_dist = dist;
        } else {
            light_dir = normalize(light.vector_type.xyz);
            attenuation = intensity;
            shadow_max_dist = MAX_DIST;
        }

        float diffuse = max(dot(normal, light_dir), 0.0);
        if (diffuse > 0.0) {
            float shadow = chunked_shadow_march(p + normal * SHADOW_NORMAL_BIAS,
                                                light_dir, camera_pos, shadow_max_dist,
                                                SHADOW_SOFTNESS, SHADOW_MAX_STEPS,
                                                int(light.source_primitive.x));
            diffuse *= shadow;
        }
        result += light_colour * diffuse * attenuation;
    }
    return result * albedo;
}

vec3 gather_hit_radiance(vec3 origin, vec3 dir) {
    int hit_material;
    float travelled = gather_raymarch(origin, dir, hit_material);
    if (hit_material < 0) {
        return vec3(push.ambient);
    }

    vec3 p = origin + dir * travelled;
    vec3 normal = calc_static_normal(p);
    vec3 albedo = scene_diffuse_colours[hit_material].rgb;

    vec3 indirect = sample_prev_probes(p) * albedo;
    return direct_diffuse_at(p, normal, albedo) + indirect;
}

vec3 fibonacci_sphere_dir(int i, int n) {
    const float GOLDEN_ANGLE = 3.14159265 * (3.0 - sqrt(5.0));
    float y = 1.0 - (float(i) / float(n - 1)) * 2.0;
    float radius_at_y = sqrt(max(0.0, 1.0 - y * y));
    float theta = GOLDEN_ANGLE * float(i);
    return vec3(cos(theta) * radius_at_y, y, sin(theta) * radius_at_y);
}

const int PROBE_GATHER_SAMPLES = 32;

void main() {
    ivec3 index = ivec3(gl_GlobalInvocationID);
    if (index.x >= PROBE_DIM || index.y >= PROBE_DIM || index.z >= PROBE_DIM) {
        return;
    }
    int probe_index = index.x + index.y * PROBE_DIM + index.z * PROBE_DIM * PROBE_DIM;

    vec3 origin = relax_probe_origin(probe_world_position(index));

    vec3 accum = vec3(0.0);
    for (int s = 0; s < PROBE_GATHER_SAMPLES; ++s) {
        vec3 dir = fibonacci_sphere_dir(s, PROBE_GATHER_SAMPLES);
        accum += gather_hit_radiance(origin, dir);
    }
    accum /= float(PROBE_GATHER_SAMPLES);

    curr_probes[probe_index] = vec4(accum, 1.0);
}
