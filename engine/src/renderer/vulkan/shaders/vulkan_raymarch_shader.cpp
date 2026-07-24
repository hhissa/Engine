#include "vulkan_raymarch_shader.h"
#include "../../../core/logger.h"
#include "../../../resources/expression.h"
#include "../../../systems/geometry_system.h"
#include "../../../systems/texture_system.h"
#include "../vulkan_commandbuffer.h"
#include "../vulkan_image.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/quaternion.hpp>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
constexpr std::string_view BUILTIN_SHADER_NAME_VOXELIZE =
    "Builtin.RaymarchVoxelize";
constexpr std::string_view BUILTIN_SHADER_NAME_PROBE_BAKE =
    "Builtin.ProbeBake";
constexpr std::string_view BUILTIN_SHADER_NAME_RAYMARCH =
    "Builtin.RaymarchShader";
constexpr std::string_view BUILTIN_SHADER_NAME_BLOOM_BLUR_H =
    "Builtin.BloomBlurH";
constexpr std::string_view BUILTIN_SHADER_NAME_POST_COMPOSITE =
    "Builtin.PostComposite";

// Matches the `push_constant` block in Builtin.RaymarchShader.comp.glsl.
// std430 (push constants use the same rules as std430, not std140) still
// aligns vec3 to 16 bytes, so both fields are declared as vec4 to make the
// padding explicit instead of implicit. Recorded directly into the command
// buffer each frame (vkCmdPushConstants) rather than a UBO: there's no
// separate buffer memory a still-in-flight previous frame could be reading
// while this frame's value is written, so no per-swapchain-image buffering
// is needed the way a UBO would require.
struct PushConstants {
  f32 camera_position[4]; // xyz + unused pad
  f32 camera_forward[4];  // xyz + unused pad
  f32 camera_right[4];    // xyz + unused pad
  f32 camera_up[4];       // xyz + unused pad
  i32 light_count;   // How many of light_buffer_'s entries to sum -- see
                     // rebuild_static_scene().
  f32 ambient;        // Scene-wide ambient factor -- see
                     // GeometrySystem::ambient().
  i32 selected_primitive_index; // scene_textures/scene_diffuse_colours index
                               // to outline, or -1 for none -- see
                               // VulkanRaymarchShader::set_selected_primitive().
  i32 grid_enabled; // Nonzero to draw the reference grid (the subdivided
                   // ground plane) -- see VulkanRaymarchShader::
                   // set_grid_visible().
  i32 layer_count; // How many layer_buffer_ entries the render pass's
                  // per-pixel material re-evaluation folds (see scene_map()
                  // in Builtin.SdfSceneCommon.inc.glsl) -- same value the
                  // voxelize pass baked with, set by rebuild_static_scene().
  i32 volumetric_start; // First index (into primitives[]/scene_textures/
                       // scene_diffuse_colours) of the tail range
                       // rebuild_static_scene() appended GeometrySystem's
                       // registered volumetrics at -- see
                       // accumulate_volumetrics() in Builtin.RaymarchShader.
                       // comp.glsl. Meaningless if volumetric_count is 0.
  i32 volumetric_count; // How many contiguous entries starting there are
                       // volumetrics, not opaque primitives.
  f32 time; // Seconds since this shader was constructed -- drives
           // accumulate_volumetrics()'s scrolling texture animation (a
           // static shaft would look like a fixed decal rather than a
           // drifting light shaft). Accumulated in render_to(), never reset.
  i32 skybox_enabled; // Nonzero to sample skybox_texture for the background
                     // (a primary ray miss) instead of the flat two-colour
                     // gradient -- see set_skybox()/apply_skybox() in
                     // Builtin.RaymarchShader.comp.glsl.
};
// 4 vec4s + 9 scalars = 100 bytes -- comfortably within Vulkan's guaranteed
// minimum push constant size of 128 bytes, so no device limit query is
// needed here.

// Fallback when GeometrySystem has no registered lights at all (e.g. an
// .sdf file with no "light" blocks, or nothing loaded yet) -- without this,
// a scene authored before lights existed (or one that just doesn't bother
// with them) would render pitch black instead of with the same single
// hardcoded directional light this engine always used to have. Matches
// that old hardcoded look exactly (including the diffuse term's old
// implicit *0.85 scale, now folded into intensity).
const glm::vec3 kDefaultLightDirection(0.6f, 0.7f, -0.6f);
const glm::vec3 kDefaultLightColour(1.0f, 1.0f, 1.0f);
constexpr f32 kDefaultLightIntensity = 0.85f;

// Matches Builtin.RaymarchVoxelize.comp.glsl's push constant block.
struct VoxelizePushConstants {
  i32 layer_count;
};

// Matches Builtin.ProbeBake.comp.glsl's push constant block.
struct ProbeBakePushConstants {
  i32 light_count;
  f32 ambient;
};

// Matches Builtin.BloomBlurH.comp.glsl's push constant block.
struct BloomBlurHPushConstants {
  i32 full_width;
  i32 full_height;
  f32 bloom_threshold;
};

// Matches Builtin.PostComposite.comp.glsl's push constant block.
struct PostCompositePushConstants {
  i32 full_width;
  i32 full_height;
  f32 bloom_intensity;
  f32 vignette_strength;
  f32 vignette_radius;
  i32 pixelation_enabled;
  i32 pixelation_block_size;
};

// Fixed tuning constants for the post-process chain -- not exposed as
// engine-side setters (unlike the on/off toggles in vulkan_raymarch_
// shader.h), since these are visual-tuning knobs more than behavioural
// ones; edit here if the look needs adjusting. Bloom's threshold/
// intensity are deliberately mild -- "subtle," matching what was asked
// for -- a much lower threshold or higher intensity turns this into a
// heavy glow instead.
constexpr f32 kBloomThreshold = 0.85f;
constexpr f32 kBloomIntensity = 0.35f;
constexpr f32 kVignetteStrength = 0.35f;
constexpr f32 kVignetteRadius = 0.55f;

// GI probe grid dimensions. Must match PROBE_DIM in both
// Builtin.ProbeBake.comp.glsl and Builtin.RaymarchShader.comp.glsl exactly.
// Probes sit at the corners of a (kProbeDim-1)^3 cell grid spanning the
// full [-BOUNDS, BOUNDS] cube inclusive (2-unit spacing at BOUNDS=16) --
// coarse relative to the voxel field's own 0.25-unit cells, deliberately:
// GI only needs to vary smoothly across room-scale distances, not resolve
// fine surface detail, and probe count is the dominant cost of
// bake_probes() (kProbeDim^3 probes x their own gather-sample count x
// kProbeBounceCount bounces).
constexpr u32 kProbeDim = 16;
constexpr u32 kProbeCount = kProbeDim * kProbeDim * kProbeDim;
// How many light bounces bake_probes() simulates -- Inigo Quilez's
// "simplegi" article (the technique this is adapted from) mentions using
// up to 3. Each additional bounce is a full extra kProbeCount x
// PROBE_GATHER_SAMPLES gather-ray pass, so this is the other major bake-
// cost lever alongside kProbeDim.
constexpr u32 kProbeBounceCount = 3;
// bake_probes()'s ping-pong loop starts bounce 0 reading probe_buffer_a_
// (zeroed) and writing probe_buffer_b_, then alternates -- so bounce i
// writes b_ if i is even, a_ if i is odd. After kProbeBounceCount
// dispatches (indices 0..kProbeBounceCount-1), the *last* write is at
// index kProbeBounceCount-1, which lands in b_ exactly when that index is
// even, i.e. when kProbeBounceCount is odd. Resolved once here so
// render_set_'s construction-time ProbeBuffer binding and bake_probes()'s
// runtime loop can never disagree about which buffer ends up "final".
constexpr bool kProbeFinalInBufferB = (kProbeBounceCount % 2) == 1;

// Sparse voxel field dimensions. Must match COARSE_DIM/BRICK_DIM/MAX_BRICKS
// in both Builtin.RaymarchVoxelize.comp.glsl and
// Builtin.RaymarchShader.comp.glsl exactly. kCoarseDim is scaled up from 16
// in lockstep with BOUNDS (2.0 -> 16.0, 8x, in both of those files) so
// COARSE_CELL_SIZE -- and therefore voxel resolution -- stays exactly what
// it was before: only the world volume actually covered grows, not how
// coarsely it's sampled.
constexpr u32 kCoarseDim = 128;
constexpr u32 kBrickDim = 8;
// Each brick stores a 1-voxel apron on every side (evaluated directly from
// the SDF, not copied from a neighbor) so trilinear sampling has continuous
// data to blend into right at brick boundaries — without this, adjacent
// bricks' data disagreed at the seam and produced visible facets.
constexpr u32 kBrickApronDim = kBrickDim + 2;
constexpr u32 kBrickVoxelCount = kBrickApronDim * kBrickApronDim * kBrickApronDim;
// Brick pool capacity. A brick is only allocated where the surface band
// crosses a coarse cell (a ~2-cell-thick shell around every surface, since
// half_diagonal in the voxelize shader slightly exceeds half a cell), so
// demand scales with total scene *surface area*: roughly area / 0.0625 x 2
// bricks. Reference points at the current 0.25-unit cell size: a full
// ground plane costs 32768 (2 x 128^2 cell-layers); games/SH's room.sdf
// loaded at .scale(5.0) -- a hollow shell ~19x12x25 whose inner+outer
// faces total ~4000 units^2 -- measured 140637 together with its other
// scenes. 262144 gives that class of scene ~1.9x headroom, at the price of
// a 1GB device-local pool (4KB per brick: 10^3 apron voxels x f32) --
// deliberately budgeted for the desktop GPUs this engine actually targets;
// if that ever needs shrinking, halve the voxel to f16
// (GL_EXT_shader_16bit_storage) or tighten the allocation band before
// shrinking the count. The original value, 2048, was sized for the old
// 16^3 grid and never scaled with the 8x kCoarseDim bump above; overflow
// drops cells in nondeterministic atomicAdd order and renders as random
// missing/stray chunks of surface. voxelize() reads the demand counter
// back after every bake and KWARNs whenever demand exceeds this.
constexpr u32 kMaxBricks = 262144;

