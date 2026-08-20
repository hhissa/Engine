#version 450
#extension GL_GOOGLE_include_directive : require
// 64-bit integers only -- this shader READS the splat visibility buffer
// (see binding 20 below), it never atomically updates it, so unlike
// Builtin.ChunkPointSplat.comp.glsl it needs no atomic-int64 extension.
#extension GL_ARB_gpu_shader_int64 : require

// Pass 2 (repeating): marches a ray per pixel against the sparse voxel
// field baked by Builtin.RaymarchVoxelize.comp.glsl (every static
// primitive currently registered with GeometrySystem). Coarse cells with
// no static brick are skipped across in one step; bricked cells are
// sampled with trilinear interpolation.
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

// This pass writes no colour -- binding 0 (the colour target) belongs to
// Builtin.DeferredShade.comp.glsl, which shades what this pass decides is
// visible. It is still declared here because both pipelines bind the same
// descriptor set, and because imageSize() needs a handle to the render
// target to size the dispatch's bounds check against.
layout(binding = 0, rgba8) uniform writeonly image2D out_image;

// --- The G-buffer. ---
// Distance along this pixel's ray to whatever it hit, or -1 for a background
// pixel. Read by Builtin.DeferredShade.comp.glsl to rebuild the hit
// position, by Builtin.TaaResolve.comp.glsl to reproject the pixel into the
// previous frame, and by the ambient-occlusion pass to trace against the
// depth buffer.
layout(binding = 24, r32f) uniform writeonly image2D out_depth;
// xyz = surface normal, w = material index. Splats carry a baked normal and
// marched hits get the field gradient, so shading never has to recompute
// either.
layout(binding = 25, rgba16f) uniform writeonly image2D out_normal_material;

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
    int frame_index;              // Builtin.DeferredShade.comp.glsl, which
                                  // binds the same descriptor set and the
                                  // same push constants; both declarations
                                  // have to match byte for byte.
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
    // Index into primitives[] of the one primitive being interactively
    // moved, or -1. Excluded from the bake and unioned in analytically
    // below instead, so dragging it re-bakes nothing. 124 of 128 bytes.
    int dynamic_primitive;
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

// How many sphere-tracing steps a won splat is allowed in order to land on
// the real isosurface instead of on its own disc -- see the snap block in
// main(). Two or three is enough from a start already within a point
// spacing; the fourth is slack for a grazing ray.
const int SPLAT_SNAP_STEPS = 4;

