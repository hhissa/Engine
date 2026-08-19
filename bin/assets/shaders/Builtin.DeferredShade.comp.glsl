#version 450
#extension GL_GOOGLE_include_directive : require
// Lets scene_textures[]/scene_bump_textures[] be indexed by a per-pixel
// material id via nonuniformEXT() -- see sample_scene_texture() below for
// what this replaces, and vulkan_device.cpp for the matching feature enable.
#extension GL_EXT_nonuniform_qualifier : require

// Deferred shading -- step 4 of the Dreams-gap work, and the pass that
// finally separates "what is visible" from "what it looks like".
//
// Builtin.RaymarchShader.comp.glsl now answers only the first question, for
// every pixel, by whichever means is cheapest (a splat it won, or a march),
// and writes the answer into a G-buffer: hit distance, surface normal and
// material. This pass reads that G-buffer and does all the shading -- the
// analytic material re-derivation, triplanar texturing and bump, direct
// lights and their soft shadows, baked indirect light, volumetrics, the
// background, the reference grid, the selection outline.
//
// Why split them at all:
//   - The splat path and the march path used to arrive at shading through
//     different code with different data in hand (one had a baked normal and
//     a brick material, the other a field gradient and a marched material).
//     Now both write the same three G-buffer values and there is exactly one
//     shading path, so they cannot drift.
//   - Shading cost stops scaling with geometric complexity. Every pixel is
//     shaded exactly once no matter how many splats or marching steps went
//     into deciding what it sees.
//   - Dreams does the same thing for the same reason: splat to a z+id
//     buffer, resolve to a G-buffer, then "totally traditional and simple at
//     the lighting end".
//
// Deliberately a near-copy of the raymarch shader's own shading helpers
// rather than a shared include: they read the same bindings from the same
// descriptor set (both pipelines bind render_set_), and GLSL has no linking,
// so the choice is between duplicating the helpers or moving several hundred
// lines into a header that only ever has one consumer each. The visibility
// shader has had every shading-only helper removed, so each function still
// lives in exactly one place.

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(binding = 0, rgba8) uniform writeonly image2D out_image;

// The G-buffer written by Builtin.RaymarchShader.comp.glsl this same frame:
// distance along each pixel's ray (negative for a background pixel), and the
// surface normal plus the material index the visibility pass had in hand.
layout(binding = 24, r32f) uniform readonly image2D gbuffer_depth;
layout(binding = 25, rgba16f) uniform readonly image2D gbuffer_normal_material;

#include "Builtin.SdfFieldConfig.inc.glsl"

// One entry per registered light (see GeometrySystem::Light,
// engine-side). vector_type.xyz is a direction (Directional) or
// world-space position (Point), vector_type.w its LightType as a float
// (0=directional, 1=point); colour_intensity.rgb/.a are the light's colour
// and intensity -- see the lighting loop in main() below for exactly how
// each type is evaluated. Read push.light_count of these, not lights.length()
// -- the buffer is sized to a fixed capacity (see kMaxLights engine-side),
// not to how many are actually registered.
// source_primitive.x is the primitives[]/scene_textures index this Point
// light was synthesized from (an emissive primitive), or -1 if it has no
// associated primitive (every authored/fallback light) -- see
// GpuLight::source_primitive's comment engine-side, and shadow_march()'s
// exclude_material parameter (Builtin.BakedFieldCommon.inc.glsl) for why
// this matters. yzw unused padding.
struct Light {
    vec4 vector_type;
    vec4 colour_intensity;
    vec4 source_primitive;
};

layout(binding = 3) readonly buffer LightBuffer {
    Light lights[];
};

// rgb: the primitive's material diffuse tint. a: its material's
// texture_scale (world units per texture tile -- see Material::
// texture_scale engine-side), NOT colour opacity; packed here because the
// alpha slot was unused and it saves a whole separate buffer + binding.
layout(binding = 5) readonly buffer ScenePrimitiveColours {
    vec4 scene_diffuse_colours[];
};

// Fixed-size, unlike the buffers above -- GLSL requires a compile-time
// array length for opaque handles like combined image samplers. Unused
// slots are always bound to a valid filler texture (engine-side) since
// Vulkan requires every element of a bound sampler array to reference a
// real image regardless of whether the shader ever reads that index.
const int MAX_SCENE_PRIMITIVES = 1000;
layout(binding = 6) uniform sampler2D scene_textures[MAX_SCENE_PRIMITIVES];
// Mirrors scene_textures above, one bump-map texture per primitive (see
// Material::bump_texture, engine-side) -- sampled purely for luminance by
// sample_scene_heights() below, never for colour. Unused/no-bump slots are
// bound to a genuinely flat/uniform-colour filler (TextureSystem::
// flat_texture()), not the checkerboard scene_textures falls back to, so a
// material with no bump map set reads as exactly zero bump perturbation.
layout(binding = 14) uniform sampler2D scene_bump_textures[MAX_SCENE_PRIMITIVES];

// The analytic scene SDF (shared with the voxelize pass -- one copy, so
// the baked field and this pass's per-pixel material provenance can never
// disagree about what the scene is). Only scene_map()'s nearest_primitive
// out-param is used here: the *distance* still comes from the baked field
// (that's the whole point of baking), but the *material* at a hit is
// re-derived analytically at the exact hit point, because the per-brick
// index above quantizes provenance to whole 0.25-unit cells.
#define SDF_PRIMITIVE_BUFFER_BINDING 7
#define SDF_LAYER_BUFFER_BINDING 8
#define SDF_PARAM_EXPR_BUFFER_BINDING 9
#include "Builtin.SdfSceneCommon.inc.glsl"

// Per-frame camera/model state, uploaded by VulkanRaymarchShader::render_to()
// each dispatch. A push constant rather than a UBO/descriptor binding: it's
// recorded directly into the command buffer, so there's no separate buffer
// memory a previous, still in-flight frame could be reading while this
// frame's value gets written -- no per-swapchain-image duplication needed.
layout(push_constant) uniform PushConstants {
    vec4 camera_position; // xyz + unused pad
    vec4 camera_forward;  // xyz + unused pad -- orthonormal basis (Camera
    vec4 camera_right;    // xyz + unused pad    class, engine-side) for
    vec4 camera_up;       // xyz + unused pad    building the view-space ray.
    int light_count;    // How many of LightBuffer's entries to sum, below.
    float ambient;        // Scene-wide ambient factor, added once regardless
                         // of light_count -- see GeometrySystem::ambient().
    int selected_primitive_index; // scene_textures/scene_diffuse_colours
                                  // index to draw a selection outline
                                  // around, or -1 for none.
    int flags;                    // RENDER_FLAG_* bits -- see below.
    int layer_count;              // How many LayerBuffer entries scene_map()
                                  // folds -- same value the voxelize pass
                                  // baked with.
    int volumetric_start;         // First index (into primitives[]/
                                  // scene_textures/scene_diffuse_colours) of
                                  // the registered-volumetrics tail range --
                                  // see accumulate_volumetrics() below.
    int volumetric_count;         // How many contiguous entries starting
                                  // there are volumetrics.
    float time;                  // Seconds since the shader was constructed
                                  // -- drives accumulate_volumetrics()'s
                                  // scrolling texture animation.
    float gi_cascade_center_x;
    float gi_cascade_center_y;
    float gi_cascade_center_z;    // Phase 5: the chunked field's GI cascade
                                  // current center (see sample_gi_cascade()
                                  // above, VulkanRaymarchShader::update_gi_
                                  // cascade()) -- only meaningful while
                                  // chunked_field_enabled is nonzero.
    int splat_mode;               // 0/1/2 = off / prime / visibility -- see
                                  // SPLAT_MODE_* below and Builtin.
                                  // ChunkPointSplat.comp.glsl's header for
                                  // what each does. Nonzero only when this
                                  // frame actually ran that prepass (see
                                  // render_to() engine-side), i.e. the
                                  // visibility buffer holds THIS frame's
                                  // cleared+splatted values, not stale ones.
    int frame_index;              // Seeds this pass's own dithering, so the
                                  // shadow-map taps differ every frame and
                                  // TAA can average them.
    float taa_jitter_x;           // Sub-pixel offset added to this pixel's
    float taa_jitter_y;           // coordinate before its ray is built, so
                                  // successive frames sample different
                                  // points inside the same pixel and
                                  // Builtin.TaaResolve.comp.glsl can average
                                  // them into an anti-aliased image. Zero
                                  // when TAA is off. Two scalars, not a
                                  // vec2, and last in the block: this block
                                  // is at 120 of Vulkan's guaranteed 128
                                  // push-constant bytes, and a vec2's
                                  // 8-byte alignment mid-block would pad it
                                  // over that limit.
} push;