// Cap on simultaneously baked static primitives. Must match
// MAX_SCENE_PRIMITIVES in Builtin.RaymarchShader.comp.glsl exactly (that's
// the one place it's compile-time-fixed, for the sampler array; everywhere
// else the count is just how many of this capacity are actually in use).
constexpr u32 kMaxScenePrimitives = 1000;

// Cap on simultaneously registered layers (see GeometrySystem::SceneLayer).
// Only bounds layer_buffer_'s allocated size -- there's no shader-side
// fixed-size array depending on this the way scene_textures depends on
// kMaxScenePrimitives, since layers are read from a plain storage buffer
// (LayerBuffer's `Layer layers[]` is unsized in both
// Builtin.RaymarchVoxelize.comp.glsl and Builtin.RaymarchShader.comp.glsl --
// raising this needs no shader recompile). Index 0 is the always-present
// default layer GeometrySystem::load_scene() never hands out (see its own
// comment), so this must exceed kMaxScenePrimitives by at least 1 for tools
// that put one primitive per layer -- e.g. the SDF editor's "Add Primitive"
// (see SdfEditorWindow::on_add_clicked()), which otherwise silently stops
// rendering anything past the 15th primitive even though
// kMaxScenePrimitives allows far more.
constexpr u32 kMaxLayers = 1001;

// Cap on simultaneously registered lights. Only bounds light_buffer_'s
// allocated size -- lights are read from a plain storage buffer (see
// GpuLight below), not a fixed-size shader array, so this could grow freely
// if a scene ever legitimately needed more.
constexpr u32 kMaxLights = 8;

// Matches the `Primitive` struct in Builtin.RaymarchVoxelize.comp.glsl.
struct GpuPrimitive {
  f32 position_type[4]; // xyz = world position, w = PrimitiveType as float.
  // xyz = Geometry::params, w = Geometry::extra_param -- per-type meaning,
  // see SdfPrimitiveDef::params' comment (sdf_scene.h) and primitive_sdf()
  // in Builtin.RaymarchVoxelize.comp.glsl for exactly what each type reads.
  f32 params[4];
  // Unit quaternion (x,y,z,w) rotating the primitive's local space into
  // world space -- computed from Geometry::rotation's Euler angles (see
  // rebuild_static_scene() below). The voxelize shader applies its inverse
  // (conjugate) to a sample point before evaluating every type but Plane
  // (always the horizontal y=height plane, which never rotates); a sphere
  // reads it too but is unaffected (rotation-invariant).
  f32 rotation[4];
  // x = Geometry::param_expr_scale (the accumulated uniform scale
  // resolve_params() applies to formula-driven slots as s*f(p/s) -- see
  // that field's comment); yzw unused padding, kept as a full vec4 so the
  // std430 layout stays a whole number of vec4s on both sides.
  f32 expr_scale[4];
};

// Matches the `Layer` struct in Builtin.RaymarchVoxelize.comp.glsl.
struct GpuLayer {
  f32 op_smoothness[4]; // x=LayerOperation as float, y=smoothness.
  i32 range[4];           // x=primitive_start, y=primitive_count.
};

// Matches the `Light` struct in Builtin.RaymarchShader.comp.glsl.
struct GpuLight {
  f32 vector_type[4]; // xyz = Light::vector (direction or position -- see
                     // LightType's comment), w = LightType as float.
  f32 colour_intensity[4]; // rgb = Light::colour, a = Light::intensity.
  // x = the index (into primitives[]/scene_textures/scene_diffuse_colours)
  // of the emissive primitive this Point light was synthesized from (see
  // rebuild_static_scene()'s emissive-primitive loop), or -1 for a light
  // with no associated primitive (every authored SdfLightDef light, plus
  // the hardcoded fallback). shadow_march() (Builtin.BakedFieldCommon.inc.
  // glsl) needs this: a synthesized light sits at its own primitive's
  // position, so a shadow ray toward it would otherwise always find that
  // same primitive's own shell in the way first (any point outside a
  // convex shape has to cross its surface to reach its interior/center) --
  // this tells the shadow march which primitive to treat as see-through
  // for that one light's rays, rather than a real occluder. yzw unused
  // padding, kept as a full vec4 so the std430 layout stays a whole number
  // of vec4s on both sides, matching every other GPU-side struct here.
  f32 source_primitive[4];
};

// A single compiled "parametric attribute" expression -- matches the
// `ParamExpr` struct in Builtin.RaymarchVoxelize.comp.glsl exactly (plain
// C arrays, not std::array, so the memory layout is the flat, tightly
// packed one std430 also produces for an all-scalar-array struct like this
// -- see rebuild_static_scene()'s load_data() call, a raw memcpy). 4
// consecutive entries per primitive (params.x/y/z/extra_param, in that
// order) in param_expr_buffer_; instruction_count == 0 means "no formula
// for this slot, use the plain constant instead" (see
// Geometry::param_expressions).
struct GpuParamExpr {
  i32 op[kMaxExprInstructions]{};
  f32 operand[kMaxExprInstructions]{};
  i32 instruction_count = 0;
};
} // namespace

