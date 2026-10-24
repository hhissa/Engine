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

// M7 polish (live brush-cursor ghost preview): the same analytic brush-
// shape distance functions Builtin.SculptStamp.comp.glsl stamps with --
// pure functions of their arguments, no SSBO/UBO dependency, so including
// this here is free. See eval_brush_preview_sdf() below (and its own
// SHAPE_*/OP_* constants, duplicated from Builtin.SculptStamp.comp.glsl the
// same way Builtin.SculptSpraypaint.comp.glsl/Builtin.SculptCrop.comp.glsl
// already each keep their own copy -- GLSL compute shaders are separate
// translation units with no linking, so there is nowhere shared to put a
// single copy without a new .inc.glsl file for four lines of constants).
#include "Builtin.BrushShapes.inc.glsl"

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

// M3c (Spraypaint): which scene_diffuse_colours[]/material_scalars[] slot
// stands in for "the paint colour" -- VulkanRaymarchShader::
// ensure_default_sculpt_material() sets this one index up (a fixed, clearly
// visible colour) once, alongside index 0's plain-white default sculpt
// material; see this file's blend site (search "paint_opacity") for where
// it's actually used. Not a per-stroke colour -- no colour-picker UI exists
// yet (out of scope for this milestone, see the plan's M3c section).
const int PAINT_MATERIAL_SLOT = 1;
layout(binding = 6) uniform sampler2D scene_textures[MAX_SCENE_PRIMITIVES];
// Mirrors scene_textures above, one bump-map texture per primitive (see
// Material::bump_texture, engine-side) -- sampled purely for luminance by
// sample_scene_heights() below, never for colour. Unused/no-bump slots are
// bound to a genuinely flat/uniform-colour filler (TextureSystem::
// flat_texture()), not the checkerboard scene_textures falls back to, so a
// material with no bump map set reads as exactly zero bump perturbation.
layout(binding = 14) uniform sampler2D scene_bump_textures[MAX_SCENE_PRIMITIVES];

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
    int flags;                    // RENDER_FLAG_* bits -- see below.
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
// Two loose per-material scalars per registered static primitive (parallel
// to scene_diffuse_colours), packed together because neither fills a vec4
// and the two already-existing per-primitive vec4 buffers
// (scene_diffuse_colours, scene_tex_transform) have no free component
// left:
//   x = 1.0 if that primitive's material is pixelation-exempt
//       (Material::pixelation_exempt), 0.0 otherwise, PLUS half its
//       metallic (MaterialDef::metallic) -- so [0, 0.5] is a non-exempt
//       material and [1, 1.5] an exempt one (material_scalar_x(), engine
//       side). Builtin.DeferredShade.comp.glsl decodes both: the exempt
//       flag goes into out_image's alpha channel (see main()) for
//       Builtin.PostComposite.comp.glsl's pixelation pass to read.
//   y = its bump-map magnitude (Material::bump_strength) -- a multiplier
//       on BUMP_STRENGTH_BASE, so 1.0 (the engine-side default) is the
//       fixed strength this used to be hardcoded at, 0.0 is a flat surface
//       even with a bump map set, and >1 deepens the relief. Only
//       Builtin.DeferredShade.comp.glsl reads it.
//   z = 1.0 if this primitive casts shadows (Material::casts_shadow), 0.0
//       if shadow rays should pass straight through it -- see
//       SHADOW_CASTER_TEST below.
//   w = its dielectric surface roughness (Material -> MaterialDef::
//       roughness), 0 = mirror-sharp, 1 = fully diffuse. Read only by
//       Builtin.DeferredShade.comp.glsl's specular lobe. This slot was
//       documented as unused until that lobe existed; roughness is what
//       every dielectric needs and what glass, plastic and still water all
//       differ in, so it is the natural occupant.
layout(binding = 11) readonly buffer ScenePrimitiveMaterialScalars {
    vec4 material_scalars[];
};

// How the shared shadow marches (shadow_march() in Builtin.BakedFieldCommon.
// inc.glsl, chunked_shadow_march() in Builtin.ChunkedFieldCommon.inc.glsl)
// ask whether a surface they ran into actually casts. Defined here, BEFORE
// those includes, because they are shared with shaders that have no
// material data bound at all (the GI bakes) and so fall back to their own
// "everything casts" default -- see SHADOW_CASTER_TEST's definition there.
#define SHADOW_CASTER_TEST(material) (material_scalars[material].z > 0.5)

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

// M3c (Spraypaint): the per-voxel paint-opacity accumulator Builtin.
// SculptSpraypaint.comp.glsl writes (see its own header comment for the
// accumulation formula) -- same size/addressing as ChunkBrickPoolBuffer
// (CHUNKED_FIELD_BRICKPOOL_BINDING above), one float per voxel.
//
// A NEW, SEPARATE descriptor set (set = 1), not a 30th binding on this
// shader's set 0 -- set 0 is render_set_ (VulkanRaymarchShader), confirmed
// fully packed at 29/29 bindings and shared, as the identical layout, with
// Builtin.RaymarchShader.comp.glsl's own pipeline (the visibility pass,
// which has no use for paint data at all). VulkanComputePipeline already
// takes a vector of descriptor-set layouts, so deferred_shade_pipeline_ alone
// (not render_pipeline_) binds a second, tiny, dedicated set here -- see
// VulkanRaymarchShader's constructor (paint_read_set_layout_/
// paint_read_set_) -- leaving render_set_'s own 29-binding layout, and the
// visibility pass that also uses it, completely untouched. This is the same
// "new work gets its own dedicated set" precedent M1a/M2b already
// established for Builtin.SculptStamp.comp.glsl/Builtin.SculptUndoRestore.
// comp.glsl, just applied to an EXISTING shader's SECOND set instead of a
// brand new pipeline's only set.
layout(set = 1, binding = 0) readonly buffer ChunkBrickPaintBuffer {
    float chunk_brick_paint[];
};

// M7 polish (live brush-cursor ghost preview): the single small UBO
// VulkanRaymarchShader::set_brush_preview() writes every frame the preview
// is visible -- binding 1 of the SAME set = 1 ChunkBrickPaintBuffer above
// already uses, not a third set (see paint_read_set_layout_'s own comment,
// vulkan_raymarch_shader.h, for why new shading-only state belongs here).
// Field layout matches GpuBrushPreview (vulkan_raymarch_shader.cpp) field-
// for-field -- three vec4-sized members, nothing narrower, so std430/std140
// (identical here, since every field is already 16-byte-aligned) can never
// silently diverge from the C++ struct the way a bare vec3 bit M1a.
layout(set = 1, binding = 1) uniform BrushPreviewBuffer {
    vec4 center_enabled; // xyz = world-space center, w = 1.0/0.0 enabled.
    vec4 params;          // Same per-shape layout as StampCell's `params`
                         // below -- see eval_brush_preview_sdf()'s own
                         // comment for the full per-shape table.
    ivec4 shape_op_slot;  // x = shape_type (SHAPE_* below), y = op (OP_*
                         // below), z = material slot (into scene_diffuse_
                         // colours/material_scalars/transmission, same
                         // arrays every real sculpt material reads), w =
                         // unused padding.
    vec4 rotation;        // the shape's orientation, quaternion xyzw
} brush_preview;

// TAA-ghosting fix: the preview used to be blended directly into `colour`
// below, which is exactly what out_image feeds into Builtin.TaaResolve.
// comp.glsl as `current_colour`. TAA reprojects by DEPTH, and the preview
// never touches gbuffer_depth (it isn't real geometry), so TAA saw "same
// surface as last frame, trust history" every frame and crushed the ghost
// down to history's tiny per-frame blend weight instead of showing it at
// the strength computed below. Writing it here, into its own image, and
// compositing it back in AFTER TAA (Builtin.PostComposite.comp.glsl,
// binding 3) keeps it out of what TAA reprojects entirely -- same reason
// scene_viewport.cpp's wireframe cursor outline (drawn later still, in the
// UI render pass over the swapchain image) has always stayed crisp. Same
// render-target size/lifetime as gbuffer_depth etc (recreate_render_
// target_images(), vulkan_raymarch_shader.cpp) and, like out_image, written
// unconditionally every pixel every frame -- vec4(0) when the preview is
// off or this pixel has no hit -- so a stale previous-frame ghost can never
// survive into a frame that no longer wants one.
layout(set = 1, binding = 2, rgba8) uniform writeonly image2D preview_overlay_image;

// Selection outline: the stamps of every object the app wants outlined
// (VulkanRaymarchShader::set_selection_outline()), grouped per object, so
// each object's own silhouette can be ray-marched analytically here --
// the baked field doesn't know which object a voxel belongs to. Layout
// matches GpuSelectionOutline (vulkan_raymarch_shader.cpp) exactly.
const int OUTLINE_MAX_OBJECTS = 16;
const int OUTLINE_MAX_STAMPS = 512;
const int SCOPE_BIT_COUNT = 65536; // = kScopeBitCount engine-side
struct OutlineObject {
    vec4 center_radius; // bounding sphere of the whole object
    ivec4 range;        // x = first stamp, y = stamp count, z = 1 for
                        // the scoped-in set (masked, not outlined)
};
struct OutlineStamp {
    vec4 center_radius; // xyz = center, w = bounding radius
    vec4 params;        // same per-shape params as a stamp
    vec4 rotation;      // quaternion xyzw
    vec4 smooth_shape_op; // x = smoothness, y = shape, z = op,
                          // w = object id (object-scoped ops)
};
layout(set = 1, binding = 4) readonly buffer SelectionOutlineBuffer {
    ivec4 outline_counts; // x = object count (0 = no outline), y = scope ids
    vec4 outline_colour;
    vec4 outline_style;   // x = dot spacing (px), y = fraction of each
                          // spacing that is dot, z = line width (px),
                          // w = alpha of the parts hidden behind geometry.
                          // The line starts OUTLINE_GAP_PX outside the
                          // silhouette so it never sits on the object.
    OutlineObject outline_objects[OUTLINE_MAX_OBJECTS];
    OutlineStamp outline_stamps[OUTLINE_MAX_STAMPS];
    // The scoped-in objects, one bit per object id; outline_counts.y is how
    // many (0: none, -1: an id past SCOPE_BIT_COUNT, so unknown).
    uint scope_bits[SCOPE_BIT_COUNT / 32];
};

