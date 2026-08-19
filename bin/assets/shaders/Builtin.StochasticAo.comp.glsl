#version 450
#extension GL_GOOGLE_include_directive : require

// Stochastic ambient occlusion -- step 6, and the last piece of the Dreams
// pipeline this renderer was missing.
//
// One cosine-weighted ray per pixel per frame. That is not enough samples
// for a smooth result in any single frame, and it is not supposed to be: the
// TAA resolve averages ten frames' worth, and a new random direction each
// frame is what turns the noise into an estimate. Dreams did precisely this
// ("I tried just picking a random ray direction from the cosine weighted
// hemisphere above each point and tracing the z buffer, one ray per pixel,
// and let the TAA sort it out") and found it more believable than a
// spiral-tap SSAO, which reads as dirt in the creases rather than as
// occlusion.
//
// The ray is traced in two stages, near to far:
//   1. Against the DEPTH BUFFER, in screen space, for contact-scale
//      occlusion -- everything smaller than a voxel, which is most of what
//      the eye reads as contact shadowing.
//   2. Against the BINARY VOXEL CASCADES (Builtin.ChunkVoxelCascade.comp.
//      glsl) for everything beyond, dropping to coarser cascades as the ray
//      gets further from its origin. That drop is what makes it cone-trace
//      -like: bigger steps and blockier occluders far away, where the
//      stochastic averaging hides both.
//
// The output is a visibility factor in [0,1] that the shading pass applies
// to indirect light only -- a directly lit surface is already handled by
// that light's own shadow, and darkening direct light here would double up
// with it.

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

const int VOXEL_CASCADE_DIM = 64;
const int VOXEL_CASCADE_COUNT = 4;
const int VOXEL_CASCADE_WORDS =
    (VOXEL_CASCADE_DIM * VOXEL_CASCADE_DIM * VOXEL_CASCADE_DIM) / 32;

// The G-buffer written by Builtin.RaymarchShader.comp.glsl this frame.
layout(binding = 0, r32f) uniform readonly image2D gbuffer_depth;
layout(binding = 1, rgba16f) uniform readonly image2D gbuffer_normal_material;
// Visibility in r, 1 = fully open. Read by Builtin.DeferredShade.comp.glsl.
layout(binding = 2, r16f) uniform writeonly image2D ao_output;
layout(binding = 3) readonly buffer VoxelCascadeBuffer {
    uint voxel_cascades[];
};

layout(push_constant) uniform PushConstants {
    vec4 camera_position;
    vec4 camera_forward;
    vec4 camera_right;
    vec4 camera_up;
    vec4 cascade_origin[VOXEL_CASCADE_COUNT]; // xyz = min corner, w = voxel size
    float jitter_x;  // Same sub-pixel offset the visibility pass used, so
    float jitter_y;  // this pass rebuilds the identical ray.
    int frame_index; // Rotates the sampling direction every frame.
    int pad;
} push;

// How far the ray looks for occluders, in world units. Beyond this a surface
// is treated as open sky -- occlusion from very distant geometry is both
// expensive to find and barely visible.
const float AO_MAX_DISTANCE = 12.0;
// Screen-space stage: how many steps, and how far along the ray it covers
// before the voxel stage takes over.
const int AO_SCREEN_STEPS = 12;
const float AO_SCREEN_DISTANCE = 1.5;
// Voxel stage: steps, and how much of a voxel to advance per step. Below 1
// the same voxel is tested repeatedly; above ~1.5 thin geometry slips
// between steps.
const int AO_VOXEL_STEPS = 24;
const float AO_VOXEL_STEP_SCALE = 1.0;
// Lifts the ray off the surface it starts on, in units of the finest
// cascade's voxel -- without it every surface occludes itself.
const float AO_ORIGIN_BIAS = 1.5;

// How much an occluder found `distance` away actually darkens: full effect
// at contact, fading to nothing by AO_MAX_DISTANCE. Smoothstep rather than a
// linear ramp so neither end has a visible edge.
float falloff(float distance) {
    return 1.0 - smoothstep(0.0, AO_MAX_DISTANCE, distance);
}

float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

// Cosine-weighted direction in the hemisphere around n, from two uniform
// randoms -- the standard concentric construction. Cosine weighting is what
// makes one sample an unbiased estimate of the cosine-weighted visibility
// integral, i.e. exactly the quantity diffuse shading wants.
vec3 cosine_hemisphere(vec3 n, float u1, float u2) {
    float r = sqrt(u1);
    float theta = 6.2831853 * u2;
    vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, n));
    vec3 bitangent = cross(n, tangent);
    return normalize(tangent * (r * cos(theta)) + bitangent * (r * sin(theta)) +
                     n * sqrt(max(1.0 - u1, 0.0)));
}