// Bits of push.flags. Four independent booleans packed into one scalar --
// this block sits close to Vulkan's guaranteed 128-byte push-constant limit,
// and four whole ints for four bits is the first thing that should give.
const int RENDER_FLAG_GRID = 1;
const int RENDER_FLAG_SKYBOX = 2;
const int RENDER_FLAG_CHUNKED_FIELD = 4;
const int RENDER_FLAG_ISM = 8;
const int RENDER_FLAG_AO = 16;

// MAX_STEPS must be large enough to guarantee a ray can actually reach
// MAX_DIST, not just bound the loop -- outside the scene's baked bounds
// (see BOUNDS/sample_field()'s out-of-range branch above), a miss ray only
// ever advances by a fixed COARSE_CELL_SIZE per step, so crossing MAX_DIST
// world units of empty space alone costs MAX_DIST/COARSE_CELL_SIZE steps.
// Falling short of that would let a distant miss ray exhaust MAX_STEPS
// while travelled is still < MAX_DIST, which main() reads as a false hit
// (hit_material left at -1) instead of background. The remaining ~128-step
// margin beyond that (640 - 128.0/COARSE_CELL_SIZE) is for genuine hits
// needing several small steps to converge near a real surface.
const int MAX_STEPS = 640;
const float MAX_DIST = 128.0;
const float SURF_DIST = 0.001;

// How much of the indirect and ambient terms survives full occlusion -- see
// the ambient block in main(). Neither is allowed to reach zero: the baked
// GI already contains its own visibility (so the traced term is a detail
// refinement, not the whole occlusion answer), and ambient is by definition
// the floor below which nothing should fall.
const float INDIRECT_AO_FLOOR = 0.35;
const float AMBIENT_AO_FLOOR = 0.5;

// Cull radius for the per-pixel analytic re-evaluation at a hit point --
// see its call site in main() for why a bound this tight is valid there.
// Raise it only if a scene ever authors a layer smoothness above it.
const float ANALYTIC_HIT_CULL_RADIUS = 2.0;

// Shadow ray tuning (see shadow_march() in Builtin.BakedFieldCommon.inc.
// glsl, included below). SHADOW_NORMAL_BIAS offsets a shadow ray's origin
// off the surface being shaded, along its normal, comfortably more than
// one baked voxel's world size (COARSE_CELL_SIZE/BRICK_DIM here is
// (2*16)/128/8 = 0.03125) so the ray's very first sample doesn't re-detect
// that same surface as its own occluder. Was 0.05 (only ~1.6x a voxel) --
// too thin a margin once a slightly-off calc_static_normal() estimate (see
// its own comment) is factored in, which could leave the ray's very first
// sample still inside/grazing its own surface and reporting a false
// occluder. ~4x a voxel gives real headroom without pushing the shadow
// visibly off-contact. SHADOW_SOFTNESS is shadow_march()'s k -- how
// hard-edged the penumbra reads. SHADOW_MAX_STEPS can stay modest (unlike
// MAX_STEPS above): a shadow ray only ever needs to cross the scene's own
// baked volume, never reach all the way out to a distant camera.
const float SHADOW_NORMAL_BIAS = 0.12;
const float SHADOW_SOFTNESS = 16.0;
const int SHADOW_MAX_STEPS = 256;

// sample_field() (the baked-field query) plus its indirection/brick-pool/
// brick-primitive bindings live in the shared include below -- also used
// by Builtin.ProbeBake.comp.glsl's GI gather rays, which must march
// against the exact same baked field this pass does.
#define BAKED_FIELD_INDIRECTION_BINDING 1
#define BAKED_FIELD_BRICKPOOL_BINDING 2
#define BAKED_FIELD_BRICKPRIMITIVE_BINDING 4
#include "Builtin.BakedFieldCommon.inc.glsl"

// Phase 4: the chunked/streamed field -- fully separate bindings/buffers
// from the fixed-cube field above (see Builtin.SdfFieldConfig.inc.glsl's
// comment on why), read-only here exactly like the fixed-cube bindings.
// Always bound (Vulkan requires it regardless of RENDER_FLAG_CHUNKED_FIELD
// -- see VulkanRaymarchShader's render_bindings comment), only actually
// sampled through sample_active_field()/chunked_shadow_march() below when
// that flag is set.
#define CHUNKED_FIELD_TABLE_BINDING 15
#define CHUNKED_FIELD_INDIRECTION_BINDING 16
#define CHUNKED_FIELD_BRICKPOOL_BINDING 17
#define CHUNKED_FIELD_BRICKPRIMITIVE_BINDING 18
#include "Builtin.ChunkedFieldCommon.inc.glsl"

// Every sample_field() call in this file goes through this instead --
// picks the fixed-cube field or the chunked/streamed one (Phase 4) based on
// RENDER_FLAG_CHUNKED_FIELD, the one place that decision is made so every
// caller (contact AO, normals, the primary hit-test, and -- via a parallel
// chunked_shadow_march()/shadow_march() branch at its own call site below
// -- shadows) automatically stays consistent with whichever field is
// active, rather than each needing its own branch.
void sample_active_field(vec3 p, vec3 ray_dir, out float dist, out float skip_dist,
                         out int material_index) {
    if ((push.flags & RENDER_FLAG_CHUNKED_FIELD) != 0) {
        sample_clipmap_field(p, ray_dir, push.camera_position.xyz, dist, skip_dist,
                             material_index);
    } else {
        sample_field(p, ray_dir, dist, skip_dist, material_index);
    }
}

// Baked indirect-light probe grid -- a dense regular grid of GI samples
// covering the same world volume as the voxel field (BOUNDS), computed by
// Builtin.ProbeBake.comp.glsl once per rebake (see
// VulkanRaymarchShader::bake_probes()) and read here every frame,
// trilinearly interpolated at each hit point (see sample_probe_grid()
// below). This is what stands in for the flat `push.ambient` scalar the
// render pass used to add everywhere: instead every point in the scene
// gets its own approximation of the light actually bouncing around it --
// darker in corners a probe's gather rays mostly found nearby geometry,
// brighter in open rooms, tinted by whatever coloured surfaces are nearby
// (colour bleeding) -- adapted from Inigo Quilez's "simplegi" technique
// (https://iquilezles.org/articles/simplegi/): that article bakes GI onto
// *mesh vertices*, gathering each one's incoming light via a rendered
// hemisphere and iterating a few bounces; this engine has no meshes
// anywhere in its SDF/voxel pipeline, so the same idea -- treat a handful
// of sample points as light emitters/receivers, gather their incoming
// light by "rendering" (raymarching) a sphere of directions from each,
// iterate a few bounces -- is retargeted onto a spatial probe grid
// instead of vertices, the natural fit for a volume that has no surface
// mesh to hang samples off of.
const int PROBE_DIM = 16; // must match kProbeDim in vulkan_raymarch_shader.cpp
layout(binding = 10) readonly buffer ProbeBuffer {
    vec4 probes[]; // rgb = baked indirect irradiance at this probe; see
                   // Builtin.ProbeBake.comp.glsl for how it's computed.
};

// One entry per registered static primitive (parallel to
// scene_diffuse_colours) -- 1.0 if that primitive's material is
// pixelation-exempt (Material::pixelation_exempt), 0.0 otherwise. Written
// into out_image's alpha channel below (see main()) for
// Builtin.PostComposite.comp.glsl's pixelation pass to read; nothing else
// in this shader uses it, so a plain float array is simpler than folding
// it into an already-full vec4 buffer.
layout(binding = 11) readonly buffer PixelationExemptBuffer {
    float pixelation_exempt[];
};