bool owner_in_scope(int owner) {
    return owner >= 0 && owner < SCOPE_BIT_COUNT &&
           (scope_bits[owner >> 5] & (1u << uint(owner & 31))) != 0u;
}

// Scope-in mask: 1 where a scoped-in object is what this pixel shows, else
// 0 (1 everywhere when nothing is scoped). Builtin.PostComposite.comp.glsl
// blurs and greys every 0 pixel.
layout(set = 1, binding = 5, rgba8) uniform writeonly image2D scope_mask_image;

// Per-material extras, indexed like scene_diffuse_colours: x = glow (0 =
// none, 1 = full) -- the surface adds its own albedo as light, so it reads
// as glowing in its colour and the bloom pass picks it up. See
// VulkanRaymarchShader::set_material_glow().
layout(set = 1, binding = 6) readonly buffer MaterialExtraBuffer {
    vec4 material_extra[];
};
const float GLOW_STRENGTH = 1.35;
// How much of a glow light scatters into the air as a halo -- see the
// glow-halo loop in main().
const float GLOW_HALO_STRENGTH = 0.06;
const float GLOW_HALO_REACH = 0.7; // halo fade distance per sqrt(intensity)

// Shape/op selectors -- brush_preview.shape_op_slot's x/y values. Must
// match VulkanRaymarchShader::BrushShape/BrushOp (and SculptBrushShape/
// SculptBrushOp, renderer_types.inl) exactly -- same lockstep note as
// Builtin.SculptStamp.comp.glsl's own copy of these constants (nothing in
// C++ checks THIS half of it either; the static_assert in vulkan_backend.
// cpp only pins the two C++-side enums against each other).
const int PREVIEW_SHAPE_SPHERE = 0;
const int PREVIEW_SHAPE_BOX = 1;
const int PREVIEW_SHAPE_PLANE = 2;
const int PREVIEW_SHAPE_TORUS = 3;
const int PREVIEW_SHAPE_CAPPED_CYLINDER = 4;
const int PREVIEW_SHAPE_CAPPED_CONE = 5;
const int PREVIEW_SHAPE_ROUND_BOX = 6;
const int PREVIEW_SHAPE_BOX_FRAME = 7;
const int PREVIEW_SHAPE_OCTAHEDRON = 8;
const int PREVIEW_SHAPE_PYRAMID = 9;
const int PREVIEW_SHAPE_HEX_PRISM = 10;
const int PREVIEW_SHAPE_ROUND_CONE = 11;
const int PREVIEW_SHAPE_CAPSULE = 12;
const int PREVIEW_SHAPE_LINK = 13;
const int PREVIEW_SHAPE_ELLIPSOID = 14;
const int PREVIEW_OP_UNION = 0;
const int PREVIEW_OP_SUBTRACTION = 1;
const int PREVIEW_OP_OBJECT_UNION = 2;
const int PREVIEW_OP_OBJECT_SUBTRACTION = 3;
const int PREVIEW_OP_OBJECT_INTERSECTION = 4;
const int PREVIEW_OP_OBJECT_PAINT = 5;

// Evaluates the selected brush shape at world point p against a brush
// centered at `center`, in world space -- deliberately NOT pre-translated
// by the caller (unlike Builtin.SculptStamp.comp.glsl's eval_brush_shape(),
// which always receives an already-local p) so main()'s sphere-trace below
// and its central-difference normal estimate can both call this directly
// with raw world-space sample points. Picks the same subset of the 4-float
// params block per shape stamp_shape() does -- see VulkanRaymarchShader::
// stamp_shape()'s header comment for the authoritative per-shape table
// (duplicated in Builtin.SculptStamp.comp.glsl's own eval_brush_shape()
// comment too): Sphere = params.x; Box/RoundBox/BoxFrame/Ellipsoid =
// params.xyz (+ params.w for RoundBox's corner radius/BoxFrame's edge
// thickness); Plane = params.x; Torus = params.xy; CappedCylinder/Capsule/
// HexPrism = params.xy; CappedCone/RoundCone/Link = params.xyz;
// Octahedron/Pyramid = params.xy.
float eval_brush_preview_sdf(vec3 p, int shape_type, vec4 params, vec3 center) {
    return brush_shape_sdf(p - center, shape_type, params);
}

const float OUTLINE_GAP_PX = 1.0;

// One outlined object's (or group's) distance at p: its stamps combined in
// list order, exactly as the sculpt combines them -- global ops on the
// running field, object-scoped ops (op >= PREVIEW_OP_OBJECT_UNION) on
// their own object's field, merged with a hard min when its run ends (see
// Builtin.SculptChunkRecompute.comp.glsl). Bounding spheres stand in for
// stamps that can't change the result (or for a conservative, never
// overshooting, step).
float outline_object_sdf(vec3 p, int first, int count) {
    float d = 1e10;
    float obj = 1e10;
    bool in_run = false;
    int run_object = -1;
    float h;
    for (int i = first; i < first + count; ++i) {
        OutlineStamp stamp = outline_stamps[i];
        float k = stamp.smooth_shape_op.x;
        int op = int(stamp.smooth_shape_op.z);
        int object = int(stamp.smooth_shape_op.w);
        bool scoped = op >= PREVIEW_OP_OBJECT_UNION;
        if (in_run && (!scoped || object != run_object)) {
            d = min(d, obj);
            in_run = false;
        }
        if (scoped && !in_run) {
            in_run = true;
            run_object = object;
            obj = 1e10;
        }
        if (op == PREVIEW_OP_OBJECT_PAINT) {
            continue; // no geometry
        }
        float bound = length(p - stamp.center_radius.xyz) - stamp.center_radius.w;
        bool adds = op == PREVIEW_OP_UNION || op == PREVIEW_OP_OBJECT_UNION;
        bool cuts = op == PREVIEW_OP_SUBTRACTION || op == PREVIEW_OP_OBJECT_SUBTRACTION;
        float target = scoped ? obj : d;
        if (adds && bound > target + k) {
            continue;
        }
        if (cuts && (bound > 0.0 + k || target >= 1e9)) {
            continue; // p is outside the cutter: nothing carved here
        }
        if (op == PREVIEW_OP_OBJECT_INTERSECTION) {
            if (obj >= 1e9) {
                continue;
            }
            if (bound > k + 0.05) {
                obj = max(obj, bound - k); // a safe under-estimate
                continue;
            }
        }
        vec3 local = quat_rotate_inverse(stamp.rotation, p - stamp.center_radius.xyz);
        float sd = eval_brush_preview_sdf(local, int(stamp.smooth_shape_op.y),
                                          stamp.params, vec3(0.0));
        if (op == PREVIEW_OP_UNION) {
            d = d >= 1e9 ? sd : smooth_union(sd, d, k, h);
        } else if (op == PREVIEW_OP_SUBTRACTION) {
            d = smooth_subtraction(sd, d, k, h);
        } else if (op == PREVIEW_OP_OBJECT_UNION) {
            obj = obj >= 1e9 ? sd : smooth_union(sd, obj, k, h);
        } else if (op == PREVIEW_OP_OBJECT_SUBTRACTION) {
            obj = smooth_subtraction(sd, obj, k, h);
        } else { // PREVIEW_OP_OBJECT_INTERSECTION
            obj = -smooth_union(-sd, -obj, k, h);
        }
    }
    if (in_run) {
        d = min(d, obj);
    }
    return d;
}

