#pragma once
#include "../../camera.h"
#include "../vulkan_buffer.h"
#include "../vulkan_compute_pipeline.h"
#include "../vulkan_shader_module.h"
#include "../vulkan_types.inl"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
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

  // Resolves a primitive's SdfPrimitiveDef::name/Geometry::name to the same
  // scene_textures/scene_diffuse_colours index set_selected_primitive()
  // above takes -- for a caller (tools/sdf_editor) that only knows a
  // primitive by name/position-within-its-layer and needs the GPU-flattened
  // index rebuild_static_scene() actually assigned it (which, unlike a
  // layer's ordinal position, depends on GeometrySystem::snapshot()'s
  // unspecified iteration order and so can't be recomputed from an SdfScene
  // alone). -1 if name isn't currently uploaded (e.g. stale, or dropped for
  // exceeding kMaxScenePrimitives).
  i32 gpu_index_for_primitive(const std::string &name) const noexcept {
    auto it = primitive_gpu_index_by_name_.find(name);
    return it != primitive_gpu_index_by_name_.end() ? static_cast<i32>(it->second)
                                                    : -1;
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

  // Luminance above which a pixel starts contributing to bloom (see
  // Builtin.BloomBlurH.comp.glsl's bright_pass()) -- lower reads as "more
  // of the scene blooms," not just the brightest highlights. Same push-
  // constant mechanics as set_bloom_enabled(); has no visible effect while
  // bloom is disabled. Clamped to non-negative (bright_pass() subtracts
  // this from luminance, so a negative threshold would make every pixel
  // bloom regardless of brightness).
  void set_bloom_threshold(f32 threshold) noexcept {
    bloom_threshold_ = std::max(threshold, 0.0f);
  }

  // How strongly the blurred bright-pass adds back on top of the scene
  // (see Builtin.PostComposite.comp.glsl's `base_colour + bloom *
  // bloom_intensity`) -- higher reads as a stronger glow. Same mechanics
  // as set_bloom_threshold() above. Clamped to non-negative (a negative
  // intensity would subtract the bloom instead of adding it).
  void set_bloom_intensity(f32 intensity) noexcept {
    bloom_intensity_ = std::max(intensity, 0.0f);
  }

  // How much the vignette darkens the screen edges -- 0 = no visible
  // effect even while vignette is enabled, 1 = fully black at the
  // furthest corner. Same mechanics as set_bloom_threshold() above.
  // Clamped to [0, 1]: the shader computes `1 - strength * falloff`, so
  // above 1 the darkened corners go negative -- past black, not just
  // darker.
  void set_vignette_strength(f32 strength) noexcept {
    vignette_strength_ = std::clamp(strength, 0.0f, 1.0f);
  }

  // Normalized distance from screen center (0 = center, 1 = a full-height/
  // width edge, sqrt(2) = the furthest corner) where the vignette's
  // falloff begins -- smaller pulls the darkening in toward the center,
  // larger pushes it out toward the corners. Same mechanics as
  // set_bloom_threshold() above. Clamped to [0, sqrt(2)]: the shader feeds
  // this straight into smoothstep(vignette_radius, sqrt(2), dist), which
  // is only well-defined for edge0 <= edge1.
  void set_vignette_radius(f32 radius) noexcept {
    vignette_radius_ = std::clamp(radius, 0.0f, 1.41421356f);
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

  // Scales pass 3/4's internal render targets (output_image_/
  // bloom_temp_image_/post_process_image_) relative to the actual
  // framebuffer size -- e.g. 0.75 renders/post-processes at 75% linear
  // resolution (~56% the pixel count), then render_to() upscales the
  // result into swapchain_image via a linear blit instead of a 1:1 copy.
  // Every pass here costs O(pixel count), so this is the lever for trading
  // sharpness for frame rate on a large/fullscreen window instead of
  // paying full native-resolution cost unconditionally -- see
  // recreate_render_target_images(). Takes effect immediately (waits for
  // the device to go idle, like set_skybox()), not just on the next
  // resize. Clamped to (0, 1] -- 0 would create zero-sized images.
  void set_render_scale(f32 scale) noexcept;
  f32 render_scale() const noexcept { return render_scale_; }

private:
  // Destroys and recreates output_image_/bloom_temp_image_/
  // post_process_image_ at render_width_/render_height_ (recomputed here
  // from base_width_/base_height_ and render_scale_, floored to at least
  // 1x1). Shared by the constructor, on_resized(), and set_render_scale()
  // -- all three need identical sizing so the images, the dispatches
  // reading/writing them, and the descriptor sets pointing at them never
  // disagree. Doesn't touch descriptor sets itself: the constructor's
  // first call runs before any exist, while on_resized()/
  // set_render_scale() must re-point their already-live sets at the new
  // image views afterward (see their callers).
  void recreate_render_target_images();

  // Re-points render_set_/bloom_blur_h_set_/post_composite_set_'s image
  // bindings at output_image_/bloom_temp_image_/post_process_image_'s
  // current views -- called right after recreate_render_target_images()
  // by on_resized()/set_render_scale(), never by the constructor (nothing
  // references those sets yet at that point).
  void rebind_render_target_descriptors();

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

  // Records the voxelize (pass 1) compute dispatch into cmd (does not
  // allocate or submit it -- see rebuild_static_scene(), the only caller,
  // which records this back-to-back with bake_probes() into a single
  // command buffer/submission instead of a separate one per pass/bounce,
  // since each one-time command buffer's allocate+submit+wait was paying
  // fixed per-round-trip overhead on top of the actual GPU work, one for
  // every rebake()). Ends with a barrier making indirection_buffer_/
  // brick_pool_buffer_/brick_primitive_buffer_'s writes visible to
  // bake_probes()'s reads -- previously implicit (a separate command
  // buffer's own submit+vkQueueWaitIdle), now explicit since both share
  // one command buffer with no queue idle between them.
  //
  // max_smoothness is the largest LayerOperation smoothness across every
  // currently-registered layer -- forwarded to the shader as a push
  // constant, where it widens scene_map()'s per-voxel cull_radius (see
  // Builtin.RaymarchVoxelize.comp.glsl's own comment) so a smooth blend
  // can't be truncated by a primitive the cull pre-check wrongly dropped.
  void voxelize(VulkanCommandBuffer &cmd, u32 layer_count, f32 max_smoothness);

  // Reads back brick_counter_buffer_ (see voxelize()) and warns if the
  // brick pool overflowed. Split out from voxelize() itself because the
  // counter isn't valid to read until the command buffer voxelize() was
  // recorded into has actually finished executing -- call once after that
  // submission's wait, not from inside voxelize().
  void check_brick_overflow();

  // Records the probe-bake (pass 2) compute dispatches -- kProbeBounceCount
  // of them, one per light bounce -- into cmd (does not allocate or submit
  // it; see voxelize()'s comment above for why). Alternates between
  // probe_bake_set_ and probe_bake_set_odd_ (each a fixed, never-rewritten
  // ping-pong binding of Prev/CurrProbeBuffer -- see their declarations
  // below) rather than rewriting one descriptor set's bindings between
  // bounces: since every bounce now shares one command buffer/submission,
  // rewriting a descriptor set already referenced by not-yet-submitted
  // commands would make every earlier bind observe only the final
  // rewrite once the GPU actually executes them, silently breaking the
  // ping-pong. Uses a compute-to-compute buffer barrier between bounces
  // instead (each bounce's write must be visible before the next reads
  // it), same reasoning as voxelize()'s trailing barrier.
  void bake_probes(VulkanCommandBuffer &cmd, u32 light_count);

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
  // Two fixed ping-pong bindings of the same layout, written once at
  // construction and never rewritten -- see bake_probes()'s comment for
  // why a single rewritten-per-bounce set stopped being safe once every
  // bounce shares one command buffer. probe_bake_set_ binds
  // Prev=probe_buffer_a_/Curr=probe_buffer_b_ (bounces 0, 2, 4, ...);
  // probe_bake_set_odd_ binds the reverse (bounces 1, 3, 5, ...).
  VkDescriptorSet probe_bake_set_ = VK_NULL_HANDLE;
  VkDescriptorSet probe_bake_set_odd_ = VK_NULL_HANDLE;
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

  // Each static (and volumetric) primitive's effective texture offset/
  // rotation -- xyz = material->texture_offset * geometry.
  // texture_offset_scale (world units), w = material->texture_rotation
  // (radians) -- parallel to primitive_colour_buffer_, read by the render
  // pass's triplanar sampling. A separate binding rather than widening
  // primitive_colour_buffer_'s existing vec4 (rgb=tint, a=texture_scale):
  // that buffer's exact 1-vec4-per-primitive layout is shared verbatim
  // with Builtin.ProbeBake.comp.glsl's own ScenePrimitiveColours binding,
  // which only ever reads .rgb for flat albedo -- restriding it here would
  // silently feed that shader's simple per-primitive indexing corrupted
  // data instead. Keeping texture transform in its own buffer touches
  // nothing outside this pass.
  std::optional<VulkanBuffer> tex_transform_buffer_;

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
  // Half-resolution (both dimensions, of render_width_/render_height_ --
  // not necessarily the swapchain's) -- bloom is a soft, low-frequency
  // effect, so blurring/storing it at quarter the pixel count of
  // output_image_ is imperceptible in the final (upsampled-via-bilinear-
  // like-taps) composite and a quarter the cost. Recreated alongside
  // output_image_ on resize (see on_resized()).
  VulkanImage bloom_temp_image_{};
  // Same resolution as output_image_ -- the actual final frame, upscaled
  // (see set_render_scale()) into the swapchain image in render_to().
  VulkanImage post_process_image_{};

  // Framebuffer size last passed to on_resized() (or, before the first
  // resize, the constructor) -- render_width_/render_height_ below are
  // derived from these and render_scale_. Stored so set_render_scale() can
  // recompute/recreate the render targets on demand without needing the
  // caller to supply the framebuffer size again.
  u32 base_width_ = 0;
  u32 base_height_ = 0;
  // output_image_/bloom_temp_image_/post_process_image_'s actual size --
  // max(1, base_{width,height}_ * render_scale_). render_to()'s dispatch
  // group counts and the bloom/post-composite passes' full_width/
  // full_height push constants use these, not the width/height parameters
  // it's called with (the swapchain's size) -- see
  // recreate_render_target_images().
  u32 render_width_ = 0;
  u32 render_height_ = 0;
  // See set_render_scale()/render_scale() above.
  f32 render_scale_ = 1.0f;

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

  // See set_bloom_threshold()/set_bloom_intensity()/set_vignette_strength()/
  // set_vignette_radius() above. Defaults match kDefaultBloomThreshold etc.
  // in vulkan_raymarch_shader.cpp (deliberately mild).
  f32 bloom_threshold_ = 0.85f;
  f32 bloom_intensity_ = 0.35f;
  f32 vignette_strength_ = 0.35f;
  f32 vignette_radius_ = 0.55f;

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

  // See gpu_index_for_primitive() above -- rebuilt from scratch by every
  // rebuild_static_scene() call (construction, and every rebake()).
  std::unordered_map<std::string, u32> primitive_gpu_index_by_name_;

  bool valid_ = false;
};