// Optional skybox -- a single equirectangular (lat/long) image, sampled by
// ray direction wherever the primary ray hits nothing at all (see
// apply_skybox() below). Only valid to sample while RENDER_FLAG_SKYBOX is
// nonzero -- otherwise this is bound to TextureSystem's filler texture (see
// VulkanRaymarchShader::disable_skybox()/the constructor), which is never
// actually read then.
layout(binding = 12) uniform sampler2D skybox_texture;

// One entry per registered static/volumetric primitive (parallel to
// scene_diffuse_colours) -- xyz = that primitive's effective texture
// offset (world units, added to the sample point before the triplanar
// projection/texture_scale divide -- a texture "translate"), w = its
// texture rotation (radians, applied to each triplanar UV plane before
// sampling -- a texture "rotate"). See Material::texture_offset/
// texture_rotation engine-side. (0,0,0,0) leaves a primitive's texture
// exactly where it always was.
layout(binding = 13) readonly buffer ScenePrimitiveTexTransform {
    vec4 scene_tex_transform[];
};

// Rotates a 2D UV by angle_radians around the origin -- used to apply a
// primitive's texture_rotation to each triplanar projection plane before
// sampling. Same rotation matrix in every plane, since a triplanar
// projection has no single consistent "up" to rotate relative to
// otherwise.
vec2 rotate_uv(vec2 uv, float angle_radians) {
    float s = sin(angle_radians);
    float c = cos(angle_radians);
    return vec2(uv.x * c - uv.y * s, uv.x * s + uv.y * c);
}