// The selection outline for this pixel: rgb + alpha (0 = none). A ray
// that misses an outlined object but passes within outline_style.z pixels
// of it is on that object's silhouette -- the raymarching "outline for
// free", here against just the selected objects' own stamps. Dots are
// spaced by angle around the object's on-screen centre. Parts hidden
// behind other geometry (scene_depth nearer than the silhouette) draw at
// outline_style.w alpha.
vec4 selection_outline(vec3 ray_origin, vec3 ray_dir, vec2 pixel,
                       vec2 image_size, float scene_depth) {
    float best_alpha = 0.0;
    float pixel_angle = 1.0 / image_size.y; // world size of a pixel per unit distance
    float outer = OUTLINE_GAP_PX + outline_style.z; // band's outer edge, px
    for (int o = 0; o < outline_counts.x; ++o) {
        OutlineObject object = outline_objects[o];
        if (object.range.z != 0) {
            continue; // the scoped-in set: masked, not outlined
        }
        // Ray vs the object's bounding sphere, widened by the line width.
        vec3 oc = object.center_radius.xyz - ray_origin;
        float tc = dot(oc, ray_dir);
        if (tc <= 0.0) {
            continue;
        }
        float radius = object.center_radius.w + (outer + 1.0) * pixel_angle * tc;
        float miss2 = dot(oc, oc) - tc * tc;
        if (miss2 > radius * radius) {
            continue;
        }
        float half_chord = sqrt(radius * radius - miss2);
        float t = max(tc - half_chord, 0.0);
        float t_end = tc + half_chord;

        // Sphere trace, keeping the closest approach in pixels.
        float closest_px = 1e10;
        float closest_t = t;
        bool hit = false;
        for (int step = 0; step < 96 && t < t_end; ++step) {
            float d = outline_object_sdf(ray_origin + ray_dir * t,
                                         object.range.x, object.range.y);
            // A tenth of a pixel: anything nearer is inside the shape.
            if (d < 0.1 * pixel_angle * t) {
                hit = true;
                break;
            }
            float px = d / (t * pixel_angle);
            if (px < closest_px) {
                closest_px = px;
                closest_t = t;
            }
            t += max(d, 1e-4);
        }
        if (hit || closest_px > outer + 0.5) {
            continue;
        }

        // Dot pattern: angle around the object's on-screen centre.
        vec3 rel = object.center_radius.xyz - push.camera_position.xyz;
        float cz = dot(rel, push.camera_forward.xyz);
        if (cz <= 0.0) {
            continue;
        }
        vec2 centre_px = vec2(dot(rel, push.camera_right.xyz),
                              dot(rel, push.camera_up.xyz)) / cz * image_size.y +
                         0.5 * image_size;
        float ring_px = object.center_radius.w / cz * image_size.y * 0.6;
        float dots = max(8.0, floor(6.2831853 * ring_px / outline_style.x));
        vec2 rel_px = pixel - centre_px;
        float phase = fract((atan(rel_px.y, rel_px.x) / 6.2831853 + 0.5) * dots);
        if (phase > outline_style.y) {
            continue;
        }

        // The band [gap, outer] px outside the silhouette, with soft
        // (anti-aliased) edges; dimmed where hidden.
        float alpha = smoothstep(OUTLINE_GAP_PX - 0.5, OUTLINE_GAP_PX + 0.5, closest_px) *
                      (1.0 - smoothstep(outer - 0.5, outer + 0.5, closest_px));
        if (scene_depth >= 0.0 && scene_depth < closest_t * 0.98) {
            alpha *= outline_style.w;
        }
        best_alpha = max(best_alpha, alpha);
    }
    return vec4(outline_colour.rgb, best_alpha * outline_colour.a);
}

// The scope-in mask for this pixel (see scope_mask_image): 1 if a scoped
// object is hit and nothing nearer covers it (the G-buffer surface is at
// about the same depth -- the baked field differs from the analytic one by
// a voxel or so), or if the ray grazes one within a pixel where the
// G-buffer surface sits right there (the rendered edge). 1 everywhere when
// nothing is scoped.
float scope_mask(vec3 ray_origin, vec3 ray_dir, vec2 image_size,
                 float scene_depth) {
    float pixel_angle = 1.0 / image_size.y;
    bool any_scope = false;
    for (int o = 0; o < outline_counts.x; ++o) {
        OutlineObject object = outline_objects[o];
        if (object.range.z == 0) {
            continue;
        }
        any_scope = true;
        vec3 oc = object.center_radius.xyz - ray_origin;
        float tc = dot(oc, ray_dir);
        float radius = object.center_radius.w;
        float miss2 = dot(oc, oc) - tc * tc;
        if (tc + radius <= 0.0 || miss2 > radius * radius) {
            continue;
        }
        float half_chord = sqrt(radius * radius - miss2);
        float t = max(tc - half_chord, 0.0);
        float t_end = tc + half_chord;
        float closest_px = 1e10;
        float closest_t = t;
        for (int step = 0; step < 128 && t < t_end; ++step) {
            float d = outline_object_sdf(ray_origin + ray_dir * t,
                                         object.range.x, object.range.y);
            if (d < 0.1 * pixel_angle * max(t, 0.01)) {
                float tolerance = 0.05 + 0.02 * t;
                if (scene_depth < 0.0 || scene_depth > t - tolerance) {
                    return 1.0;
                }
                break;
            }
            float px = d / (max(t, 0.01) * pixel_angle);
            if (px < closest_px) {
                closest_px = px;
                closest_t = t;
            }
            t += max(d, 1e-4);
        }
        if (closest_px < 1.5 && scene_depth >= 0.0 &&
            abs(scene_depth - closest_t) < 0.05 + 0.02 * closest_t) {
            return 1.0;
        }
    }
    return any_scope ? 0.0 : 1.0;
}

// Level-0 brick addressing below mirrors sample_chunked_field_at_level_
// slot()'s brick-resolve + trilinear block (every sculpt brush only touches
// level 0 of the chunked field).
// The paint buffer holds two things (see Builtin.SculptChunkRecompute.
// comp.glsl): a value >= 1.5 is a per-voxel owner code -- the material slot
// of the stamp owning the surface there (which can differ within one
// brick) and its object (out_owner, -1 if unknown) -- and a
// value in (0, 1] is the Qt tool's spray-paint opacity of
// PAINT_MATERIAL_SLOT. Resolves both at world point p: out_slot is the slot
// covering the most of p's trilinear neighbourhood (-1 if none; smooth
// boundaries, since the switch happens where the weights cross), and
// out_opacity the interpolated spray opacity. Level 0 only, as above.
void sample_chunk_material_at_point(vec3 p, out int out_slot,
                                    out float out_opacity, out int out_owner) {
    out_slot = -1;
    out_opacity = 0.0;
    out_owner = -1;
    const int level = 0;
    float chunk_world_size = chunk_level_world_size(level);
    float cell_size = chunk_world_size / float(CHUNK_COARSE_DIM);
    int slot = chunk_table_slot_at(p, level);
    if (slot < 0) {
        return;
    }
    ivec3 chunk_coord = ivec3(floor(p / chunk_world_size));
    vec3 chunk_world_min = vec3(chunk_coord) * chunk_world_size;
    vec3 local = (p - chunk_world_min) / cell_size;
    ivec3 cell = clamp(ivec3(floor(local)), ivec3(0), ivec3(CHUNK_COARSE_DIM - 1));
    int local_cell_index = cell.x + cell.y * CHUNK_COARSE_DIM +
        cell.z * CHUNK_COARSE_DIM * CHUNK_COARSE_DIM;
    int brick_index = chunk_indirection[slot * CHUNK_CELL_COUNT + local_cell_index];
    if (brick_index < 0) {
        return;
    }
    vec3 cell_min = chunk_world_min + vec3(cell) * cell_size;
    float voxel_size = cell_size / float(CHUNK_BRICK_DIM);
    vec3 f = (p - cell_min) / voxel_size - 0.5;
    ivec3 i0 = clamp(ivec3(floor(f)), ivec3(-1), ivec3(CHUNK_BRICK_DIM));
    ivec3 i1 = clamp(i0 + ivec3(1), ivec3(-1), ivec3(CHUNK_BRICK_DIM));
    vec3 t = clamp(f - vec3(i0), 0.0, 1.0);
    ivec3 s0 = i0 + ivec3(1);
    ivec3 s1 = i1 + ivec3(1);
    int base = brick_index * CHUNK_BRICK_VOXEL_COUNT;

    int slots[8];
    float weights[8];
    int distinct = 0;
    float best_weight = 0.0;
    for (int c = 0; c < 8; ++c) {
        ivec3 corner = ivec3((c & 1) != 0 ? s1.x : s0.x,
                             (c & 2) != 0 ? s1.y : s0.y,
                             (c & 4) != 0 ? s1.z : s0.z);
        float w = ((c & 1) != 0 ? t.x : 1.0 - t.x) *
                  ((c & 2) != 0 ? t.y : 1.0 - t.y) *
                  ((c & 4) != 0 ? t.z : 1.0 - t.z);
        float v = chunk_brick_paint[base + corner.x +
                                    corner.y * CHUNK_BRICK_APRON_DIM +
                                    corner.z * CHUNK_BRICK_APRON_DIM * CHUNK_BRICK_APRON_DIM];
        if (v < 1.5) {
            out_opacity += w * clamp(v, 0.0, 1.0);
            continue;
        }
        int corner_slot = int(round(v)); // the owner code, decoded below
        int found = -1;
        for (int k = 0; k < distinct; ++k) {
            if (slots[k] == corner_slot) {
                found = k;
            }
        }
        if (found < 0) {
            found = distinct++;
            slots[found] = corner_slot;
            weights[found] = 0.0;
        }
        weights[found] += w;
        if (weights[found] > best_weight) {
            best_weight = weights[found];
            out_slot = corner_slot;
        }
    }
    // out_slot holds the winning owner code, (owner + 1) * 1024 + slot (see
    // Builtin.SculptChunkRecompute.comp.glsl's owner_code()); older
    // bricks may still hold a bare slot.
    if (out_slot >= 1024) {
        out_owner = out_slot / 1024 - 1;
        out_slot = out_slot % 1024;
    }
    if (out_slot < 2 || out_slot >= MAX_SCENE_PRIMITIVES) {
        out_slot = -1; // the engine defaults: keep the brick's material
    }
}

// The brush blended into what's there (an adding brush with smoothness,
// brush_preview.shape_op_slot.w as float bits): the surface a stamp here
// would leave, fillet included. Only the sculpture being edited (the
// scoped-in objects, in sculpt mode) blends -- strokes weld only within
// their sculpture -- so field that belongs to anything else counts as
// empty here, and the brush meets it hard, as the stamp will.
float preview_blend_sdf(vec3 p, float k) {
    vec3 local = quat_rotate_inverse(brush_preview.rotation,
                                     p - brush_preview.center_enabled.xyz);
    float brush = eval_brush_preview_sdf(local, brush_preview.shape_op_slot.x,
                                         brush_preview.params, vec3(0.0));
    float dist, skip_dist;
    int material;
    sample_active_field(p, vec3(0.0, 0.0, 1.0), dist, skip_dist, material);
    float scene = skip_dist > 0.0 ? max(skip_dist, k) : dist;
    if (scene < k && outline_counts.y > 0) {
        int slot_unused;
        float opacity_unused;
        int owner;
        sample_chunk_material_at_point(p, slot_unused, opacity_unused, owner);
        if (owner >= 0 && !owner_in_scope(owner)) {
            return brush; // another object's (unknown: count it in)
        }
    }
    float h;
    return smooth_union(brush, scene, k, h);
}