// True if the binary cascades say something solid sits at p. Picks the
// finest cascade that contains the point, which is what gives near-field
// occlusion fine voxels and far-field occlusion cheap ones.
bool voxel_occupied(vec3 p, int min_cascade) {
    for (int c = min_cascade; c < VOXEL_CASCADE_COUNT; ++c) {
        vec3 origin = push.cascade_origin[c].xyz;
        float voxel = push.cascade_origin[c].w;
        ivec3 v = ivec3(floor((p - origin) / voxel));
        if (any(lessThan(v, ivec3(0))) ||
            any(greaterThanEqual(v, ivec3(VOXEL_CASCADE_DIM)))) {
            continue; // outside this cascade; try a coarser one
        }
        int bit_index = v.x + v.y * VOXEL_CASCADE_DIM +
            v.z * VOXEL_CASCADE_DIM * VOXEL_CASCADE_DIM;
        uint word =
            voxel_cascades[c * VOXEL_CASCADE_WORDS + (bit_index >> 5)];
        return (word & (1u << (bit_index & 31))) != 0u;
    }
    return false; // past every cascade -- open space by definition
}

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(gbuffer_depth);
    if (pixel.x >= size.x || pixel.y >= size.y) {
        return;
    }

    float depth = imageLoad(gbuffer_depth, pixel).r;
    if (depth < 0.0) {
        imageStore(ao_output, pixel, vec4(1.0)); // background: nothing to occlude
        return;
    }

    vec2 uv = (vec2(pixel) + vec2(push.jitter_x, push.jitter_y) -
               0.5 * vec2(size)) / float(size.y);
    vec3 ray_dir = normalize(uv.x * push.camera_right.xyz +
                             uv.y * push.camera_up.xyz +
                             push.camera_forward.xyz);
    vec3 p = push.camera_position.xyz + ray_dir * depth;
    vec3 normal = normalize(imageLoad(gbuffer_normal_material, pixel).xyz);

    // One direction per pixel per frame, decorrelated in space and time.
    float u1 = hash13(vec3(pixel, push.frame_index));
    float u2 = hash13(vec3(pixel.yx, push.frame_index + 91));
    vec3 dir = cosine_hemisphere(normal, u1, u2);

    float finest_voxel = push.cascade_origin[0].w;
    vec3 origin = p + normal * (finest_voxel * AO_ORIGIN_BIAS);

    // Occlusion accumulates as a WEIGHTED amount rather than a hard hit /
    // no-hit verdict. That distinction is the difference between this pass
    // reading as ambient occlusion and it reading as blocks: a binary answer
    // makes every voxel-scale change in what the ray happens to strike a
    // hard-edged step in the final image, and the cascades' voxels are 0.25
    // world units across at the finest level -- exactly the size of the
    // blobs that produces on a wall a couple of metres away. Weighting by
    // distance also matches what occlusion physically is: a surface 10cm
    // away blocks far more of the hemisphere than one 8m away.
    float occlusion = 0.0;

    // --- Stage 1: march the depth buffer, for contact-scale detail. A
    // sample is occluded when it projects onto a pixel whose own surface is
    // nearer to the camera than the sample is -- the classic screen-space
    // test. The thickness bound stops a distant surface from occluding
    // through everything in front of it, which is what makes screen-space
    // tracing read as haloing.
    float step_len = AO_SCREEN_DISTANCE / float(AO_SCREEN_STEPS);
    for (int i = 1; i <= AO_SCREEN_STEPS; ++i) {
        float travel = step_len * float(i);
        vec3 sample_pos = origin + dir * travel;
        vec3 v = sample_pos - push.camera_position.xyz;
        float z = dot(v, push.camera_forward.xyz);
        if (z < 0.05) {
            break;
        }
        vec2 sample_uv = vec2(dot(v, push.camera_right.xyz),
                              dot(v, push.camera_up.xyz)) / z;
        vec2 sample_pixel = sample_uv * float(size.y) + 0.5 * vec2(size);
        ivec2 sp = ivec2(sample_pixel);
        if (sp.x < 0 || sp.y < 0 || sp.x >= size.x || sp.y >= size.y) {
            break;
        }
        float scene_depth = imageLoad(gbuffer_depth, sp).r;
        if (scene_depth < 0.0) {
            continue; // background there -- nothing in the way
        }
        float difference = length(v) - scene_depth;
        if (difference > 0.02 && difference < 0.5) {
            occlusion = max(occlusion, falloff(travel));
            break; // the nearest occluder along the ray is the one that counts
        }
    }

    // --- Stage 2: keep going through the voxel cascades for everything
    // beyond contact scale.
    //
    // No cascade is skipped by distance any more. Choosing a coarsest-
    // allowed cascade from log2(t) switched resolution at fixed distances
    // along the ray (2, 4 and 8 units), and a hard change in sampling
    // resolution at a fixed radius draws exactly the concentric rings that
    // showed up across large flat surfaces. voxel_occupied() already picks
    // the finest cascade that CONTAINS the sample, which is the same
    // level-of-detail behaviour arrived at by geometry rather than by a
    // threshold. The step still grows with distance -- smoothly.
    float t = AO_SCREEN_DISTANCE;
    for (int i = 0; i < AO_VOXEL_STEPS && t < AO_MAX_DISTANCE; ++i) {
        vec3 sample_pos = origin + dir * t;
        if (voxel_occupied(sample_pos, 0)) {
            occlusion = max(occlusion, falloff(t));
            break;
        }
        // Grow the step in proportion to how far out the ray already is, so
        // near-field detail keeps fine steps and the far field is cheap --
        // with no discontinuity anywhere in between.
        t += push.cascade_origin[0].w * AO_VOXEL_STEP_SCALE *
            max(1.0, t / AO_SCREEN_DISTANCE);
    }

    imageStore(ao_output, pixel, vec4(1.0 - clamp(occlusion, 0.0, 1.0)));
}
