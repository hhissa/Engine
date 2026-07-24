#pragma once
#include "../../camera.h"
#include "../vulkan_buffer.h"
#include "../vulkan_compute_pipeline.h"
#include "../vulkan_shader_module.h"
#include "../vulkan_types.inl"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

class VulkanCommandBuffer;
class VulkanTexture;
struct Geometry;

// Three-pass sparse-voxel raymarching with baked indirect lighting:
//
//  Pass 1 ("voxelize"): evaluates the analytic scene SDF -- the union of
//  every static primitive currently registered with GeometrySystem -- and
//  bakes it into a sparse voxel field: a coarse indirection grid where each
//  cell either points at an allocated "brick" of fine SDF samples (only
//  where a surface actually passes through it) or is empty.
//
//  Pass 2 ("probe bake"): fills a regular 3D grid of indirect-light probes
//  by raymarching a sphere of directions from each probe against the
//  field pass 1 just baked, accumulating incoming light (direct light at
//  each gather ray's hit, plus whatever indirect light the previous
//  bounce's probe grid already found nearby), run kProbeBounceCount times
//  so light bounces a few times before settling — see
//  Builtin.ProbeBake.comp.glsl for the full technique (adapted from Inigo
//  Quilez's "simplegi" article) and bake_probes() below.
//
//  Both of the above run once per rebake() (construction, or a scene
//  edit) -- neither runs per frame.
//
//  Pass 3 ("render", repeating): every frame, marches a ray per pixel
//  against the baked field instead of evaluating the SDF directly — for
//  cells with no brick it skips straight across (the actual point of
//  sparsity: empty space costs one lookup, not per-voxel sampling), and for
//  bricked cells it trilinearly samples the fine distance values. Indirect
//  lighting at a hit comes from trilinearly sampling pass 2's baked probe
//  grid, not a re-evaluation -- like the voxel field itself, GI is baked,
//  not recomputed every frame. Writes its result (rgb=colour, a=whether the
//  hit primitive is pixelation-exempt) to output_image_, not straight to
//  the swapchain -- pass 4 below reads it first.
//
//  Pass 4 ("post-process", repeating): screen-space effects applied after
//  the scene itself is fully shaded --
//    - Bloom: Builtin.BloomBlurH.comp.glsl extracts a bright-pass from
//      output_image_ and blurs it horizontally into a half-resolution
//      buffer; Builtin.PostComposite.comp.glsl finishes the blur
//      vertically and adds it back on top (a standard separable-blur
//      split, so a wide blur radius costs O(2N) samples instead of
//      O(N^2)).
//    - Vignette: a smooth radial darkening from screen center, in
//      Builtin.PostComposite.comp.glsl.
//    - Pixelation: also in Builtin.PostComposite.comp.glsl -- quantizes
//      non-exempt pixels to their containing block's representative
//      colour (the classic flat-block "pixelation" look), skipping any
//      pixel whose hit primitive opted out via Material::
//      pixelation_exempt (see output_image_'s alpha channel above), so a
//      marked primitive stays crisp while its surroundings pixelate.
//  Bloom and vignette default on (subtly); pixelation defaults off, since
//  it's a deliberate stylistic choice rather than something every game
//  using this engine would want turned on unasked. See set_bloom_enabled()/
//  set_vignette_enabled()/set_pixelation_enabled() below.
class VulkanRaymarchShader {
public:
  explicit VulkanRaymarchShader(VulkanContext &context);
  ~VulkanRaymarchShader();

  VulkanRaymarchShader(const VulkanRaymarchShader &) = delete;
  VulkanRaymarchShader &operator=(const VulkanRaymarchShader &) = delete;
  VulkanRaymarchShader(VulkanRaymarchShader &&) = delete;
  VulkanRaymarchShader &operator=(VulkanRaymarchShader &&) = delete;

  // True only if both shader modules compiled, both pipelines were created,
  // and the field buffers were allocated.
  bool is_valid() const noexcept { return valid_; }
  explicit operator bool() const noexcept { return valid_; }

  // Dispatches the render (pass 2) compute shader into the internal storage
  // image (one invocation per pixel, sized width x height), then copies the
  // result into swapchain_image. Must be called outside an active render
  // pass instance — barriers and vkCmdDispatch are not valid inside one.
  // swapchain_image's prior contents are not preserved: the copy overwrites
  // it completely.
  //
  // camera's position/basis are sent as push constants (see PushConstants
  // in vulkan_raymarch_shader.cpp) rather than a UBO: they're recorded
  // directly into this call's command buffer, so unlike a separate UBO
  // buffer there's no risk of overwriting a value a still-in-flight
  // previous frame might still be reading — no per-image buffering needed.
  void render_to(VulkanCommandBuffer &command_buffer, VkImage swapchain_image,
                 u32 width, u32 height, f32 delta_time, const Camera &camera);