// The scene with a carving brush taken out of the sculpture being edited
// (other objects are left as they are, as the stamp will leave them): the
// surface a carve here would leave, live. The field only holds bricks near
// surfaces, so a cell with none is empty space OR deep inside a solid:
// `inside` carries which, from the last brick the ray passed through.
float preview_carve_sdf(vec3 p, float k, inout bool inside) {
    vec3 local = quat_rotate_inverse(brush_preview.rotation,
                                     p - brush_preview.center_enabled.xyz);
    float brush = eval_brush_preview_sdf(local, brush_preview.shape_op_slot.x,
                                         brush_preview.params, vec3(0.0));
    float dist, skip_dist;
    int material;
    sample_active_field(p, vec3(0.0, 0.0, 1.0), dist, skip_dist, material);
    float scene;
    if (skip_dist > 0.0) {
        scene = inside ? -skip_dist : skip_dist;
    } else {
        scene = dist;
        inside = dist < 0.0;
    }
    if (brush > k) {
        return scene; // out of the brush's reach
    }
    if (outline_counts.y > 0) {
        int slot_unused;
        float opacity_unused;
        int owner;
        sample_chunk_material_at_point(p, slot_unused, opacity_unused, owner);
        if (owner >= 0 && !owner_in_scope(owner)) {
            return scene;
        }
    }
    float h;
    return smooth_subtraction(brush, scene, k, h);
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

        // A material that opts out of casting (Material::casts_shadow)
        // darkens nothing here either -- this term is what occludes the
        // ambient floor and the indirect light on the frames the traced AO
        // pass did not run, so without the test the flag would hold for
        // direct light and silently not hold for ambient. Treated as the
        // fully open reading (d == step_dist, zero contribution) rather
        // than skipped, so the tap still costs its weight and a caster
        // further along the normal is not promoted into its place.
        if (skip_dist == 0.0 && material >= 0 && !SHADOW_CASTER_TEST(material)) {
            d = step_dist;
        }

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
// The magnitude a material of bump_strength 1.0 perturbs at -- the value
// this whole computation used to be fixed at, kept as the base so an
// authored strength reads as a plain multiple of "the usual amount" (see
// Material::bump_strength engine-side / material_scalars.y above).
const float BUMP_STRENGTH_BASE = 0.25;

float luminance(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

// Converts 3 nearby luminance samples (centre, +epsilon along each local
// axis) into a tangent-space bump normal: (dHeight/du, dHeight/dv, 1),
// negated and scaled so brighter neighbours slope the normal away from
// them, then normalized -- the same convention an authored tangent-space
// normal-map texture's RGB channels would encode.
// strength is the material's own bump magnitude, already multiplied by
// BUMP_STRENGTH_BASE by the caller. 0 makes both slopes vanish and this
// returns the unperturbed (0,0,1), i.e. exactly what a material with no
// bump map set already produced -- so a bump map can be dialled all the
// way out without clearing it.
vec3 bump_from_heights(float h_centre, float h_u, float h_v, float strength) {
    float du = (h_u - h_centre) / BUMP_UV_EPSILON;
    float dv = (h_v - h_centre) / BUMP_UV_EPSILON;
    return normalize(vec3(-du * strength, -dv * strength, 1.0));
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

// One entry per primitive index, but only ever read for indices inside the
// TRANSMISSIVE tail range (see transmissive_range() below).
//   xyz = the per-channel Beer-Lambert absorption coefficient, solved
//         engine-side from the material's authored absorption tint and
//         reference thickness so nobody has to author reciprocal metres.
//   w   = index of refraction, with its SIGN carrying the thin-walled
//         flag: |w| is the IOR, w < 0 means parallel-walled glass thin
//         enough that refraction displaces the ray by less than a pixel.
//         Packed into the sign rather than given its own slot because
//         material_scalars' vec4 is full and the push block is at exactly
//         Vulkan's guaranteed 128 bytes. w == 0 means opaque, which is
//         what every slot outside the range holds.
layout(binding = 28) readonly buffer ScenePrimitiveTransmission {
    vec4 transmission[];
};

// --- Dielectric specular -------------------------------------------
//
// Until this existed the shading model terminated at
// `colour = albedo * lighting`, a pure Lambert sum with no specular term,
// no Fresnel and no environment reflection anywhere in the frame. That is
// a bigger gap than it sounds: an opaque plastic IS a diffuse substrate
// under a dielectric coat and needs nothing else, and for glass the
// Fresnel reflection is a stronger recognition cue than the refraction --
// a pane with a correct highlight and no refraction reads as glass, while
// one with perfect refraction and no reflection reads as a warped hole.
//
// Everything here is DIELECTRIC. There is no metalness input, so the
// specular colour is always white: tinting it is precisely what makes a
// surface read as metal, so keeping it neutral is what keeps plastic
// looking like plastic.

// Reflectance at normal incidence for an air-to-medium interface.
// n = 1.5 gives 0.04, the "dielectric default" every PBR renderer
// hardcodes -- which is exactly glass, and near enough every plastic.
const float DEFAULT_DIELECTRIC_F0 = 0.04;

float fresnel_schlick(float cos_theta, float f0) {
    // The 5th power is the whole effect: reflectance sits near 4% face-on
    // and climbs toward 1 at grazing incidence. That climb is why a still
    // pond mirrors the far shore but shows you the bottom at your feet.
    float m = clamp(1.0 - cos_theta, 0.0, 1.0);
    float m2 = m * m;
    return f0 + (1.0 - f0) * (m2 * m2 * m);
}

// Reflectance at normal incidence implied by an index of refraction.
// ior <= 0 means "not authored as transmissive", which is the overwhelming
// majority of materials, and those take the dielectric default above.
float f0_from_ior(float ior) {
    if (ior <= 0.0) {
        return DEFAULT_DIELECTRIC_F0;
    }
    float r = (1.0 - ior) / (1.0 + ior);
    return r * r;
}

// M5 (Finishes) -- "Wax"/"Shiny Wax"/"Rough Wax": a real wax or polished
// surface shows two highlights at once -- a broad one from its own bulk
// roughness, and a small tight one from a thin, much smoother top coat.
// There is no true two-layer BSDF here; the cheap, honest approximation is
// a SECOND specular_ggx() evaluation at a fixed, small roughness, blended
// in at a fixed, modest weight on top of the ordinary roughness-driven
// lobe below -- see the per-light accumulation sites (search
// "waxy_strength") for where this is actually added in. Fixed rather than
// authored per-material: a bool "waxy" flag (VulkanRaymarchShader::
// set_material()'s own parameter) is all ten Finishes need, so there is no
// per-material strength/roughness to read here. WEIGHT is deliberately
// < 1 so a waxy material still reads primarily as its own base roughness,
// with the sheen as an accent, not a second dominant highlight.
const float WAXY_LOBE_ROUGHNESS = 0.08;
const float WAXY_LOBE_WEIGHT = 0.5;

// Cook-Torrance GGX, Smith height-correlated visibility folded in.
// Returns the specular response for one light, WITHOUT the light colour or
// attenuation, and without any albedo -- specular does not tint by the
// diffuse texture.
float specular_ggx(vec3 normal, vec3 view_dir, vec3 light_dir,
                   float roughness, float f0) {
    vec3 h = normalize(light_dir + view_dir);
    float n_dot_l = max(dot(normal, light_dir), 0.0);
    float n_dot_v = max(dot(normal, view_dir), 0.0);
    if (n_dot_l <= 0.0 || n_dot_v <= 0.0) {
        return 0.0;
    }
    float n_dot_h = max(dot(normal, h), 0.0);
    float v_dot_h = max(dot(view_dir, h), 0.0);

    // Clamped away from a perfect mirror: at alpha == 0 the distribution
    // becomes a delta function that a single point sample either misses
    // entirely or resolves as one blown-out pixel, which reads as fireflies
    // rather than as a sharp highlight.
    float alpha = max(roughness * roughness, 0.002);
    float alpha2 = alpha * alpha;

    float denom = n_dot_h * n_dot_h * (alpha2 - 1.0) + 1.0;
    float d = alpha2 / max(3.14159265 * denom * denom, 1e-7);

    // Smith height-correlated visibility (Heitz), which already carries
    // the 1/(4 NoL NoV) of the microfacet denominator.
    float lambda_v = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - alpha2) + alpha2);
    float lambda_l = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - alpha2) + alpha2);
    float vis = 0.5 / max(lambda_v + lambda_l, 1e-7);

    float f = fresnel_schlick(v_dot_h, f0);
    return d * vis * f * n_dot_l;
}

// Transmissive-glass rendering (transmissive_range()/transmissive_sdf()/
// transmissive_normal()/transmissive_march()/transmissive_interior_
// distance()), the shade_secondary_hit()/trace_secondary() helpers it alone
// used, and accumulate_volumetrics() were all removed here (M0 CSG
// deletion pass, see /home/h-i/.claude/plans/joyful-skipping-giraffe.md):
// every one of them evaluated primitive_sdf()/scene_map()/primitives[]
// directly against the deleted CSG primitive list (Builtin.SdfSceneCommon.
// inc.glsl's scene graph), with no other data source. The ScenePrimitive
// Transmission buffer declaration above is left in place, unused, for the
// same reason the rest of this pass's otherwise-primitive-keyed SSBOs
// (ScenePrimitiveColours, ScenePrimitiveMaterialScalars, etc.) are: a
// structurally-intact-but-currently-unreachable GPU pipeline object, not a
// hard dependency on anything just removed.

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

