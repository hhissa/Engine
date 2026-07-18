#version 450
#extension GL_GOOGLE_include_directive : require

// Bakes a sparse-in-time (dense-in-space), regular 3D grid of indirect-
// light probes -- adapted from Inigo Quilez's "simplegi" technique
// (https://iquilezles.org/articles/simplegi/), which treats *mesh
// vertices* as light emitters/receivers: gather each vertex's incoming
// light by rendering a ~180-degree hemisphere from it, iterate a few
// bounces. This engine's SDF/voxel pipeline has no mesh anywhere in it
// (every surface is a procedural primitive, voxelized into a sparse brick
// field -- see Builtin.RaymarchVoxelize.comp.glsl), so there are no
// vertices to hang samples off of. The same core idea -- a handful of
// sample points that emit/receive light, gathered by "rendering" (here,
// raymarching) a sphere of directions from each, iterated a few bounces --
// is retargeted onto a spatial probe grid instead, the natural fit for a
// volume rather than a mesh surface. Two differences from the source
// technique, both noted where they matter below: probes gather over the
// *full* sphere (not a hemisphere -- a probe is a point in space, not a
// point *on* a surface, so it has no single normal to orient a hemisphere
// around), and this uses raymarching against the already-baked field
// rather than rasterizing a fresh scene render per sample point.
//
// Runs kProbeBounceCount times per rebake (see VulkanRaymarchShader::
// bake_probes()), immediately after voxelize() -- each dispatch is one
// bounce: every probe gathers PROBE_GATHER_SAMPLES directions, and each
// gather ray that hits a surface adds that surface's direct lighting
// PLUS whatever indirect light the *previous* bounce's probe grid already
// found near that hit point (see PrevProbeBuffer below) -- a standard
// Jacobi-style iterative relaxation: PrevProbeBuffer starts zeroed before
// the first bounce, so bounce 0 naturally gathers direct light only, and
// each subsequent bounce picks up one more level of indirection with no
// special-casing needed in the shader itself.
//
// One invocation per probe (PROBE_DIM^3 total).
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

// Must match COARSE_DIM/BRICK_DIM/BOUNDS etc. in Builtin.RaymarchShader.
// comp.glsl/Builtin.RaymarchVoxelize.comp.glsl exactly -- these describe
// the baked field this pass marches gather rays against, not this pass's
// own probe grid (see PROBE_DIM below for that).
const int COARSE_DIM = 128;
const int BRICK_DIM = 8;
const int BRICK_APRON_DIM = BRICK_DIM + 2;
const int BRICK_VOXEL_COUNT = BRICK_APRON_DIM * BRICK_APRON_DIM * BRICK_APRON_DIM;
const float BOUNDS = 16.0;
const float COARSE_CELL_SIZE = (2.0 * BOUNDS) / float(COARSE_DIM);
// Gather rays use a coarser hit tolerance and a shorter march cap than the
// primary camera ray (MAX_DIST=128/SURF_DIST=0.001 in the render pass): GI
// doesn't need pixel-precision convergence, and a gather ray only ever
// needs to cross this pass's own probe volume (BOUNDS), not reach a
// distant camera. Keeping both small is the main lever on total bake cost
// (PROBE_DIM^3 * PROBE_GATHER_SAMPLES rays per bounce, each up to this
// many steps).
const float MAX_DIST = 48.0; // comfortably covers the BOUNDS cube's diagonal from any interior point
const float SURF_DIST = 0.01;
const int GATHER_MAX_STEPS = 128;

#define BAKED_FIELD_INDIRECTION_BINDING 0
#define BAKED_FIELD_BRICKPOOL_BINDING 1
#define BAKED_FIELD_BRICKPRIMITIVE_BINDING 2
#include "Builtin.BakedFieldCommon.inc.glsl"

// rgb: the hit primitive's diffuse tint (see ScenePrimitiveColours in
// Builtin.RaymarchShader.comp.glsl, the same buffer) -- gather-ray hits
// use this flat albedo, not the full triplanar-textured/bump-mapped
// shading the primary ray does: a GI probe is already a coarse spatial
// approximation, so the extra cost of texture sampling here wouldn't
// buy back meaningfully more accuracy. a: material texture_scale, unused
// here.
layout(binding = 3) readonly buffer ScenePrimitiveColours {
    vec4 scene_diffuse_colours[];
};