// Trilinearly samples the baked probe grid at world point p -- the
// render-time counterpart to the GI bake, reading exactly the regular
// PROBE_DIM^3 grid Builtin.ProbeBake.comp.glsl wrote, covering the same
// [-BOUNDS, BOUNDS] cube the voxel field does. Unlike sample_field()'s
// brick apron there's no sparsity to route around here -- probes exist
// everywhere in the grid, so this is a plain 8-corner trilinear fetch, no
// indirection layer needed. Deliberately ignores the surface normal at p
// (a probe stores one isotropic average over its whole surrounding
// sphere, not a directional/hemispherical value) -- a further
// simplification on top of an already-approximate technique, in the same
// spirit as the source article's own acknowledged shortcut of not
// cosine-weighting each gathered sample by the angle it arrived from.
vec3 sample_probe_grid(vec3 p) {
    float cell_size = (2.0 * BOUNDS) / float(PROBE_DIM - 1);
    vec3 local = (p + vec3(BOUNDS)) / cell_size;
    vec3 local_clamped = clamp(local, vec3(0.0), vec3(float(PROBE_DIM - 1)));
    ivec3 i0 = ivec3(floor(local_clamped));
    ivec3 i1 = min(i0 + ivec3(1), ivec3(PROBE_DIM - 1));
    vec3 t = local_clamped - vec3(i0);

    #define PROBE_AT(ix, iy, iz) probes[(ix) + (iy) * PROBE_DIM + (iz) * PROBE_DIM * PROBE_DIM].rgb

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

// Phase 5: the chunked/streamed field's own GI cascade -- read-only
// counterpart to Builtin.ChunkProbeBake.comp.glsl's PrevProbeBuffer/
// CurrProbeBuffer, binding whichever physical buffer held the final
// bounce's result (see kProbeFinalInBufferB's identical role for the
// fixed-cube field's own ProbeBuffer). A single camera-centered cascade
// for now (see kGiProbeDim/kGiCascadeCellSize's own comment, engine-side,
// on why just one) -- GI_PROBE_DIM/GI_CASCADE_CELL_SIZE must match those
// exactly, and push.gi_cascade_center must match whatever VulkanRaymarch
// Shader::update_gi_cascade() last (re-)baked around.
const int GI_PROBE_DIM = 16;
const float GI_CASCADE_CELL_SIZE = 2.0;

layout(binding = 19) readonly buffer ChunkProbeBuffer {
    vec4 chunk_probes[];
};

// This frame's ambient-occlusion estimate, one cosine-weighted ray per pixel
// (see Builtin.StochasticAo.comp.glsl). Noisy on its own by design -- the
// TAA resolve is what averages it into a usable term, which is exactly the
// trade that makes one ray per pixel enough.
layout(binding = 27, r16f) uniform readonly image2D ao_visibility;

// The imperfect shadow maps built this frame by Builtin.ChunkShadowSplat.
// comp.glsl -- ISM_COUNT tiles of ISM_RESOLUTION^2 distances, packed as raw
// float bits, ~0 where nothing was splatted. See that shader's header for
// the technique and why the maps are deliberately low quality.
layout(binding = 26) readonly buffer ShadowAtlasBuffer {
    uint shadow_atlas[];
};

const int ISM_RESOLUTION = 128;
const int ISM_COUNT = 64;
const int ISM_ATLAS_DIM = 8;

// Distance slack when comparing a surface against the shadow map. It has to
// absorb the map's texel footprint plus the gap between the splatted points
// themselves, or every surface shadows itself in a moire of its own point
// spacing. The dominant term is RELATIVE, not absolute: one texel of a
// 128x128 full-sphere map subtends a fixed solid angle, so the world-space
// depth range it covers grows linearly with distance from the light -- and
// it grows again by 1/tan(incidence) on a surface the light grazes, which is
// where a fixed bias visibly failed (rings and mottling on walls and
// ceilings adjacent to their own light). ISM_RELATIVE_BIAS is the face-on
// fraction of distance; ISM_RELATIVE_BIAS_SLOPE is how much more grazing
// incidence adds; ISM_DEPTH_BIAS remains as an absolute floor for very short
// light-to-surface distances where the relative terms underflow the point
// spacing itself.
const float ISM_DEPTH_BIAS = 0.08;
const float ISM_RELATIVE_BIAS = 0.02;
const float ISM_RELATIVE_BIAS_SLOPE = 0.10;

vec2 direction_to_octahedral(vec3 d) {
    vec3 a = d / max(abs(d.x) + abs(d.y) + abs(d.z), 1e-8);
    vec2 oct = a.z >= 0.0
        ? a.xy
        : (1.0 - abs(a.yx)) * vec2(a.x >= 0.0 ? 1.0 : -1.0,
                                   a.y >= 0.0 ? 1.0 : -1.0);
    return clamp(oct * 0.5 + 0.5, 0.0, 1.0);
}

// Fraction of this light that reaches p, from its imperfect shadow map. Four
// taps in a small rotated disc rather than one: a single tap through a map
// this coarse reads as hard-edged noise, and the rotation (per pixel, per
// frame) turns the remaining error into something TAA can average instead of
// a fixed pattern. Returns 1.0 where the map has nothing recorded -- an
// empty direction means no caster, not full shadow.
float sample_ism(int slot, vec3 p, vec3 light_pos, float n_dot_l,
                 float dither) {
    vec3 to_point = p - light_pos;
    float dist = length(to_point);
    if (dist < 1e-4) {
        return 1.0;
    }
    vec2 oct = direction_to_octahedral(to_point / dist);
    ivec2 tile = ivec2(slot % ISM_ATLAS_DIM, slot / ISM_ATLAS_DIM);
    vec2 base = oct * float(ISM_RESOLUTION - 1);

    // Slope-scaled: a surface seen edge-on by the light spans more depth
    // inside one texel, so it needs more slack to not shadow itself. See the
    // ISM_RELATIVE_* constants for why the slack scales with distance.
    float slack = ISM_DEPTH_BIAS +
        dist * (ISM_RELATIVE_BIAS +
                ISM_RELATIVE_BIAS_SLOPE * (1.0 - clamp(n_dot_l, 0.0, 1.0)));

    float angle = dither * 6.2831853;
    vec2 rot = vec2(cos(angle), sin(angle));
    float lit = 0.0;
    for (int i = 0; i < 4; ++i) {
        // Four points on a unit square's diagonals, rotated together.
        vec2 offset = i == 0 ? vec2(0.7, 0.7)
                    : i == 1 ? vec2(-0.7, 0.7)
                    : i == 2 ? vec2(-0.7, -0.7)
                             : vec2(0.7, -0.7);
        offset = vec2(offset.x * rot.x - offset.y * rot.y,
                      offset.x * rot.y + offset.y * rot.x);
        ivec2 q = ivec2(clamp(base + offset, vec2(0.0),
                              vec2(float(ISM_RESOLUTION - 1))));
        ivec2 atlas = tile * ISM_RESOLUTION + q;
        uint bits = shadow_atlas[atlas.y * (ISM_ATLAS_DIM * ISM_RESOLUTION) +
                                 atlas.x];
        float occluder = bits == ~0u ? 1e30 : uintBitsToFloat(bits);
        // Soft comparison instead of a hard step: fully lit when the
        // recorded occluder is within one slack of the surface's own
        // distance (that is measurement error, not occlusion), fully
        // shadowed when it is three slacks nearer, smooth in between. A
        // hard threshold turned the atlas's texel-quantised depths into
        // hard-edged bands wherever the surface's distance drifted across
        // one texel's recorded value.
        lit += smoothstep(dist - 3.0 * slack, dist - slack, occluder);
    }
    return lit * 0.25;
}

// Bindings 20-23 (the splat visibility buffer, the cluster pool and the
// per-tile near bounds) are deliberately NOT declared here even though the
// descriptor set carries them: deciding which splat won a pixel is the
// visibility pass's job, and by the time shading runs that decision is
// already baked into the G-buffer. Vulkan is happy for a shader to leave
// bindings of its set undeclared.


// push.splat_mode values -- must match SplatMode engine-side
// (vulkan_raymarch_shader.h).
const int SPLAT_MODE_OFF = 0;
const int SPLAT_MODE_PRIME = 1;
const int SPLAT_MODE_VISIBILITY = 2;

// Trilinearly samples the GI cascade at world point p -- same structure as
// sample_probe_grid() above, just centered on push.gi_cascade_center
// (recentered engine-side in discrete whole-cell steps as the camera
// moves) instead of spanning a fixed [-BOUNDS, BOUNDS] cube from the
// world origin.
vec3 sample_gi_cascade(vec3 p) {
    vec3 center = vec3(push.gi_cascade_center_x, push.gi_cascade_center_y,
                       push.gi_cascade_center_z);
    float half_extent = float(GI_PROBE_DIM - 1) * 0.5 * GI_CASCADE_CELL_SIZE;
    vec3 grid_min = center - vec3(half_extent);

    vec3 local = (p - grid_min) / GI_CASCADE_CELL_SIZE;
    vec3 local_clamped = clamp(local, vec3(0.0), vec3(float(GI_PROBE_DIM - 1)));
    ivec3 i0 = ivec3(floor(local_clamped));
    ivec3 i1 = min(i0 + ivec3(1), ivec3(GI_PROBE_DIM - 1));
    vec3 t = local_clamped - vec3(i0);

    #define GI_PROBE_AT(ix, iy, iz) chunk_probes[(ix) + (iy) * GI_PROBE_DIM + (iz) * GI_PROBE_DIM * GI_PROBE_DIM].rgb

    vec3 c000 = GI_PROBE_AT(i0.x, i0.y, i0.z);
    vec3 c100 = GI_PROBE_AT(i1.x, i0.y, i0.z);
    vec3 c010 = GI_PROBE_AT(i0.x, i1.y, i0.z);
    vec3 c110 = GI_PROBE_AT(i1.x, i1.y, i0.z);
    vec3 c001 = GI_PROBE_AT(i0.x, i0.y, i1.z);
    vec3 c101 = GI_PROBE_AT(i1.x, i0.y, i1.z);
    vec3 c011 = GI_PROBE_AT(i0.x, i1.y, i1.z);
    vec3 c111 = GI_PROBE_AT(i1.x, i1.y, i1.z);

    #undef GI_PROBE_AT

    vec3 c00 = mix(c000, c100, t.x);
    vec3 c10 = mix(c010, c110, t.x);
    vec3 c01 = mix(c001, c101, t.x);
    vec3 c11 = mix(c011, c111, t.x);

    vec3 c0 = mix(c00, c10, t.y);
    vec3 c1 = mix(c01, c11, t.y);

    return mix(c0, c1, t.z);
}

// Cheap contact AO: a short march of AO_STEPS samples along the surface
// normal, comparing the baked field's actual distance at each tap against
// what an unoccluded point that far out ought to read -- if something's
// crowding closer than expected (a nearby wall, a crease), the shortfall
// darkens the result. This is Inigo Quilez's classic SDF ambient-occlusion
// trick (https://iquilezles.org/articles/nvscene2008/rwwtt.pdf), applied
// against the already-baked field via sample_field() (Builtin.
// BakedFieldCommon.inc.glsl) instead of re-evaluating the analytic scene
// per tap. Distinct from -- and much cheaper than -- sample_probe_grid()'s
// multi-bounce GI above: this only reaches a few fine voxels out (voxel
// size here is COARSE_CELL_SIZE/BRICK_DIM = 0.03125, see
// SHADOW_NORMAL_BIAS's own comment below), so it darkens contacts/creases
// at a resolution the coarse, sparse probe grid can't represent, rather
// than standing in for indirect lighting itself.
//
// An unbricked sample (skip_dist != 0.0, see sample_field()'s own comment)
// means genuinely empty space at least that far out -- treated as exactly
// step_dist away (zero contribution to occlusion), the same "valid vs.
// safe-to-step" distinction shadow_march() already makes.
//
// AO_STRENGTH is a local constant, not a push constant, for now -- this is
// a prototype to evaluate whether the look is worth keeping at all; if so,
// promoting it to a tunable (mirroring set_bloom_intensity()/
// set_vignette_strength()'s push-constant plumbing) is a mechanical
// follow-up, not a design decision.
float calc_contact_ao(vec3 p, vec3 normal) {
    const int AO_STEPS = 5;
    // ~2 fine voxels per step (voxel = COARSE_CELL_SIZE/BRICK_DIM =
    // 0.03125) -- short enough to read as contact/crease darkening, not
    // general-purpose occlusion (that's what sample_probe_grid() is for).
    const float AO_STEP_SIZE = 0.06;
    const float AO_STRENGTH = 1.5;

    vec3 origin = p + normal * SHADOW_NORMAL_BIAS; // same off-surface bias
                                                   // shadow rays use, for
                                                   // the same reason (avoid
                                                   // re-detecting this same
                                                   // surface as the first tap)
    float occlusion = 0.0;
    float weight = 1.0;
    for (int i = 1; i <= AO_STEPS; ++i) {
        float step_dist = AO_STEP_SIZE * float(i);
        vec3 sample_pos = origin + normal * step_dist;

        float dist, skip_dist;
        int material;
        sample_active_field(sample_pos, normal, dist, skip_dist, material);
        float d = (skip_dist == 0.0) ? dist : step_dist;

        occlusion += weight * max(step_dist - d, 0.0);
        weight *= 0.6; // farther taps count for progressively less
    }
    return clamp(1.0 - AO_STRENGTH * occlusion, 0.0, 1.0);
}

// Normal of the baked static field at p via finite differences. Offsets
// are tiny (0.0025) relative to a coarse cell (0.25), so these queries
// almost always land in the same bricked cell as p itself -- but a hit
// right at a coarse-cell boundary (grid lines every 0.25 world units,
// starting at -BOUNDS -- i.e. every "round" coordinate, exactly where
// authored geometry like a floor at y=0 tends to sit) can still have one
// or more of the six taps below land in a brick-less neighbour. There,
// sample_field() returns a placeholder dist (== COARSE_CELL_SIZE, not a
// real signed distance -- see its own comment) and flags it via
// skip_dist != 0. Blindly differencing that placeholder against a real
// distance corrupted the gradient right on that grid -- read as banding/
// fake shadow lines tracking coarse-cell boundaries. p itself is always a
// valid, bricked sample (this is only ever called at a confirmed raymarch
// hit), so it's the safe fallback for whichever side of a tap comes back
// unbricked.
vec3 calc_static_normal(vec3 p) {
    vec3 dummy_dir = vec3(0.0, 0.0, 1.0);
    vec2 e = vec2(0.0025, 0.0);
    float dist_p, skip_p;
    int unused_material;
    sample_active_field(p, dummy_dir, dist_p, skip_p, unused_material);

    float dx0, dx1, dy0, dy1, dz0, dz1, skip;
    sample_active_field(p + e.xyy, dummy_dir, dx1, skip, unused_material);
    if (skip != 0.0) dx1 = dist_p;
    sample_active_field(p - e.xyy, dummy_dir, dx0, skip, unused_material);
    if (skip != 0.0) dx0 = dist_p;
    sample_active_field(p + e.yxy, dummy_dir, dy1, skip, unused_material);
    if (skip != 0.0) dy1 = dist_p;
    sample_active_field(p - e.yxy, dummy_dir, dy0, skip, unused_material);
    if (skip != 0.0) dy0 = dist_p;
    sample_active_field(p + e.yyx, dummy_dir, dz1, skip, unused_material);
    if (skip != 0.0) dz1 = dist_p;
    sample_active_field(p - e.yyx, dummy_dir, dz0, skip, unused_material);
    if (skip != 0.0) dz0 = dist_p;
    return normalize(vec3(dx1 - dx0, dy1 - dy0, dz1 - dz0));
}

// Indexes the sampler array directly by the pixel's own material id.
//
// Vulkan only defines the result of indexing a combined-image-sampler array
// with a "dynamically uniform" value -- the same for every invocation --
// and a per-pixel hit's material is anything but. This used to work around
// that by looping over all MAX_SCENE_PRIMITIVES (1000) slots and keeping
// the one where the loop counter, which IS dynamically uniform, matched.
// Correct, and enormously expensive: shading one pixel calls this three
// times (triplanar) and sample_scene_heights() three more, so 6,000
// iterations ran to fetch 6 texels, on every hit pixel, every frame. It is
// the reason the deferred pass measured ~24ms with a screen full of hits
// against 0.2ms with an empty one.
//
// nonuniformEXT() is the language-level way to say "this index genuinely
// varies across the wave, handle it": the hardware re-issues the fetch per
// distinct index rather than the shader spelling that out as a 1000-step
// linear scan. It needs shaderSampledImageArrayNonUniformIndexing, which
// is core in Vulkan 1.2 and now enabled in vulkan_device.cpp.
vec3 sample_scene_texture(int index, vec2 uv) {
    return texture(scene_textures[nonuniformEXT(index)], uv).rgb;
}

// Raymarch analog of kohi's "051 Normal Maps" commit: perturbs the shaded
// normal with fine surface detail. Kohi's version samples a dedicated
// tangent-space normal-map texture unwrapped onto a mesh's authored UVs;
// SDF primitives here have neither a mesh nor authored UVs (see the
// triplanar diffuse sampling above for why colour is already triplanar), so
// this instead derives bump detail from a separate, explicitly-set bump
// map texture's luminance (Material::bump_map_name/bump_texture,
// scene_bump_textures above) -- a standard technique ("bump mapping from a
// height/luminance field") for when there's no authored tangent-space
// normal map -- via a 3-tap finite difference per axis-plane, then
// recombines the three tangent-space bumps into the base normal using the
// "whiteout blend" construction (Golus, "Normal Mapping for a Triplanar
// Shader"), reusing the same per-axis `blend` weights as the diffuse
// triplanar mix above. A material with no bump map set samples
// flat_texture()'s uniform colour here (see scene_bump_textures' own
// comment), so all three taps read identical luminance and this comes out
// to exactly zero perturbation -- bump mapping is opt-in per material, not
// derived from whatever diffuse texture (even the default checkerboard)
// happens to be assigned.
const float BUMP_UV_EPSILON = 0.015;
const float BUMP_STRENGTH = 0.25;

float luminance(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

// Converts 3 nearby luminance samples (centre, +epsilon along each local
// axis) into a tangent-space bump normal: (dHeight/du, dHeight/dv, 1),
// negated and scaled so brighter neighbours slope the normal away from
// them, then normalized -- the same convention an authored tangent-space
// normal-map texture's RGB channels would encode.
vec3 bump_from_heights(float h_centre, float h_u, float h_v) {
    float du = (h_u - h_centre) / BUMP_UV_EPSILON;
    float dv = (h_v - h_centre) / BUMP_UV_EPSILON;
    return normalize(vec3(-du * BUMP_STRENGTH, -dv * BUMP_STRENGTH, 1.0));
}

// Same direct indexing as sample_scene_texture() above, for the same
// reason -- see its comment.
void sample_scene_heights(int index, vec2 uv, out float h_centre, out float h_u, out float h_v) {
    int slot = nonuniformEXT(index);
    h_centre = luminance(texture(scene_bump_textures[slot], uv).rgb);
    h_u = luminance(texture(scene_bump_textures[slot], uv + vec2(BUMP_UV_EPSILON, 0.0)).rgb);
    h_v = luminance(texture(scene_bump_textures[slot], uv + vec2(0.0, BUMP_UV_EPSILON)).rgb);
}

// Whiteout-blend recombination: each axis-plane's tangent-space bump is
// added into the two components of the base normal it doesn't itself
// represent, swizzled back into world space, then blended by the same
// per-axis weights used for triplanar colour.
vec3 apply_triplanar_bump(vec3 base_normal, vec3 blend, vec3 bump_x, vec3 bump_y, vec3 bump_z) {
    vec3 nx = vec3(bump_x.xy + base_normal.zy, base_normal.x);
    vec3 ny = vec3(bump_y.xy + base_normal.xz, base_normal.y);
    vec3 nz = vec3(bump_z.xy + base_normal.xy, base_normal.z);

    return normalize(nx.zyx * blend.x + ny.xzy * blend.y + nz.xyz * blend.z);
}

// Reference grid: the ground plane y=0 subdivided into 1-unit cells, with a
// heavier line every GRID_MAJOR_SPACING units and the world X/Z axes tinted
// (the usual modelling-package look). The plane is intersected analytically
// per-ray rather than baked into the voxel field: it's an infinite editor
// aid, not scene content, so it must never show up in the SDF field the
// authoring tools measure against -- and an analytic hit costs nothing while
// a baked plane would burn bricks across the whole volume.
const float GRID_MINOR_SPACING = 1.0;
const float GRID_MAJOR_SPACING = 10.0;
// Distance (world units, along the ray) over which the grid fades out.
// Without a fade the 1-unit lines alias into shimmer long before MAX_DIST.
const float GRID_FADE_DIST = 60.0;

const vec3 GRID_MINOR_COLOUR = vec3(0.32, 0.32, 0.35);
const vec3 GRID_MAJOR_COLOUR = vec3(0.5, 0.5, 0.55);
const vec3 GRID_X_AXIS_COLOUR = vec3(0.85, 0.3, 0.3);  // the z=0 line
const vec3 GRID_Z_AXIS_COLOUR = vec3(0.3, 0.45, 0.9);  // the x=0 line

// Anti-aliased coverage of the line family {coord = n * spacing}: distance
// from coord to the nearest line, pushed through a smoothstep whose ramp is
// one filter_width wide. filter_width is the ~1-pixel world-space footprint
// at the hit point (computed in apply_reference_grid() -- no fwidth() in a
// compute shader), so lines stay about a pixel wide on screen at any
// distance instead of a fixed world width that aliases far away and looks
// bloated up close.
float grid_line_coverage(float coord, float spacing, float filter_width) {
    float dist_to_line = abs(fract(coord / spacing + 0.5) - 0.5) * spacing;
    return 1.0 - smoothstep(0.5 * filter_width, 1.5 * filter_width, dist_to_line);
}

// Composites the reference grid into colour (in place) for this pixel's
// ray. scene_dist is how far the ray travelled before hitting scene content
// (>= MAX_DIST on a miss): geometry in front of the plane occludes the grid,
// while the grid draws over the background and over anything behind it.
void apply_reference_grid(inout vec3 colour, vec3 ray_origin, vec3 ray_dir,
                          float scene_dist, float pixel_height) {
    if (abs(ray_dir.y) < 1e-6) {
        return; // Ray parallel to the plane -- no intersection.
    }
    float plane_t = -ray_origin.y / ray_dir.y;
    if (plane_t <= 0.0 || plane_t >= min(scene_dist, GRID_FADE_DIST)) {
        return;
    }

    vec3 hit = ray_origin + ray_dir * plane_t;

    // Approximate world-space size of one pixel on the plane at the hit
    // point: angular pixel size scaled by distance, stretched by how
    // obliquely the ray strikes the plane (grazing rays smear a pixel
    // across far more of the plane). The 0.05 clamp caps that stretch near
    // the horizon, where the fade below takes over anyway.
    float filter_width = plane_t * pixel_height / max(abs(ray_dir.y), 0.05);

    float minor = max(grid_line_coverage(hit.x, GRID_MINOR_SPACING, filter_width),
                      grid_line_coverage(hit.z, GRID_MINOR_SPACING, filter_width));
    float major = max(grid_line_coverage(hit.x, GRID_MAJOR_SPACING, filter_width),
                      grid_line_coverage(hit.z, GRID_MAJOR_SPACING, filter_width));
    // The X axis is the line z=0 (and vice versa), so each axis' coverage
    // comes from the *other* coordinate's distance to 0. Slightly wider
    // than the regular lines so the axes read at a glance.
    float axis_x = 1.0 - smoothstep(0.75 * filter_width, 2.0 * filter_width, abs(hit.z));
    float axis_z = 1.0 - smoothstep(0.75 * filter_width, 2.0 * filter_width, abs(hit.x));

    // Layered: axis colour wins over major, major over minor; opacity is
    // the strongest layer present. The plane between lines stays fully
    // transparent -- it's a reference grid, not a floor.
    vec3 grid_colour = GRID_MINOR_COLOUR;
    float grid_alpha = minor * 0.5;
    grid_colour = mix(grid_colour, GRID_MAJOR_COLOUR, major);
    grid_alpha = max(grid_alpha, major * 0.75);
    grid_colour = mix(grid_colour, GRID_X_AXIS_COLOUR, axis_x);
    grid_colour = mix(grid_colour, GRID_Z_AXIS_COLOUR, axis_z);
    grid_alpha = max(grid_alpha, max(axis_x, axis_z) * 0.9);

    float fade = 1.0 - smoothstep(0.5 * GRID_FADE_DIST, GRID_FADE_DIST, plane_t);
    colour = mix(colour, grid_colour, grid_alpha * fade);
}

// Samples skybox_texture by ray direction alone (no position -- an
// infinitely distant background, exactly like a real sky) via the standard
// equirectangular/lat-long projection: longitude (angle around the world Y
// axis) maps to u, latitude (angle from the north pole down to the south
// pole) maps to v. The one seam this projects along (dir.x >= 0, dir.z == 0,
// i.e. straight behind the u=0/u=1 wraparound) falls on whatever the
// authored image has there, same as any equirectangular skybox in any
// engine -- not something this shader can avoid by itself.
const float PI = 3.14159265359;

vec3 sample_skybox(vec3 dir) {
    vec2 uv = vec2(atan(dir.z, dir.x) / (2.0 * PI) + 0.5,
                  acos(clamp(dir.y, -1.0, 1.0)) / PI);
    return texture(skybox_texture, uv).rgb;
}

// Cheap per-pixel pseudo-random value (interleaved-gradient-noise style) --
// used below to dither accumulate_volumetrics()'s fixed step count, so
// banding between samples turns into fine-grained noise instead of visible
// stepped bands (the same problem, and the same fix, as any fixed-step
// volumetric/fog march).
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// How far past the primary ray's own hit (or MAX_DIST on a miss) a
// volumetric shaft is still marched through -- keeps the fixed step count
// below reasonably fine-grained without needing to cover the whole of
// MAX_DIST, which no authored light-shaft primitive should ever need to
// reach.
const float VOLUMETRIC_MAX_DIST = 48.0;
const int VOLUMETRIC_STEPS = 48;
// World units per texture tile, and world units per second the texture
// scrolls along a shaft's local Y (its height axis, matching every other
// primitive type's local-space convention) -- the drifting-dust-in-a-
// sunbeam look "textured to look like god rays" implies, rather than a
// static decal. Independent of any one primitive's texture_scale (packed in
// scene_diffuse_colours[idx].a, same as an opaque primitive's) which still
// controls the pattern's overall tiling frequency.
const float VOLUMETRIC_SCROLL_SPEED = 0.06;

// Secondary analytic pass, run once per pixel after raymarch() has already
// resolved the opaque hit: marches the *same* primary ray a second time,
// but through GeometrySystem's registered volumetrics instead of the baked
// field -- these were never baked into it (see rebuild_static_scene()), so
// a primary ray always passes straight through one, exactly like a fog
// volume rather than a solid surface. Reuses primitive_sdf() (shared with
// the opaque scene, see Builtin.SdfSceneCommon.inc.glsl) directly against
// primitives[push.volumetric_start + i] -- the full 15-type shape catalogue,
// rotation, and per-type params all come for free from that reuse. Returns
// an additive glow contribution (never subtracts/occludes -- a light shaft
// brightens whatever is behind it, it doesn't block it), textured/tinted by
// each shape's own material exactly like an opaque primitive's triplanar
// shading, but projected as a single planar UV across the shape's local XZ
// footprint (a light shaft has no meaningful "surface normal" to blend
// triplanar sampling by, since a ray marching through it never actually
// hits it) and scrolling along local Y over time for a drifting look.
// max_march_dist is min(the primary ray's own travel distance, MAX_DIST) --
// clamped again internally by VOLUMETRIC_MAX_DIST so this stays cheap even
// on a total miss (background) ray.
vec3 accumulate_volumetrics(vec3 ray_origin, vec3 ray_dir, float max_march_dist, float dither) {
    vec3 accum = vec3(0.0);
    if (push.volumetric_count <= 0) {
        return accum;
    }

    float march_dist = min(max_march_dist, VOLUMETRIC_MAX_DIST);
    if (march_dist <= 0.0) {
        return accum;
    }
    float step_size = march_dist / float(VOLUMETRIC_STEPS);
    float t = step_size * dither; // dithered start offset -- breaks banding

    for (int s = 0; s < VOLUMETRIC_STEPS; ++s) {
        vec3 p = ray_origin + ray_dir * t;

        for (int i = 0; i < push.volumetric_count; ++i) {
            int idx = push.volumetric_start + i;
            if (primitive_sdf(idx, p) >= 0.0) {
                continue; // outside this shaft -- no contribution here
            }

            vec3 local = primitive_local_space(idx, p) + scene_tex_transform[idx].xyz;
            float texture_scale = max(scene_diffuse_colours[idx].a, 0.01);
            vec2 uv = local.xz / texture_scale +
                     vec2(0.0, local.y / texture_scale - push.time * VOLUMETRIC_SCROLL_SPEED);
            uv = rotate_uv(uv, scene_tex_transform[idx].w);

            vec3 tex_colour = sample_scene_texture(idx, uv);
            vec3 tint = scene_diffuse_colours[idx].rgb;
            float density = max(primitives[idx].expr_scale.x, 0.0);

            accum += tex_colour * tint * density * step_size;
        }

        t += step_size;
    }

    return accum;
}

// Spatially filtered ambient occlusion.
//
// The AO pass traces ONE cosine-weighted ray per pixel (see Builtin.
// StochasticAo.comp.glsl), which is nowhere near enough for a smooth
// estimate on its own -- it leans entirely on the TAA resolve to average
// successive frames into one. That works, but only so far: at a 0.1 blend
// factor the steady state still carries roughly a quarter of the
// single-sample variance, and because the ray direction rotates every frame
// the residual is not static grain, it crawls. Reading a single texel here
// is what left that visible over every surface.
//
// Averaging a neighbourhood first cuts the variance the temporal filter has
// to remove by about the square root of the tap count, for a fraction of
// what the AO pass itself costs. Nine taps on a stride of two cover a 5x5
// footprint while paying for 3x3 -- the gaps do not matter because TAA is
// jittering the sample grid underneath anyway.
//
// TAPS ARE REJECTED ACROSS DEPTH AND NORMAL DISCONTINUITIES. An unweighted
// blur would drag occlusion across every silhouette and haloe exactly the
// contact shadows AO exists to produce; the depth tolerance is relative so
// it scales with distance, and the normal test keeps one face of a corner
// from bleeding into the other. A pixel whose whole neighbourhood is
// rejected falls back to its own unfiltered sample.
const int AO_FILTER_RADIUS = 1;
const int AO_FILTER_STRIDE = 2;
const float AO_FILTER_NORMAL_ALIGN = 0.9; // ~25 degrees
float filtered_ao(ivec2 centre, vec3 centre_normal, float centre_depth) {
    ivec2 bounds = imageSize(ao_visibility) - ivec2(1);
    float depth_tolerance = centre_depth * 0.02 + 0.01;
    float total = 0.0;
    float weight_sum = 0.0;
    for (int dy = -AO_FILTER_RADIUS; dy <= AO_FILTER_RADIUS; ++dy) {
        for (int dx = -AO_FILTER_RADIUS; dx <= AO_FILTER_RADIUS; ++dx) {
            ivec2 tap = clamp(centre + ivec2(dx, dy) * AO_FILTER_STRIDE,
                              ivec2(0), bounds);
            float tap_depth = imageLoad(gbuffer_depth, tap).r;
            if (tap_depth < 0.0 ||
                abs(tap_depth - centre_depth) > depth_tolerance) {
                continue; // background, or a different surface entirely
            }
            float align = dot(imageLoad(gbuffer_normal_material, tap).xyz,
                              centre_normal);
            if (align < AO_FILTER_NORMAL_ALIGN) {
                continue; // facing somewhere else -- not the same surface
            }
            total += imageLoad(ao_visibility, tap).r * align;
            weight_sum += align;
        }
    }
    return weight_sum > 0.0 ? total / weight_sum
                            : imageLoad(ao_visibility, centre).r;
}

void main() {
    ivec2 pixel_coord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 image_size = imageSize(out_image);
    if (pixel_coord.x >= image_size.x || pixel_coord.y >= image_size.y) {
        return;
    }

    // Rebuild this pixel's ray exactly as the visibility pass did, jitter
    // included -- the G-buffer stores distance ALONG that ray, so the ray
    // has to be identical for the hit position to come back out right.
    vec2 uv = (vec2(pixel_coord) + vec2(push.taa_jitter_x, push.taa_jitter_y) -
               0.5 * vec2(image_size)) / float(image_size.y);
    vec3 ray_origin = push.camera_position.xyz;
    vec3 ray_dir = normalize(uv.x * push.camera_right.xyz +
                            uv.y * push.camera_up.xyz +
                            push.camera_forward.xyz);

    // The G-buffer: distance (negative for "nothing hit"), surface normal,
    // and the material the visibility pass had in hand -- a splat's baked
    // per-brick material, or a marched hit's. It is only a starting point:
    // the analytic re-derivation below replaces it wherever the scene can
    // answer more precisely.
    float depth = imageLoad(gbuffer_depth, pixel_coord).r;
    vec4 normal_material = imageLoad(gbuffer_normal_material, pixel_coord);

    float travelled = depth < 0.0 ? MAX_DIST : depth;
    vec3 p = ray_origin + ray_dir * travelled;
    vec3 gbuffer_normal = normal_material.xyz;
    int hit_material = int(normal_material.w);

    // Background: the skybox if one is enabled (see set_skybox()), else the
    // flat two-colour gradient this shader always used before skyboxes
    // existed. Sampled by ray direction alone, so it reads as infinitely
    // distant -- geometry in front of it simply overwrites colour.
    vec3 colour = (push.flags & RENDER_FLAG_SKYBOX) != 0
        ? sample_skybox(ray_dir)
        : mix(vec3(0.02, 0.02, 0.05), vec3(0.05, 0.05, 0.12), uv.y + 0.5);
    // Whether Builtin.PostComposite.comp.glsl's pixelation pass should leave
    // this pixel alone -- see out_image's alpha channel below. The
    // background (a miss) is never exempt: it's not a primitive, so it
    // pixelates along with everything else that doesn't opt out.
    float exempt_flag = 0.0;

    if (travelled < MAX_DIST) {
        // The normal comes from the G-buffer, not from six more field
        // lookups here: for a splat it is the normal baked into the point
        // itself, for a marched hit it is the gradient the march already
        // computed.
        vec3 normal = gbuffer_normal;

        // Re-derive the material at the exact hit point from the analytic
        // scene, replacing the baked per-brick guess raymarch() returned --
        // the brick index quantizes material provenance to whole coarse
        // cells, which bleeds each primitive's texture up to a full
        // 0.25-unit cell onto whatever it touches. Falls back to the brick
        // material only if the analytic scene is somehow empty here (e.g.
        // a stale field mid-edit).
        // A BOUNDED cull radius, not UNBOUNDED_BOUNDING_RADIUS.
        //
        // scene_map()'s pre-check needs the caller's radius to be a genuine
        // upper bound on the scene's true nearest distance at p. This call
        // used to pass UNBOUNDED on the grounds that a single per-pixel
        // query has no such guarantee -- but that is only true of an
        // arbitrary point. p is a HIT point: the visibility pass put it on
        // the isosurface, so the scene's nearest distance there is within
        // SURF_DIST of zero by construction. That is the strongest bound
        // the pre-check could ask for.
        //
        // The margin covers the one way a far primitive can still matter:
        // a smooth blend reaches beyond its operands by up to the layer's
        // smoothness, so anything within that distance has to stay in the
        // loop. It is a fixed conservative constant rather than the scene's
        // real max_smoothness only because this pass's push block is at
        // 124 of Vulkan's guaranteed 128 bytes; any authored smoothness
        // below it is handled correctly, and cutters are never culled at
        // all (see scene_map()'s own note), so subtraction is unaffected
        // either way.
        //
        // This matters because the loop it shortens runs at every shaded
        // pixel against every primitive in the scene. Measured at 20-34ms
        // for this pass on a large scene -- three times everything else in
        // the frame combined.
        int analytic_material;
        scene_map(p, push.layer_count, ANALYTIC_HIT_CULL_RADIUS,
                  analytic_material);
        if (analytic_material >= 0) {
            hit_material = analytic_material;
        }
        exempt_flag = pixelation_exempt[hit_material];

        // World units per texture tile, from this hit's material (packed
        // in the colour's alpha slot -- see ScenePrimitiveColours above).
        // The clamp only guards a degenerate authored scale of ~0 turning
        // the division below into inf/NaN UVs.
        float texture_scale = max(scene_diffuse_colours[hit_material].a, 0.01);

        // Triplanar sample: project p onto each of the 3 axis planes, then
        // blend by how much the normal faces that axis (a normal facing
        // mostly +/-X should be dominated by the YZ-plane projection, since
        // that's the plane you'd actually be looking at face-on there).
        // The offset is added in world space *before* the texture_scale
        // divide (so it's authored in world units, like texture_scale
        // itself), and the rotation is applied per-plane just before each
        // sample below -- see ScenePrimitiveTexTransform above for both.
        vec3 tex_offset = scene_tex_transform[hit_material].xyz;
        float tex_rotation = scene_tex_transform[hit_material].w;
        vec3 tri_p = (p + tex_offset) / texture_scale;
        vec3 blend = abs(normal);
        blend /= (blend.x + blend.y + blend.z);

        vec2 uv_yz = rotate_uv(tri_p.yz, tex_rotation);
        vec2 uv_xz = rotate_uv(tri_p.xz, tex_rotation);
        vec2 uv_xy = rotate_uv(tri_p.xy, tex_rotation);

        vec3 tex_colour = sample_scene_texture(hit_material, uv_yz) * blend.x +
                          sample_scene_texture(hit_material, uv_xz) * blend.y +
                          sample_scene_texture(hit_material, uv_xy) * blend.z;
        vec3 tint = scene_diffuse_colours[hit_material].rgb;

        float h_centre, h_u, h_v;
        sample_scene_heights(hit_material, uv_yz, h_centre, h_u, h_v);
        vec3 bump_x = bump_from_heights(h_centre, h_u, h_v);
        sample_scene_heights(hit_material, uv_xz, h_centre, h_u, h_v);
        vec3 bump_y = bump_from_heights(h_centre, h_u, h_v);
        sample_scene_heights(hit_material, uv_xy, h_centre, h_u, h_v);
        vec3 bump_z = bump_from_heights(h_centre, h_u, h_v);

        normal = apply_triplanar_bump(normal, blend, bump_x, bump_y, bump_z);

        // Sum every registered light's direct diffuse contribution on top
        // of this point's baked indirect/bounce light (see
        // sample_probe_grid() above) and the ambient floor below. The probe
        // grid is the scene's real indirect answer; `ambient` also feeds the
        // bake (it is what a probe's gather rays pick up when they escape
        // the scene entirely -- see Builtin.ProbeBake.comp.glsl), but it is
        // added here as well so that raising it always brightens the image,
        // including on surfaces the bake found enclosed and on frames before
        // a rebake has run.
        //
        // calc_contact_ao() only darkens this indirect term, not the direct
        // lights summed below -- a directly-lit crease is already handled
        // by that light's own shadow_march() ray, and darkening direct
        // light too would double up with it (and read wrong for a crease
        // lit brightly enough to wash out any real occlusion).
        vec3 indirect = (push.flags & RENDER_FLAG_CHUNKED_FIELD) != 0
            ? sample_gi_cascade(p)
            : sample_probe_grid(p);
        // Occlusion of the indirect term. The traced estimate (screen-space
        // for contact scale, then the binary voxel cascades for everything
        // beyond) replaces the old field-marched contact AO wherever it is
        // available -- it sees geometry the field's local samples cannot,
        // and it costs one ray per pixel instead of a march per pixel.
        // calc_contact_ao() stays as the fallback for when the AO pass did
        // not run this frame.
        float ao = (push.flags & RENDER_FLAG_AO) != 0
            ? filtered_ao(pixel_coord, gbuffer_normal, depth)
            : calc_contact_ao(p, normal);
        // AMBIENT. A flat floor that is added regardless of how many lights
        // the scene has -- the scene-wide `ambient` control (GeometrySystem::
        // ambient()) is meant to be the knob that keeps unlit surfaces from
        // reading as black, and a term that only reaches the image through
        // the probe bake's escaped gather rays is not that knob: it does
        // nothing at all for a surface the bake decided was enclosed.
        //
        // Occlusion applies to it, but only down to AMBIENT_AO_FLOOR --
        // ambient's whole job is to be the floor, so letting occlusion take
        // it to zero would defeat the control the moment it is needed most.
        //
        // INDIRECT gets the same treatment for a different reason: the baked
        // GI already carries its own visibility (a probe's gather rays are
        // what compute it), so multiplying it by a freshly traced occlusion
        // term counts the same occlusion twice and is most of why interiors
        // came out very dark. The traced term still applies -- it sees
        // contact-scale detail no 16^3 probe grid can -- it just darkens
        // rather than erases.
        float occlusion = mix(INDIRECT_AO_FLOOR, 1.0, ao);
        vec3 lighting = indirect * occlusion +
            vec3(push.ambient) * mix(AMBIENT_AO_FLOOR, 1.0, ao);
        for (int i = 0; i < push.light_count; ++i) {
            Light light = lights[i];
            int light_type = int(light.vector_type.w);
            vec3 light_colour = light.colour_intensity.rgb;
            float intensity = light.colour_intensity.a;

            vec3 light_dir;
            float attenuation;
            float shadow_max_dist;
            if (light_type == 1) {
                // Point: direction from the surface to the light, with
                // inverse-square falloff (intensity == brightness at 1
                // world unit away).
                vec3 to_light = light.vector_type.xyz - p;
                float dist = length(to_light);
                light_dir = to_light / max(dist, 0.0001);
                attenuation = intensity / max(dist * dist, 0.0001);
                shadow_max_dist = dist; // nothing to occlude past the light itself
            } else {
                // Directional: shines uniformly from this direction, no
                // falloff.
                light_dir = normalize(light.vector_type.xyz);
                attenuation = intensity;
                shadow_max_dist = MAX_DIST; // no fixed distance to stop at
            }

            float diffuse = max(dot(normal, light_dir), 0.0);
            if (diffuse > 0.0) {
                // Only bother marching a shadow ray if this light would
                // otherwise contribute anything at all -- a surface facing
                // away from it is already at zero regardless of occlusion.
                vec3 shadow_origin = p + normal * SHADOW_NORMAL_BIAS;
                // An imperfect shadow map, where this light has one: a
                // single lookup instead of a whole march, which is what
                // makes dozens of shadowed local lights affordable at all
                // (see Builtin.ChunkShadowSplat.comp.glsl). Directional
                // lights, and anything past the map budget, still march --
                // that path is sharper, and the hero light is exactly where
                // that quality shows.
                if ((push.flags & RENDER_FLAG_ISM) != 0 && light_type == 1 &&
                    i < ISM_COUNT) {
                    float ism_dither = hash12(vec2(pixel_coord) +
                                              vec2(float(push.frame_index) * 0.618,
                                                   float(i)));
                    float ism_shadow = sample_ism(i, p, light.vector_type.xyz,
                                                  diffuse, ism_dither);
                    lighting += light_colour * attenuation * diffuse * ism_shadow;
                    continue;
                }
                float shadow = (push.flags & RENDER_FLAG_CHUNKED_FIELD) != 0
                    ? chunked_shadow_march(shadow_origin, light_dir,
                                          push.camera_position.xyz, shadow_max_dist,
                                          SHADOW_SOFTNESS, SHADOW_MAX_STEPS,
                                          int(light.source_primitive.x))
                    : shadow_march(shadow_origin, light_dir, shadow_max_dist,
                                   SHADOW_SOFTNESS, SHADOW_MAX_STEPS,
                                   int(light.source_primitive.x));
                diffuse *= shadow;
            }
            lighting += light_colour * diffuse * attenuation;
        }

        colour = tex_colour * tint * lighting;

        // Self-illumination: added straight in, independent of every light/
        // ambient/GI term above -- an emissive primitive (Material::
        // emissive_colour/emissive_intensity, packed engine-side into this
        // primitive's expr_scale.yzw) looks lit even in complete darkness,
        // the way a real light bulb or glowing panel would. (0,0,0) for a
        // non-emissive material, so this is a no-op for everything else.
        // VulkanRaymarchShader::rebuild_static_scene() also registers a
        // matching synthesized point light per emissive primitive, so it
        // doesn't just look bright here -- it actually illuminates the
        // rest of the scene too (see the direct-lighting loop above and
        // Builtin.ProbeBake.comp.glsl, which both read the same light
        // buffer).
        colour += primitives[hit_material].expr_scale.yzw;

        // Selection outline: a rim-light glow, brightest where the surface
        // grazes away from the camera (silhouette edges) and fading toward
        // the center of the shape when viewed face-on -- the standard "glow
        // outline" look editors use to show what's selected, without
        // needing a second geometry pass or edge-detection post-process.
        if (hit_material == push.selected_primitive_index) {
            float facing = max(dot(normal, -ray_dir), 0.0);
            float rim = pow(1.0 - facing, 3.0);
            const vec3 HIGHLIGHT_COLOUR = vec3(1.0, 0.55, 0.1);
            colour += HIGHLIGHT_COLOUR * rim * 1.5;
        }
    }

    // Volumetric light shafts -- see accumulate_volumetrics()'s comment.
    // Additive, and unconditional on whether the primary ray hit anything:
    // a shaft crossing empty space (in front of the background gradient)
    // should glow just as much as one crossing in front of solid geometry.
    // The dither offset is derived from pixel_coord (not a per-frame seed),
    // so it's stable frame-to-frame -- the moving element is the texture
    // scroll (push.time) below, not this noise pattern re-randomizing.
    float dither = hash12(vec2(pixel_coord));
    colour += accumulate_volumetrics(ray_origin, ray_dir, min(travelled, MAX_DIST), dither);

    if ((push.flags & RENDER_FLAG_GRID) != 0) {
        // travelled is >= MAX_DIST on a miss, which apply_reference_grid()
        // reads as "nothing occludes the plane". pixel_height matches the
        // uv mapping above: one pixel subtends 1/image_size.y of the ray
        // basis regardless of aspect.
        apply_reference_grid(colour, ray_origin, ray_dir, travelled,
                             1.0 / float(image_size.y));
    }

    imageStore(out_image, pixel_coord, vec4(colour, exempt_flag));
}