  // Recreates the output storage image at the new size and re-points the
  // render descriptor set's binding 0 at it. Must be called whenever the
  // framebuffer resizes — the output image is sized to match it, unlike
  // the sparse voxel field (which represents world-space scene geometry
  // and is independent of screen resolution, so it never needs this). Call
  // while the device is idle (VulkanRendererBackend::recreate_swapchain()
  // already waits before calling this).
  void on_resized(u32 width, u32 height);

  // Re-bakes the sparse voxel field against whatever is currently
  // registered with GeometrySystem -- for a caller (e.g. renderer_load_
  // scene()/renderer_remove_scene()) that adds/removes static primitives
  // at runtime after construction, unlike the constructor's own one-time
  // bake. Waits for the device to go idle first: rebuild_static_scene()
  // below rewrites primitive_buffer_/layer_buffer_/render_set_'s
  // texture-array binding directly (single instances, not buffered per
  // frame-in-flight) -- safe at construction (nothing submitted yet), but
  // not once the render loop is running, where a previous frame's pass-2
  // dispatch could still be reading them.
  void rebake();

  // Marks which scene_textures/scene_diffuse_colours index (see
  // rebuild_static_scene() below) render_to() should draw a selection
  // outline around, or -1 for none. Just a push constant, applied the very
  // next render_to() call -- unlike rebake(), no device-idle wait or
  // re-upload needed.
  void set_selected_primitive(i32 index) noexcept {
    selected_primitive_index_ = index;
  }

  // Shows/hides the reference grid (the subdivided y=0 plane the render
  // pass composites analytically -- see apply_reference_grid() in
  // Builtin.RaymarchShader.comp.glsl). Same mechanics as
  // set_selected_primitive(): just a push constant, applied the very next
  // render_to() call. Hidden by default -- it's an editor aid, so games
  // never see it unless they opt in; tools/sdf_editor turns it on.
  void set_grid_visible(b8 visible) noexcept { grid_visible_ = visible; }

  // Enables/disables the bloom post-process (see the class comment's pass
  // 4 section). Just a push constant read by Builtin.PostComposite.
  // comp.glsl -- takes effect the very next render_to() call, no rebake
  // needed. On by default, subtly.
  void set_bloom_enabled(b8 enabled) noexcept { bloom_enabled_ = enabled; }

  // Enables/disables the vignette post-process. Same mechanics as
  // set_bloom_enabled() above. On by default, subtly.
  void set_vignette_enabled(b8 enabled) noexcept {
    vignette_enabled_ = enabled;
  }

  // Enables/disables the pixelation post-process. Same mechanics as
  // set_bloom_enabled() above. Off by default -- see the class comment for
  // why.
  void set_pixelation_enabled(b8 enabled) noexcept {
    pixelation_enabled_ = enabled;
  }

  // Sets the pixelation block size (edge length, in full-resolution
  // screen pixels) -- larger blocks read as a chunkier/lower-fidelity
  // pixelation. Has no visible effect unless pixelation is also enabled.
  // Clamped to at least 1 (a block size of 0 would divide by zero in the
  // shader).
  void set_pixelation_block_size(u32 block_size) noexcept {
    pixelation_block_size_ = std::max(block_size, 1u);
  }

  // Enables a skybox: an equirectangular (lat/long, NOT 6-face cubemap)
  // texture sampled by ray direction and shown wherever the primary ray
  // doesn't hit anything, replacing the flat two-colour background
  // gradient this shader used before any skybox existed -- see
  // apply_skybox() in Builtin.RaymarchShader.comp.glsl. texture_name is a
  // TextureSystem name, exactly like every other texture reference in this
  // engine (Material::diffuse_map_name, etc.) -- it resolves to
  // assets/textures/<texture_name>.png, not an arbitrary filesystem path.
  // Waits for the device to go idle first (like rebake()/remove_scene()):
  // this rewrites render_set_'s skybox binding directly, which a still-
  // in-flight render_to() dispatch could be reading through.
  void set_skybox(std::string_view texture_name);

  // Disables the skybox (falls back to the flat gradient background) and
  // releases the texture reference set_skybox() acquired. No-op if no
  // skybox is currently enabled.
  void disable_skybox();

private:
  // Common to set_skybox()/disable_skybox()/the constructor's initial
  // binding -- (re-)points render_set_'s skybox binding (12) at texture.
  // Doesn't itself wait for device idle -- callers do that first.
  void write_skybox_binding(VulkanTexture &texture);

  // Reads every currently-registered Geometry from GeometrySystem, uploads
  // it to primitive_buffer_/primitive_colour_buffer_ and the render set's
  // scene_textures array, then calls voxelize() to bake it. Called once
  // from the constructor, and again from rebake() (above) whenever the
  // registered set changes after that.
  void rebuild_static_scene();