// Mirrors the `Light` struct in Builtin.RaymarchShader.comp.glsl exactly
// (must match its GpuLight layout engine-side).
struct Light {
    vec4 vector_type;
    vec4 colour_intensity;
};

layout(binding = 4) readonly buffer LightBuffer {
    Light lights[];
};

// This probe grid's own dimensions -- must match kProbeDim in
// vulkan_raymarch_shader.cpp and PROBE_DIM in Builtin.RaymarchShader.
// comp.glsl exactly. Probes sit at the corners of a (PROBE_DIM-1)^3 cell
// grid spanning the full [-BOUNDS, BOUNDS] cube inclusive (index 0 at
// -BOUNDS, index PROBE_DIM-1 at +BOUNDS) -- see probe_world_position()
// below and sample_probe_grid() in Builtin.RaymarchShader.comp.glsl,
// which must use the exact same placement formula to read back what this
// pass wrote.
const int PROBE_DIM = 16;

layout(binding = 5) readonly buffer PrevProbeBuffer {
    vec4 prev_probes[];
};

layout(binding = 6) writeonly buffer CurrProbeBuffer {
    vec4 curr_probes[];
};

layout(push_constant) uniform PushConstants {
    int light_count;  // How many of LightBuffer's entries a gather-ray hit's
                     // direct lighting sums -- see direct_diffuse_at() below.
    float ambient;    // "Sky" colour a gather ray that exits the scene
                     // entirely (misses everything within MAX_DIST)
                     // contributes instead -- see main() below.
} push;

vec3 probe_world_position(ivec3 index) {
    float cell_size = (2.0 * BOUNDS) / float(PROBE_DIM - 1);
    return vec3(-BOUNDS) + vec3(index) * cell_size;
}