// Fraction of the field's reported distance each snap step actually takes.
// Under 1 because a smoothly blended or subtracted field is not a true
// distance field and can report more room than exists -- the standard
// relaxation for sphere tracing a field whose Lipschitz bound is not
// guaranteed. The cost of the smaller step is nothing here: the splat has
// already put the ray within about a point spacing of the surface.
const float SPLAT_SNAP_RELAXATION = 0.7;

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

    // The interactively-moved primitive is not in the baked field at all
    // (that is the whole point -- moving it re-bakes nothing), so it is
    // unioned in here, evaluated analytically, one primitive per step.
    //
    // The subtlety is skip_dist. It is a "trust dist" flag, not a
    // magnitude: nonzero means the field has nothing here and the ray may
    // jump that far. Taking that jump unchanged would sail straight
    // through the dynamic primitive, which occupies space the baked field
    // has never heard of. So a skip is CLAMPED to the analytic distance,
    // and becomes a real hit once the analytic surface is the nearer of
    // the two.
    if (push.dynamic_primitive >= 0) {
        float dynamic_dist = primitive_sdf(push.dynamic_primitive, p);
        if (skip_dist != 0.0) {
            if (dynamic_dist < skip_dist) {
                // Inside the region the field wanted to skip: the analytic
                // surface is what the ray must march against from here.
                dist = dynamic_dist;
                skip_dist = 0.0;
                material_index = push.dynamic_primitive;
            }
        } else if (dynamic_dist < dist) {
            dist = dynamic_dist;
            material_index = push.dynamic_primitive;
        }
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

// The splat visibility buffer written by Builtin.ChunkPointSplat.comp.glsl
// earlier this same frame -- one uint64 per pixel, depth (float bits) in
// the high half, winning point id in the low half; ~0 means no splat
// covered this pixel. See that shader's header comment for the full
// scheme. Only meaningful while push.splat_mode is nonzero; always bound
// regardless (same Vulkan requirement as every other conditional binding
// here).
layout(binding = 20) readonly buffer SplatVisibilityBuffer {
    uint64_t splat_visibility[];
};

// The baked point clusters themselves, needed to turn a visibility-buffer
// payload back into a world position, a surface normal and a material.
// Identical buffers/layout to Builtin.ChunkPointSplat.comp.glsl's bindings
// 0/1 -- see CHUNK_CLUSTER_POINTS (Builtin.SdfFieldConfig.inc.glsl) for the
// packing.
layout(binding = 21) readonly buffer ChunkClusterPointBuffer {
    uvec2 cluster_points[];
};
layout(binding = 22) readonly buffer ChunkClusterBuffer {
    ChunkCluster chunk_clusters[];
};

// Conservative per-tile near bound, written by the same splat pass (see
// its DECLINED BRICKS STILL OCCLUDE note and SplatTileBoundBuffer there):
// for each SPLAT_TILE_SIZE-pixel tile, the distance to the nearest brick
// that chose NOT to splat, as raw float bits, or ~0 if none did. A splat
// farther than its tile's bound cannot be trusted as the first surface --
// something unsplatted is in front of it -- so read_splat() reports "no
// splat" there and the pixel marches instead.
layout(binding = 23) readonly buffer SplatTileBoundBuffer {
    uint splat_tile_bound[];
};

// Must match SPLAT_TILE_SIZE in Builtin.ChunkPointSplat.comp.glsl (and
// kSplatTileSize engine-side, which sizes the buffer) exactly.
const int SPLAT_TILE_SIZE = 16;

// push.splat_mode values -- must match SplatMode engine-side
// (vulkan_raymarch_shader.h).
const int SPLAT_MODE_OFF = 0;
const int SPLAT_MODE_PRIME = 1;
const int SPLAT_MODE_VISIBILITY = 2;

// Unpacks this pixel's visibility-buffer entry. Returns false if no splat
// covered it (the ~0 sentinel, or splatting disabled entirely), in which
// case out_dist/out_pos/out_material are untouched. Shared by both splat
// modes -- prime mode wants only the distance and the point's spacing,
// visibility mode wants the position and material too.
bool read_splat(ivec2 pixel_coord, ivec2 image_size, out float out_dist,
                out vec3 out_pos, out vec3 out_normal, out float out_spacing,
                out int out_material) {
    if (push.splat_mode == SPLAT_MODE_OFF) {
        return false;
    }
    uint64_t entry = splat_visibility[pixel_coord.y * image_size.x + pixel_coord.x];
    if (entry == ~uint64_t(0)) {
        return false;
    }

    out_dist = uintBitsToFloat(uint(entry >> 32));

    // Reject a splat that something unsplatted is standing in front of --
    // see SplatTileBoundBuffer above. Returning false here sends the pixel
    // down the ordinary march-from-the-camera path, which is exactly the
    // behavior the declined brick wanted for its own pixels; without this
    // the pixel would instead shade (or prime from) whatever splatted
    // BEHIND that brick, and the near surface would simply not be drawn.
    // Only Visibility mode ever declines a brick, so in Prime mode every
    // tile still holds the cleared sentinel and nothing is rejected.
    int tiles_x = (image_size.x + SPLAT_TILE_SIZE - 1) / SPLAT_TILE_SIZE;
    uint bound_bits = splat_tile_bound[(pixel_coord.y / SPLAT_TILE_SIZE) *
                                       tiles_x + pixel_coord.x / SPLAT_TILE_SIZE];
    if (bound_bits != ~0u && out_dist > uintBitsToFloat(bound_bits)) {
        return false;
    }

    // Low half: the point id (bits 0-24), with the spacing its splat was
    // drawn at in bits 25-27 as a power-of-two multiple of the cluster's own
    // voxel size (see where this is packed in Builtin.ChunkPointSplat.comp.
    // glsl -- carrying it avoids recomputing that pass's whole LOD selection
    // and its stochastic thinning here).
    uint payload = uint(entry & 0xFFFFFFFFul);
    uint spacing_code = (payload >> 25) & 7u;
    uint id = payload & 0x01FFFFFFu;
    int cluster_index = int(id / uint(CHUNK_CLUSTER_POINTS));
    int point_index = int(id % uint(CHUNK_CLUSTER_POINTS));

    ChunkCluster cluster = chunk_clusters[cluster_index];
    uvec2 packed = cluster_points[cluster_index * CHUNK_CLUSTER_POINTS +
                                  point_index];
    vec3 frac = vec3(float(packed.x & 1023u), float((packed.x >> 10) & 1023u),
                     float((packed.x >> 20) & 1023u)) * (1.0 / 1023.0);
    out_pos = cluster.bbox_min_cell.xyz + frac * cluster.bbox_min_cell.w;
    // The baked surface normal. Cheaper AND better than re-deriving it with
    // calc_static_normal() at the splat point: that costs six field lookups
    // per shaded pixel, and it is the normal of the field, not of the point,
    // which drift apart exactly where the field is coarsest.
    out_normal = unpack_point_normal(packed.y);
    out_spacing = (cluster.bbox_min_cell.w / float(CHUNK_BRICK_DIM)) *
        float(1u << spacing_code);
    // The baked per-brick material, exactly the provenance a raymarch hit in
    // this brick would have reported; main() re-derives it analytically at
    // the hit point afterward either way (see its own comment there).
    out_material = chunk_brick_primitive[int(cluster.meta.z)];
    return true;
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

const float PI = 3.14159265359;

// Marches the ray against the baked static field. hit_material receives
// the winning surface's material index once something is hit (always >=0
// -- an index into scene_textures/scene_diffuse_colours).
//
// start_t is the splat-primed conservative starting distance (see Builtin.
// ChunkPointSplat.comp.glsl's header comment), or 0.0 for the classic
// march-from-the-camera behavior. The prime is built conservative (always
// short of any surface its pixel can hit near the splatted point), so
// starting there changes only cost, not the result -- but as a safety net
// against a stale/mistaken prime that lands INSIDE geometry (where sphere
// tracing on abs(dist) would march deeper in, not out), the very first
// sample is checked: clearly negative means the prime overshot through a
// surface, and the ray restarts from 0 as if never primed.
float raymarch(vec3 ray_origin, vec3 ray_dir, float start_t,
               out int hit_material) {
    float travelled = start_t;
    hit_material = -1;
    bool prime_checked = (start_t <= 0.0);
    for (int i = 0; i < MAX_STEPS; ++i) {
        vec3 p = ray_origin + ray_dir * travelled;

        float static_dist, skip_dist;
        int static_material;
        sample_active_field(p, ray_dir, static_dist, skip_dist, static_material);
        bool static_valid = (skip_dist == 0.0);

        if (!prime_checked) {
            prime_checked = true;
            if (static_valid && static_dist < -SURF_DIST) {
                travelled = 0.0;
                continue;
            }
        }

        if (static_valid && abs(static_dist) < SURF_DIST) {
            hit_material = static_material;
            break;
        }

        float step = static_valid ? abs(static_dist) : skip_dist;
        travelled += max(step, SURF_DIST);

        if (travelled > MAX_DIST) {
            break;
        }
    }
    return travelled;
}

// Turns a won splat into a per-pixel shading position, by treating it as
// an oriented disc (its baked isosurface point plus the field's normal
// there) and intersecting that disc's plane with this pixel's own ray --
// the surface-splatting/EWA formulation.
//
// Without this, every pixel inside one splat's footprint shades at the
// splat point itself, so a whole block of pixels shares one position, one
// triplanar UV and therefore one texel: flat constant-colour blocks the
// size of the footprint, which reads as a drastic resolution loss against
// the marched image even though the geometry is in exactly the right
// place. The plane intersection costs one dot product per pixel and
// restores full texture resolution, because the position it produces
// varies continuously across the footprint.
//
// Falls back to the splat point itself when the intersection is
// ill-conditioned: a ray nearly parallel to the disc (denominator near
// zero, where the intersection shoots off to infinity), or a solution that
// lands implausibly far from the splat -- which means the normal used was
// a poor description of the local surface (a crease, or a splat that won a
// pixel near a silhouette). Clamping to a few point spacings keeps such a
// case no worse than the un-refined behavior instead of teleporting the
// shading point somewhere unrelated.
vec3 refine_splat_hit(vec3 ray_origin, vec3 ray_dir, vec3 splat_pos,
                      vec3 splat_normal, float spacing, out float out_dist) {
    float denom = dot(ray_dir, splat_normal);
    if (abs(denom) > 1e-3) {
        float t = dot(splat_pos - ray_origin, splat_normal) / denom;
        if (t > 0.0) {
            vec3 hit = ray_origin + ray_dir * t;
            if (dot(hit - splat_pos, hit - splat_pos) <=
                (spacing * 3.0) * (spacing * 3.0)) {
                out_dist = t;
                return hit;
            }
        }
    }
    out_dist = length(splat_pos - ray_origin);
    return splat_pos;
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

void main() {
    ivec2 pixel_coord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 image_size = imageSize(out_image);
    if (pixel_coord.x >= image_size.x || pixel_coord.y >= image_size.y) {
        return;
    }

    vec2 uv = (vec2(pixel_coord) + vec2(push.taa_jitter_x, push.taa_jitter_y) -
               0.5 * vec2(image_size)) / float(image_size.y);

    // Fully world-space: static registered primitives already carry their
    // own world position, baked directly into the field.
    vec3 ray_origin = push.camera_position.xyz;
    // Build the ray in world space from the camera's basis instead of the
    // fixed world axes: uv.x moves along camera_right, uv.y along
    // camera_up, and the base direction is camera_forward -- this is what
    // lets the camera look around instead of always facing +Z. At the
    // identity orientation (yaw = pitch = 0) the basis is exactly
    // (right=+X, up=+Y, forward=+Z), so this reduces to the old
    // normalize(vec3(uv, 1.0)) in that case.
    vec3 ray_dir = normalize(uv.x * push.camera_right.xyz +
                            uv.y * push.camera_up.xyz +
                            push.camera_forward.xyz);

    // Primary visibility. Three paths, all producing the same three
    // outputs (travelled / p / hit_material) for the shading block below,
    // which is why that block needed no splat-awareness of its own:
    //
    //   Visibility mode, splat covered this pixel: the winning splat IS
    //     the hit -- no primary march at all. The shading position is NOT
    //     the splat point itself, though: one splat covers a whole pixel
    //     footprint, so shading every pixel in it at that one point gives
    //     every pixel the same position, the same texture UV and therefore
    //     the same texel -- flat blocks of constant colour at the splat's
    //     footprint size, which is exactly what naive splatting looks
    //     like. Instead each splat is treated as an oriented disc (the
    //     standard surface/EWA-splatting formulation) and intersected with
    //     THIS pixel's own ray, so every pixel recovers its own position
    //     and its own UV -- see refine_splat_hit() above.
    //   Prime mode (or visibility mode where no splat covered the pixel):
    //     march as always, but start just short of the splat if there was
    //     one. Backing off by twice the winning point's own spacing keeps
    //     the start strictly in front of the surface (see Builtin.
    //     ChunkPointSplat.comp.glsl's depth note on why the margin is
    //     derived per-point here rather than baked in there).
    //   Splatting off entirely: an ordinary march from t=0, bit-for-bit
    //     what this shader did before any of this existed.
    //
    // The fallback is what keeps visibility mode complete without TAA: a
    // pixel NO splat covered (a brick too close to splat, footprint
    // clamping at extreme close-ups, or simply geometry outside the
    // chunked field) costs a march, never a hole.
    //
    // Note the limit of that guarantee, since it's the sharp edge here: it
    // saves a pixel with no splat at all, NOT a pixel whose nearest surface
    // was missed while something behind it splatted -- that pixel has a
    // splat, so it never reaches the march, and the near surface simply
    // isn't drawn. Keeping every brick's baked point set gap-free is what
    // rules that case out (see Builtin.ChunkVoxelize.comp.glsl's BUDGET
    // OVERFLOW comment); a brick that is not splatted at ALL is fine, a
    // brick splatted with holes in it is not.
    int hit_material = -1;
    float travelled;
    vec3 p;

    float splat_dist, splat_spacing;
    vec3 splat_pos, splat_normal;
    int splat_material;
    bool have_splat = read_splat(pixel_coord, image_size, splat_dist, splat_pos,
                                 splat_normal, splat_spacing, splat_material);

    if (have_splat && push.splat_mode == SPLAT_MODE_VISIBILITY) {
        // The disc's orientation is the normal baked into the point itself
        // (see read_splat()): the generator had the field's gradient in hand
        // when it projected the point onto the isosurface, so this is that
        // same gradient, for free, instead of six more field lookups here.
        p = refine_splat_hit(ray_origin, ray_dir, splat_pos, splat_normal,
                             splat_spacing, travelled);

        // ...and then SNAP that onto the real isosurface with a few sphere-
        // tracing steps, starting just short of the disc hit.
        //
        // The disc intersection alone is not a surface, it is a quilt of
        // them: neighbouring points carry slightly different baked normals,
        // so their discs tilt differently, and nearest-disc-wins makes the
        // shading position jump between planes at splat spacing. Everything
        // derived from that position inherits the jumps -- the triplanar UVs
        // (so the texture ripples), the height-derived bump normal (which
        // amplifies it), and the field gradient sampled there. That is what
        // reads as lumpy, quilted, wavy surfaces rather than flat ones.
        //
        // Sphere tracing from just before the splat converges in two or
        // three steps because the splat has already delivered the ray to
        // within about a point spacing of the surface -- which is the whole
        // point of splatting here: it buys the empty-space traversal, not
        // the final position. The disc result stands if the field has
        // nothing to say there (no brick baked, a stale field mid-edit).
        //
        // The stepping is deliberately CONSERVATIVE, and that matters most
        // exactly where this renderer's scenes are most interesting. A
        // smooth blend and a subtraction both produce a field that is not a
        // true Euclidean distance: max(a, -b) can report more distance than
        // there really is, so a sphere-trace step of the full reported
        // distance walks straight THROUGH the cut surface and lands on
        // whatever is behind it. That reads as the subtraction having no
        // effect (the hole looks filled) and as a blend losing its
        // rounding -- both symptoms of the same overshoot, in the two cases
        // where the field is least Euclidean. Relaxing the step and refusing
        // any result that did not actually converge close to the splat keeps
        // the disc's answer whenever the trace cannot be trusted.
        {
            float snap_t = max(travelled - splat_spacing, 0.0);
            for (int i = 0; i < SPLAT_SNAP_STEPS; ++i) {
                float snap_dist, snap_skip;
                int snap_material;
                sample_active_field(ray_origin + ray_dir * snap_t, ray_dir,
                                    snap_dist, snap_skip, snap_material);
                if (snap_skip != 0.0) {
                    break; // no baked surface here -- keep the disc's answer
                }
                if (abs(snap_dist) < SURF_DIST) {
                    // Converged -- but only trust it if it converged HERE,
                    // on the surface the splat was describing, rather than
                    // somewhere the ray wandered off to.
                    if (abs(snap_t - travelled) <= splat_spacing) {
                        travelled = snap_t;
                        p = ray_origin + ray_dir * snap_t;
                        hit_material = snap_material;
                    }
                    break;
                }
                snap_t += snap_dist * SPLAT_SNAP_RELAXATION;
                if (snap_t > travelled + splat_spacing) {
                    break; // wandered past the splat -- the disc was closer
                }
            }
        }
        if (hit_material < 0) {
            hit_material = splat_material;
        }
    } else {
        float start_t = have_splat ? max(splat_dist - splat_spacing * 2.0, 0.0)
                                   : 0.0;
        travelled = raymarch(ray_origin, ray_dir, start_t, hit_material);
        p = ray_origin + ray_dir * travelled;
    }

    // --- G-buffer stores. This is the whole output of the pass: what each
    // pixel sees, in the form Builtin.DeferredShade.comp.glsl needs to shade
    // it (and Builtin.TaaResolve.comp.glsl needs to reproject it). ---
    //
    // The shading normal is ALWAYS the field gradient at this pixel's own hit
    // position -- including on the splat path, where a baked per-point normal
    // is available and is not good enough.
    //
    // A splat covers a whole pixel footprint. Giving every pixel in that
    // footprint the same normal makes lighting constant across it, which
    // reads as flat facets the size of the splat: exactly the artefact
    // refine_splat_hit() exists to prevent for POSITION, reintroduced through
    // the normal instead. Diffuse shading is dominated by dot(normal,
    // light_dir), so a constant normal is if anything the more visible of the
    // two. The octahedral 8-bits-per-axis storage adds its own banding on top.
    //
    // The baked normal keeps its real job -- orienting the disc that
    // refine_splat_hit() intersects to find this pixel's position, where it
    // costs nothing and per-pixel variation would be meaningless. It is the
    // shading that needs the gradient, and it needs it at the REFINED
    // position, which is what makes the result vary smoothly across the
    // footprint.
    vec3 out_normal = vec3(0.0, 1.0, 0.0);
    if (travelled < MAX_DIST) {
        out_normal = calc_static_normal(p);
    }
    // Negative distance marks "nothing hit". travelled is measured along the
    // normalized ray direction built above, so any reader can recover the
    // hit position as origin + dir * depth.
    imageStore(out_depth, pixel_coord,
               vec4(travelled < MAX_DIST ? travelled : -1.0, 0.0, 0.0, 0.0));
    // Material is the provenance the visibility path had -- a splat's baked
    // per-brick primitive, or the brick the march ended in. The shading pass
    // refines it analytically at the exact hit point; this is its fallback
    // for the case where the analytic scene comes back empty (a stale field
    // mid-edit).
    imageStore(out_normal_material, pixel_coord,
               vec4(out_normal, float(hit_material)));
}