  // Dispatches the voxelize (pass 1) compute shader once, via a one-time
  // command buffer, and waits for it to complete before returning — so by
  // the time it returns, the field is fully baked against the first
  // layer_count entries currently in layer_buffer_ (see
  // rebuild_static_scene(), the only caller).
  void voxelize(u32 layer_count);

  // Dispatches the probe-bake (pass 2) compute shader kProbeBounceCount
  // times, one per light bounce, each via its own one-time command buffer
  // (mirroring voxelize()'s synchronous-wait style) -- called right after
  // voxelize() from rebuild_static_scene(), since gather rays need the
  // field voxelize() just baked. See Builtin.ProbeBake.comp.glsl for the
  // technique itself.
  void bake_probes(u32 light_count);

  static void transition_image(VkCommandBuffer cmd, VkImage image,
                               VkImageLayout old_layout,
                               VkImageLayout new_layout,
                               VkAccessFlags src_access,
                               VkAccessFlags dst_access,
                               VkPipelineStageFlags src_stage,
                               VkPipelineStageFlags dst_stage);

  VulkanContext *context_;

  // Pass 1: SDF -> sparse voxel field. Run once, at construction.
  VulkanShaderModule voxelize_stage_;
  VkDescriptorSetLayout voxelize_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet voxelize_set_ = VK_NULL_HANDLE;
  std::optional<VulkanComputePipeline> voxelize_pipeline_;

  // The sparse voxel field: a coarse indirection grid of brick indices
  // (kCoarseDim^3 i32s, -1 = no brick) pointing into a pool of fixed-size
  // bricks (kMaxBricks * kBrickVoxelCount f32 distances), allocated by an
  // atomic counter during voxelize(). See vulkan_raymarch_shader.cpp for the
  // dimension constants — they must match the ones hardcoded in the two
  // .comp.glsl shaders exactly, since there's no shared config between them.
  std::optional<VulkanBuffer> indirection_buffer_;
  std::optional<VulkanBuffer> brick_pool_buffer_;
  std::optional<VulkanBuffer> brick_counter_buffer_;

  // GeometrySystem's registered static primitives, uploaded for the
  // voxelize pass to bake (position/type/params per primitive, grouped
  // contiguously by layer) -- see rebuild_static_scene().
  std::optional<VulkanBuffer> primitive_buffer_;
  // One entry per registered layer (operation/smoothness/primitive range
  // into primitive_buffer_) -- see GeometrySystem::SceneLayer and
  // rebuild_static_scene().
  std::optional<VulkanBuffer> layer_buffer_;
  // Which primitive is nearest at each brick, baked by voxelize() -- lets
  // the render pass pick a material for a hit without re-evaluating every
  // primitive itself.
  std::optional<VulkanBuffer> brick_primitive_buffer_;
  // Each static primitive's material diffuse tint, parallel to
  // primitive_buffer_ -- read directly by the render pass (a plain buffer
  // read, unlike the textures below, has no "dynamically uniform index"
  // restriction to work around).
  std::optional<VulkanBuffer> primitive_colour_buffer_;
  // 4 consecutive entries per primitive (params.x/y/z/extra_param, in that
  // order), each a compiled "parametric attribute" formula -- see
  // Geometry::param_expressions and evaluate_expr() in
  // Builtin.RaymarchVoxelize.comp.glsl. An entry's instruction_count == 0
  // means that slot has no formula; the voxelize shader falls back to
  // primitive_buffer_'s plain constant for it. Voxelize-only, like
  // primitive_buffer_/layer_buffer_ -- the render pass never evaluates
  // primitive_sdf() itself.
  std::optional<VulkanBuffer> param_expr_buffer_;
  // GeometrySystem's registered lights (see GeometrySystem::light_snapshot()),
  // read directly by the render pass's per-pixel lighting loop -- unlike
  // primitives, lights have no voxelize-time role at all (they don't affect
  // the field's shape), so this is the render pass's only light-related
  // binding.
  std::optional<VulkanBuffer> light_buffer_;

  // Pass 2: GI probe bake -- runs kProbeBounceCount times per rebake(),
  // right after voxelize(). Its own shader/pipeline/descriptor set (a
  // small, fixed 7-binding layout -- see vulkan_raymarch_shader.cpp),
  // separate from voxelize_set_/render_set_ since none of its bindings are
  // shared 1:1 with either (some buffers are the same underlying objects,
  // e.g. indirection_buffer_, but the set/binding-index pairing differs).
  VulkanShaderModule probe_bake_stage_;
  VkDescriptorSetLayout probe_bake_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet probe_bake_set_ = VK_NULL_HANDLE;
  std::optional<VulkanComputePipeline> probe_bake_pipeline_;