// M7 polish (live brush-cursor ghost preview): shades one brush-preview
// hit through the SAME lighting building blocks main()'s own per-pixel
// block below uses for a real surface -- specular_ggx()/fresnel_schlick(),
// sample_probe_grid()/
// sample_gi_cascade(), calc_contact_ao(), shadow_march()/chunked_shadow_
// march() -- so the ghost is a truthful preview of what committing the
// stamp will actually look like, not a cheaper stand-in. Reads `slot`'s
// material straight out of scene_diffuse_colours/material_scalars/
// transmission -- the exact same per-slot arrays every real sculpt
// material uses (VulkanRaymarchShader::set_material()'s packing), so a
// Finish change the user just made shows up in the preview with no
// separate sync path.
//
// Deliberately narrower than main()'s own block in two documented ways:
//   - No triplanar diffuse texture/bump sampling. Every sculpt material's
//     scene_textures[slot] is already bound to a spatially uniform
//     flat_texture() (see set_material()'s own comment) for exactly the
//     slots this preview can ever target, so tex_colour would always
//     resolve to (1,1,1) and bump to zero anyway -- skipping both costs
//     nothing visible and several texture fetches per preview pixel.
//   - Always takes the marched shadow_march()/chunked_shadow_march()
//     branch, never main()'s ISM lookup path. ISM exists to make MANY
//     shadowed local lights cheap simultaneously; a preview is one extra
//     shaded pixel per frame regardless of light count, so the ordinary
//     marched path is already cheap enough and this avoids duplicating
//     ISM_COUNT/sample_ism() bookkeeping for a second call site.
vec3 shade_brush_preview(vec3 p, vec3 normal, vec3 view_dir, int slot) {
    float scalar_x = material_scalars[slot].x;
    float exempt_flag = scalar_x > 0.75 ? 1.0 : 0.0;
    float metallic = clamp((scalar_x - exempt_flag) * 2.0, 0.0, 1.0);
    float roughness = clamp(material_scalars[slot].w, 0.0, 1.0);
    float waxy_strength = clamp(transmission[slot].x, 0.0, 1.0);

    vec3 tint = scene_diffuse_colours[slot].rgb;

    vec3 indirect = (push.flags & RENDER_FLAG_CHUNKED_FIELD) != 0
        ? sample_gi_cascade(p)
        : sample_probe_grid(p);
    float ao = calc_contact_ao(p, normal);
    float occlusion = mix(INDIRECT_AO_FLOOR, 1.0, ao);
    vec3 ambient_indirect = indirect * occlusion +
        vec3(push.ambient) * mix(AMBIENT_AO_FLOOR, 1.0, ao);
    vec3 lighting = ambient_indirect;
    vec3 specular = vec3(0.0);
    float f0 = mix(DEFAULT_DIELECTRIC_F0, 1.0, metallic);

    for (int i = 0; i < push.light_count; ++i) {
        Light light = lights[i];
        int light_type = int(light.vector_type.w);
        vec3 light_colour = light.colour_intensity.rgb;
        float intensity = light.colour_intensity.a;
        // A switched-off light (e.g. a released glow light, kept in the
        // list for reuse) adds exactly nothing -- skip it before its
        // shadow ray, which is most of a light's cost.
        if (intensity <= 0.0) {
            continue;
        }

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
            vec3 shadow_origin = p + normal * SHADOW_NORMAL_BIAS;
            float shadow = (push.flags & RENDER_FLAG_CHUNKED_FIELD) != 0
                ? chunked_shadow_march(shadow_origin, light_dir,
                                      push.camera_position.xyz, shadow_max_dist,
                                      SHADOW_SOFTNESS, SHADOW_MAX_STEPS,
                                      int(light.source_primitive.x))
                : shadow_march(shadow_origin, light_dir, shadow_max_dist,
                               SHADOW_SOFTNESS, SHADOW_MAX_STEPS,
                               int(light.source_primitive.x));
            diffuse *= shadow;
            specular += light_colour * attenuation * shadow *
                        specular_ggx(normal, view_dir, light_dir, roughness, f0);
            if (waxy_strength > 0.0) {
                specular += light_colour * attenuation * shadow *
                            waxy_strength * WAXY_LOBE_WEIGHT *
                            specular_ggx(normal, view_dir, light_dir,
                                         WAXY_LOBE_ROUGHNESS, f0);
            }
        }
        lighting += light_colour * diffuse * attenuation;
    }

    if ((push.flags & RENDER_FLAG_SKYBOX) != 0) {
        float n_dot_v = max(dot(normal, view_dir), 0.0);
        float env_fresnel = fresnel_schlick(n_dot_v, f0);
        float env_strength = env_fresnel * (1.0 - roughness) * ao;
        specular += sample_skybox(reflect(-view_dir, normal)) * env_strength;
    }

    vec3 albedo = tint;
    const float PREVIEW_METAL_AMBIENT_SCALE = 0.6; // matches main()'s own
                                                   // METAL_AMBIENT_SCALE.
    vec3 metal_ambient = albedo * ambient_indirect * metallic *
                          PREVIEW_METAL_AMBIENT_SCALE;

    return albedo * lighting * (1.0 - metallic) +
           specular * mix(vec3(1.0), albedo, metallic) +
           metal_ambient;
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
    float metallic = 0.0;

    // Per-voxel material and spray paint at the hit (see
    // sample_chunk_material_at_point()): a surface built from several
    // stamps shows each one's own material, not just its brick's.
    float paint_opacity = 0.0;
    if (travelled < MAX_DIST) {
        int voxel_material;
        int voxel_owner;
        sample_chunk_material_at_point(p, voxel_material, paint_opacity,
                                       voxel_owner);
        if (voxel_material >= 0) {
            hit_material = voxel_material;
        }
    }

    if (travelled < MAX_DIST) {
        // The normal comes from the G-buffer, not from six more field
        // lookups here: for a splat it is the normal baked into the point
        // itself, for a marched hit it is the gradient the march already
        // computed.
        vec3 normal = gbuffer_normal;

        // The per-pixel analytic material re-derivation that used to run
        // here (scene_map() against the CSG primitive list, replacing the
        // baked per-brick guess raymarch() returned) was removed along with
        // that scene graph (M0 CSG deletion pass, see /home/h-i/.claude/
        // plans/joyful-skipping-giraffe.md) -- hit_material is now exactly
        // what the G-buffer carried, unrefined.
        // x packs the exempt flag and half the metallic -- see
        // ScenePrimitiveMaterialScalars.
        float scalar_x = material_scalars[hit_material].x;
        exempt_flag = scalar_x > 0.75 ? 1.0 : 0.0;
        metallic = clamp((scalar_x - exempt_flag) * 2.0, 0.0, 1.0);

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

        // M3c (Spraypaint): blend in whatever opacity has accumulated at
        // this exact hit point -- see sample_chunk_material_at_point()'s own
        // comment for the chunk-table/brick resolve this does, and Builtin.
        // SculptSpraypaint.comp.glsl for how that opacity got there. mix(),
        // not an overwrite: 0 opacity (every surface that's never been
        // sprayed -- i.e. almost all of them) leaves tint completely
        // unchanged, and partial opacity genuinely blends toward the paint
        // colour rather than hard-cutting between "painted"/"not" -- which
        // is what lets repeated passes read as the colour visibly building
        // up instead of only ever being fully on or fully off.
        tint = mix(tint, scene_diffuse_colours[PAINT_MATERIAL_SLOT].rgb,
                  paint_opacity);

        // This primitive's authored bump magnitude (see material_scalars
        // above). A material with no bump map reads flat_texture() and
        // comes out to zero perturbation whatever this is; a strength of
        // 0 does the same for one that HAS a bump map.
        float bump_strength = BUMP_STRENGTH_BASE * material_scalars[hit_material].y;

        float h_centre, h_u, h_v;
        sample_scene_heights(hit_material, uv_yz, h_centre, h_u, h_v);
        vec3 bump_x = bump_from_heights(h_centre, h_u, h_v, bump_strength);
        sample_scene_heights(hit_material, uv_xz, h_centre, h_u, h_v);
        vec3 bump_y = bump_from_heights(h_centre, h_u, h_v, bump_strength);
        sample_scene_heights(hit_material, uv_xy, h_centre, h_u, h_v);
        vec3 bump_z = bump_from_heights(h_centre, h_u, h_v, bump_strength);

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
        // Captured separately from `lighting` below (which the per-light
        // loop goes on to add direct Lambertian diffuse into) because the
        // metal-ambient-floor fix further down (search "METAL_AMBIENT_
        // SCALE") needs exactly this piece -- indirect GI + the flat
        // ambient floor, NOT the direct n.L diffuse term, which is not a
        // metal behaviour at all and must stay excluded from it.
        vec3 ambient_indirect = indirect * occlusion +
            vec3(push.ambient) * mix(AMBIENT_AO_FLOOR, 1.0, ao);
        vec3 lighting = ambient_indirect;

        // Specular is accumulated SEPARATELY from `lighting` because the
        // two are combined differently: diffuse is modulated by the
        // surface albedo, specular is not (it is light reflected off the
        // dielectric coat, not scattered out of the substrate). Folding it
        // into `lighting` would tint every highlight by the diffuse
        // texture, which is what makes a plastic read as metal.
        vec3 specular = vec3(0.0);
        vec3 view_dir = -ray_dir;
        float roughness = clamp(material_scalars[hit_material].w, 0.0, 1.0);
        // A metal reflects nearly everything at every angle (Schlick with
        // f0 -> 1), and tints what it reflects by its own colour -- see
        // the final combine below.
        float f0 = mix(DEFAULT_DIELECTRIC_F0, 1.0, metallic);
        // M5 (Finishes): the "waxy" second-specular-lobe flag -- see
        // transmission_buffer_'s own header comment (vulkan_raymarch_
        // shader.h) for why this component is free to repurpose for an
        // opaque material (the glass/transmission block that used to read
        // it was removed in the M0 CSG deletion pass, so nothing else in
        // this shader touches transmission[] at all). 0.0 for every
        // material this flag was never set on, which is every material
        // that existed before this milestone -- so a non-waxy stamp's
        // shading is bit-for-bit what it always was (see the accumulation
        // sites below, all gated by `> 0.0`).
        float waxy_strength = clamp(transmission[hit_material].x, 0.0, 1.0);

        for (int i = 0; i < push.light_count; ++i) {
            Light light = lights[i];
            int light_type = int(light.vector_type.w);
            vec3 light_colour = light.colour_intensity.rgb;
            float intensity = light.colour_intensity.a;
            if (intensity <= 0.0) {
                continue; // switched off -- see the loop above
            }

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
                    // This branch `continue`s, so its specular has to be
                    // added here rather than after the loop body -- easy to
                    // miss, and the symptom would be highlights that vanish
                    // for exactly the lights that have shadow maps.
                    specular += light_colour * attenuation * ism_shadow *
                                specular_ggx(normal, view_dir, light_dir,
                                             roughness, f0);
                    // M5 (Finishes): the waxy top-coat sheen -- see
                    // WAXY_LOBE_ROUGHNESS/WAXY_LOBE_WEIGHT's own comment.
                    // Same shadow/attenuation gating as the base lobe just
                    // above; only the roughness (fixed, low) differs.
                    if (waxy_strength > 0.0) {
                        specular += light_colour * attenuation * ism_shadow *
                                    waxy_strength * WAXY_LOBE_WEIGHT *
                                    specular_ggx(normal, view_dir, light_dir,
                                                 WAXY_LOBE_ROUGHNESS, f0);
                    }
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
                // The same shadow term gates the highlight: a surface in
                // shadow has no light reaching it to reflect specularly
                // either.
                specular += light_colour * attenuation * shadow *
                            specular_ggx(normal, view_dir, light_dir,
                                         roughness, f0);
                // M5 (Finishes): the waxy top-coat sheen -- see this
                // branch's ISM counterpart above for the full comment.
                if (waxy_strength > 0.0) {
                    specular += light_colour * attenuation * shadow *
                                waxy_strength * WAXY_LOBE_WEIGHT *
                                specular_ggx(normal, view_dir, light_dir,
                                             WAXY_LOBE_ROUGHNESS, f0);
                }
            }
            lighting += light_colour * diffuse * attenuation;
        }

        // AMBIENT SPECULAR -- the environment reflected off the coat.
        //
        // Without this a dielectric only ever highlights where a punctual
        // light happens to line up, so a smooth plastic in a lit room reads
        // as flat matte everywhere else. Reflecting the skybox at a
        // Fresnel-weighted strength is the cheap version of the right
        // answer, and it is what makes the grazing edges of a surface come
        // alive.
        //
        // Occluded by the same AO term the indirect diffuse uses: a
        // surface deep in a crease cannot see the sky, and letting it
        // reflect the sky anyway is what makes interiors glow.
        if ((push.flags & RENDER_FLAG_SKYBOX) != 0) {
            float n_dot_v = max(dot(normal, view_dir), 0.0);
            float env_fresnel = fresnel_schlick(n_dot_v, f0);
            // Rough surfaces do not mirror, they scatter -- and there is no
            // prefiltered environment here to sample a blurred version
            // from, so the honest approximation is to fade the reflection
            // out as roughness rises rather than to reflect a sharp sky off
            // a matte surface.
            float env_strength = env_fresnel * (1.0 - roughness) * ao;
            specular += sample_skybox(reflect(ray_dir, normal)) * env_strength;
        }

        // Metals have no diffuse substrate; their reflection carries their
        // colour instead. A dielectric (metallic 0) is exactly the old
        // albedo * lighting + white specular.
        vec3 albedo = tex_colour * tint;

        // M5 (Finishes) fix -- "Rough Metal near-black": a metal's only
        // brightness sources are its per-light GGX lobe and the env-
        // reflection term just above, and BOTH collapse at high roughness
        // in a single-key-light scene -- the direct lobe because a point/
        // directional light's energy is a delta function spread over a wide
        // cone (dim everywhere instead of bright in one place), and env_
        // strength because it is deliberately faded by (1 - roughness) (a
        // rough surface should not mirror a sharp, undistorted skybox).
        // That second fade is correct for a focused MIRROR reflection, but
        // wrong for what it leaves behind: real rough metal still visibly
        // picks up a soft, unfocused tint of its surroundings (brushed
        // steel in a room is not black), which this engine has no
        // prefiltered/blurred environment map to render properly. The
        // cheap, honest stand-in is this: give metallic surfaces a second,
        // roughness-INDEPENDENT ambient term, built from the exact same
        // indirect-GI + flat-ambient floor a dielectric's diffuse already
        // gets (`ambient_indirect`, captured before the per-light loop
        // added direct diffuse into `lighting`), tinted by the metal's own
        // albedo (so a gold Rough Metal still reads gold, never grey) and
        // scaled down by METAL_AMBIENT_SCALE so a ROUGH metal gains the
        // most relative brightness from it (its other two terms are nearly
        // zero) while a SHINY metal -- already bright from its tight direct
        // + env highlights -- is barely affected proportionally. Zero for
        // every non-metallic material (scaled by `metallic`), so plastic/
        // wax finishes are bit-for-bit unaffected.
        const float METAL_AMBIENT_SCALE = 0.6;
        vec3 metal_ambient = albedo * ambient_indirect * metallic *
                              METAL_AMBIENT_SCALE;

        colour = albedo * lighting * (1.0 - metallic) +
                 specular * mix(vec3(1.0), albedo, metallic) +
                 metal_ambient;
        // Glow: blend toward the material's own colour, lit from within,
        // so it reads as glowing IN that colour rather than washing out to
        // white on top of the lighting.
        float glow = clamp(material_extra[hit_material].x, 0.0, 1.0);
        colour = mix(colour, albedo * GLOW_STRENGTH, glow);

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
        // The emissive add (primitives[hit_material].expr_scale.yzw) and
        // the selection outline (push.selected_primitive_index) that used
        // to live here were both CSG-primitive-specific and removed along
        // with the rest of the scene graph (M0 CSG deletion pass).
    }

    // Glow halo: a glowing object's light (a Point light with a source
    // material -- see VulkanRaymarchShader::set_point_light()) scatters a
    // little into the air, so the space around the object glows too. This
    // is the closed-form integral of its inverse-square falloff along the
    // view ray, from the camera to whatever the ray hits (so geometry in
    // front hides it): I/h * (atan((t1 - t0)/h) - atan(-t0/h)), with t0 the
    // ray distance closest to the light and h the ray's distance from it.
    for (int i = 0; i < push.light_count; ++i) {
        Light light = lights[i];
        if (int(light.vector_type.w) != 1 || light.source_primitive.x < 0.0 ||
            light.colour_intensity.a <= 0.0) {
            continue;
        }
        vec3 to_light = light.vector_type.xyz - ray_origin;
        float t0 = dot(to_light, ray_dir);
        float h = max(length(to_light - ray_dir * t0), 0.05);
        float t1 = travelled;
        float scatter = (atan((t1 - t0) / h) - atan(-t0 / h)) / h;
        // The light's intensity encodes its object's size (see the tool's
        // sync_glow_lights()), so the halo's reach scales with it too.
        float reach = sqrt(light.colour_intensity.a);
        float halo = light.colour_intensity.a * scatter * GLOW_HALO_STRENGTH *
                     exp(-h / (GLOW_HALO_REACH * reach));
        // Blended toward the glow's colour rather than added on, so a red
        // glow stays red over a bright sky instead of bleaching to white.
        colour = mix(colour, light.colour_intensity.rgb * GLOW_STRENGTH,
                     1.0 - exp(-halo));
    }

    // Scope-in mask (unjittered pixel-centre ray, like the outline below,
    // since it's applied after TAA). Kept as scope_here: the brush preview
    // below is only hidden by surfaces in scope.
    float scope_here = 1.0;
    {
        float mask = 1.0;
        if (outline_counts.x > 0) {
            vec2 mask_uv = (vec2(pixel_coord) + 0.5 - 0.5 * vec2(image_size)) /
                           float(image_size.y);
            vec3 mask_dir = normalize(mask_uv.x * push.camera_right.xyz +
                                      mask_uv.y * push.camera_up.xyz +
                                      push.camera_forward.xyz);
            mask = scope_mask(ray_origin, mask_dir, vec2(image_size), depth);
            // Where the surface records its object, focus is exactly "is it
            // one of the scoped-in objects" -- nothing touching or near them
            // comes into focus with them, and their loose flecks do.
            if (depth >= 0.0 && outline_counts.y >= 0) {
                int slot_unused;
                float opacity_unused;
                int owner;
                sample_chunk_material_at_point(ray_origin + mask_dir * depth,
                                               slot_unused, opacity_unused, owner);
                if (owner >= 0) {
                    mask = owner_in_scope(owner) ? 1.0 : 0.0;
                }
            }
        }
        imageStore(scope_mask_image, pixel_coord, vec4(mask));
        scope_here = mask;
    }

    // M7 polish: live brush-cursor "ghost" preview -- sphere-traces ONE
    // analytic shape (brush_preview, set = 1 binding = 1 above) against
    // THIS pixel's exact same ray_origin/ray_dir, shades any hit through
    // shade_brush_preview() (the same lighting building blocks the real-
    // surface block just above used), and writes it to preview_overlay_
    // image as a solid shape -- Dreams' own "about to place this"
    // cue, not a committed edit. Runs unconditionally (not just when
    // travelled < MAX_DIST above) so the preview still shows over open
    // background/sky, which is the common case when sculpting into empty
    // space.
    //
    // Deliberately NOT blended into `colour` here any more -- see preview_
    // overlay_image's own declaration comment above for why (TAA ghosting).
    // preview_overlay_colour/alpha default to a fully transparent "no
    // preview this pixel" and are only overwritten below on an actual hit;
    // the unconditional imageStore after this block is what guarantees a
    // disabled/missed preview still clears any ghost a previous frame left
    // in this image.
    vec3 preview_overlay_colour = vec3(0.0);
    float preview_overlay_alpha = 0.0;
    if (brush_preview.center_enabled.w > 0.5) {
        // Cheap early-out already happened above (the whole block is
        // skipped once center_enabled.w is 0) -- this is the ONLY cost a
        // hidden preview adds to every other pixel in the frame: one
        // float compare.
        vec3 preview_center = brush_preview.center_enabled.xyz;
        int preview_shape = brush_preview.shape_op_slot.x;
        int preview_op = brush_preview.shape_op_slot.y;
        int preview_slot = brush_preview.shape_op_slot.z;
        vec4 preview_params = brush_preview.params;
        // The shape is traced in its own frame (rotated about its centre).
        vec4 preview_rotation = brush_preview.rotation;
        vec3 preview_origin = quat_rotate_inverse(preview_rotation,
                                                  ray_origin - preview_center);
        vec3 preview_dir = quat_rotate_inverse(preview_rotation, ray_dir);

        // March bound: must cover the distance from THIS pixel's ray
        // origin (the camera) to the shape, not just the shape's own
        // size -- a previous version used only `preview_extent * 2 + 1`
        // (the shape's own diameter plus a flat margin), which is enough
        // to cross the shape once the march is already near it but nowhere
        // near enough to ever REACH it from a camera sitting several units
        // away, so the loop always exhausted its bound before the first
        // ray ever touched the sphere -- the preview never rendered a
        // single hit, on any pixel, regardless of brush size (confirmed by
        // direct testing: a full-screen debug fill placed right at the top
        // of this block was visible, proving the enabled flag/UBO read
        // were fine, while the real per-pixel trace below it never once
        // fired). distance(ray_origin, preview_center) covers the gap to
        // the shape; the rest is this block's own former bound (enough to
        // cross the shape itself once reached).
        float preview_bound = distance(ray_origin, preview_center) +
            max(max(preview_params.x, preview_params.y),
               max(preview_params.z, preview_params.w)) * 2.0 + 1.0;

        float preview_t = 0.0;
        bool preview_hit = false;
        // Closest the ray came to the shape, in pixels (the wireframe's
        // silhouette line).
        float preview_closest_px = 1e10;
        const int PREVIEW_MAX_STEPS = 64; // a handful of steps is enough
                                          // for one analytic shape -- see
                                          // this block's own header comment.
        for (int i = 0; i < PREVIEW_MAX_STEPS; ++i) {
            if (preview_t >= preview_bound) {
                break;
            }
            vec3 sample_p = preview_origin + preview_dir * preview_t;
            float d = eval_brush_preview_sdf(sample_p, preview_shape,
                                             preview_params, vec3(0.0));
            if (d < SURF_DIST) {
                preview_hit = true;
                break;
            }
            if (preview_t > 0.01) {
                preview_closest_px = min(preview_closest_px,
                                         d / preview_t * float(image_size.y));
            }
            preview_t += d;
        }

        // Hidden behind what's already there: the scene surface at this
        // pixel is nearer than the preview's (when scoped in -- sculpt
        // mode's sculpture included -- only a surface in scope hides it,
        // the rest being greyed out anyway). The slack covers the baked
        // field sitting a voxel or so off the analytic shape, so a shape
        // just stamped doesn't flicker through the preview at the same
        // spot.
        // (A carving brush is drawn as a see-through wireframe below
        // instead, so a hollow can be carved inside a shape.)
        const float PREVIEW_DEPTH_SLACK = 0.02;
        bool preview_carving = preview_op == PREVIEW_OP_SUBTRACTION ||
                               preview_op == PREVIEW_OP_OBJECT_SUBTRACTION;
        if (preview_hit && !preview_carving && depth >= 0.0 && scope_here > 0.5 &&
            depth < preview_t - (PREVIEW_DEPTH_SLACK + 0.002 * preview_t)) {
            preview_hit = false;
        }

        // Smoothness: trace the brush blended into the scene instead, so
        // the fillet it would make shows before the stamp lands. Only what
        // stands in front of the existing surface is drawn (the brush and
        // the fillet), with the same slack as above.
        float preview_k = intBitsToFloat(brush_preview.shape_op_slot.w);
        bool preview_blended = false;
        vec3 preview_blend_normal = vec3(0.0);
        if ((preview_op == PREVIEW_OP_UNION || preview_op == PREVIEW_OP_OBJECT_UNION) &&
            preview_k > 0.0001 && !(preview_k > 1e6)) {
            float extent = max(max(preview_params.x, preview_params.y),
                               max(preview_params.z, preview_params.w));
            float reach = extent * 1.75 + preview_k;
            vec3 oc = preview_center - ray_origin;
            float tc = dot(oc, ray_dir);
            float miss2 = dot(oc, oc) - tc * tc;
            bool found = false;
            float t = 0.0;
            if (miss2 < reach * reach) {
                float half_chord = sqrt(reach * reach - miss2);
                t = max(tc - half_chord, 0.0);
                float t_end = tc + half_chord;
                if (depth >= 0.0) {
                    t_end = min(t_end, depth + 0.05);
                }
                for (int i = 0; i < 96 && t < t_end; ++i) {
                    float f = preview_blend_sdf(ray_origin + ray_dir * t, preview_k);
                    if (f < max(0.003, 0.001 * t)) { // the field is a voxel grid
                        found = true;
                        break;
                    }
                    t += max(f, 0.002);
                }
            }
            preview_hit = found &&
                (depth < 0.0 || scope_here <= 0.5 ||
                 t < depth - (PREVIEW_DEPTH_SLACK + 0.002 * t));
            if (preview_hit) {
                preview_t = t;
                preview_blended = true;
                vec3 hp = ray_origin + ray_dir * t;
                const float E = 0.01;
                preview_blend_normal = normalize(vec3(
                    preview_blend_sdf(hp + vec3(E, 0.0, 0.0), preview_k) -
                    preview_blend_sdf(hp - vec3(E, 0.0, 0.0), preview_k),
                    preview_blend_sdf(hp + vec3(0.0, E, 0.0), preview_k) -
                    preview_blend_sdf(hp - vec3(0.0, E, 0.0), preview_k),
                    preview_blend_sdf(hp + vec3(0.0, 0.0, E), preview_k) -
                    preview_blend_sdf(hp - vec3(0.0, 0.0, E), preview_k)));
            }
        }

        // Carving: only a wireframe of the shape -- its silhouette, and a
        // lattice of lines across it (half-size steps of its extents in its
        // own frame, so a box's edges are lines), front and back, seen
        // through everything.
        // Carving, live: where the brush reaches the surface this pixel
        // shows, trace the scene with the brush carved out instead -- the
        // hollow's walls in the sculpture's own material, or what's behind
        // where it cuts right through.
        vec3 carve_colour = vec3(0.0);
        float carve_alpha = 0.0;
        if (preview_carving && depth >= 0.0) {
            float k = max(intBitsToFloat(brush_preview.shape_op_slot.w), 0.0);
            if (!(k < 1e6)) {
                k = 0.0;
            }
            float extent = max(max(preview_params.x, preview_params.y),
                               max(preview_params.z, preview_params.w));
            float reach = extent * 1.75 + k;
            vec3 oc = preview_center - ray_origin;
            float tc = dot(oc, ray_dir);
            float miss2 = dot(oc, oc) - tc * tc;
            if (miss2 < reach * reach) {
                float half_chord = sqrt(reach * reach - miss2);
                float t0 = max(tc - half_chord, 0.0);
                float t1 = tc + half_chord;
                if (depth >= t0 && depth <= t1) {
                    float t = t0;
                    bool found = false;
                    bool inside = false; // t0 is in front of the first surface
                    float t_max = t1 + MAX_DIST;
                    for (int i = 0; i < 192 && t < t_max; ++i) {
                        float f = preview_carve_sdf(ray_origin + ray_dir * t, k, inside);
                        if (f < max(0.003, 0.001 * t)) {
                            found = true;
                            break;
                        }
                        // Short steps through the brush, so the ray can't
                        // jump over the band of bricks at a surface (and
                        // lose track of being inside).
                        t += t < t1 ? clamp(f, 0.002, 0.08) : max(f, 0.002);
                    }
                    if (!found) {
                        carve_colour = (push.flags & RENDER_FLAG_SKYBOX) != 0
                            ? sample_skybox(ray_dir)
                            : mix(vec3(0.02, 0.02, 0.05), vec3(0.05, 0.05, 0.12),
                                  uv.y + 0.5);
                        carve_alpha = 1.0;
                    } else if (abs(t - depth) > PREVIEW_DEPTH_SLACK + 0.002 * t) {
                        vec3 hp = ray_origin + ray_dir * t;
                        const float E = 0.01;
                        bool i0 = inside, i1 = inside, i2 = inside,
                             i3 = inside, i4 = inside, i5 = inside;
                        vec3 n = normalize(vec3(
                            preview_carve_sdf(hp + vec3(E, 0.0, 0.0), k, i0) -
                            preview_carve_sdf(hp - vec3(E, 0.0, 0.0), k, i1),
                            preview_carve_sdf(hp + vec3(0.0, E, 0.0), k, i2) -
                            preview_carve_sdf(hp - vec3(0.0, E, 0.0), k, i3),
                            preview_carve_sdf(hp + vec3(0.0, 0.0, E), k, i4) -
                            preview_carve_sdf(hp - vec3(0.0, 0.0, E), k, i5)));
                        float dist, skip_dist;
                        int material;
                        sample_active_field(hp, ray_dir, dist, skip_dist, material);
                        int slot;
                        float opacity_unused;
                        int owner_unused;
                        sample_chunk_material_at_point(hp, slot, opacity_unused, owner_unused);
                        if (slot < 0) {
                            slot = clamp(material, 0, MAX_SCENE_PRIMITIVES - 1);
                        }
                        carve_colour = shade_brush_preview(hp, n, -ray_dir, slot);
                        carve_alpha = 1.0;
                    }
                }
            }
        }

        if (preview_carving) {
            const float WIRE_PX = 1.25;
            float px_per_unit = float(image_size.y);
            vec3 wire_e = (preview_params.y == 0.0 && preview_params.z == 0.0)
                              ? vec3(abs(preview_params.x))
                              : max(abs(preview_params.xyz), vec3(1e-3));
            float wire = 0.0;
            if (!preview_hit) {
                wire = clamp(1.0 - (preview_closest_px - WIRE_PX) / WIRE_PX, 0.0, 1.0);
            } else {
                float t_front = preview_t;
                // The far side: step through the inside to where it leaves.
                float t_back = t_front + 0.002;
                bool back = false;
                for (int i = 0; i < 48; ++i) {
                    float d = eval_brush_preview_sdf(preview_origin + preview_dir * t_back,
                                                     preview_shape, preview_params, vec3(0.0));
                    if (d >= 0.0) {
                        back = true;
                        break;
                    }
                    t_back += max(-d, 0.002);
                }
                for (int side = 0; side < 2; ++side) {
                    if (side == 1 && !back) {
                        break;
                    }
                    float t = side == 0 ? t_front : t_back;
                    vec3 q = preview_origin + preview_dir * t;
                    const float E = 0.0005;
                    vec3 n = normalize(vec3(
                        eval_brush_preview_sdf(q + vec3(E, 0, 0), preview_shape, preview_params, vec3(0.0)) -
                        eval_brush_preview_sdf(q - vec3(E, 0, 0), preview_shape, preview_params, vec3(0.0)),
                        eval_brush_preview_sdf(q + vec3(0, E, 0), preview_shape, preview_params, vec3(0.0)) -
                        eval_brush_preview_sdf(q - vec3(0, E, 0), preview_shape, preview_params, vec3(0.0)),
                        eval_brush_preview_sdf(q + vec3(0, 0, E), preview_shape, preview_params, vec3(0.0)) -
                        eval_brush_preview_sdf(q - vec3(0, 0, E), preview_shape, preview_params, vec3(0.0))));
                    float line_w = WIRE_PX * t / px_per_unit; // a line's width here
                    float on = 0.0;
                    for (int a = 0; a < 3; ++a) {
                        if (abs(n[a]) > 0.75) {
                            continue; // the face across this axis: no lines on it
                        }
                        float step_a = 0.5 * wire_e[a];
                        float x = q[a] / step_a;
                        float off = abs(x - round(x)) * step_a;
                        on = max(on, clamp(1.0 - (off - line_w) / line_w, 0.0, 1.0));
                    }
                    // The rim, where the surface turns away.
                    float facing = abs(dot(n, preview_dir));
                    on = max(on, clamp(1.0 - facing / 0.12, 0.0, 1.0));
                    wire = max(wire, on * (side == 0 ? 1.0 : 0.55));
                }
            }
            preview_hit = false; // no fill
            if (wire > 0.0 || carve_alpha > 0.0) {
                vec3 wire_colour = vec3(1.0, 0.55, 0.15);
                preview_overlay_colour = carve_alpha > 0.0
                    ? mix(carve_colour, wire_colour, wire) : wire_colour;
                preview_overlay_alpha = max(wire, carve_alpha);
            }
        }

        if (preview_hit) {
            vec3 preview_p = ray_origin + ray_dir * preview_t;
            vec3 preview_local = preview_origin + preview_dir * preview_t;
            // Central-difference gradient of the same analytic function --
            // 6 extra SDF evals, cheap for one shape (no field/brick
            // lookups involved, unlike calc_static_normal()'s equivalent
            // for a real marched hit) -- in the shape's frame, then turned
            // back into the world's.
            const float PREVIEW_NORMAL_EPS = 0.001;
            vec3 local_normal = normalize(vec3(
                eval_brush_preview_sdf(preview_local + vec3(PREVIEW_NORMAL_EPS, 0.0, 0.0),
                                       preview_shape, preview_params, vec3(0.0)) -
                eval_brush_preview_sdf(preview_local - vec3(PREVIEW_NORMAL_EPS, 0.0, 0.0),
                                       preview_shape, preview_params, vec3(0.0)),
                eval_brush_preview_sdf(preview_local + vec3(0.0, PREVIEW_NORMAL_EPS, 0.0),
                                       preview_shape, preview_params, vec3(0.0)) -
                eval_brush_preview_sdf(preview_local - vec3(0.0, PREVIEW_NORMAL_EPS, 0.0),
                                       preview_shape, preview_params, vec3(0.0)),
                eval_brush_preview_sdf(preview_local + vec3(0.0, 0.0, PREVIEW_NORMAL_EPS),
                                       preview_shape, preview_params, vec3(0.0)) -
                eval_brush_preview_sdf(preview_local - vec3(0.0, 0.0, PREVIEW_NORMAL_EPS),
                                       preview_shape, preview_params, vec3(0.0))));
            vec3 preview_normal = quat_rotate_inverse(
                vec4(-preview_rotation.xyz, preview_rotation.w), local_normal);
            if (preview_blended) {
                preview_normal = preview_blend_normal;
            }

            vec3 preview_colour = shade_brush_preview(preview_p, preview_normal,
                                                       -ray_dir, preview_slot);

            // Solid where it shows -- it reads as the shape about to be
            // placed, not a ghost; the parts behind existing geometry are
            // hidden above (with slack for the near-ties a brush resting on
            // the surface makes).
            const float PREVIEW_ALPHA = 1.0;
            float preview_alpha = PREVIEW_ALPHA;

            // A thin Union(cyan)/Subtraction(orange) rim tint ON TOP of
            // the real shaded fill -- same colours scene_viewport.cpp's
            // own wireframe preview already uses, so the two cues agree
            // rather than contradict. Strongest at grazing angles (where a
            // flat shaded fill alone reads most ambiguously against a
            // similarly-lit background), via the same (1 - n.v) Fresnel-
            // like falloff fresnel_schlick() uses elsewhere in this file.
            // Carving orange, adding cyan, cropping violet, painting none
            // (the fill already shows the paint).
            bool preview_carves = preview_op == PREVIEW_OP_SUBTRACTION ||
                                  preview_op == PREVIEW_OP_OBJECT_SUBTRACTION;
            vec3 preview_op_tint = preview_carves ? vec3(1.0, 0.55, 0.15)
                : preview_op == PREVIEW_OP_OBJECT_INTERSECTION ? vec3(0.75, 0.45, 1.0)
                : vec3(0.2, 0.95, 1.0);
            if (preview_op == PREVIEW_OP_OBJECT_PAINT) {
                preview_op_tint = preview_colour;
            }
            float preview_rim = pow(1.0 - max(dot(preview_normal, -ray_dir), 0.0), 2.0);
            preview_colour = mix(preview_colour, preview_op_tint, preview_rim * 0.35);

            preview_overlay_colour = preview_colour;
            preview_overlay_alpha = preview_alpha;
        }
    }

    // Selection outline, layered over the brush preview in the same
    // overlay (so it also stays out of TAA's history). The unjittered
    // pixel-centre ray: the outline must not shimmer with TAA jitter.
    if (outline_counts.x > 0) {
        vec2 centre_uv = (vec2(pixel_coord) + 0.5 - 0.5 * vec2(image_size)) /
                         float(image_size.y);
        vec3 centre_dir = normalize(centre_uv.x * push.camera_right.xyz +
                                    centre_uv.y * push.camera_up.xyz +
                                    push.camera_forward.xyz);
        vec4 outline = selection_outline(ray_origin, centre_dir,
                                         vec2(pixel_coord) + 0.5,
                                         vec2(image_size), depth);
        if (outline.a > 0.0) {
            float out_alpha = outline.a + preview_overlay_alpha * (1.0 - outline.a);
            preview_overlay_colour =
                (outline.rgb * outline.a +
                 preview_overlay_colour * preview_overlay_alpha * (1.0 - outline.a)) /
                out_alpha;
            preview_overlay_alpha = out_alpha;
        }
    }
    imageStore(preview_overlay_image, pixel_coord,
              vec4(preview_overlay_colour, preview_overlay_alpha));

    // The Transmission block (glass refraction/reflection against
    // transmissive CSG primitives, via transmissive_range()/
    // transmissive_march()/transmissive_normal()/transmissive_interior_
    // distance()/trace_secondary()/shade_secondary_hit()) and the
    // Volumetric-light-shaft block (accumulate_volumetrics()) that used to
    // run here were both removed (M0 CSG deletion pass, see /home/h-i/
    // .claude/plans/joyful-skipping-giraffe.md) -- see the comment above
    // specular_ggx() for the full removal list and why.

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