// Trilinearly samples the *previous* bounce's probe grid -- how
// gather_hit_radiance() below picks up indirect light already found near
// a gather ray's hit point, which is what turns repeated single-bounce
// gathers into genuine multi-bounce GI. Identical in structure to
// sample_probe_grid() in Builtin.RaymarchShader.comp.glsl (that one reads
// this pass's *final* output at render time); duplicated rather than
// shared since the two read different-named buffers (prev_probes here vs.
// this pass's own final result there) and the function is short enough
// that factoring it out would cost more in indirection than it saves.
vec3 sample_prev_probes(vec3 p) {
    float cell_size = (2.0 * BOUNDS) / float(PROBE_DIM - 1);
    vec3 local = (p + vec3(BOUNDS)) / cell_size;
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

// Normal of the baked static field at p via finite differences -- same
// technique as calc_static_normal() in Builtin.RaymarchShader.comp.glsl
// (duplicated rather than shared for the same reason sample_prev_probes()
// above is: short, and the two shaders' sample_field() come from a shared
// include already, so this is the one remaining small piece of drift risk
// -- acceptable given its size).
vec3 calc_static_normal(vec3 p) {
    vec3 dummy_dir = vec3(0.0, 0.0, 1.0);
    vec2 e = vec2(0.0025, 0.0);
    float dx0, dx1, dy0, dy1, dz0, dz1, skip;
    int unused_material;
    sample_field(p + e.xyy, dummy_dir, dx1, skip, unused_material);
    sample_field(p - e.xyy, dummy_dir, dx0, skip, unused_material);
    sample_field(p + e.yxy, dummy_dir, dy1, skip, unused_material);
    sample_field(p - e.yxy, dummy_dir, dy0, skip, unused_material);
    sample_field(p + e.yyx, dummy_dir, dz1, skip, unused_material);
    sample_field(p - e.yyx, dummy_dir, dz0, skip, unused_material);
    return normalize(vec3(dx1 - dx0, dy1 - dy0, dz1 - dz0));
}

// Marches from origin along dir against the baked field, coarser and
// shorter-ranged than the primary camera ray's raymarch() (see the
// SURF_DIST/MAX_DIST/GATHER_MAX_STEPS comments above) since GI gathering
// doesn't need pixel-precision convergence.
float gather_raymarch(vec3 origin, vec3 dir, out int hit_material) {
    float travelled = 0.0;
    hit_material = -1;
    for (int i = 0; i < GATHER_MAX_STEPS; ++i) {
        vec3 p = origin + dir * travelled;
        float dist, skip_dist;
        int material;
        sample_field(p, dir, dist, skip_dist, material);
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

// Direct diffuse lighting at a gather ray's hit point, tinted by albedo --
// no shadow rays (this engine's direct lighting has never cast shadows,
// on the primary ray or here; kept consistent between the two rather than
// introducing a discrepancy). This is deliberately the *only* light
// source term evaluated fresh per gather ray -- indirect/bounced light
// comes from sample_prev_probes() instead (see gather_hit_radiance()
// below), not from re-summing direct light recursively.
vec3 direct_diffuse_at(vec3 p, vec3 normal, vec3 albedo) {
    vec3 result = vec3(0.0);
    for (int i = 0; i < push.light_count; ++i) {
        Light light = lights[i];
        int light_type = int(light.vector_type.w);
        vec3 light_colour = light.colour_intensity.rgb;
        float intensity = light.colour_intensity.a;

        vec3 light_dir;
        float attenuation;
        if (light_type == 1) {
            vec3 to_light = light.vector_type.xyz - p;
            float dist = length(to_light);
            light_dir = to_light / max(dist, 0.0001);
            attenuation = intensity / max(dist * dist, 0.0001);
        } else {
            light_dir = normalize(light.vector_type.xyz);
            attenuation = intensity;
        }

        float diffuse = max(dot(normal, light_dir), 0.0);
        result += light_colour * diffuse * attenuation;
    }
    return result * albedo;
}

// The radiance a single gather ray brings back to the probe: on a miss,
// the scene's ambient/sky colour (so a probe near an open side of a scene
// isn't starved of light just because most of its rays escape the
// baked volume); on a hit, that surface's direct lighting plus whatever
// indirect light the previous bounce already found nearby (albedo-tinted
// both times, since a surface only reflects light in its own colour).
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

// Well-distributed points on the unit sphere (the golden-angle / Fibonacci
// sphere construction) -- deterministic, no RNG state or precomputed
// direction buffer needed, just this probe's sample index. A full sphere
// rather than IQ's original 180-degree hemisphere: a probe is a point in
// open space, not a point sitting *on* a surface, so it has no single
// normal to bias a hemisphere toward -- see the file header comment.
vec3 fibonacci_sphere_dir(int i, int n) {
    const float GOLDEN_ANGLE = 3.14159265 * (3.0 - sqrt(5.0));
    float y = 1.0 - (float(i) / float(n - 1)) * 2.0; // 1..-1
    float radius_at_y = sqrt(max(0.0, 1.0 - y * y));
    float theta = GOLDEN_ANGLE * float(i);
    return vec3(cos(theta) * radius_at_y, y, sin(theta) * radius_at_y);
}

// How many directions each probe gathers per bounce. Total bake cost per
// bounce is PROBE_DIM^3 * PROBE_GATHER_SAMPLES gather rays, each up to
// GATHER_MAX_STEPS field samples -- this is the other main cost lever
// alongside PROBE_DIM itself; raise it for smoother-looking probes at a
// proportional bake-time cost.
const int PROBE_GATHER_SAMPLES = 32;

void main() {
    ivec3 index = ivec3(gl_GlobalInvocationID);
    if (index.x >= PROBE_DIM || index.y >= PROBE_DIM || index.z >= PROBE_DIM) {
        return;
    }
    int probe_index = index.x + index.y * PROBE_DIM + index.z * PROBE_DIM * PROBE_DIM;

    vec3 origin = probe_world_position(index);

    vec3 accum = vec3(0.0);
    for (int s = 0; s < PROBE_GATHER_SAMPLES; ++s) {
        vec3 dir = fibonacci_sphere_dir(s, PROBE_GATHER_SAMPLES);
        accum += gather_hit_radiance(origin, dir);
    }
    accum /= float(PROBE_GATHER_SAMPLES);

    curr_probes[probe_index] = vec4(accum, 1.0);
}