  // The baked probe grid, double-buffered so each bounce can read the
  // previous bounce's full result while writing this bounce's -- see
  // bake_probes()'s ping-pong loop. Whichever buffer holds the *final*
  // bounce's result (determined by kProbeBounceCount's parity, resolved
  // once in bake_probes()) is what render_set_'s ProbeBuffer binding
  // points at; the other is dead until the next rebake().
  std::optional<VulkanBuffer> probe_buffer_a_;
  std::optional<VulkanBuffer> probe_buffer_b_;

  // Pass 3: repeating — marches rays against the baked field each frame.
  // A single descriptor set: none of its bindings change per frame (the
  // camera is a push constant instead), so there's no need for one set per
  // swapchain image.
  VulkanShaderModule render_stage_;
  VkDescriptorSetLayout render_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet render_set_ = VK_NULL_HANDLE;
  std::optional<VulkanComputePipeline> render_pipeline_;

  // Each static primitive's Material::pixelation_exempt, parallel to
  // primitive_colour_buffer_ -- read by the render pass and written into
  // output_image_'s alpha channel (see rebuild_static_scene()) for the
  // post-process pass's pixelation step to read.
  std::optional<VulkanBuffer> pixelation_exempt_buffer_;

  // Pass 4a: bright-pass + horizontal half of the bloom blur (see the
  // class comment). Reads output_image_, writes bloom_temp_image_ below.
  VulkanShaderModule bloom_blur_h_stage_;
  VkDescriptorSetLayout bloom_blur_h_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet bloom_blur_h_set_ = VK_NULL_HANDLE;
  std::optional<VulkanComputePipeline> bloom_blur_h_pipeline_;

  // Pass 4b: finishes the bloom blur vertically, then composites bloom +
  // vignette + pixelation and writes the final frame. Reads output_image_
  // and bloom_temp_image_, writes post_process_image_ below.
  VulkanShaderModule post_composite_stage_;
  VkDescriptorSetLayout post_composite_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet post_composite_set_ = VK_NULL_HANDLE;
  std::optional<VulkanComputePipeline> post_composite_pipeline_;

  // Backs every descriptor set above.
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;

  VulkanImage output_image_{};
  // Half-resolution (both dimensions) -- bloom is a soft, low-frequency
  // effect, so blurring/storing it at quarter the pixel count of
  // output_image_ is imperceptible in the final (upsampled-via-bilinear-
  // like-taps) composite and a quarter the cost. Recreated alongside
  // output_image_ on resize (see on_resized()).
  VulkanImage bloom_temp_image_{};
  // Full-resolution -- the actual final frame, copied to the swapchain
  // image in place of output_image_ (see render_to()).
  VulkanImage post_process_image_{};

  // See set_selected_primitive() above.
  i32 selected_primitive_index_ = -1;

  // See set_grid_visible() above.
  b8 grid_visible_ = false;

  // See set_bloom_enabled()/set_vignette_enabled()/
  // set_pixelation_enabled()/set_pixelation_block_size() above.
  b8 bloom_enabled_ = true;
  b8 vignette_enabled_ = true;
  b8 pixelation_enabled_ = false;
  u32 pixelation_block_size_ = 6;

  // How many of light_buffer_'s kMaxLights slots are actually populated,
  // and the scene-wide ambient factor -- both set by rebuild_static_scene()
  // (from GeometrySystem::light_snapshot()/ambient()) and sent as push
  // constants every render_to() call, the same way camera state is.
  i32 light_count_ = 0;
  f32 ambient_ = 0.15f;

  // How many layers rebuild_static_scene() last uploaded -- the render
  // pass's per-pixel material re-evaluation (scene_map() in
  // Builtin.SdfSceneCommon.inc.glsl) folds exactly this many LayerBuffer
  // entries, matching what the voxelize pass baked.
  i32 layer_count_ = 0;

  // The [volumetric_start_, volumetric_start_ + volumetric_count_) range
  // rebuild_static_scene() last appended GeometrySystem's registered
  // volumetrics at, in primitive_buffer_/scene_diffuse_colours/
  // scene_textures -- outside every GpuLayer's range, so the opaque scene
  // never sees them. Sent as push constants every render_to() call for
  // accumulate_volumetrics() in Builtin.RaymarchShader.comp.glsl to iterate.
  i32 volumetric_start_ = 0;
  i32 volumetric_count_ = 0;

  // Seconds since construction, accumulated every render_to() call -- see
  // PushConstants::time in vulkan_raymarch_shader.cpp, which drives
  // accumulate_volumetrics()'s scrolling texture animation.
  f32 elapsed_time_ = 0.0f;

  // See set_skybox()/disable_skybox() above. skybox_texture_name_ is only
  // meaningful while skybox_enabled_ is true -- it's what disable_skybox()
  // releases through TextureSystem.
  bool skybox_enabled_ = false;
  std::string skybox_texture_name_;

  bool valid_ = false;
};