VulkanRaymarchShader::VulkanRaymarchShader(VulkanContext &context)
    : context_(&context),
      voxelize_stage_(context, BUILTIN_SHADER_NAME_VOXELIZE, "comp",
                     VK_SHADER_STAGE_COMPUTE_BIT),
      probe_bake_stage_(context, BUILTIN_SHADER_NAME_PROBE_BAKE, "comp",
                       VK_SHADER_STAGE_COMPUTE_BIT),
      render_stage_(context, BUILTIN_SHADER_NAME_RAYMARCH, "comp",
                   VK_SHADER_STAGE_COMPUTE_BIT),
      bloom_blur_h_stage_(context, BUILTIN_SHADER_NAME_BLOOM_BLUR_H, "comp",
                        VK_SHADER_STAGE_COMPUTE_BIT),
      post_composite_stage_(context, BUILTIN_SHADER_NAME_POST_COMPOSITE, "comp",
                           VK_SHADER_STAGE_COMPUTE_BIT) {
  if (!voxelize_stage_.is_valid() || !probe_bake_stage_.is_valid() ||
      !render_stage_.is_valid() || !bloom_blur_h_stage_.is_valid() ||
      !post_composite_stage_.is_valid()) {
    KERROR("Unable to create shader module(s) for the raymarch field "
          "pipeline.");
    return;
  }

  // Output storage image that pass 3 (render) writes into. Only STORAGE
  // now, not TRANSFER_SRC -- the post-process passes (4a/4b) read it via
  // imageLoad, and it's post_process_image_ below that gets copied to the
  // swapchain now, not this one directly.
  vulkan_image_create(context_, VK_IMAGE_TYPE_2D, context_->framebuffer_width,
                      context_->framebuffer_height,
                      context_->swapchain.image_format.format,
                      VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_STORAGE_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, TRUE,
                      VK_IMAGE_ASPECT_COLOR_BIT, &output_image_);

  // Half-resolution scratch buffer for the bloom blur's horizontal pass
  // (pass 4a) -- see bloom_temp_image_'s header comment for why half-res.
  vulkan_image_create(context_, VK_IMAGE_TYPE_2D,
                      (context_->framebuffer_width + 1) / 2,
                      (context_->framebuffer_height + 1) / 2,
                      context_->swapchain.image_format.format,
                      VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_STORAGE_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, TRUE,
                      VK_IMAGE_ASPECT_COLOR_BIT, &bloom_temp_image_);

  // Final frame, written by pass 4b and copied to the swapchain in place
  // of output_image_ (see render_to()) -- TRANSFER_SRC for that copy, like
  // output_image_ used to be.
  vulkan_image_create(context_, VK_IMAGE_TYPE_2D, context_->framebuffer_width,
                      context_->framebuffer_height,
                      context_->swapchain.image_format.format,
                      VK_IMAGE_TILING_OPTIMAL,
                      VK_IMAGE_USAGE_STORAGE_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, TRUE,
                      VK_IMAGE_ASPECT_COLOR_BIT, &post_process_image_);

  // Sparse voxel field storage.
  const u64 indirection_size =
      static_cast<u64>(kCoarseDim) * kCoarseDim * kCoarseDim * sizeof(i32);
  const u64 brick_pool_size =
      static_cast<u64>(kMaxBricks) * kBrickVoxelCount * sizeof(f32);

  indirection_buffer_.emplace(*context_, indirection_size,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  brick_pool_buffer_.emplace(*context_, brick_pool_size,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  // Host-visible, unlike the two above: voxelize() reads the counter back
  // after every bake to detect brick-pool overflow (demand > kMaxBricks,
  // which the shader can only handle by silently dropping cells). A single
  // u32 the GPU only touches via atomicAdd -- not worth a staging-copy
  // round trip.
  brick_counter_buffer_.emplace(*context_, sizeof(u32),
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  // GeometrySystem's registered scene: primitive_buffer_/
  // primitive_colour_buffer_ are written directly from the CPU (host
  // visible) whenever rebuild_static_scene() runs, since that's cheap and
  // infrequent -- unlike indirection/brick_pool/brick_counter above, which
  // only the GPU ever writes.
  const u64 primitive_buffer_size =
      static_cast<u64>(kMaxScenePrimitives) * sizeof(GpuPrimitive);
  const u64 primitive_colour_size =
      static_cast<u64>(kMaxScenePrimitives) * sizeof(f32) * 4;
  const u64 layer_buffer_size = static_cast<u64>(kMaxLayers) * sizeof(GpuLayer);
  const u64 brick_primitive_size = static_cast<u64>(kMaxBricks) * sizeof(i32);

  primitive_buffer_.emplace(
      *context_, primitive_buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  primitive_colour_buffer_.emplace(
      *context_, primitive_colour_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  layer_buffer_.emplace(
      *context_, layer_buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  brick_primitive_buffer_.emplace(*context_, brick_primitive_size,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  const u64 light_buffer_size = static_cast<u64>(kMaxLights) * sizeof(GpuLight);
  light_buffer_.emplace(
      *context_, light_buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  // GI probe grid, double-buffered -- see bake_probes()/probe_buffer_a_'s
  // header comment for the ping-pong scheme. Device-local and
  // TRANSFER_DST: probe_buffer_a_ is zeroed via vkCmdFillBuffer at the
  // start of every bake_probes() call (the "previous bounce" input to
  // bounce 0, which must start at zero -- see the shader's file header
  // comment), the same idiom brick_counter_buffer_ already uses.
  const u64 probe_buffer_size = static_cast<u64>(kProbeCount) * sizeof(f32) * 4;
  probe_buffer_a_.emplace(*context_, probe_buffer_size,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  probe_buffer_b_.emplace(*context_, probe_buffer_size,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  // 4 entries per primitive (params.x/y/z/extra_param) -- see GpuParamExpr.
  const u64 param_expr_buffer_size =
      static_cast<u64>(kMaxScenePrimitives) * 4 * sizeof(GpuParamExpr);
  param_expr_buffer_.emplace(
      *context_, param_expr_buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  // One float (0.0/1.0) per primitive -- see Material::pixelation_exempt.
  const u64 pixelation_exempt_buffer_size =
      static_cast<u64>(kMaxScenePrimitives) * sizeof(f32);
  pixelation_exempt_buffer_.emplace(
      *context_, pixelation_exempt_buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  if (!indirection_buffer_->is_valid() || !brick_pool_buffer_->is_valid() ||
      !brick_counter_buffer_->is_valid() || !primitive_buffer_->is_valid() ||
      !primitive_colour_buffer_->is_valid() || !layer_buffer_->is_valid() ||
      !brick_primitive_buffer_->is_valid() || !light_buffer_->is_valid() ||
      !param_expr_buffer_->is_valid() || !probe_buffer_a_->is_valid() ||
      !probe_buffer_b_->is_valid() || !pixelation_exempt_buffer_->is_valid()) {
    KERROR("Failed to create sparse voxel field buffers.");
    return;
  }

  // Descriptor set layout for pass 1 (voxelize): writes indirection, brick
  // pool, and the allocation counter; reads the registered static
  // primitives, layers, and parametric-attribute expressions, and writes
  // which primitive wins at each allocated brick.
  VkDescriptorSetLayoutBinding voxelize_bindings[7]{};
  for (u32 i = 0; i < 7; ++i) {
    voxelize_bindings[i].binding = i;
    voxelize_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    voxelize_bindings[i].descriptorCount = 1;
    voxelize_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }

  VkDescriptorSetLayoutCreateInfo voxelize_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  voxelize_layout_info.bindingCount = 7;
  voxelize_layout_info.pBindings = voxelize_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &voxelize_layout_info,
                                       context_->allocator,
                                       &voxelize_set_layout_));

  // Descriptor set layout for pass 2 (render): the output image, read-only
  // access to indirection/brick_pool/brick_primitive, the static
  // primitives' diffuse tints, their textures (a small fixed-size array --
  // see scene_textures in the shader for why), and the registered lights.
  // The camera state that used to be a UBO binding here is now a push
  // constant instead (see PushConstants above) — no separate binding
  // needed, and no per-swapchain-image duplication either. Binding 3 used
  // to be an intentional gap (previously the now-removed dynamic
  // primitive's own texture) -- now filled by light_buffer_ instead of
  // staying unused.
  // 0 = out_image, 1 = indirection, 2 = brick pool, 3 = lights, 4 = brick
  // materials, 5 = primitive colours (+texture scale in .a), 6 = the
  // texture array, 7/8/9 = primitives/layers/param exprs -- the same
  // analytic-scene buffers the voxelize set binds at 3/5/6, re-bound here
  // for the render pass's per-pixel material re-evaluation (see the
  // Builtin.SdfSceneCommon.inc.glsl include in
  // Builtin.RaymarchShader.comp.glsl) -- 10 = the baked GI probe grid
  // (see ProbeBuffer in the same shader), read-only here just like every
  // other baked-field binding -- 11 = per-primitive pixelation-exempt
  // flags (see PixelationExemptBuffer in the same shader), written into
  // out_image's alpha channel for the post-process pass to read -- and
  // 12 = the optional skybox texture (see set_skybox()), a single combined
  // image sampler rather than a fixed-size array like binding 6 since
  // there's only ever one at a time.
  VkDescriptorSetLayoutBinding render_bindings[13]{};
  for (u32 i = 0; i < 13; ++i) {
    render_bindings[i].binding = i;
    render_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    render_bindings[i].descriptorCount = 1;
    render_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  render_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  render_bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  render_bindings[6].descriptorCount = kMaxScenePrimitives;
  render_bindings[12].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

  VkDescriptorSetLayoutCreateInfo render_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  render_layout_info.bindingCount = 13;
  render_layout_info.pBindings = render_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &render_layout_info, context_->allocator,
                                       &render_set_layout_));

  // Descriptor set layout for pass 2 (probe bake): read-only access to the
  // baked field (indirection/brick_pool/brick_primitive) and the static
  // scene's diffuse colours/lights (for a gather ray's hit shading), plus
  // one read and one write binding for the previous/current bounce's
  // probe grid -- see bake_probes(), which rewrites bindings 5/6 to
  // alternate probe_buffer_a_/probe_buffer_b_ before every bounce's
  // dispatch (everything else here is static, set once below).
  VkDescriptorSetLayoutBinding probe_bake_bindings[7]{};
  for (u32 i = 0; i < 7; ++i) {
    probe_bake_bindings[i].binding = i;
    probe_bake_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    probe_bake_bindings[i].descriptorCount = 1;
    probe_bake_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }

  VkDescriptorSetLayoutCreateInfo probe_bake_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  probe_bake_layout_info.bindingCount = 7;
  probe_bake_layout_info.pBindings = probe_bake_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &probe_bake_layout_info,
                                       context_->allocator,
                                       &probe_bake_set_layout_));

  // Descriptor set layout for pass 4a (bloom blur, horizontal): reads
  // output_image_, writes bloom_temp_image_ -- see Builtin.BloomBlurH.
  // comp.glsl.
  VkDescriptorSetLayoutBinding bloom_blur_h_bindings[2]{};
  bloom_blur_h_bindings[0].binding = 0;
  bloom_blur_h_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  bloom_blur_h_bindings[0].descriptorCount = 1;
  bloom_blur_h_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  bloom_blur_h_bindings[1] = bloom_blur_h_bindings[0];
  bloom_blur_h_bindings[1].binding = 1;

  VkDescriptorSetLayoutCreateInfo bloom_blur_h_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  bloom_blur_h_layout_info.bindingCount = 2;
  bloom_blur_h_layout_info.pBindings = bloom_blur_h_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &bloom_blur_h_layout_info,
                                       context_->allocator,
                                       &bloom_blur_h_set_layout_));

  // Descriptor set layout for pass 4b (post-composite): reads
  // output_image_ and bloom_temp_image_, writes post_process_image_ -- see
  // Builtin.PostComposite.comp.glsl.
  VkDescriptorSetLayoutBinding post_composite_bindings[3]{};
  for (u32 i = 0; i < 3; ++i) {
    post_composite_bindings[i].binding = i;
    post_composite_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    post_composite_bindings[i].descriptorCount = 1;
    post_composite_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }

  VkDescriptorSetLayoutCreateInfo post_composite_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  post_composite_layout_info.bindingCount = 3;
  post_composite_layout_info.pBindings = post_composite_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &post_composite_layout_info,
                                       context_->allocator,
                                       &post_composite_set_layout_));

  // One pool backing all five sets.
  VkDescriptorPoolSize pool_sizes[3]{};
  pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  pool_sizes[0].descriptorCount = 24; // voxelize's 7 + render's 10 + probe bake's 7
  pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  pool_sizes[1].descriptorCount = 6; // render's 1 + bloom blur h's 2 + post composite's 3
  pool_sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  pool_sizes[2].descriptorCount = kMaxScenePrimitives + 1; // scene_textures + skybox_texture

  VkDescriptorPoolCreateInfo pool_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.poolSizeCount = 3;
  pool_info.pPoolSizes = pool_sizes;
  pool_info.maxSets = 5;
  VK_CHECK(vkCreateDescriptorPool(context_->device.logical_device, &pool_info,
                                  context_->allocator, &descriptor_pool_));

  VkDescriptorSetLayout layouts[5] = {
      voxelize_set_layout_, render_set_layout_, probe_bake_set_layout_,
      bloom_blur_h_set_layout_, post_composite_set_layout_};
  VkDescriptorSet sets[5];
  VkDescriptorSetAllocateInfo alloc_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc_info.descriptorPool = descriptor_pool_;
  alloc_info.descriptorSetCount = 5;
  alloc_info.pSetLayouts = layouts;
  VK_CHECK(vkAllocateDescriptorSets(context_->device.logical_device,
                                    &alloc_info, sets));
  voxelize_set_ = sets[0];
  render_set_ = sets[1];
  probe_bake_set_ = sets[2];
  bloom_blur_h_set_ = sets[3];
  post_composite_set_ = sets[4];

  // Populate voxelize_set_: {indirection, brick_pool, brick_counter,
  // primitive_buffer, brick_primitive_buffer, layer_buffer, param_expr_buffer}.
  VkDescriptorBufferInfo voxelize_buffer_infos[7] = {
      {indirection_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {brick_pool_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {brick_counter_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {primitive_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {brick_primitive_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {layer_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {param_expr_buffer_->handle(), 0, VK_WHOLE_SIZE},
  };

  VkWriteDescriptorSet voxelize_writes[7]{};
  for (u32 i = 0; i < 7; ++i) {
    voxelize_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    voxelize_writes[i].dstSet = voxelize_set_;
    voxelize_writes[i].dstBinding = i;
    voxelize_writes[i].descriptorCount = 1;
    voxelize_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    voxelize_writes[i].pBufferInfo = &voxelize_buffer_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 7, voxelize_writes,
                        0, nullptr);

  // Populate render_set_'s bindings that don't depend on the registered
  // static scene: {out_image, indirection, brick_pool, light_buffer,
  // brick_primitive_buffer, primitive_colour_buffer}. Binding 6
  // (scene_textures) is written separately by rebuild_static_scene()
  // below, since it depends on whichever textures are actually registered.
  VkDescriptorImageInfo render_image_info{};
  render_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  render_image_info.imageView = output_image_.view;

  // Buffer bindings, in binding order (binding 0 is the image, 6 the
  // texture array -- both written separately below/by
  // rebuild_static_scene()): 1..5 as before, 7/8/9 = the analytic-scene
  // buffers the per-pixel material re-evaluation reads, 10 = whichever
  // probe buffer holds the final bounce's result (see
  // kProbeFinalInBufferB) -- fixed at construction, since which physical
  // buffer is "final" never changes once kProbeBounceCount is compiled in
  // -- and 11 = per-primitive pixelation-exempt flags.
  struct RenderBufferBinding {
    u32 binding;
    VkBuffer buffer;
  } render_buffer_bindings[10] = {
      {1, indirection_buffer_->handle()},
      {2, brick_pool_buffer_->handle()},
      {3, light_buffer_->handle()},
      {4, brick_primitive_buffer_->handle()},
      {5, primitive_colour_buffer_->handle()},
      {7, primitive_buffer_->handle()},
      {8, layer_buffer_->handle()},
      {9, param_expr_buffer_->handle()},
      {10, kProbeFinalInBufferB ? probe_buffer_b_->handle()
                               : probe_buffer_a_->handle()},
      {11, pixelation_exempt_buffer_->handle()},
  };

  VkDescriptorBufferInfo render_buffer_infos[10];
  VkWriteDescriptorSet render_writes[11]{};
  render_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  render_writes[0].dstSet = render_set_;
  render_writes[0].dstBinding = 0;
  render_writes[0].descriptorCount = 1;
  render_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  render_writes[0].pImageInfo = &render_image_info;

  for (u32 i = 0; i < 10; ++i) {
    render_buffer_infos[i] = {render_buffer_bindings[i].buffer, 0,
                              VK_WHOLE_SIZE};
    render_writes[i + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    render_writes[i + 1].dstSet = render_set_;
    render_writes[i + 1].dstBinding = render_buffer_bindings[i].binding;
    render_writes[i + 1].descriptorCount = 1;
    render_writes[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    render_writes[i + 1].pBufferInfo = &render_buffer_infos[i];
  }

  vkUpdateDescriptorSets(context_->device.logical_device, 11, render_writes,
                        0, nullptr);

  // Binding 12 (skybox_texture) starts pointed at the filler texture --
  // skybox_enabled_ starts false, so Builtin.RaymarchShader.comp.glsl never
  // actually samples it, but Vulkan still requires every combined-image-
  // sampler binding to reference a real image regardless (same reasoning as
  // scene_textures' unused slots -- see rebuild_static_scene()).
  write_skybox_binding(context_->texture_system->default_texture());

  // Populate probe_bake_set_'s static bindings (0=indirection,
  // 1=brick_pool, 2=brick_primitive -- the baked field a gather ray
  // marches against, always the current bake's; 3=scene_diffuse_colours,
  // 4=light_buffer -- a gather ray's hit shading). Bindings 5/6
  // (Prev/CurrProbeBuffer) are deliberately left unwritten here --
  // bake_probes() rewrites them before every bounce's dispatch, since
  // which physical buffer is "previous" vs. "current" alternates each
  // bounce.
  struct ProbeBakeBufferBinding {
    u32 binding;
    VkBuffer buffer;
  } probe_bake_buffer_bindings[5] = {
      {0, indirection_buffer_->handle()},
      {1, brick_pool_buffer_->handle()},
      {2, brick_primitive_buffer_->handle()},
      {3, primitive_colour_buffer_->handle()},
      {4, light_buffer_->handle()},
  };

  VkDescriptorBufferInfo probe_bake_buffer_infos[5];
  VkWriteDescriptorSet probe_bake_writes[5]{};
  for (u32 i = 0; i < 5; ++i) {
    probe_bake_buffer_infos[i] = {probe_bake_buffer_bindings[i].buffer, 0,
                                  VK_WHOLE_SIZE};
    probe_bake_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    probe_bake_writes[i].dstSet = probe_bake_set_;
    probe_bake_writes[i].dstBinding = probe_bake_buffer_bindings[i].binding;
    probe_bake_writes[i].descriptorCount = 1;
    probe_bake_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    probe_bake_writes[i].pBufferInfo = &probe_bake_buffer_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 5, probe_bake_writes,
                        0, nullptr);

  // Populate bloom_blur_h_set_: 0 = output_image_ (read), 1 =
  // bloom_temp_image_ (write). Both fixed for the object's lifetime --
  // only on_resized() ever needs to repoint these (the images themselves
  // get recreated then).
  VkDescriptorImageInfo bloom_blur_h_image_infos[2] = {
      {VK_NULL_HANDLE, output_image_.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, bloom_temp_image_.view, VK_IMAGE_LAYOUT_GENERAL},
  };
  VkWriteDescriptorSet bloom_blur_h_writes[2]{};
  for (u32 i = 0; i < 2; ++i) {
    bloom_blur_h_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    bloom_blur_h_writes[i].dstSet = bloom_blur_h_set_;
    bloom_blur_h_writes[i].dstBinding = i;
    bloom_blur_h_writes[i].descriptorCount = 1;
    bloom_blur_h_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bloom_blur_h_writes[i].pImageInfo = &bloom_blur_h_image_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 2, bloom_blur_h_writes,
                        0, nullptr);

  // Populate post_composite_set_: 0 = output_image_ (read), 1 =
  // bloom_temp_image_ (read), 2 = post_process_image_ (write).
  VkDescriptorImageInfo post_composite_image_infos[3] = {
      {VK_NULL_HANDLE, output_image_.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, bloom_temp_image_.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, post_process_image_.view, VK_IMAGE_LAYOUT_GENERAL},
  };
  VkWriteDescriptorSet post_composite_writes[3]{};
  for (u32 i = 0; i < 3; ++i) {
    post_composite_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    post_composite_writes[i].dstSet = post_composite_set_;
    post_composite_writes[i].dstBinding = i;
    post_composite_writes[i].descriptorCount = 1;
    post_composite_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    post_composite_writes[i].pImageInfo = &post_composite_image_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 3,
                        post_composite_writes, 0, nullptr);

  voxelize_pipeline_.emplace(
      *context_, voxelize_stage_,
      std::vector<VkDescriptorSetLayout>{voxelize_set_layout_},
      sizeof(VoxelizePushConstants));
  probe_bake_pipeline_.emplace(
      *context_, probe_bake_stage_,
      std::vector<VkDescriptorSetLayout>{probe_bake_set_layout_},
      sizeof(ProbeBakePushConstants));
  render_pipeline_.emplace(*context_, render_stage_,
                           std::vector<VkDescriptorSetLayout>{render_set_layout_},
                           sizeof(PushConstants));
  bloom_blur_h_pipeline_.emplace(
      *context_, bloom_blur_h_stage_,
      std::vector<VkDescriptorSetLayout>{bloom_blur_h_set_layout_},
      sizeof(BloomBlurHPushConstants));
  post_composite_pipeline_.emplace(
      *context_, post_composite_stage_,
      std::vector<VkDescriptorSetLayout>{post_composite_set_layout_},
      sizeof(PostCompositePushConstants));

  if (!voxelize_pipeline_->is_valid() || !probe_bake_pipeline_->is_valid() ||
      !render_pipeline_->is_valid() || !bloom_blur_h_pipeline_->is_valid() ||
      !post_composite_pipeline_->is_valid()) {
    KERROR("Failed to create compute pipeline(s) for the raymarch field.");
    return;
  }

  valid_ = true;

  // Upload the registered static scene and bake it, now that everything
  // above exists.
  rebuild_static_scene();
}

VulkanRaymarchShader::~VulkanRaymarchShader() {
  // Release the skybox's texture reference (if any) -- every acquire()
  // needs exactly one matching release() (see TextureSystem's contract).
  // Safe to do unconditionally before device/descriptor teardown below:
  // VulkanRendererBackend destroys raymarch_shader before texture_system.
  if (skybox_enabled_) {
    context_->texture_system->release(skybox_texture_name_);
  }

  // Destroy pipelines before the descriptor set layouts they were created
  // with.
  post_composite_pipeline_.reset();
  bloom_blur_h_pipeline_.reset();
  render_pipeline_.reset();
  probe_bake_pipeline_.reset();
  voxelize_pipeline_.reset();

  if (descriptor_pool_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(context_->device.logical_device, descriptor_pool_,
                            context_->allocator);
    descriptor_pool_ = VK_NULL_HANDLE;
  }
  if (post_composite_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 post_composite_set_layout_,
                                 context_->allocator);
    post_composite_set_layout_ = VK_NULL_HANDLE;
  }
  if (bloom_blur_h_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 bloom_blur_h_set_layout_, context_->allocator);
    bloom_blur_h_set_layout_ = VK_NULL_HANDLE;
  }
  if (render_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 render_set_layout_, context_->allocator);
    render_set_layout_ = VK_NULL_HANDLE;
  }
  if (probe_bake_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 probe_bake_set_layout_, context_->allocator);
    probe_bake_set_layout_ = VK_NULL_HANDLE;
  }
  if (voxelize_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 voxelize_set_layout_, context_->allocator);
    voxelize_set_layout_ = VK_NULL_HANDLE;
  }

  layer_buffer_.reset();
  light_buffer_.reset();
  param_expr_buffer_.reset();
  pixelation_exempt_buffer_.reset();
  primitive_colour_buffer_.reset();
  brick_primitive_buffer_.reset();
  primitive_buffer_.reset();

  probe_buffer_b_.reset();
  probe_buffer_a_.reset();

  brick_counter_buffer_.reset();
  brick_pool_buffer_.reset();
  indirection_buffer_.reset();

  vulkan_image_destroy(context_, &post_process_image_);
  vulkan_image_destroy(context_, &bloom_temp_image_);
  vulkan_image_destroy(context_, &output_image_);
}

void VulkanRaymarchShader::on_resized(u32 width, u32 height) {
  if (!valid_) {
    return;
  }

  vulkan_image_destroy(context_, &output_image_);
  vulkan_image_create(context_, VK_IMAGE_TYPE_2D, width, height,
                      context_->swapchain.image_format.format,
                      VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_STORAGE_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, TRUE,
                      VK_IMAGE_ASPECT_COLOR_BIT, &output_image_);

  vulkan_image_destroy(context_, &bloom_temp_image_);
  vulkan_image_create(context_, VK_IMAGE_TYPE_2D, (width + 1) / 2,
                      (height + 1) / 2, context_->swapchain.image_format.format,
                      VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_STORAGE_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, TRUE,
                      VK_IMAGE_ASPECT_COLOR_BIT, &bloom_temp_image_);

  vulkan_image_destroy(context_, &post_process_image_);
  vulkan_image_create(context_, VK_IMAGE_TYPE_2D, width, height,
                      context_->swapchain.image_format.format,
                      VK_IMAGE_TILING_OPTIMAL,
                      VK_IMAGE_USAGE_STORAGE_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, TRUE,
                      VK_IMAGE_ASPECT_COLOR_BIT, &post_process_image_);

  // Re-point every descriptor binding that referenced one of the images
  // just destroyed/recreated above -- their old views no longer exist, so
  // the descriptor sets would otherwise reference dangling handles.
  VkDescriptorImageInfo render_image_info{VK_NULL_HANDLE, output_image_.view,
                                          VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet render_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  render_write.dstSet = render_set_;
  render_write.dstBinding = 0;
  render_write.descriptorCount = 1;
  render_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  render_write.pImageInfo = &render_image_info;
  vkUpdateDescriptorSets(context_->device.logical_device, 1, &render_write, 0,
                        nullptr);

  VkDescriptorImageInfo bloom_blur_h_image_infos[2] = {
      {VK_NULL_HANDLE, output_image_.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, bloom_temp_image_.view, VK_IMAGE_LAYOUT_GENERAL},
  };
  VkWriteDescriptorSet bloom_blur_h_writes[2]{};
  for (u32 i = 0; i < 2; ++i) {
    bloom_blur_h_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    bloom_blur_h_writes[i].dstSet = bloom_blur_h_set_;
    bloom_blur_h_writes[i].dstBinding = i;
    bloom_blur_h_writes[i].descriptorCount = 1;
    bloom_blur_h_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bloom_blur_h_writes[i].pImageInfo = &bloom_blur_h_image_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 2, bloom_blur_h_writes,
                        0, nullptr);

  VkDescriptorImageInfo post_composite_image_infos[3] = {
      {VK_NULL_HANDLE, output_image_.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, bloom_temp_image_.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, post_process_image_.view, VK_IMAGE_LAYOUT_GENERAL},
  };
  VkWriteDescriptorSet post_composite_writes[3]{};
  for (u32 i = 0; i < 3; ++i) {
    post_composite_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    post_composite_writes[i].dstSet = post_composite_set_;
    post_composite_writes[i].dstBinding = i;
    post_composite_writes[i].descriptorCount = 1;
    post_composite_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    post_composite_writes[i].pImageInfo = &post_composite_image_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 3,
                        post_composite_writes, 0, nullptr);
}

void VulkanRaymarchShader::rebake() {
  if (!valid_) {
    KWARN("VulkanRaymarchShader::rebake called on an invalid shader.");
    return;
  }
  vkDeviceWaitIdle(context_->device.logical_device);
  rebuild_static_scene();
}

void VulkanRaymarchShader::rebuild_static_scene() {
  std::vector<Geometry> all = context_->geometry_system->snapshot();
  const std::vector<SceneLayer> &scene_layers =
      context_->geometry_system->layers();

  std::vector<GpuPrimitive> gpu_primitives(kMaxScenePrimitives, GpuPrimitive{});
  std::vector<f32> gpu_colours(static_cast<size_t>(kMaxScenePrimitives) * 4,
                              1.0f);
  std::vector<f32> gpu_pixelation_exempt(kMaxScenePrimitives, 0.0f);
  std::vector<VkDescriptorImageInfo> texture_infos(kMaxScenePrimitives);
  std::vector<GpuLayer> gpu_layers(kMaxLayers, GpuLayer{});
  std::vector<GpuParamExpr> gpu_param_exprs(static_cast<size_t>(kMaxScenePrimitives) * 4,
                                            GpuParamExpr{});

  // Default filler for unused slots -- Vulkan requires every element of a
  // fixed-size combined-image-sampler array to be a valid, bound image even
  // if the shader never actually samples that index.
  VulkanTexture &filler_texture = context_->texture_system->default_texture();
  for (auto &info : texture_infos) {
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    info.imageView = filler_texture.view();
    info.sampler = filler_texture.sampler();
  }

  // geometry.name -> its uploaded index in gpu_primitives, filled in as the
  // loop below assigns each one -- read back afterward by the emissive-
  // light loop (see GpuLight::source_primitive's comment) to record which
  // primitive a synthesized light came from.
  std::unordered_map<std::string, u32> primitive_index_by_name;

  // Primitives must land in primitive_buffer_ grouped contiguously by
  // layer (so each GpuLayer's start/count can slice a contiguous range),
  // but GeometrySystem::snapshot() has no ordering guarantee at all -- so
  // walk layers in index order and, for each, pull out just its own
  // primitives from the snapshot.
  u32 index = 0;
  u32 layer_count = std::min(static_cast<u32>(scene_layers.size()), kMaxLayers);
  for (u32 layer_i = 0; layer_i < layer_count; ++layer_i) {
    u32 layer_start = index;

    for (const Geometry &geometry : all) {
      if (geometry.layer != layer_i) {
        continue;
      }
      if (index >= kMaxScenePrimitives) {
        KWARN("GeometrySystem has more static primitives than this shader "
             "supports ({}); '{}' will not be rendered.",
             kMaxScenePrimitives, geometry.name);
        continue;
      }

      GpuPrimitive &prim = gpu_primitives[index];
      prim.position_type[0] = geometry.position.x;
      prim.position_type[1] = geometry.position.y;
      prim.position_type[2] = geometry.position.z;
      prim.position_type[3] =
          static_cast<f32>(static_cast<u32>(geometry.type));
      prim.params[0] = geometry.params.x;
      prim.params[1] = geometry.params.y;
      prim.params[2] = geometry.params.z;
      prim.params[3] = geometry.extra_param;

      glm::quat rotation(geometry.rotation);
      prim.rotation[0] = rotation.x;
      prim.rotation[1] = rotation.y;
      prim.rotation[2] = rotation.z;
      prim.rotation[3] = rotation.w;

      prim.expr_scale[0] = geometry.param_expr_scale;
      // Pre-multiplied emissive radiance (see Material::emissive_colour/
      // emissive_intensity and expr_scale's struct comment in
      // Builtin.SdfSceneCommon.inc.glsl) -- (0,0,0) for a non-emissive
      // material, since both fields default to a state that multiplies to
      // zero.
      prim.expr_scale[1] = geometry.material->emissive_colour.r *
                          geometry.material->emissive_intensity;
      prim.expr_scale[2] = geometry.material->emissive_colour.g *
                          geometry.material->emissive_intensity;
      prim.expr_scale[3] = geometry.material->emissive_colour.b *
                          geometry.material->emissive_intensity;

      // Compile each slot's "parametric attribute" formula, if it has one
      // -- an empty string (the default) or a compile failure both just
      // leave instruction_count at 0, meaning "use params[slot]'s plain
      // constant instead" (see evaluate_expr()/resolve_params() in
      // Builtin.RaymarchVoxelize.comp.glsl). compile_expression() already
      // logs why on failure.
      for (size_t slot = 0; slot < geometry.param_expressions.size(); ++slot) {
        const std::string &source = geometry.param_expressions[slot];
        if (source.empty()) {
          continue;
        }
        std::optional<CompiledExpression> compiled = compile_expression(source);
        if (!compiled) {
          continue;
        }
        GpuParamExpr &expr = gpu_param_exprs[static_cast<size_t>(index) * 4 + slot];
        expr.instruction_count = static_cast<i32>(compiled->instructions.size());
        for (size_t i = 0; i < compiled->instructions.size(); ++i) {
          expr.op[i] = static_cast<i32>(compiled->instructions[i].op);
          expr.operand[i] = compiled->instructions[i].operand;
        }
      }

      const glm::vec4 &colour = geometry.material->diffuse_colour;
      gpu_colours[index * 4 + 0] = colour.r;
      gpu_colours[index * 4 + 1] = colour.g;
      gpu_colours[index * 4 + 2] = colour.b;
      // The alpha slot carries this primitive's *effective* texture_scale
      // (world units per texture tile), not colour opacity -- nothing ever
      // read the opacity, and packing the scale here spares a whole extra
      // buffer + binding. See ScenePrimitiveColours in
      // Builtin.RaymarchShader.comp.glsl, the only reader. Multiplied by
      // texture_scale_factor (see its comment on Geometry) rather than
      // uploading material->texture_scale directly, so a scaled scene's
      // texture tiling scales right along with it instead of staying at
      // its authored-size frequency.
      gpu_colours[index * 4 + 3] =
          geometry.material->texture_scale * geometry.texture_scale_factor;

      gpu_pixelation_exempt[index] =
          geometry.material->pixelation_exempt ? 1.0f : 0.0f;

      texture_infos[index].imageView =
          geometry.material->diffuse_texture->view();
      texture_infos[index].sampler =
          geometry.material->diffuse_texture->sampler();

      primitive_index_by_name[geometry.name] = index;

      ++index;
    }

    GpuLayer &gpu_layer = gpu_layers[layer_i];
    gpu_layer.op_smoothness[0] =
        static_cast<f32>(static_cast<u32>(scene_layers[layer_i].operation));
    gpu_layer.op_smoothness[1] = scene_layers[layer_i].smoothness;
    gpu_layer.range[0] = static_cast<i32>(layer_start);
    gpu_layer.range[1] = static_cast<i32>(index - layer_start);
  }

  // Volumetric "light shaft" primitives -- appended directly after every
  // opaque primitive, in their own tail range of primitive_buffer_/
  // scene_diffuse_colours/scene_textures that no GpuLayer's range covers, so
  // scene_map() (the voxelize pass, and the render pass's per-pixel material
  // re-evaluation) never iterates them: they're not part of the opaque
  // scene at all. accumulate_volumetrics() in Builtin.RaymarchShader.comp.
  // glsl instead marches through exactly this range directly via
  // primitive_sdf(), which is why the upload below mirrors the opaque loop
  // above closely (same GpuPrimitive/colour/texture layout).
  volumetric_start_ = static_cast<i32>(index);
  std::vector<Volumetric> volumetrics =
      context_->geometry_system->volumetric_snapshot();
  for (const Volumetric &volumetric : volumetrics) {
    if (index >= kMaxScenePrimitives) {
      KWARN("GeometrySystem has more volumetrics than this shader supports "
           "(combined with opaque primitives, cap is {}); '{}' will not be "
           "rendered.",
           kMaxScenePrimitives, volumetric.name);
      continue;
    }

    GpuPrimitive &prim = gpu_primitives[index];
    prim.position_type[0] = volumetric.position.x;
    prim.position_type[1] = volumetric.position.y;
    prim.position_type[2] = volumetric.position.z;
    prim.position_type[3] =
        static_cast<f32>(static_cast<u32>(volumetric.type));
    prim.params[0] = volumetric.params.x;
    prim.params[1] = volumetric.params.y;
    prim.params[2] = volumetric.params.z;
    prim.params[3] = volumetric.extra_param;

    glm::quat rotation(volumetric.rotation);
    prim.rotation[0] = rotation.x;
    prim.rotation[1] = rotation.y;
    prim.rotation[2] = rotation.z;
    prim.rotation[3] = rotation.w;

    // expr_scale.x is param_expr_scale for an opaque primitive's formula-
    // driven params (see resolve_params() in Builtin.SdfSceneCommon.inc.
    // glsl) -- volumetrics never carry a param_expressions formula (every
    // ParamExpr entry at this index is left at instruction_count == 0
    // below), so that value is otherwise dead for an index in this range,
    // and is repurposed here to carry density instead -- the only reader
    // for a volumetric index is accumulate_volumetrics(). yzw stay zero: a
    // volumetric isn't emissive.
    prim.expr_scale[0] = volumetric.density;

    const glm::vec4 &colour = volumetric.material->diffuse_colour;
    gpu_colours[index * 4 + 0] = colour.r;
    gpu_colours[index * 4 + 1] = colour.g;
    gpu_colours[index * 4 + 2] = colour.b;
    gpu_colours[index * 4 + 3] = volumetric.material->texture_scale;

    texture_infos[index].imageView =
        volumetric.material->diffuse_texture->view();
    texture_infos[index].sampler =
        volumetric.material->diffuse_texture->sampler();

    ++index;
  }
  volumetric_count_ = static_cast<i32>(index) - volumetric_start_;

  // The render pass re-evaluates the analytic scene per hit pixel for
  // material provenance and needs the same layer count the voxelize pass
  // bakes with -- sent as a push constant every render_to() call, like
  // light_count_.
  layer_count_ = static_cast<i32>(layer_count);

  primitive_buffer_->load_data(0, gpu_primitives.size() * sizeof(GpuPrimitive),
                               0, gpu_primitives.data());
  primitive_colour_buffer_->load_data(0, gpu_colours.size() * sizeof(f32), 0,
                                      gpu_colours.data());
  pixelation_exempt_buffer_->load_data(
      0, gpu_pixelation_exempt.size() * sizeof(f32), 0,
      gpu_pixelation_exempt.data());
  layer_buffer_->load_data(0, gpu_layers.size() * sizeof(GpuLayer), 0,
                           gpu_layers.data());
  param_expr_buffer_->load_data(0, gpu_param_exprs.size() * sizeof(GpuParamExpr),
                                0, gpu_param_exprs.data());

  std::vector<Light> registered_lights = context_->geometry_system->light_snapshot();
  std::vector<GpuLight> gpu_lights(kMaxLights, GpuLight{});
  if (registered_lights.empty()) {
    // No lights registered anywhere -- fall back to the old hardcoded
    // single directional light so a scene that predates lights (or just
    // doesn't define any) still renders lit exactly as before.
    GpuLight &light = gpu_lights[0];
    light.vector_type[0] = kDefaultLightDirection.x;
    light.vector_type[1] = kDefaultLightDirection.y;
    light.vector_type[2] = kDefaultLightDirection.z;
    light.vector_type[3] = static_cast<f32>(static_cast<u32>(LightType::Directional));
    light.colour_intensity[0] = kDefaultLightColour.r;
    light.colour_intensity[1] = kDefaultLightColour.g;
    light.colour_intensity[2] = kDefaultLightColour.b;
    light.colour_intensity[3] = kDefaultLightIntensity;
    light.source_primitive[0] = -1.0f; // no associated primitive
    light_count_ = 1;
  } else {
    light_count_ = static_cast<i32>(
        std::min(static_cast<u32>(registered_lights.size()), kMaxLights));
    if (registered_lights.size() > kMaxLights) {
      KWARN("GeometrySystem has more lights than this shader supports ({}); "
           "only the first {} will contribute to lighting.",
           kMaxLights, kMaxLights);
    }
    for (i32 i = 0; i < light_count_; ++i) {
      const Light &src = registered_lights[static_cast<size_t>(i)];
      GpuLight &dst = gpu_lights[static_cast<size_t>(i)];
      dst.vector_type[0] = src.vector.x;
      dst.vector_type[1] = src.vector.y;
      dst.vector_type[2] = src.vector.z;
      dst.vector_type[3] = static_cast<f32>(static_cast<u32>(src.type));
      dst.colour_intensity[0] = src.colour.r;
      dst.colour_intensity[1] = src.colour.g;
      dst.colour_intensity[2] = src.colour.b;
      dst.colour_intensity[3] = src.intensity;
      dst.source_primitive[0] = -1.0f; // no associated primitive
    }
  }

  // Emissive primitives (Material::emissive_intensity > 0) also act as
  // light sources -- append one synthesized point light per emissive
  // primitive, positioned at the primitive itself, on top of whatever
  // real Light registrations/fallback populated above. This is what lets
  // an authored "glowing bulb"/"light panel" primitive actually
  // illuminate the rest of the scene (and, since it lands in the same
  // buffer, the GI probe bake too), not just look bright on its own -- see
  // the emissive term in Builtin.RaymarchShader.comp.glsl for the visual
  // half of this.
  for (const Geometry &geometry : all) {
    if (!geometry.material || geometry.material->emissive_intensity <= 0.0f) {
      continue;
    }
    if (static_cast<u32>(light_count_) >= kMaxLights) {
      KWARN("GeometrySystem has more lights (including emissive primitives) "
           "than this shader supports ({}); '{}' will glow but won't "
           "illuminate the rest of the scene.",
           kMaxLights, geometry.name);
      continue;
    }

    // A Plane's own position is always (0,0,0) (see GeometryConfig::
    // plane()/add_plane()) -- only its height (params.x) means anything,
    // so its effective world position for a light is (0, height, 0),
    // mirroring the SDF editor's gizmo_effective_position() convention.
    glm::vec3 position = geometry.type == PrimitiveType::Plane
                             ? glm::vec3(0.0f, geometry.params.x, 0.0f)
                             : geometry.position;

    GpuLight &dst = gpu_lights[static_cast<size_t>(light_count_)];
    dst.vector_type[0] = position.x;
    dst.vector_type[1] = position.y;
    dst.vector_type[2] = position.z;
    dst.vector_type[3] = static_cast<f32>(static_cast<u32>(LightType::Point));
    dst.colour_intensity[0] = geometry.material->emissive_colour.r;
    dst.colour_intensity[1] = geometry.material->emissive_colour.g;
    dst.colour_intensity[2] = geometry.material->emissive_colour.b;
    dst.colour_intensity[3] = geometry.material->emissive_intensity;

    // See GpuLight::source_primitive's comment -- shadow_march() needs
    // this to avoid an emissive primitive's own shell always self-
    // occluding the light it casts. Falls back to -1 (no exclusion) only
    // if this geometry somehow never got uploaded above (e.g. it exceeded
    // kMaxScenePrimitives) -- shouldn't happen in practice since the same
    // cap applies to both loops.
    auto it = primitive_index_by_name.find(geometry.name);
    dst.source_primitive[0] =
        it != primitive_index_by_name.end() ? static_cast<f32>(it->second) : -1.0f;

    ++light_count_;
  }

  ambient_ = context_->geometry_system->ambient();
  light_buffer_->load_data(0, gpu_lights.size() * sizeof(GpuLight), 0,
                          gpu_lights.data());

  VkWriteDescriptorSet texture_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  texture_write.dstSet = render_set_;
  texture_write.dstBinding = 6;
  texture_write.descriptorCount = kMaxScenePrimitives;
  texture_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  texture_write.pImageInfo = texture_infos.data();
  vkUpdateDescriptorSets(context_->device.logical_device, 1, &texture_write, 0,
                        nullptr);

  voxelize(layer_count);

  // GI needs the field voxelize() just baked to march gather rays against
  // -- and light_count_/ambient_ (just set above) to shade what those
  // rays hit.
  bake_probes(static_cast<u32>(light_count_));
}

void VulkanRaymarchShader::write_skybox_binding(VulkanTexture &texture) {
  VkDescriptorImageInfo image_info{};
  image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  image_info.imageView = texture.view();
  image_info.sampler = texture.sampler();

  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = render_set_;
  write.dstBinding = 12;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = &image_info;
  vkUpdateDescriptorSets(context_->device.logical_device, 1, &write, 0, nullptr);
}

void VulkanRaymarchShader::set_skybox(std::string_view texture_name) {
  // Waits for the device to go idle first, like rebake()/remove_scene() --
  // about to rewrite render_set_'s skybox binding directly (and possibly
  // release() the previous texture's last reference, destroying its
  // VkImageView/VkSampler), either of which a still-in-flight render_to()
  // dispatch could be reading through.
  vkDeviceWaitIdle(context_->device.logical_device);

  if (skybox_enabled_) {
    context_->texture_system->release(skybox_texture_name_);
  }

  VulkanTexture &texture =
      context_->texture_system->acquire(texture_name, /*auto_release=*/true);
  skybox_texture_name_ = std::string(texture_name);
  skybox_enabled_ = true;

  write_skybox_binding(texture);
}

void VulkanRaymarchShader::disable_skybox() {
  if (!skybox_enabled_) {
    return;
  }
  vkDeviceWaitIdle(context_->device.logical_device); // see set_skybox()'s comment

  context_->texture_system->release(skybox_texture_name_);
  skybox_texture_name_.clear();
  skybox_enabled_ = false;

  // Re-point the binding at the filler texture rather than leaving it
  // referencing whatever set_skybox() last acquired (now possibly
  // destroyed, if this was its only reference) -- see the constructor's own
  // initial write_skybox_binding() call for the same reasoning.
  write_skybox_binding(context_->texture_system->default_texture());
}

void VulkanRaymarchShader::voxelize(u32 layer_count) {
  auto cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
      *context_, context_->device.graphics_command_pool);

  // Zero the brick allocation counter before the shader's atomicAdd calls.
  vkCmdFillBuffer(cmd->handle(), brick_counter_buffer_->handle(), 0,
                 sizeof(u32), 0);

  VkBufferMemoryBarrier fill_barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  fill_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  fill_barrier.dstAccessMask =
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  fill_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  fill_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  fill_barrier.buffer = brick_counter_buffer_->handle();
  fill_barrier.offset = 0;
  fill_barrier.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(cmd->handle(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                      &fill_barrier, 0, nullptr);

  voxelize_pipeline_->bind(*cmd);
  vkCmdBindDescriptorSets(cmd->handle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                         voxelize_pipeline_->layout(), 0, 1, &voxelize_set_, 0,
                         nullptr);

  VoxelizePushConstants push_constants{static_cast<i32>(layer_count)};
  vkCmdPushConstants(cmd->handle(), voxelize_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0,
                    sizeof(VoxelizePushConstants), &push_constants);

  constexpr u32 local_size = 4; // must match local_size_x/y/z in the shader
  u32 groups = (kCoarseDim + local_size - 1) / local_size;
  vkCmdDispatch(cmd->handle(), groups, groups, groups);

  // end_single_use submits and calls vkQueueWaitIdle, so once this returns
  // the field is fully baked and visible — no extra barrier needed for a
  // dependency that's about to be enforced by a full queue idle anyway.
  VulkanCommandBuffer::end_single_use(*context_,
                                      context_->device.graphics_command_pool,
                                      std::move(cmd),
                                      context_->device.graphics_queue);

  // The counter counts *demand* (every cell that wanted a brick bumps it),
  // not just successful allocations, so after the queue idle above it tells
  // us whether the pool was big enough. Overflow doesn't crash -- the
  // shader just leaves the losing cells brickless, in nondeterministic
  // atomicAdd order -- but it renders as random missing/stray chunks of
  // surface, so make it loud instead of leaving the next person to
  // rediscover that from the artifacts alone.
  if (void *mapped = brick_counter_buffer_->lock(0, sizeof(u32), 0)) {
    u32 bricks_demanded = *static_cast<u32 *>(mapped);
    brick_counter_buffer_->unlock();
    if (bricks_demanded > kMaxBricks) {
      KWARN("Voxelize pass wanted {} bricks but the pool only holds {} -- "
            "{} surface cells were dropped and will render as missing/"
            "corrupted chunks. Raise kMaxBricks (and MAX_BRICKS in "
            "Builtin.RaymarchVoxelize.comp.glsl) or shrink the scene.",
            bricks_demanded, kMaxBricks, bricks_demanded - kMaxBricks);
    }
  }
}

void VulkanRaymarchShader::bake_probes(u32 light_count) {
  // Zero probe_buffer_a_ -- bounce 0's "previous bounce" input, which must
  // start at zero (see Builtin.ProbeBake.comp.glsl's file header comment:
  // this is what makes bounce 0 naturally gather direct light only, with
  // no special-casing needed in the shader). probe_buffer_b_ doesn't need
  // zeroing -- it's always a write target before it's ever read.
  {
    auto cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
        *context_, context_->device.graphics_command_pool);
    vkCmdFillBuffer(cmd->handle(), probe_buffer_a_->handle(), 0,
                   VK_WHOLE_SIZE, 0);

    VkBufferMemoryBarrier fill_barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    fill_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    fill_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    fill_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    fill_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    fill_barrier.buffer = probe_buffer_a_->handle();
    fill_barrier.offset = 0;
    fill_barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd->handle(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                        &fill_barrier, 0, nullptr);

    VulkanCommandBuffer::end_single_use(*context_,
                                        context_->device.graphics_command_pool,
                                        std::move(cmd),
                                        context_->device.graphics_queue);
  }

  ProbeBakePushConstants push_constants{static_cast<i32>(light_count), ambient_};
  constexpr u32 local_size = 4; // must match local_size_x/y/z in the shader
  u32 groups = (kProbeDim + local_size - 1) / local_size;

  for (u32 bounce = 0; bounce < kProbeBounceCount; ++bounce) {
    // Bounce 0 reads probe_buffer_a_ (just zeroed above) and writes
    // probe_buffer_b_; each subsequent bounce swaps which buffer is which
    // -- see kProbeFinalInBufferB's comment for why this alternation
    // determines which buffer holds the result after the loop.
    bool write_to_b = (bounce % 2) == 0;
    VkBuffer prev = write_to_b ? probe_buffer_a_->handle() : probe_buffer_b_->handle();
    VkBuffer curr = write_to_b ? probe_buffer_b_->handle() : probe_buffer_a_->handle();

    VkDescriptorBufferInfo ping_pong_infos[2] = {
        {prev, 0, VK_WHOLE_SIZE},
        {curr, 0, VK_WHOLE_SIZE},
    };
    VkWriteDescriptorSet ping_pong_writes[2]{};
    ping_pong_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ping_pong_writes[0].dstSet = probe_bake_set_;
    ping_pong_writes[0].dstBinding = 5; // PrevProbeBuffer
    ping_pong_writes[0].descriptorCount = 1;
    ping_pong_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ping_pong_writes[0].pBufferInfo = &ping_pong_infos[0];
    ping_pong_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ping_pong_writes[1].dstSet = probe_bake_set_;
    ping_pong_writes[1].dstBinding = 6; // CurrProbeBuffer
    ping_pong_writes[1].descriptorCount = 1;
    ping_pong_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ping_pong_writes[1].pBufferInfo = &ping_pong_infos[1];
    // Safe to rewrite unconditionally, no in-flight risk: the previous
    // bounce's end_single_use() call (or, for bounce 0, the zero-fill
    // above) already did a vkQueueWaitIdle before this point.
    vkUpdateDescriptorSets(context_->device.logical_device, 2, ping_pong_writes,
                          0, nullptr);

    auto cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
        *context_, context_->device.graphics_command_pool);

    probe_bake_pipeline_->bind(*cmd);
    vkCmdBindDescriptorSets(cmd->handle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                           probe_bake_pipeline_->layout(), 0, 1,
                           &probe_bake_set_, 0, nullptr);
    vkCmdPushConstants(cmd->handle(), probe_bake_pipeline_->layout(),
                      VK_SHADER_STAGE_COMPUTE_BIT, 0,
                      sizeof(ProbeBakePushConstants), &push_constants);
    vkCmdDispatch(cmd->handle(), groups, groups, groups);

    // Synchronous, like voxelize() -- the next bounce's descriptor rewrite
    // above depends on this one having fully finished reading/writing.
    VulkanCommandBuffer::end_single_use(*context_,
                                        context_->device.graphics_command_pool,
                                        std::move(cmd),
                                        context_->device.graphics_queue);
  }
}

void VulkanRaymarchShader::transition_image(
    VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
    VkImageLayout new_layout, VkAccessFlags src_access,
    VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
    VkPipelineStageFlags dst_stage) {
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;

  vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1,
                       &barrier);
}

namespace {
void write_vec3(f32 (&dest)[4], glm::vec3 v) {
  dest[0] = v.x;
  dest[1] = v.y;
  dest[2] = v.z;
  dest[3] = 0.0f;
}
} // namespace

void VulkanRaymarchShader::render_to(VulkanCommandBuffer &command_buffer,
                                    VkImage swapchain_image, u32 width,
                                    u32 height, f32 delta_time,
                                    const Camera &camera) {
  if (!valid_) {
    KWARN("VulkanRaymarchShader::render_to called on an invalid shader.");
    return;
  }

  PushConstants push_constants{};
  write_vec3(push_constants.camera_position, camera.position());
  write_vec3(push_constants.camera_forward, camera.forward());
  write_vec3(push_constants.camera_right, camera.right());
  write_vec3(push_constants.camera_up, camera.up());

  push_constants.light_count = light_count_;
  push_constants.ambient = ambient_;
  push_constants.selected_primitive_index = selected_primitive_index_;
  push_constants.grid_enabled = grid_visible_ ? 1 : 0;
  push_constants.layer_count = layer_count_;
  push_constants.volumetric_start = volumetric_start_;
  push_constants.volumetric_count = volumetric_count_;
  elapsed_time_ += delta_time;
  push_constants.time = elapsed_time_;
  push_constants.skybox_enabled = skybox_enabled_ ? 1 : 0;

  VkCommandBuffer cmd = command_buffer.handle();

  constexpr u32 local_size = 16; // must match local_size_x/y in every pass 3/4 shader
  u32 group_x = (width + local_size - 1) / local_size;
  u32 group_y = (height + local_size - 1) / local_size;

  // --- Pass 3: render the scene into output_image_ (rgb=colour,
  // a=pixelation-exempt flag). ---
  // Storage image -> GENERAL for the compute shader to write into. Old
  // layout is claimed UNDEFINED (discard) since every pixel gets
  // overwritten unconditionally each frame; src stage/access wait on the
  // post-process passes' compute-shader reads of it from the previous
  // frame -- output_image_ is read by compute now (pass 4a/4b below), not
  // copied via transfer directly, so that's what this write must wait on.
  transition_image(cmd, output_image_.handle, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  render_pipeline_->bind(command_buffer);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                         render_pipeline_->layout(), 0, 1, &render_set_, 0,
                         nullptr);
  vkCmdPushConstants(cmd, render_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants),
                    &push_constants);
  vkCmdDispatch(cmd, group_x, group_y, 1);

  // --- Pass 4a: bloom bright-pass + horizontal blur, into
  // bloom_temp_image_ (half-resolution). ---
  // output_image_ stays in GENERAL (no layout change, just a memory
  // barrier): pass 4a reads it via imageLoad right after pass 3 wrote it,
  // so that write must be visible before this read.
  transition_image(cmd, output_image_.handle, VK_IMAGE_LAYOUT_GENERAL,
                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  // bloom_temp_image_ -> GENERAL to write into; discard (every half-res
  // pixel is overwritten unconditionally), waiting on the previous
  // frame's read of it by pass 4b.
  transition_image(cmd, bloom_temp_image_.handle, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  BloomBlurHPushConstants bloom_blur_h_push{
      static_cast<i32>(width), static_cast<i32>(height), kBloomThreshold};
  bloom_blur_h_pipeline_->bind(command_buffer);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                         bloom_blur_h_pipeline_->layout(), 0, 1,
                         &bloom_blur_h_set_, 0, nullptr);
  vkCmdPushConstants(cmd, bloom_blur_h_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0,
                    sizeof(BloomBlurHPushConstants), &bloom_blur_h_push);
  u32 half_group_x = ((width + 1) / 2 + local_size - 1) / local_size;
  u32 half_group_y = ((height + 1) / 2 + local_size - 1) / local_size;
  vkCmdDispatch(cmd, half_group_x, half_group_y, 1);

  // --- Pass 4b: finish the bloom blur vertically, composite bloom +
  // vignette + pixelation, write the final frame into
  // post_process_image_. ---
  transition_image(cmd, bloom_temp_image_.handle, VK_IMAGE_LAYOUT_GENERAL,
                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  transition_image(cmd, post_process_image_.handle, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  PostCompositePushConstants post_composite_push{
      static_cast<i32>(width),
      static_cast<i32>(height),
      bloom_enabled_ ? kBloomIntensity : 0.0f,
      vignette_enabled_ ? kVignetteStrength : 0.0f,
      kVignetteRadius,
      pixelation_enabled_ ? 1 : 0,
      static_cast<i32>(pixelation_block_size_),
  };
  post_composite_pipeline_->bind(command_buffer);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                         post_composite_pipeline_->layout(), 0, 1,
                         &post_composite_set_, 0, nullptr);
  vkCmdPushConstants(cmd, post_composite_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0,
                    sizeof(PostCompositePushConstants), &post_composite_push);
  vkCmdDispatch(cmd, group_x, group_y, 1);

  // Final frame -> transfer source.
  transition_image(cmd, post_process_image_.handle, VK_IMAGE_LAYOUT_GENERAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);

  // Swapchain image -> transfer destination. The render pass that ran
  // earlier this frame wrote it via COLOR_ATTACHMENT_OUTPUT; wait for that
  // before overwriting. Old layout is claimed UNDEFINED since the copy
  // below overwrites every pixel, so the swapchain image's prior contents
  // (the render pass's clear) don't need preserving.
  transition_image(cmd, swapchain_image, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);

  VkImageCopy region{};
  region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.srcSubresource.layerCount = 1;
  region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.dstSubresource.layerCount = 1;
  region.extent = {width, height, 1};

  // Copies from post_process_image_ now, not output_image_ directly -- the
  // post-process chain's final result.
  vkCmdCopyImage(cmd, post_process_image_.handle,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchain_image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  // Swapchain image -> colour attachment, ready for the UI renderpass that
  // runs right after this (see VulkanRendererBackend::end_frame()) to draw
  // on top of it. Not a direct transition to VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
  // here anymore -- that now happens at the end of the UI renderpass
  // instead (its has_next_pass=false), since it's the last thing to touch
  // this image before vkQueuePresentKHR.
  transition_image(cmd, swapchain_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
}
