#pragma once
#include "../../../systems/chunk_types.h"
#include "../../camera.h"
#include "../vulkan_buffer.h"
#include "../vulkan_commandbuffer.h"
#include "../vulkan_compute_pipeline.h"
#include "../vulkan_fence.h"
#include "../vulkan_shader_module.h"
#include "../vulkan_types.inl"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class VulkanCommandBuffer;
class VulkanTexture;
struct Geometry;

// Hashes the {level, key} pairs update_streaming()'s pending_forced_
// evictions_/pending_forced_eviction_keys_ store -- ChunkKey alone (via
// ChunkKeyHash, chunk_types.h) never encodes which clip level it came
// from (chunks_touched_by() always stamps ChunkKey::level as 0; the real
// level lives in the pair's own first element), so the two must be
// combined here rather than just delegating to ChunkKeyHash.
struct PendingForcedEvictionHash {
  size_t operator()(const std::pair<u32, ChunkKey> &entry) const noexcept {
    return std::hash<u32>{}(entry.first) * 1099511628211ull ^
        ChunkKeyHash{}(entry.second);
  }
};

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
// Phase 6: the caller-facing shape of one chunk in a voxelize_chunk_
// batch() call -- NOT a direct mirror of any single GPU-side buffer
// layout (see voxelize_chunk_batch()'s own .cpp comment for why it's
// split into two parallel arrays, batch_chunk_slot[]/batch_chunk_data[]
// in Builtin.ChunkVoxelize.comp.glsl, before upload: a plain packed array
// of this 5-scalar struct would misalign under std430's struct-array-
// stride rounding). A free-standing struct (not nested in the class
// below, unlike the .cpp's other Gpu*/push-constant mirrors) since
// voxelize_chunk_batch()'s own declaration needs the complete type here,
// not just in vulkan_raymarch_shader.cpp where it's actually split/
// uploaded.
struct GpuChunkBatchEntry {
  i32 chunk_slot;
  f32 chunk_world_min_x;
  f32 chunk_world_min_y;
  f32 chunk_world_min_z;
  f32 chunk_cell_size;
};

// How render_to() uses the chunked field's baked surface-point cloud (see
// Builtin.ChunkPointSplat.comp.glsl's header comment for the technique).
// Values must match SPLAT_MODE_* in Builtin.RaymarchShader.comp.glsl
// exactly -- they're passed straight through as a push constant.
//
// Only meaningful while the chunked field is enabled (see
// set_chunked_field_enabled()); the fixed-cube field bakes no points, so
// render_to() skips the splat prepass entirely there regardless of this.
enum class SplatMode : i32 {
  // No splat prepass. Every primary ray marches from the camera, exactly
  // as this renderer always did -- the safe fallback if anything about the
  // point cloud is ever in doubt.
  Off = 0,
  // Splat, then start each primary ray just short of its pixel's nearest
  // splat instead of at the camera. The image is exactly what Off
  // produces (the starts are conservative, and an uncovered pixel just
  // starts at 0) -- only the cost of crossing empty space changes.
  Prime = 1,
  // Shade the winning splat directly: no primary march at all for a
  // covered pixel, which is the actual Dreams-style renderer. Pixels no
  // splat covers fall back to Prime's behavior, so the image stays
  // complete (no holes, no dependence on temporal accumulation) -- see
  // main()'s own comment in Builtin.RaymarchShader.comp.glsl.
  Visibility = 2,
};

class VulkanRaymarchShader {
public:
  // M2a: mirrors SculptBrushShape (engine/src/renderer/renderer_types.inl)
  // exactly, and MUST match Builtin.SculptStamp.comp.glsl's SHAPE_*
  // constants value-for-value too -- see the static_assert pinning this
  // enum against SculptBrushShape in vulkan_backend.cpp (same pattern
  // SplatMode/RendererSplatMode already use just above). Declared here,
  // not pulled from renderer_types.inl directly, so this still-deep-
  // Vulkan-internals header stays decoupled from the frontend's public API
  // header, matching SplatMode's own established precedent.
  enum class BrushShape : i32 {
    Sphere = 0,
    Box,
    Plane,
    Torus,
    CappedCylinder,
    CappedCone,
    RoundBox,
    BoxFrame,
    Octahedron,
    Pyramid,
    HexPrism,
    RoundCone,
    Capsule,
    Link,
    Ellipsoid,
  };

  // M2a: mirrors SculptBrushOp exactly -- see BrushShape's comment above
  // for why this is a separate, statically-asserted mirror rather than a
  // direct reuse of the frontend type.
  enum class BrushOp : i32 {
    Union = 0,
    Subtraction = 1,
  };


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

  // update_streaming() -- the camera-driven chunk streaming entry point --
  // was removed in the M0 CSG deletion pass (see /home/h-i/.claude/plans/
  // joyful-skipping-giraffe.md): it drove chunk_streaming_levels_
  // (ChunkStreamingManager, also removed) against GeometrySystem's
  // registered CSG primitives, which no longer exist.

  // Phase 5: recenters gi_cascade_center_ (in discrete whole-cell steps,
  // classic terrain-clipmap texture behavior -- see
  // kGiCascadeRecenterThreshold's comment, .cpp) if the camera has drifted
  // far enough from it, and if so (re)starts an amortized cascade bake --
  // ONE bake_gi_cascade_bounce() per frame for kProbeBounceCount frames,
  // not every bounce at once (see gi_cascade_next_bounce_'s comment) --
  // recording each frame's bounce into cmd. Call once per frame, right after
  // update_streaming() (a cascade bake gathers against the chunked field,
  // so it must be recorded after that frame's chunk load/evict budget --
  // see this method's .cpp comment). A no-op most frames (the cascade only
  // needs to move every kGiCascadeRecenterThreshold world units of camera
  // travel, not every frame).
  void update_gi_cascade(VulkanCommandBuffer &cmd, glm::vec3 camera_pos);

  // set_selected_primitive()/gpu_index_for_primitive() -- the selection-
  // outline API -- were removed in the M0 CSG deletion pass along with
  // the CSG primitive scene they indexed into.

  // Shows/hides the reference grid (the subdivided y=0 plane the render
  // pass composites analytically -- see apply_reference_grid() in
  // Builtin.RaymarchShader.comp.glsl). Same mechanics as
  // set_selected_primitive(): just a push constant, applied the very next
  // render_to() call. Hidden by default -- it's an editor aid, so games
  // never see it unless they opt in; tools/sdf_editor turns it on.
  void set_grid_visible(b8 visible) noexcept { grid_visible_ = visible; }

  // Phase 4: switches render_to()'s primary hit-test/normals/contact-AO/
  // shadows (see Builtin.RaymarchShader.comp.glsl's sample_active_field()/
  // chunked_shadow_march()) from the fixed-cube field every caller has
  // always used onto the chunked/streamed field (see update_streaming(),
  // reset_chunked_field()) instead. Just a push constant, takes effect the
  // very next render_to() call, no rebake needed. Off by default -- so
  // tools/sdf_editor and games/SH are completely unaffected unless a
  // caller explicitly opts in (a game that actually wants to free-roam
  // through a streamed world, once update_streaming() is being driven by
  // its camera every frame -- see VulkanRendererBackend::begin_frame()).
  void set_chunked_field_enabled(b8 enabled) noexcept {
    chunked_field_enabled_ = enabled;
  }

  // Re-arms chunk cache pre-warming, so the next streaming tick bakes the
  // newly loaded geometry into the cache. Called by
  // VulkanRendererBackend::load_scene() -- NOT by reconcile_scene(), since
  // an edit touches a handful of chunks and must not stall for a full
  // scene sweep. No effect unless KENGINE_PREWARM_CACHE asked for it.
  void request_cache_prewarm() noexcept { prewarm_done_ = false; }

  // How many chunks the last update_streaming() still wanted around the
  // camera -- not yet loaded, or loaded but still baking -- across every
  // clip level. 0 once the field around the camera is complete; jumps up
  // the frame after the camera is moved somewhere new, and drains as the
  // chunks there are restored from the cache or baked.
  u32 streaming_backlog() const noexcept { return streaming_backlog_; }

  // is_dynamic_primitive()/set_dynamic_primitive()/dynamic_primitive_name()/
  // update_primitive_transform() -- the interactive-primitive-drag API --
  // were removed in the M0 CSG deletion pass along with the CSG primitive
  // scene they dragged.

  // Selects how render_to() uses the chunked field's baked point cloud --
  // see SplatMode above for what each value does. Just a push constant
  // (plus whether the splat prepass gets recorded at all), applied the very
  // next render_to() call, no rebake needed. Defaults to Prime: it's a
  // pure win where the chunked field is active (same image, less empty
  // space marched) and inert everywhere else, whereas Visibility changes
  // how surfaces are resolved and so stays opt-in.
  void set_splat_mode(SplatMode mode) noexcept { splat_mode_ = mode; }
  SplatMode splat_mode() const noexcept { return splat_mode_; }

  // Temporal anti-aliasing (see Builtin.TaaResolve.comp.glsl). On by
  // default: with it the renderer can afford the stochastic techniques the
  // later steps need (one-ray AO, undersampled many-light shadow maps,
  // stochastic LOD), and without it every one of those has to be exact.
  // Turning it off costs nothing but a full-resolution copy -- the resolve
  // pass still runs, with a blend factor of 1.0, so no descriptor set has
  // to be rewritten when this is toggled.
  // Imperfect shadow maps for local point lights (step 5 -- see Builtin.
  // ChunkShadowSplat.comp.glsl). On by default: a lookup per light replaces
  // a march per light, which is what makes dozens of shadowed lights
  // affordable. Directional lights keep the marched shadow either way.
  void set_ism_enabled(b8 enabled) noexcept { ism_enabled_ = enabled; }
  b8 ism_enabled() const noexcept { return ism_enabled_; }

  // Stochastic ambient occlusion over the binary voxel cascades (step 6 --
  // see Builtin.StochasticAo.comp.glsl). On by default. One ray per pixel
  // per frame, so it depends on TAA to be usable; turning TAA off and this
  // on gives a very noisy image, which is why the shading pass falls back
  // to the marched contact AO whenever this pass did not run.
  void set_ao_enabled(b8 enabled) noexcept { ao_enabled_ = enabled; }
  b8 ao_enabled() const noexcept { return ao_enabled_; }

  // Samples the live chunked field at one world point and logs what it
  // finds -- the diagnostic that separates "the bake produced wrong data"
  // from "the bake is right and the splats are stale". Self-contained: it
  // records, submits and waits on its own command buffer, so a caller needs
  // nothing but a position. Debug-only, and deliberately synchronous.
  void debug_probe_field(glm::vec3 world_pos, glm::vec3 camera_pos);

  void set_taa_enabled(b8 enabled) noexcept;
  b8 taa_enabled() const noexcept { return taa_enabled_; }
  // Drops the accumulated history for one frame. Anything that invalidates
  // the relationship between last frame's pixels and this frame's world
  // must call this: a resize, a floating-origin recenter (every render-
  // space position moves), or a rebake that changes what the field
  // contains. Cheaper and more honest than trying to reproject through the
  // discontinuity.
  void invalidate_temporal_history() noexcept { history_valid_ = false; }

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

  // M5 (Finishes): configures material slot `slot` (must be >= 2 -- slots 0
  // and 1 are reserved, see ensure_default_sculpt_material()'s own comment
  // for what they hold and why overwriting them would corrupt every
  // existing stamp/spray) with a base colour/roughness/metallic/waxy
  // "Finish", using the EXACT SAME packing convention ensure_default_
  // sculpt_material() already established for slots 0/1 (ScenePrimitive
  // Colours/ScenePrimitiveMaterialScalars/ScenePrimitiveTransmission in
  // Builtin.DeferredShade.comp.glsl -- read/write consistently, don't
  // invent a second encoding):
  //   primitive_colour_buffer_[slot]    = (colour.rgb, texture_scale=1)
  //   material_scalar_buffer_[slot].x   = metallic*0.5 (exempt flag always
  //                                        0 for an authored Finish -- see
  //                                        the x-packing comment on
  //                                        ScenePrimitiveMaterialScalars)
  //   material_scalar_buffer_[slot].y   = 1.0 (ordinary bump strength --
  //                                        no bump map is ever bound for a
  //                                        sculpt material, so this value
  //                                        has no visible effect; kept
  //                                        consistent with slot 0/1's own
  //                                        default rather than 0, in case a
  //                                        future milestone binds one)
  //   material_scalar_buffer_[slot].z   = 1.0 (casts_shadow)
  //   material_scalar_buffer_[slot].w   = roughness
  //   transmission_buffer_[slot].x      = waxy ? 1.0 : 0.0
  //   transmission_buffer_[slot].y      = M6 (Flecks): fleck_pattern, a
  //                                        0-15 preset id written as a
  //                                        rounded float (ignored, along
  //                                        with .w, whenever .z is 0)
  //   transmission_buffer_[slot].z      = M6 (Flecks): fleck_strength,
  //                                        clamped to [0, 1] -- 0 (the
  //                                        default for every slot this
  //                                        call never touches, and for
  //                                        every slot that existed before
  //                                        M6) means "no Fleck", read by
  //                                        Builtin.FleckPattern.inc.glsl's
  //                                        two entry points as the gate
  //                                        that makes them a no-op
  //   transmission_buffer_[slot].w      = M6 (Flecks): fleck_scale, world
  //                                        units per pattern cycle (only
  //                                        meaningful when .z > 0)
  // transmission_buffer_ is where both the M5 "waxy" flag and the M6 Fleck
  // params live because it is otherwise completely dead for every opaque
  // sculpt material: Builtin.DeferredShade.comp.glsl declares
  // ScenePrimitiveTransmission (binding 28) but the whole glass/
  // transmission shading block that used to READ it was deleted in the M0
  // CSG pass (confirmed -- grep the shader for "transmission[" outside its
  // own declaration: nothing before M5), so every component of every slot
  // was free going into M5, which claimed only .x. M6 investigated
  // reusing tex_transform_buffer_'s own apparently-free components instead
  // (it packs texture UV offset/rotation, and no sculpt material has ever
  // set a non-zero one) and found that NOT safe at the time: Builtin.
  // DeferredShade.comp.glsl unconditionally uses tex_transform_buffer_
  // [slot] to compute the UV every sculpt material's triplanar diffuse
  // sample reads, and at that point every sculpt material's scene_textures
  // [slot] was bound to texture_system->default_texture() -- a genuinely
  // visible procedural checkerboard, NOT a flat/uniform fallback. Writing
  // anything non-zero into tex_transform_buffer_ for a Fleck-carrying slot
  // would have shifted/rotated that checkerboard out from under it, a
  // visible side effect entirely unrelated to the Fleck itself. A later M5
  // visual-quality pass rebound every slot set_material() touches to
  // flat_texture() instead (see this function's own definition for why --
  // the checkerboard was swamping every Finish's actual roughness/metallic/
  // waxy look), so tex_colour is spatially uniform for those slots now too
  // -- but the conclusion is unchanged, since a UV offset/rotation still
  // has no reader-visible effect on a spatially uniform texture.
  // transmission_buffer_'s .y/.z/.w have no such reader anywhere (the glass
  // block that used to read any component of it is gone), so they remain
  // the simplest home regardless.
  //
  // No-op (logs a warning, touches nothing) if slot < 2 or
  // slot >= kMaxScenePrimitives -- the caller (tools/modelling_software_v2)
  // is expected to allocate slots itself starting from 2, since this is a
  // hand-authored sculpting session, not thousands of auto-generated
  // materials, and a fixed cap needs no fancier allocator. Host-visible
  // direct load_data() writes, same as ensure_default_sculpt_material() --
  // no staging buffer, no device-idle wait, cheap enough to call once per
  // Finish/Fleck/colour pick in the tool (not per-stamp). fleck_pattern is
  // only meaningful when fleck_strength > 0 (pass 0/0.0f/1.0f for "no
  // Fleck", what every Finish preset did before this milestone existed).
  void set_material(i32 slot, glm::vec3 colour, f32 roughness, f32 metallic,
                    bool waxy, i32 fleck_pattern, f32 fleck_strength,
                    f32 fleck_scale);

  // M7 polish: live brush-cursor "ghost" preview -- shows the ACTUAL
  // shaded material (colour/roughness/metallic/waxy Finish, Fleck pattern)
  // the next stamp_shape() call at `center` would place, not just the
  // wireframe outline tools/modelling_software_v2's scene_viewport.cpp drew
  // before this existed. `shape_type`/`params`/`op`/`material_slot` use the
  // EXACT SAME per-shape layout and enum values as stamp_shape() -- see its
  // own header comment -- so the preview can never drift from what a real
  // stamp right now would place. Writes a single small UBO (brush_preview_
  // buffer_ above) that Builtin.DeferredShade.comp.glsl's shading pass
  // sphere-traces ONE analytic shape against per pixel (eval_brush_preview_
  // sdf(), mirroring Builtin.SculptStamp.comp.glsl's eval_brush_shape()) --
  // no voxel field involved, since a preview never touches real geometry.
  // `enabled = false` is the cheap early-out: the shader's very first read
  // of the uniform skips the trace entirely, so calling this with false
  // every frame the preview is hidden costs one float compare, not a wasted
  // march. Cheap, synchronous, NOT device-stalling (a direct host-visible
  // load_data() write, same idiom as set_material() above) -- unlike the
  // sculpt-stamp family, this IS meant to be called every frame the
  // preview is visible (scene_viewport.cpp's tick()), not just on an
  // authoring-time action.
  void set_brush_preview(bool enabled, glm::vec3 center, BrushShape shape_type,
                         glm::vec4 params, BrushOp op, i32 material_slot);

  // Minimal standing-light registration -- fills the next free slot of
  // light_buffer_ and bumps light_count_, the same "direct host-visible
  // buffer write, no device-idle wait" idiom set_material() above uses, NOT
  // a rebake (rebake() has had nothing to do since rebuild_static_scene()
  // was deleted in the M0 CSG pass -- see rebake()'s own comment). Note
  // this deliberately bypasses GeometrySystem::acquire_light()/
  // light_snapshot(): that CPU-side registry still exists and still
  // compiles, but nothing has called light_snapshot() into light_buffer_
  // since rebuild_static_scene() (its only bridge) was removed, so it is
  // currently a dead API with no live writer on either end. Reinstating
  // that bridge (a per-frame or per-change GeometrySystem->GPU sync) is
  // more machinery than a tool that only ever wants a handful of standing
  // lights needs -- this writes the GPU buffer directly instead, mirroring
  // how set_material() above already bypasses MaterialSystem for the same
  // reason. Returns the assigned GpuLight slot (useful for a future
  // move/remove call), or -1 if kMaxLights slots are already taken (logs a
  // warning, otherwise a no-op). GpuLight::source_primitive.x is set to -1
  // (no associated primitive) for every light added this way -- see
  // GpuLight's own header comment in the .cpp for what that field is for.
  i32 add_directional_light(glm::vec3 direction, glm::vec3 colour,
                            f32 intensity);
  i32 add_point_light(glm::vec3 position, glm::vec3 colour, f32 intensity);

  // M2a: stamps one analytic brush shape (any BrushShape value, generalized
  // from M1a's sphere-only/hard-union-only version -- see /home/h-i/.claude/
  // plans/joyful-skipping-giraffe.md's M2a section), blended via smooth_
  // union()/smooth_subtraction() (op, blend radius `smoothness`) against
  // whatever a touched level-0 coarse cell already holds -- empty space for
  // one that's freshly allocated -- directly into the EXISTING chunk/brick/
  // cluster GPU buffers, then regenerates the splat points for every brick
  // it touched, so the existing splat pipeline (already fully wired -- see
  // render_to()'s splat-mode gating) has something to actually draw.
  //
  // `params`' meaning depends on shape_type -- the full table (also
  // duplicated as a comment on Builtin.SculptStamp.comp.glsl's
  // eval_brush_shape(), which this must match exactly):
  //   Sphere:          params.x = radius
  //   Box:             params.xyz = half_extents
  //   Plane:           params.x = height
  //   Torus:           params.x = major_radius, params.y = minor_radius
  //   CappedCylinder:  params.x = radius, params.y = half_height
  //   CappedCone:      params.x = half_height, params.y = r1, params.z = r2
  //   RoundBox:        params.xyz = half_extents, params.w = corner_radius
  //   BoxFrame:        params.xyz = half_extents, params.w = edge_thickness
  //   Octahedron:      params.x = s
  //   Pyramid:         params.x = h, params.y = base
  //   HexPrism:        params.x = inradius, params.y = half_height
  //   RoundCone:       params.x = r1, params.y = r2, params.z = half_height
  //   Capsule:         params.x = radius, params.y = half_height
  //   Link:            params.x = half_length, params.y = r1, params.z = r2
  //   Ellipsoid:       params.xyz = radii
  // Brush ROTATION is out of scope for M2a -- every shape stamps in its
  // canonical, axis-aligned local orientation (world_pos - center, no
  // basis change).
  //
  // Self-contained and synchronous (records, submits, and fence-waits on
  // its OWN command buffer, same pattern as debug_probe_field() below) --
  // this is authoring-time work, not a frame-rate-critical streaming
  // system, so there is no fence-gated double-buffer ring here, unlike
  // update_streaming()'s async ring. Only ever touches level 0 (see
  // acquire_sculpt_chunk_slot()'s comment for why that's safe as long as
  // the stamp stays within roughly a ±64-unit cube of the origin). Cell
  // enumeration uses a conservative per-shape bounding radius (see
  // sculpt_shape_bounding_radius() in the .cpp) in place of M1a's exact
  // sphere radius -- deliberately not tight for non-sphere shapes, just
  // never smaller than the shape's real extent (plus `smoothness`, since
  // a smooth blend's influence reaches slightly past the hard surface).
  //
  // material_slot indexes the per-primitive material arrays (Scene
  // PrimitiveColours/ScenePrimitiveMaterialScalars/ScenePrimitiveTransmission
  // in Builtin.DeferredShade.comp.glsl) -- defaults to 0, the one material
  // ensure_default_sculpt_material() populates; nothing yet populates any
  // other slot. Returns how many coarse cells were actually touched (0 if
  // the shape's bounding radius is <= 0, or if every touched cell failed to
  // get a chunk-table slot -- see kMaxResidentChunks), for a caller
  // (SculptSystem) to record.
  i32 stamp_shape(glm::vec3 center, BrushShape shape_type, glm::vec4 params,
                  f32 smoothness, BrushOp op, i32 material_slot = 0);

  // M2b: reverts the most recent still-undoable stamp_shape() call -- see
  // undo_stack_'s own comment for how far back "still-undoable" reaches.
  // For every cell that stamp touched (up to its own capture cap -- see
  // kMaxSculptUndoCellsPerStamp in the .cpp): a cell the stamp newly
  // allocated a brick for is freed back onto the shared pool (chunk_
  // indirection cleared to -1, same per-brick idiom Builtin.ChunkEvict.
  // comp.glsl uses for a whole chunk); a cell that blended into an already-
  // resident brick has its pre-stamp voxels and material written straight
  // back, no blending. Every brick left resident afterward gets its splat
  // points regenerated (Builtin.SculptSplatRegen.comp.glsl, same as
  // stamp_shape()'s own tail) so the point cloud matches the reverted
  // state. Self-contained and synchronous, same pattern as stamp_shape()
  // itself (records, submits, and fence-waits its own command buffer).
  // Returns false (no-op) if undo_stack_ is empty -- nothing left to undo,
  // either because nothing has been stamped yet or kMaxUndoDepth stamps'
  // worth of undo history have already been consumed/evicted.
  //
  // Deliberately does NOT know about Redo: VulkanRendererBackend::
  // redo_stamp() just calls stamp_shape() again with the original stamp's
  // own stored parameters (SculptSystem is what remembers those) -- safe
  // and deterministic on a strictly linear undo/redo stack, since nothing
  // else can have touched the same voxels in between. See the plan file's
  // M2b section for why re-stamping, not replaying this capture forward,
  // is the simplest correct Redo.
  bool undo_stamp();

  // M3c: Spraypaint -- a paint-ONLY brush. Unlike stamp_shape(), this never
  // touches chunk_bricks[]/chunk_brick_primitive[] (the SDF distance/per-
  // brick-material data) at all, so there is no `op`/`smoothness` here --
  // those are geometry-blend concepts this brush has none of. Instead it
  // accumulates `opacity_per_pass` (clamped into [0,1] by the shader, and
  // added -- not overwritten -- to whatever a voxel already holds, also
  // clamped into [0,1]) into chunk_brick_paint_buffer_ for every voxel
  // INSIDE the brush shape's volume (see Builtin.SculptSpraypaint.comp.glsl's
  // PAINT_THRESHOLD/header comment for exactly what "inside" means and why
  // that's deliberately not limited to a thin shell around the existing
  // surface), in every touched coarse cell that ALREADY has a brick --
  // cells with none are skipped (no surface there yet, nothing to paint;
  // see the shader's own comment), and -- unlike stamp_shape() -- this never
  // allocates a fresh chunk-table slot either: a chunk nothing has ever
  // stamped into cannot have any bricks in it, so there is nothing for a
  // spray to find there regardless.
  //
  // `params` uses the exact same per-shape layout table as stamp_shape()
  // (see its own header comment) -- only the blend concepts (smoothness/op/
  // material_slot) are dropped, since paint accumulates independently of
  // geometry and always targets the one fixed "paint colour" material slot
  // (see ensure_default_sculpt_material()'s own comment; no colour-picker
  // UI exists yet, out of scope for this milestone).
  //
  // Self-contained and synchronous, same pattern as stamp_shape() (records,
  // submits, and fence-waits its own command buffer) -- no splat-regen tail
  // is needed (painting changes no distance/point-cloud data, just a tint
  // the shading pass reads directly). Returns how many coarse cells were
  // actually touched (0 if the shape's bounding radius is <= 0, or if the
  // brush's whole AABB falls outside every chunk anything has ever been
  // stamped into).
  //
  // Known limitation (documented, not a bug): a Spraypaint stroke is NOT
  // currently undoable/redoable -- undo_stack_ above only ever captures
  // chunk_bricks[]/chunk_brick_primitive[] state (per-cell, via Builtin.
  // SculptStamp.comp.glsl's own capture path), which has no concept of the
  // NEW per-voxel paint buffer this milestone adds. Teaching undo_stack_ a
  // second, parallel per-voxel capture format for paint alone was judged
  // not worth building for this pass; a future milestone that wants
  // Spraypaint undo should extend SculptUndoCell/SculptUndoEntry with an
  // optional paint-voxel snapshot, mirroring old_voxels' own shape.
  i32 spray_paint(glm::vec3 center, BrushShape shape_type, glm::vec4 params,
                  f32 opacity_per_pass);

  // M4b (Delete-of-an-arbitrary-edit): captures a full-volume checkpoint --
  // every currently resident chunk/brick's voxel+material data -- tagged
  // with `after_edit_index` (the index, into SculptVolume::stamps(), of the
  // stamp that was just applied when this is called; the caller,
  // VulkanRendererBackend, is responsible for picking that number
  // correctly). Pushed onto sculpt_checkpoints_ (oldest dropped once
  // kMaxSculptCheckpoints is exceeded -- see its own comment). This is the
  // ONLY way a later delete_edit() can reconstruct the volume's state as of
  // an arbitrary past edit without replaying the ENTIRE history from
  // scratch every time -- see restore_sculpt_checkpoint_before()'s own
  // comment for how it's used.
  //
  // Mechanics: reads back chunk_indirection_buffer_'s level-0 sub-range in
  // ONE device-to-staging copy (it's a flat scan over every live chunk
  // slot's cells to learn which hold a brick at all -- no per-brick
  // granularity needed for that part), then reads back every DISTINCT
  // resident brick's chunk_brick_primitive_buffer_ entry and full
  // CHUNK_BRICK_VOXEL_COUNT-float chunk_brick_pool_buffer_ voxel array in
  // TWO more device-to-staging copies, each a single vkCmdCopyBuffer call
  // covering every touched brick's own (scattered) offset via VulkanBuffer::
  // copy_regions_to() -- one bulk, multi-region round trip per buffer,
  // never one round trip per brick. Synchronous and device-stalling, same
  // as every other sculpt-family call -- an authoring-time operation, not
  // a per-frame one.
  void capture_sculpt_checkpoint(i32 after_edit_index);

  // M4b: finds the checkpoint with the largest after_edit_index STRICTLY
  // LESS than `edit_index` and restores the GPU field to exactly that
  // state (reset_chunked_field() first, for a clean slate, then every
  // captured brick re-allocated and written back verbatim -- see
  // restore_sculpt_checkpoint()'s own .cpp comment for the full mechanics);
  // if no such checkpoint exists (e.g. edit_index is small, or nothing has
  // been captured yet this session), just calls reset_chunked_field() and
  // restores nothing, which IS the correct "state before edit 0" answer.
  // Returns the after_edit_index actually restored to, or -1 if it reset to
  // empty instead -- the caller (VulkanRendererBackend::delete_edit()) uses
  // this to know where its own stamp_shape() replay needs to resume (the
  // very next index after whatever this returns).
  //
  // Deliberately does NOT touch sculpt_checkpoints_ itself (neither adding
  // nor removing entries) -- restoring an OLDER state doesn't invalidate
  // checkpoints that are older still, and the caller decides separately
  // which checkpoints a delete makes stale (see
  // invalidate_sculpt_checkpoints_from()). It DOES clear undo_stack_ (via
  // reset_chunked_field(), or explicitly if a checkpoint is actually
  // restored) -- every entry in it refers to chunk slots/brick indices from
  // whatever GPU state existed a moment ago, which this call just replaced
  // out from under it.
  i32 restore_sculpt_checkpoint_before(i32 edit_index);

  // M4b: drops every checkpoint whose after_edit_index is >= edit_index --
  // called by VulkanRendererBackend::delete_edit() once it has committed to
  // removing the stamp at `edit_index`, since every edit from that point on
  // shifts down by one position in SculptVolume::stamps() and any
  // checkpoint tagged at or after it refers to a state (and an index) that
  // no longer corresponds to anything in the rebuilt list. Checkpoints
  // tagged STRICTLY BEFORE edit_index are untouched -- nothing about their
  // own state or index changed (only entries at-or-after the deleted one
  // move). The ring simply refills from here as new edits accumulate going
  // forward -- there is no need to immediately recapture anything.
  void invalidate_sculpt_checkpoints_from(i32 edit_index);

  // M4 (Crop/Cutout): the whole-volume boolean filter both tools share --
  // see the plan file's §4 table (/home/h-i/.claude/plans/joyful-skipping-
  // giraffe.md) for why this is NOT scoped to the shape's own AABB the way
  // every prior stamp/spray/undo call has been: Crop/Cutout constrain
  // EVERYTHING that currently exists in the volume, not just whatever one
  // brush stamp would touch. Mechanics: one bulk copy of level 0's entire
  // chunk_indirection_buffer_ sub-range (identical first step to capture_
  // sculpt_checkpoint()'s own, see its comment) to learn which cells are
  // occupied, then every occupied (slot, local_cell_index) pair is batched
  // into chunk_sculpt_cell_buffer_ at kMaxSculptStampCellsPerCall entries
  // per dispatch (reusing that buffer's existing fixed capacity, same
  // batching idiom restore_sculpt_checkpoint() already uses at kMaxSculpt
  // UndoCellsPerStamp granularity) -- each batch dispatches Builtin.
  // SculptCrop.comp.glsl (keep-inside identity: smooth_subtraction(-crop_
  // sdf, old_dist, k, h), the standard SDF-intersection identity -- see
  // that shader's own header comment for the full verification against
  // smooth_subtraction()'s real cutter/base argument order) then Builtin.
  // SculptSplatRegen.comp.glsl over the SAME cell list (a cell the crop
  // pass just freed back to the pool is silently skipped by splat regen's
  // own brick_index<0 early-return, needing no special-casing here).
  //
  // Empty-brick freeing: Builtin.SculptCrop.comp.glsl itself frees any
  // brick that ends up with no isosurface left inside it (every apron
  // voxel strictly positive) back onto the shared brick/cluster free
  // lists, mirroring Builtin.ChunkEvict.comp.glsl's own per-brick cleanup
  // -- decided yes (not deferred) since a large Crop/Cutout discarding most
  // of the volume would otherwise leave the pool full of dead, unreachable
  // bricks for the rest of the session.
  //
  // keep_outside selects which side survives: false (Crop's own effect, and
  // the LIVE half of Cutout -- see export_crop_outside()'s own comment for
  // where Cutout's complementary "removed" counterpart actually comes from)
  // keeps what's INSIDE shape_type/params/center; true keeps what's
  // OUTSIDE (wired for symmetry -- Builtin.BrushShapes.inc.glsl's own
  // comment calls this "the mirror operation" -- even though no current
  // call site drives the LIVE volume with it set).
  //
  // Clears undo_stack_ (a crop can free/reallocate bricks any pending undo_
  // stamp() entry refers to, exactly the same hazard restore_sculpt_
  // checkpoint() already documents for its own GPU-state rewrite) and
  // invalidates TAA history. Deliberately NOT integrated into SculptVolume/
  // the checkpoint-replay engine -- see this method's own .cpp comment for
  // the documented, accepted scope decision (mirrors Spraypaint's own
  // precedent: not undoable/redoable for this pass, and not replayed by
  // delete_edit()/move_edit() if either targets an edit that landed before
  // a Crop ran).
  //
  // Self-contained and synchronous (records, submits, and fence-waits its
  // own command buffer(s) per batch), like every other sculpt-family call.
  // Returns how many resident bricks were actually touched (0 if the
  // volume is currently empty, or the shape's bounding radius is
  // non-positive).
  i32 crop_volume(glm::vec3 center, BrushShape shape_type, glm::vec4 params,
                  f32 smoothness, bool keep_outside);

  // M4 (Cutout): reads back EVERY resident brick's current voxel/material
  // state (same bulk-copy/scan/batched-brick-readback mechanics as capture_
  // sculpt_checkpoint()'s own steps 1-3, duplicated rather than shared --
  // see this method's .cpp comment for why), then computes, ENTIRELY ON THE
  // CPU (a small C++ port of Builtin.BrushShapes.inc.glsl's eval_brush_
  // shape()/smooth_subtraction(), file-local to the .cpp), the COMPLEMENT of
  // whatever crop_volume(..., keep_outside=false) is about to keep live --
  // i.e. the geometry a Crop/Cutout would discard. Any brick whose CPU-
  // computed complement ends up with no voxel <= 0 (nothing survives
  // outside the shape either) is simply omitted from the written file,
  // same "no surface, nothing to write" reasoning Builtin.SculptCrop.comp.
  // glsl's own empty-brick-freeing uses GPU-side.
  //
  // Writes a small, self-contained binary snapshot (magic/version/brick
  // count, then per brick: chunk coord, local cell index, material,
  // CHUNK_BRICK_VOXEL_COUNT raw float distances) to `path` -- deliberately
  // NOT the tool's .stamps replayable-edit-list format (sculpt_document.h):
  // a Crop/Cutout's removed portion is an arbitrary post-boolean voxel
  // field, not expressible as a short list of brush stamps, and retrofitting
  // a non-stamp entry into that ordered, position-indexed list would also
  // have to flow through SculptVolume/the edit-history UI's Move/Delete-of-
  // an-arbitrary-edit machinery (M4's already-complete, must-not-touch
  // pieces) to stay index-consistent. A brand-new, self-contained snapshot
  // format sidesteps that entirely -- see load_crop_outside_file()'s own
  // comment for how a fresh tool session opens it back up.
  //
  // Does NOT itself modify the live volume -- call crop_volume() separately
  // afterward (VulkanRendererBackend::cutout() does both, in this order, so
  // the snapshot this reads is always the PRE-crop state). Returns false if
  // the file couldn't be opened for writing, or if a staging-buffer
  // allocation failed (logged either way); an empty/all-inside volume
  // still writes a valid (zero-brick) file and returns true.
  bool export_crop_outside(const std::string &path, glm::vec3 center,
                           BrushShape shape_type, glm::vec4 params,
                           f32 smoothness);

  // M4 (Cutout): the counterpart to export_crop_outside() -- reads a
  // snapshot file it (or an identical process) wrote and injects every
  // brick in it into the LIVE volume, reusing restore_sculpt_checkpoint()'s
  // own batched-upload-then-dispatch mechanics (chunk_sculpt_checkpoint_
  // restore_pipeline_ + chunk_sculpt_splat_regen_pipeline_, kMaxSculptUndo
  // CellsPerStamp entries per batch) rather than duplicating them --
  // acquire_sculpt_chunk_slot() resolves (or allocates) each brick's chunk-
  // table slot exactly as every other sculpt-family loader path already
  // does. ADDITIVE onto whatever the volume currently holds, same
  // documented limitation main_window.cpp's own on_load_triggered() already
  // carries for the ordinary .stamps Load path (there is still no "clear
  // the chunked field" API plumbed through the renderer frontend) -- opening
  // a cutout file into a FRESH tool session (nothing stamped yet) is the
  // supported, correct use (see the plan's own verification notes), not
  // loading it on top of an unrelated in-progress sculpture.
  //
  // Clears undo_stack_ (new/reallocated bricks, same hazard crop_volume()'s
  // own comment documents) and invalidates TAA history. Returns false if
  // the file couldn't be read/parsed, or is malformed (bad magic/version/
  // truncated).
  bool load_crop_outside_file(const std::string &path);

private:
  // M2b: one entry per coarse cell a stamp_shape() call captured pre-
  // overwrite state for (see Builtin.SculptStamp.comp.glsl's own header
  // comment for what change_type/old_material/old_voxels mean and exactly
  // where each is captured). Deliberately plain CPU data, not a mirror of
  // any GPU buffer layout -- an authoring tool's undo history living in
  // regular process memory is normal, and sidesteps having to budget GPU
  // memory for history depth. old_voxels is the dominant per-entry cost
  // and is only ever populated for change_type == 2 (MODIFIED) cells --
  // change_type == 1 (NEW_BRICK) cells have nothing worth saving (a freshly
  // allocated brick's prior contents are garbage, not geometry; Undo just
  // frees it), and change_type == 0 (NOOP) cells changed nothing at all.
  struct SculptUndoCell {
    i32 chunk_slot = 0;
    i32 local_cell_index = 0;
    glm::vec3 cell_min{0.0f}; // Needed to re-run Builtin.SculptSplatRegen.
                             // comp.glsl over a MODIFIED cell after
                             // restoring it -- see undo_stamp().
    i32 change_type = 0;      // 0 = NOOP, 1 = NEW_BRICK, 2 = MODIFIED.
    i32 old_material = -1;    // Meaningful only for change_type == 2.
    std::vector<f32> old_voxels; // Exactly kChunkBrickVoxelCount (5832)
                                 // entries for change_type == 2, empty
                                 // otherwise.
  };
  struct SculptUndoEntry {
    std::vector<SculptUndoCell> cells;
  };
  // One entry per undoable stamp, oldest first (undo_stamp() always pops
  // the back = most recent). Capped at kMaxSculptUndoDepth (see the .cpp)
  // -- past that, stamp_shape() silently drops the oldest entry to make
  // room for the newest, exactly like kMaxSculptStampCellsPerCall's own
  // "degrade, don't fail" policy: an authoring session that has stamped
  // more than kMaxSculptUndoDepth times without ever undoing simply loses
  // the ability to undo its earliest strokes, which is the documented,
  // accepted limitation of a linear-stack undo design (see the plan
  // file's M2b section).
  std::vector<SculptUndoEntry> undo_stack_;

  // M4b (Delete-of-an-arbitrary-edit): one captured brick, as seen by
  // capture_sculpt_checkpoint() -- the arbitrary-checkpoint counterpart to
  // SculptUndoCell's MODIFIED case, just unconditional (a checkpoint always
  // captures every resident brick's FULL state, not a before/after delta,
  // since it has to stand on its own as a restore target with nothing else
  // to blend against).
  struct SculptCheckpointBrick {
    i32 local_cell_index = 0; // Into this chunk's CHUNK_CELL_COUNT sub-block
                             // -- see Builtin.ChunkedFieldCommon.inc.glsl.
    i32 material = 0;
    std::vector<f32> voxels; // Exactly kChunkBrickVoxelCount entries.
  };
  // One resident chunk's worth of captured bricks.
  struct SculptCheckpointChunk {
    glm::ivec3 chunk_coord{0};
    std::vector<SculptCheckpointBrick> bricks;
  };
  // A full-volume checkpoint -- see capture_sculpt_checkpoint()'s own
  // comment. after_edit_index is which SculptVolume::stamps() index this
  // represents the state immediately AFTER (i.e. stamps [0, after_edit_
  // index] have all been applied, nothing past that).
  struct SculptCheckpoint {
    i32 after_edit_index = -1;
    std::vector<SculptCheckpointChunk> chunks;
  };
  // Oldest first, capped at kMaxSculptCheckpoints (see restore_sculpt_
  // checkpoint_before()'s own .cpp comment for why 5/every-10-stamps was
  // chosen) -- plain process memory, same "an authoring tool's history
  // living in regular heap memory is normal" reasoning undo_stack_ above
  // already documents. VulkanRendererBackend decides WHEN to call
  // capture_sculpt_checkpoint() (the every-~10-stamps cadence); this class
  // only owns the ring itself and the capture/restore mechanics, which stay
  // physically next to the buffers they read/write, mirroring undo_stack_'s
  // own placement.
  static constexpr size_t kMaxSculptCheckpoints = 5;
  std::vector<SculptCheckpoint> sculpt_checkpoints_;

  // Restores the GPU field to exactly the state `checkpoint` captured --
  // the shared tail of both restore_sculpt_checkpoint_before() branches
  // (checkpoint found vs. not). See the .cpp for the full mechanics
  // (reset_chunked_field() first, then re-acquire each chunk's slot, batch-
  // upload every captured brick's voxel/material snapshot into the reused
  // chunk_sculpt_cell_buffer_/chunk_sculpt_undo_capture_meta_buffer_/
  // chunk_sculpt_undo_capture_voxel_buffer_ scratch at kMaxSculptUndoCells
  // PerStamp entries per batch, and dispatch chunk_sculpt_checkpoint_
  // restore_pipeline_ + chunk_sculpt_splat_regen_pipeline_ once per batch).
  void restore_sculpt_checkpoint(const SculptCheckpoint &checkpoint);

  // Shared body of add_directional_light()/add_point_light() -- light_type
  // matches GpuLight::vector_type.w's convention (0=directional, 1=point).
  i32 add_light_internal(f32 light_type, glm::vec3 vector, glm::vec3 colour,
                         f32 intensity);

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

  // rebuild_static_scene()/voxelize()/check_brick_overflow() -- the
  // primitive-driven fixed-cube bake path -- and prewarm_chunk_cache()/
  // cached_chunk_brick_count()/check_chunk_brick_overflow()/
  // record_pool_gauges() -- its chunked-field disk-cache/diagnostic
  // counterparts -- were removed in the M0 CSG deletion pass (see
  // /home/h-i/.claude/plans/joyful-skipping-giraffe.md) along with
  // GeometrySystem's primitive-authoring surface they baked.
  // KENGINE_PREWARM_CACHE requested it; and whether it has already run for
  // this scene (it is a one-shot warm-up, not a per-frame concern). No
  // longer has a producer, but left declared -- see
  // request_cache_prewarm()/streaming_backlog().
  b8 prewarm_requested_ = false;
  b8 prewarm_done_ = false;
  // See streaming_backlog().
  u32 streaming_backlog_ = 0;
  // Low-water marks over the current reporting interval, refilled to their
  // pool sizes each time collect_frame_timings() prints them. UINT32_MAX
  // means "no sample yet this interval".
  u32 chunk_bricks_free_ = 0xFFFFFFFFu;
  u32 chunk_clusters_free_ = 0xFFFFFFFFu;
  // Run-total splat LOD rollbacks and the value at the last report, so the
  // log can print the interval's delta from a counter the GPU never resets.
  u32 splat_rollbacks_total_ = 0;
  u32 splat_rollbacks_reported_ = 0;
  // The subset that ended up with no points at all -- see
  // kChunkStatSplatNoPointsWord.
  u32 splat_no_points_total_ = 0;
  u32 splat_no_points_reported_ = 0;
  // Diagnostic only -- see KENGINE_SPLAT_ONLY_LEVEL. -1 = render normally.
  i32 splat_only_level_ = -1;
  // Incremented once per update_streaming() call; paces cold bakes (see
  // kChunkColdBakeFrameStride).
  u64 chunk_stream_tick_ = 0;
  // Seeds a device-local buffer from CPU data through a throwaway staging
  // buffer -- see its definition for why the chunked field's pools need it.
  // Generic (no primitive/streaming dependency); still used by
  // reset_chunked_field() below.
  void upload_to_device_local(VulkanBuffer &dest, const void *data, u64 size);

  // build_cell_alias_map()/invalidate_slot_content()/
  // publish_completed_bakes()/drain_streaming_for_debug()/
  // set_chunk_slot_published()/voxelize_chunk()/write_chunk_table_entry() --
  // the camera-streaming/primitive-voxelize machinery these existed to
  // serve -- were removed in the M0 CSG deletion pass (see /home/h-i/
  // .claude/plans/joyful-skipping-giraffe.md).

  // Resets the chunked field to empty: fills chunk_table_buffer_ with -1
  // (no chunk resident anywhere) and re-seeds chunk_brick_free_list_buffer_/
  // chunk_brick_free_list_top_buffer_ to a full free pool ([0, 1, ...,
  // kMaxChunkBricks-1], top = kMaxChunkBricks). Called once from the
  // constructor. Also resets the M1a sculpt-stamp chunk-slot bookkeeping
  // (sculpt_chunk_slot_used_/sculpt_chunk_slot_by_coord_) -- see
  // acquire_sculpt_chunk_slot().
  void reset_chunked_field();

  // M1a: populates index 0 of EVERY per-primitive material array (Scene
  // PrimitiveColours/ScenePrimitiveMaterialScalars/
  // ScenePrimitiveTransmission/ScenePrimitiveTexTransform, plus -- unlike
  // what the plan originally called out -- every element of the scene_
  // textures/scene_bump_textures combined-image-sampler arrays too) with a
  // plain white, default-roughness, opaque, shadow-casting, non-metallic
  // material, so a stamped sphere's hit_material (always 0 in M1a) never
  // samples uninitialized/unbound descriptor data. See its .cpp comment for
  // why the texture arrays need this at all: rebuild_static_scene(), their
  // only previous populator, was deleted in the M0 CSG pass, and nothing
  // since has written them. Idempotent (default_sculpt_material_ready_
  // guards it); called once, from the constructor.
  void ensure_default_sculpt_material();

  // CPU-side chunk-table-slot allocator for stamp_shape() (level 0 only --
  // M1a never touches another level). chunk_coord is the chunk's own
  // (unwrapped) integer coordinate (see Builtin.ChunkedFieldCommon.inc.
  // glsl's chunk_table_slot_at() -- floor(world_pos / chunk_world_size)).
  // Returns the slot now owning it (allocating a fresh one from
  // sculpt_chunk_slot_used_ and writing chunk_table_buffer_/chunk_slot_
  // published_buffer_ directly if this is the first time anything has
  // stamped into this chunk -- both host-visible, same idiom reset_
  // chunked_field() already uses), or -1 if every one of kMaxResidentChunks
  // level-0 slots is already taken (M1a has no eviction path, so this is a
  // hard cap on how many distinct chunks a session's stamps can span).
  // out_is_new is set to whether a fresh slot was allocated this call --
  // stamp_shape() uses it to know which slots' chunk_indirection_buffer_
  // sub-block still needs clearing to empty via vkCmdFillBuffer before
  // anything writes a real brick index into it.
  i32 acquire_sculpt_chunk_slot(glm::ivec3 chunk_coord, bool &out_is_new);

  // voxelize_chunk_batch() (the chunked field's primitive-driven bake
  // dispatch), the disk-backed brick cache (record_cache_gather()/
  // record_cache_restore()/chunk_content_hash()/chunk_cache_path()/
  // chunk_key_for()/acquire_cache_region()/load_cached_chunk()/
  // store_cached_chunk() and their backing primitive_content_hash_/
  // layer_content_hash_/chunk_cache_*/pending_cache_gathers_ members), and
  // build_chunk_candidates() (the per-chunk primitive candidate-list
  // builder) were all removed in the M0 CSG deletion pass (see
  // /home/h-i/.claude/plans/joyful-skipping-giraffe.md) along with the
  // primitive-driven voxelization they served.

  // Records a single Builtin.ChunkedFieldDebugQuery.comp.glsl dispatch
  // (one invocation, one sample_chunked_field() call at query_world_pos)
  // into cmd, writing its result to chunk_debug_query_output_buffer_ for
  // the caller to read back once cmd's submission has finished -- see
  // debug_verify_chunked_field() below, this method's only real caller.
  void query_chunked_field(VulkanCommandBuffer &cmd, glm::vec3 query_world_pos);

  // Like query_chunked_field() above, but dispatches sample_clipmap_field()
  // (multi-level selection) instead of the level-0-only sample_chunked_
  // field() -- see debug_verify_multi_level_field(), this method's only
  // real caller.
  void query_clipmap_field(VulkanCommandBuffer &cmd, glm::vec3 query_world_pos,
                           glm::vec3 camera_pos);

  // evict_chunk()/evict_chunk_batch() -- the chunk-eviction dispatch
  // wrappers -- were removed in the M0 CSG deletion pass: evict_chunk()
  // itself had no remaining caller (its own header comment named only the
  // debug_verify_*() harness and update_streaming(), both also removed),
  // and evict_chunk_batch()'s only callers were prewarm_chunk_cache()/
  // update_streaming()/evict_chunk(), likewise all removed.

  // Phase 5: records ONE Builtin.ChunkProbeBake.comp.glsl bounce dispatch
  // into cmd, gathering around gi_cascade_center_ -- the same ping-pong
  // scheme as bake_probes() (see its own comment), just targeting
  // chunk_gi_probe_buffer_a_/_b_ and reading the chunked field instead,
  // and AMORTIZED: update_gi_cascade() calls this once per frame for
  // kProbeBounceCount consecutive frames instead of recording every bounce
  // into one frame's command buffer (see gi_cascade_next_bounce_'s comment
  // for why -- the all-at-once version was the dominant per-frame hitch on
  // both scene edits and chunk-boundary camera travel). Only called by
  // update_gi_cascade(), which owns deciding *when* a recenter (and
  // therefore a rebake) is actually due.
  void bake_gi_cascade_bounce(VulkanCommandBuffer &cmd, u32 light_count,
                              u32 bounce);

  // One-shot, self-contained correctness check for the chunked field
  // introduced above: uploads a single hand-placed test sphere directly
  // into primitive_buffer_/layer_buffer_ (bypassing GeometrySystem
  // entirely, since nothing may be registered yet this early), bakes the
  // one chunk it lives in, then queries three points with independently,
  // The debug_verify_chunked_field()/debug_verify_multi_level_field()/
  // debug_verify_extended_range_field()/debug_verify_dirty_rebake()/
  // debug_verify_bulk_sweep_TEMP()/debug_verify_rapid_drag_crash_TEMP()/
  // debug_verify_lod_blend()/debug_verify_thin_geometry_at_coarse_level()
  // one-shot regression harnesses were removed in the M0 CSG deletion pass
  // (see /home/h-i/.claude/plans/joyful-skipping-giraffe.md): every one of
  // them exercised primitive-driven voxelization/camera streaming/
  // ChunkStreamingManager directly (hand-placed test primitives uploaded
  // straight into primitive_buffer_/layer_buffer_, update_streaming()'s
  // dirty-rebake path, etc.), none were called by anything even before
  // this pass, and none would compile against the types they tested.

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

  // The voxelize_stage_/voxelize_set_layout_/voxelize_set_/voxelize_
  // pipeline_ shader module/descriptor set/pipeline for pass 1 (SDF ->
  // sparse voxel field) were removed along with Builtin.RaymarchVoxelize.
  // comp.glsl (M0 CSG deletion pass).

  // The sparse voxel field: a coarse indirection grid of brick indices
  // (kCoarseDim^3 i32s, -1 = no brick) pointing into a pool of fixed-size
  // bricks (kMaxBricks * kBrickVoxelCount f32 distances), allocated by an
  // atomic counter during voxelize(). See vulkan_raymarch_shader.cpp for the
  // dimension constants — they must match the ones hardcoded in the two
  // .comp.glsl shaders exactly, since there's no shared config between them.
  std::optional<VulkanBuffer> indirection_buffer_;
  std::optional<VulkanBuffer> brick_pool_buffer_;
  std::optional<VulkanBuffer> brick_counter_buffer_;

  // The chunk_voxelize_stage_/chunk_voxelize_set_layout_/chunk_voxelize_
  // set_/chunk_voxelize_pipeline_ equivalents for the chunked field's own
  // primitive-scanning voxelize pass were likewise removed along with
  // Builtin.ChunkVoxelize.comp.glsl.

  VulkanShaderModule chunk_debug_query_stage_;
  VkDescriptorSetLayout chunk_debug_query_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet chunk_debug_query_set_ = VK_NULL_HANDLE;
  std::optional<VulkanComputePipeline> chunk_debug_query_pipeline_;

  // Phase 3b: frees an evicted chunk's bricks back onto the shared
  // free-list -- see evict_chunk().
  VulkanShaderModule chunk_evict_stage_;
  VkDescriptorSetLayout chunk_evict_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet chunk_evict_set_ = VK_NULL_HANDLE;
  std::optional<VulkanComputePipeline> chunk_evict_pipeline_;

  // Phase 5: bakes the chunked field's single camera-centered GI cascade
  // -- see bake_gi_cascade()/update_gi_cascade(). Ping-pong sets, same
  // reasoning as probe_bake_set_/probe_bake_set_odd_ above (every bounce
  // shares one command buffer, so a set rewritten mid-bake would
  // retroactively corrupt an already-recorded earlier bounce's bind).
  VulkanShaderModule chunk_probe_bake_stage_;
  VkDescriptorSetLayout chunk_probe_bake_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet chunk_probe_bake_set_ = VK_NULL_HANDLE;
  VkDescriptorSet chunk_probe_bake_set_odd_ = VK_NULL_HANDLE;
  std::optional<VulkanComputePipeline> chunk_probe_bake_pipeline_;

  // Splat priming (the depth-priming stage of the Dreams-style point-
  // splatting technique -- see Builtin.ChunkPointSplat.comp.glsl's header
  // comment for the whole scheme): splats the chunked field's baked
  // per-brick surface points into primed_depth_buffer_ every frame the
  // chunked field is enabled, so render_to()'s primary rays can start at a
  // conservative distance instead of marching all the empty space from the
  // camera. Recorded by render_to() into the same command buffer, right
  // before the render dispatch.
  VulkanShaderModule chunk_cluster_cull_stage_;
  VkDescriptorSetLayout chunk_cluster_cull_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet chunk_cluster_cull_set_ = VK_NULL_HANDLE;
  std::optional<VulkanComputePipeline> chunk_cluster_cull_pipeline_;
  VulkanShaderModule chunk_point_splat_stage_;
  VkDescriptorSetLayout chunk_point_splat_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet chunk_point_splat_set_ = VK_NULL_HANDLE;
  std::optional<VulkanComputePipeline> chunk_point_splat_pipeline_;

  // M1a/M2a: the brush-stamp compute pass (Builtin.SculptStamp.comp.glsl) --
  // see stamp_shape(). Own dedicated descriptor pool (sculpt_descriptor_
  // pool_ below), not chunk_descriptor_pool_/descriptor_pool_: same
  // reasoning chunk_descriptor_pool_'s own comment already gives for being
  // separate from descriptor_pool_ -- this still-new path's descriptor
  // lifetime stays fully decoupled from both the working render pipeline's
  // and the rest of the chunked-field machinery's.
  VulkanShaderModule chunk_sculpt_stamp_stage_;
  VkDescriptorSetLayout chunk_sculpt_stamp_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet chunk_sculpt_stamp_set_ = VK_NULL_HANDLE;
  std::optional<VulkanComputePipeline> chunk_sculpt_stamp_pipeline_;

  // M1a: regenerates splat points for every brick the stamp pass above
  // just touched (Builtin.SculptSplatRegen.comp.glsl) -- see stamp_shape().
  VulkanShaderModule chunk_sculpt_splat_regen_stage_;
  VkDescriptorSetLayout chunk_sculpt_splat_regen_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet chunk_sculpt_splat_regen_set_ = VK_NULL_HANDLE;
  std::optional<VulkanComputePipeline> chunk_sculpt_splat_regen_pipeline_;

  // M2b: reverts one past stamp using the snapshot Builtin.SculptStamp.
  // comp.glsl captured for it (Builtin.SculptUndoRestore.comp.glsl) -- see
  // undo_stamp(). Shares chunk_sculpt_cell_buffer_/chunk_sculpt_splat_
  // regen_set_ below (the cell list and the splat-regen pass are reused
  // unmodified), but gets its own descriptor set/layout/pipeline since its
  // binding list (the brick/cluster free-list buffers PLUS the two undo
  // capture buffers) matches neither chunk_sculpt_stamp_set_'s nor chunk_
  // sculpt_splat_regen_set_'s exactly.
  VulkanShaderModule chunk_sculpt_undo_restore_stage_;
  VkDescriptorSetLayout chunk_sculpt_undo_restore_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet chunk_sculpt_undo_restore_set_ = VK_NULL_HANDLE;
  std::optional<VulkanComputePipeline> chunk_sculpt_undo_restore_pipeline_;

  VkDescriptorPool sculpt_descriptor_pool_ = VK_NULL_HANDLE;

  // M4b: Builtin.SculptCheckpointRestore.comp.glsl -- restores one batch of
  // a checkpoint's captured bricks (always allocating a fresh brick, then
  // copying its captured voxel/material snapshot verbatim, no blending) --
  // see restore_sculpt_checkpoint(). Deliberately reuses chunk_sculpt_
  // stamp_set_layout_/chunk_sculpt_stamp_set_ wholesale rather than a new
  // descriptor set: its binding list (StampCellBuffer/ChunkIndirection/
  // ChunkBrickPool/ChunkBrickPrimitive/the brick free list/UndoCaptureMeta/
  // UndoCaptureVoxel/ChunkBrickPaint) is byte-for-byte identical to that
  // set's own -- only the SHADER differs (a different pipeline reading the
  // same bound buffers, with the undo-capture buffers' fields repurposed to
  // carry restore data instead -- see the shader's own header comment). No
  // new descriptor pool/layout/set needed, just a new shader module +
  // pipeline sharing the existing layout.
  VulkanShaderModule chunk_sculpt_checkpoint_restore_stage_;
  std::optional<VulkanComputePipeline> chunk_sculpt_checkpoint_restore_pipeline_;

  // M4 (Crop/Cutout): Builtin.SculptCrop.comp.glsl -- see crop_volume().
  // Reuses chunk_sculpt_undo_restore_set_layout_/chunk_sculpt_undo_
  // restore_set_ wholesale (same "a different pipeline reading the same
  // bound buffers" reuse chunk_sculpt_checkpoint_restore_pipeline_ already
  // established just above) -- no new descriptor set/layout/pool needed.
  VulkanShaderModule chunk_sculpt_crop_stage_;
  std::optional<VulkanComputePipeline> chunk_sculpt_crop_pipeline_;

  // M3c (Spraypaint): the paint-only brush pass (Builtin.SculptSpraypaint.
  // comp.glsl) -- see spray_paint(). Own dedicated descriptor pool (paint_
  // descriptor_pool_ below), same "new work gets its own decoupled
  // descriptor lifetime" reasoning chunk_sculpt_stamp_stage_'s own comment
  // above gives -- this set's binding list (StampCellBuffer/chunk_
  // indirection/the NEW paint buffer) matches none of the sculpt-stamp-
  // family sets exactly, so it gets its own rather than widening one of
  // theirs.
  VulkanShaderModule chunk_sculpt_spraypaint_stage_;
  VkDescriptorSetLayout chunk_sculpt_spraypaint_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet chunk_sculpt_spraypaint_set_ = VK_NULL_HANDLE;
  std::optional<VulkanComputePipeline> chunk_sculpt_spraypaint_pipeline_;

  // M3c: the tiny, SEPARATE descriptor set (set = 1) Builtin.DeferredShade.
  // comp.glsl's pipeline binds ALONGSIDE render_set_ (set = 0) purely to
  // read chunk_brick_paint_buffer_ below -- see its own comment in the
  // shader for why this, not widening render_set_'s already-fully-packed
  // 29-binding layout, is how the paint accumulator reaches the shading
  // pass. Never bound to render_pipeline_ (the visibility pass) -- that
  // shader has no use for paint data, and leaving its pipeline layout at
  // just {render_set_layout_} means this addition cannot affect it at all.
  // M7 polish ADDS a second binding (1) to this SAME set -- brush_preview_
  // buffer_, read as BrushPreviewBuffer in the shader -- rather than
  // standing up a third set: the guidance that landed this feature was
  // explicit that new shading-only state belongs on "the shading
  // pipeline's existing additive descriptor set", and this already-
  // existing set = 1 is exactly that (set 0/render_set_ stays completely
  // untouched by either addition).
  VkDescriptorPool paint_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout paint_read_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet paint_read_set_ = VK_NULL_HANDLE;

  // Host-visible scratch stamp_shape() fills every call: one entry per
  // coarse cell the current brush shape's world-space AABB touches, read by
  // both shaders above. Fixed capacity (kMaxSculptStampCellsPerCall,
  // vulkan_raymarch_shader.cpp) -- a stamp wanting more cells than that in
  // one call is truncated with a warning rather than growing this buffer
  // dynamically, which is more headroom than one sphere needs for M1a.
  // undo_stamp() also re-uses this same buffer (re-uploaded with the
  // ORIGINAL stamp's cell list) for its own restore + splat-regen dispatch.
  std::optional<VulkanBuffer> chunk_sculpt_cell_buffer_;

  // M2b: Builtin.SculptStamp.comp.glsl's own undo-capture scratch (see its
  // header comment) -- host-visible, like chunk_sculpt_cell_buffer_, so
  // stamp_shape() can lock()/read the capture straight back after its
  // single synchronous dispatch completes, with no separate device-local-
  // to-staging copy step (the buffer already lives in host-visible/host-
  // coherent memory, so the GPU writes land somewhere the CPU can map
  // directly -- see stamp_shape()'s own comment). Sized at
  // kMaxSculptUndoCellsPerStamp entries (vulkan_raymarch_shader.cpp) --
  // DELIBERATELY smaller than kMaxSculptStampCellsPerCall: capturing full
  // per-voxel undo data for every cell a stamp could possibly touch would
  // be tens of MB more than any realistic brush stamp needs (see that
  // constant's own comment for the sizing rationale). chunk_sculpt_undo_
  // capture_voxel_buffer_ is the big one (kMaxSculptUndoCellsPerStamp *
  // kChunkBrickVoxelCount floats); chunk_sculpt_undo_capture_meta_buffer_
  // is a small (16-byte-per-cell) sidecar telling stamp_shape() which cells
  // in the voxel buffer are actually meaningful.
  std::optional<VulkanBuffer> chunk_sculpt_undo_capture_meta_buffer_;
  std::optional<VulkanBuffer> chunk_sculpt_undo_capture_voxel_buffer_;

  // See acquire_sculpt_chunk_slot()'s comment. sculpt_chunk_slot_used_ is
  // sized kMaxResidentChunks (one bool per level-0 slot); sculpt_chunk_
  // slot_by_coord_ maps a chunk's own coordinate (ChunkKey, chunk_types.h
  // -- level always 0 here) to whichever slot currently owns it. Both are
  // reset by reset_chunked_field(), exactly like the GPU buffers they
  // track the allocation state of.
  std::vector<b8> sculpt_chunk_slot_used_;
  std::unordered_map<ChunkKey, u32, ChunkKeyHash> sculpt_chunk_slot_by_coord_;

  // See ensure_default_sculpt_material().
  b8 default_sculpt_material_ready_ = false;

  // Backs chunk_voxelize_set_/chunk_debug_query_set_/chunk_evict_set_/
  // chunk_probe_bake_set_(_odd_)/chunk_point_splat_set_ -- deliberately
  // not descriptor_pool_
  // below (which backs every
  // fixed-cube-field set), so this still-unproven path's descriptor
  // lifetime is fully decoupled from the working render pipeline's.
  VkDescriptorPool chunk_descriptor_pool_ = VK_NULL_HANDLE;

  // Command pool for async_command_buffers_ below -- same queue FAMILY as
  // graphics_command_pool (VulkanDevice::async_compute_queue is queue
  // index 1 of that same family, not a separate one -- see its own
  // comment), just a distinct pool object so this ring's alloc/reset
  // cycle never contends with the main per-frame command buffers'.
  VkCommandPool async_command_pool_ = VK_NULL_HANDLE;
  // How many (command buffer, fence, semaphore) slots update_streaming()
  // cycles through for its async chunk-bake submissions -- see
  // update_streaming()'s own comment for the full design. 2 is standard
  // double-buffering depth for a queue whose own submission cadence is
  // tied to the same per-frame cadence as the graphics queue (nothing
  // here needs more slack than that): async_ring_index_ can't advance
  // past a slot whose PREVIOUS submission hasn't been fence-confirmed
  // complete, so this also bounds how many frames the async queue can
  // ever lag behind the CPU -- comfortably under kFramesInFlightDelay
  // (3), which every chunk's Baking->Ready transition already assumes is
  // enough slack for GPU work to have actually finished.
  //
  // Raised from 2 once the graphics queue stopped waiting on the bake.
  // With the wait in place the ring could never get more than a frame
  // ahead anyway, so depth 2 cost nothing. Now update_streaming() SKIPS
  // queueing chunk work on any frame whose ring slot is still in flight
  // (rather than blocking the CPU on its fence, which would just
  // reintroduce the stall a level down), so the depth directly bounds how
  // much streaming can be in flight at once -- and a bake measured in
  // seconds against a frame measured in milliseconds wants more than one
  // spare slot. Each slot costs a command buffer, a fence, two semaphores
  // and a batch-buffer region of a few kilobytes.
  static constexpr u32 kAsyncRingDepth = 4;
  std::vector<std::unique_ptr<VulkanCommandBuffer>> async_command_buffers_;
  std::vector<std::unique_ptr<VulkanFence>> async_fences_;
  // Chains consecutive async_compute_queue submissions to each other --
  // NOT the same thing async_fences_ already provides. The fence only
  // gates when the CPU may safely reuse a ring slot's command buffer;
  // Vulkan does NOT guarantee memory visibility between two separate
  // vkQueueSubmit calls to the same queue just because they were
  // submitted in order (execution order is guaranteed; a memory
  // dependency is not, without an explicit semaphore/barrier). Without
  // this, two back-to-back update_streaming() calls -- e.g. a large,
  // multi-frame forced-rebake sweep, which is exactly what surfaced this
  // live -- have no dependency stopping their dispatches from executing
  // concurrently/out of order on the GPU, both touching the same shared
  // chunk_indirection_buffer_/chunk_brick_pool_buffer_/chunk_brick_free_
  // list_buffer_/chunk_brick_free_list_top_buffer_. Each submission waits
  // on the immediately-preceding one's entry here (if any) and signals
  // its own before returning -- see update_streaming()'s own comment.
  std::vector<VkSemaphore> async_chain_semaphores_;
  // The chain semaphore the NEXT async submission must wait on, or
  // VK_NULL_HANDLE if there's no prior submission yet to depend on (the
  // very first one, or after a device-wide vkDeviceWaitIdle already made
  // any dependency moot).
  VkSemaphore pending_async_chain_semaphore_ = VK_NULL_HANDLE;
  u32 async_ring_index_ = 0;

  // One entry per chunk whose bake is in flight in a given ring slot, held
  // until that slot's fence confirms the bake actually finished -- see
  // publish_completed_bakes(), and update_streaming()'s own comment for
  // why publication is deferred at all.
  struct PendingChunkPublish {
    u32 level = 0;
    ChunkKey key{};
    // The slot the bake wrote into. Written into the chunk table (and
    // marked published) only once the fence says the bake is done.
    u32 slot = kInvalidChunkSlot;
    // The slot this bake REPLACES, or kInvalidChunkSlot for an ordinary
    // boundary load. Non-empty for a dirty-scene in-place rebake, which is
    // now double-buffered: the chunk keeps sampling its old slot's content
    // for the whole duration of the new bake, and only when the new one is
    // published does the old slot get its bricks freed and go back to the
    // pool. That is what lets the rebake be slow without either flickering
    // (the old "evict then re-bake in place" behaviour) or stalling (the
    // old semaphore wait).
    u32 retire_slot = kInvalidChunkSlot;
    // resident_[key].bake_generation at queue time -- a stale entry whose
    // chunk was evicted and reloaded meanwhile must not publish over the
    // newer bake's slot.
    u64 bake_generation = 0;
    // Disk cache bookkeeping: the chunk's content key, and which staging
    // region its gather was recorded into (kInvalidChunkSlot for a chunk
    // that was restored from cache, or that no region was free for). When
    // the fence signals, a region here is a finished payload to write out.
    u64 cache_key = 0;
    u32 cache_region = kInvalidChunkSlot;
  };
  // submit_async_chunk_work() -- update_streaming()'s own per-frame async
  // submission helper -- was removed along with it (M0 CSG deletion pass).

  std::vector<PendingChunkPublish> async_pending_publish_[kAsyncRingDepth];
  // Slots whose replacement has been published and whose bricks/cluster
  // pages therefore now need freeing -- drained into the next frame's
  // evict batch (an evict is GPU work, so it can't be issued from
  // publish_completed_bakes(), which records nothing).
  std::vector<u32> pending_slot_retirements_;

  // Rolling estimate of what one chunk costs to bake on this GPU, in this
  // scene, right now -- the input to the adaptive submission size (see
  // kSubmissionTargetMs). Updated from the async ring's own timestamps
  // every time a submission's fence is observed, so it tracks the content
  // the camera is actually moving through rather than any authored guess.
  // Seeded PESSIMISTIC, not zero. This drives how many chunks one
  // submission may bake, and the very first submission has no measurement
  // to go on -- starting optimistic would make it the largest one the cap
  // allows, at exactly the moment a scene is loading and chunks are at
  // their most expensive. Starting at the whole per-submission budget means
  // the first submission bakes a single chunk, and the estimate walks down
  // from there as fast as the measurements justify.
  f64 bake_ms_per_chunk_estimate_ = 40.0;
  // Whether the voxelizer tallies its bake-cost counters (see
  // ChunkBrickDemandBuffer in Builtin.ChunkVoxelize.comp.glsl). Off by
  // default -- the atomics are individually cheap but there are millions of
  // them per bake. Flipped by setting KENGINE_BAKE_STATS in the environment.
  b8 bake_stats_enabled_ = false;
  // Brick deduplication (build_cell_alias_map() + the voxelizer's pass 1),
  // OFF unless explicitly enabled.
  //
  // It landed without ever being run against real content -- the GPU was
  // occupied for the whole session it was written in -- and it can remove
  // geometry when wrong rather than merely slowing things down: a mis-keyed
  // alias copies the wrong cell's brick (or a cell that has none). Correct-
  // by-default with an opt-in switch is the right shape for that until it
  // has been watched running. KENGINE_CHUNK_DEDUP=1.
  b8 chunk_dedup_enabled_ = false;
  // Smoothed camera velocity, for the streaming window's lead bias (see
  // kStreamLeadFrames), and the position it was measured against.
  glm::vec3 stream_velocity_{0.0f};
  glm::vec3 last_stream_camera_pos_{0.0f};
  b8 have_last_stream_camera_pos_ = false;

  // Phase 5: this cascade's current center, in RENDER space -- recentered
  // in discrete whole-cell steps by update_gi_cascade() as the camera
  // moves (see kGiCascadeRecenterThreshold's comment, .cpp). Starts at the
  // origin; the first update_gi_cascade() call wherever the camera
  // actually is will recenter it there before anything reads it.
  glm::vec3 gi_cascade_center_{0.0f};

  // Set by update_streaming() whenever a scene edit (not just camera
  // motion) forced a dirty-rebake cycle this frame, and consumed by the
  // very next update_gi_cascade() call (always the same frame, since
  // begin_frame() calls update_streaming() then update_gi_cascade() back
  // to back) to force a cascade rebake even though the camera itself
  // hasn't crossed kGiCascadeRecenterThreshold -- without this,
  // update_gi_cascade()'s only rebake trigger is camera drift, so editing
  // a light or primitive with a stationary camera left the cascade's
  // indirect-bounce term frozen on the pre-edit scene indefinitely (direct
  // lighting updates immediately every frame regardless, since render_to()
  // reads light_buffer_ fresh every frame -- only the baked INDIRECT term
  // was stuck).
  bool gi_cascade_dirty_ = false;

  // Which bounce of an in-progress amortized cascade bake the NEXT
  // update_gi_cascade() call should record -- >= kProbeBounceCount (the
  // initial state, see update_gi_cascade() which compares against it)
  // means no bake is in progress. A cascade rebake used to record all
  // kProbeBounceCount bounces into a single frame's graphics command
  // buffer; at 16^3 probes x 32 gather rays x up-to-128 steps each, six of
  // those in one frame was a guaranteed hitch -- and it fired on EVERY
  // scene edit and EVERY kGiCascadeCellSize (2 world units) of camera
  // travel, which is exactly the "hitches while moving between chunks"
  // symptom. One bounce per frame bounds the per-frame cost at a sixth of
  // that; in exchange the indirect term converges over kProbeBounceCount
  // frames instead of instantly, which for a low-frequency GI cascade is
  // invisible. A new trigger (edit or recenter) mid-bake simply restarts
  // from bounce 0 against the new center/scene.
  u32 gi_cascade_next_bounce_ = 0xFFFFFFFFu;

  // Whether chunk_gi_probe_buffer_a_ has ever been written. Bounce 0 used
  // to zero-fill it every bake ("previous bounce" input must not be
  // garbage); amortized, that fill would blank the very buffer the render
  // pass is still reading GI from, flickering the scene dark for the
  // frames until a later bounce refills it. Instead bounce 0 now SEEDS
  // from the previous cascade's own content -- a stale-but-plausible
  // radiance estimate whose error decays by roughly an albedo factor per
  // bounce, so after the full kProbeBounceCount bounces it's gone -- and
  // the zero-fill happens exactly once, on the first bake ever, when the
  // buffer genuinely holds garbage and there's no previous content to
  // flicker away from.
  bool gi_probes_initialized_ = false;

  // Phase 5: the camera position update_gi_cascade() was most recently
  // called with -- stashed because ChunkProbeBakePushConstants' camera_x/y/z
  // (used for gather-ray origin, exactly like Builtin.ProbeBake.comp.glsl's
  // own camera-relative gather rays) is bake_gi_cascade()'s to fill in, but
  // its signature deliberately mirrors bake_probes()'s (no camera_pos
  // parameter) since it's only ever called from update_gi_cascade(), which
  // already has the current camera_pos in scope when it decides a bake is
  // due.
  glm::vec3 last_camera_pos_{0.0f};

  // chunk_streaming_levels_ (one ChunkStreamingManager per clip level) was
  // removed along with that class and update_streaming() (M0 CSG deletion
  // pass, see /home/h-i/.claude/plans/joyful-skipping-giraffe.md) -- it was
  // the camera-relative streaming core's own per-level bookkeeping, with no
  // remaining driver.

  // A scene edit invalidates every already-Ready resident chunk's baked
  // content (see update_streaming()'s own comment for why this queue
  // exists and isn't just a live re-check of GeometrySystem::dirty_since_
  // last_snapshot() every frame -- that would re-evict chunks the previous
  // sweep had already refreshed, forever, instead of converging). Captured
  // as {level, key} pairs in one sweep the frame a scene edit is first
  // noticed, then drained a budget's worth per level per frame exactly like
  // an ordinary boundary eviction, until empty.
  std::vector<std::pair<u32, ChunkKey>> pending_forced_evictions_;
  // Mirrors pending_forced_evictions_' own membership -- every {level, key}
  // pair currently sitting in that vector (queued but not yet drained) also
  // has an entry here, and vice versa; kept in exact lockstep by every push/
  // erase of the vector. Exists purely so update_streaming()'s producers can
  // check "is this chunk already queued, from THIS or an EARLIER dirty
  // cycle" in O(1) before pushing a duplicate -- see pending_forced_
  // evictions_dirty_cycle_dedup_'s own comment (update_streaming()'s body)
  // for why a duplicate here isn't just harmless redundancy: a rapid
  // sequence of dirty cycles (a gizmo drag firing one every tick, each
  // BEFORE the previous cycle's forced-rebakes fully drain) would otherwise
  // requeue the SAME still-Baking/still-queued chunk over and over, with
  // nothing ever shrinking the backlog faster than new duplicates pile onto
  // it -- confirmed live as sustained, worsening per-frame lag/GPU cost
  // during a real drag, not just a theoretical waste.
  std::unordered_set<std::pair<u32, ChunkKey>, PendingForcedEvictionHash>
      pending_forced_eviction_keys_;

  // CHUNK_TABLE_DIM^3 i32s, host-visible -- the CPU writes this directly
  // (see reset_chunked_field()/voxelize_chunk()), the GPU only ever reads
  // it (Builtin.ChunkedFieldCommon.inc.glsl's sample_chunked_field()).
  std::optional<VulkanBuffer> chunk_table_buffer_;
  // kMaxResidentChunks * kChunkCellCount i32s -- device-local, like
  // indirection_buffer_ above; see voxelize_chunk().
  std::optional<VulkanBuffer> chunk_indirection_buffer_;
  // kMaxChunkBricks * kChunkBrickVoxelCount f32s -- this field's own,
  // separate, brick pool (see Builtin.SdfFieldConfig.inc.glsl's comment on
  // why it doesn't share brick_pool_buffer_ above).
  std::optional<VulkanBuffer> chunk_brick_pool_buffer_;
  std::optional<VulkanBuffer> chunk_brick_primitive_buffer_;
  // M3c (Spraypaint): exactly the same size/addressing as chunk_brick_pool_
  // buffer_ above (one f32 per voxel, CHUNK_BRICK_VOXEL_COUNT per brick,
  // same brick-index-based layout) but holds an accumulated PAINT opacity
  // in [0, 1] instead of a signed distance -- written by Builtin.
  // SculptSpraypaint.comp.glsl (spray_paint()), cleared to 0 for a brick's
  // voxels the instant it's freshly allocated by Builtin.SculptStamp.
  // comp.glsl (see that shader's ChunkBrickPaintBuffer comment for why),
  // and read back in Builtin.DeferredShade.comp.glsl (sample_chunk_paint_
  // at_point()) to tint the final shaded colour. Device-local, TRANSFER_DST
  // added (like chunk_indirection_buffer_ above) so it can be zero-filled
  // once via vkCmdFillBuffer in reset_chunked_field() without a ~1.9GB CPU-
  // side staging array.
  std::optional<VulkanBuffer> chunk_brick_paint_buffer_;
  // M7 polish (live brush-cursor ghost preview): the single small UBO
  // backing set_brush_preview() -- see that method's own comment, and
  // Builtin.DeferredShade.comp.glsl's BrushPreviewBuffer (set = 1, binding
  // = 1 -- the SAME additive set paint_read_set_ already adds for
  // ChunkBrickPaintBuffer at binding 0, not a new set of its own) for the
  // GPU side. Host-visible/host-coherent: set_brush_preview() is called
  // every frame the preview is visible and must stay a cheap direct
  // load_data() write, not a staged upload. Deliberately NOT a push
  // constant -- PushConstants in that shader is already called out, in its
  // own comment, as sitting close to Vulkan's guaranteed 128-byte limit.
  std::optional<VulkanBuffer> brush_preview_buffer_;
  // M7 polish FIX (TAA-ghosting): render-target-sized (same lifetime as
  // depth_image_/ao_image_ -- recreate_render_target_images()) rgba8 image
  // Builtin.DeferredShade.comp.glsl writes the brush preview's shaded
  // colour+alpha into (paint_read_set_'s NEW binding 2) instead of
  // blending it straight into out_image/colour. Builtin.PostComposite.
  // comp.glsl reads it back (post_composite_set_'s NEW binding 3) and
  // composites it AFTER TAA resolve -- see either shader's binding comment
  // for why: TAA reprojects by depth, the preview never touches depth, so
  // blending it in before TAA fed the ghost into TAA's own history and
  // crushed it down to history's tiny per-frame blend weight every frame.
  VulkanImage preview_overlay_image_{};
  // Host-visible -- unconditional per-cell demand counter, zeroed before
  // every voxelize dispatch and read back afterward, exactly mirroring
  // brick_counter_buffer_'s overflow-diagnostic role above (see its own
  // comment) for this field's own, separate pool.
  std::optional<VulkanBuffer> chunk_brick_demand_buffer_;
  // The free-list stack (see Builtin.ChunkVoxelize.comp.glsl's
  // BrickFreeListBuffer) and its top-of-stack pointer -- both host-visible
  // since reset_chunked_field() (re-)initializes them directly from the
  // CPU; the GPU only ever pops from them via atomicAdd.
  std::optional<VulkanBuffer> chunk_brick_free_list_buffer_;
  std::optional<VulkanBuffer> chunk_brick_free_list_top_buffer_;
  // Phase 6: one frame's worth of chunk bake/evict work, uploaded once and
  // covered by a single dispatch each instead of one dispatch per chunk --
  // see update_streaming() and Builtin.ChunkVoxelize.comp.glsl's
  // ChunkVoxelizeBatchSlotBuffer comment (including why the voxelize batch
  // is two parallel arrays rather than one array of structs). Host-visible:
  // update_streaming() writes them directly from the CPU every call that
  // records work.
  // Staging for the disk-backed brick cache -- see kChunkCacheWords.
  std::optional<VulkanBuffer> chunk_cache_staging_buffer_;
  std::optional<VulkanBuffer> chunk_voxelize_batch_slot_buffer_;
  std::optional<VulkanBuffer> chunk_voxelize_batch_data_buffer_;
  std::optional<VulkanBuffer> chunk_evict_batch_buffer_;

  // Splat point CLUSTERS (step 2 -- see CHUNK_CLUSTER_POINTS in Builtin.
  // SdfFieldConfig.inc.glsl). A shared pool of fixed-capacity pages, handed
  // out from its own free list exactly like the brick pool: Builtin.
  // ChunkVoxelize.comp.glsl claims as many pages as a brick's surface needs
  // and fills them, Builtin.ChunkEvict.comp.glsl gives them back, and every
  // consumer (splat, cull, shadow, render) works in whole clusters.
  //
  // This replaced a fixed-size point array per brick, whose failure mode was
  // silent and severe: a brick whose surface wanted more points than the
  // array held lost the remainder, and a splat gap does not fall back to
  // marching -- it shows whatever is behind the surface.
  //
  // The point pool stays host-visible on purpose (the two brick pools
  // already sit within ~1GB of a 4GB card's whole device-local heap, and
  // this one is ~268MB); the small records/free-list buffers follow it so
  // the CPU can seed them at reset with no staging copy.
  std::optional<VulkanBuffer> chunk_cluster_point_buffer_;
  std::optional<VulkanBuffer> chunk_cluster_buffer_;
  std::optional<VulkanBuffer> chunk_cluster_free_list_buffer_;
  std::optional<VulkanBuffer> chunk_cluster_free_top_buffer_;
  // Which cluster pages each brick owns (kChunkMaxClustersPerBrick entries
  // per brick, -1 for unused) -- eviction needs it to know what to free.
  std::optional<VulkanBuffer> chunk_brick_cluster_buffer_;
  // Step 3: this frame's visible clusters, compacted by Builtin.
  // ChunkClusterCull.comp.glsl, plus the four-uint block holding the splat
  // pass's vkCmdDispatchIndirect arguments and the visible count. Both
  // device-local and rewritten every frame; the args buffer carries
  // INDIRECT_BUFFER usage on top of STORAGE, and TRANSFER_DST so the count
  // can be zeroed with a fill before each cull.
  // Per-chunk primitive candidate lists (the bake's own broad phase).
  //
  // scene_map() walks every primitive in the scene at every sample, paying
  // a bounding-sphere test to reject each one. A chunk bake is hundreds of
  // bricks x ~5,800 voxels, so on a scene of any size that rejection work
  // dominates everything else the bake does -- measured at 100-300ms per
  // chunk. But a chunk is a small box: the primitives that can possibly
  // matter anywhere inside it are known before the dispatch starts, from
  // bounds the CPU already has.
  //
  // So each batch entry carries a list of just those primitives, and the
  // voxelizer folds only them. This is the same insight Dreams' evaluator
  // is built on -- refine a list of candidate edits per region, then
  // evaluate against the short list -- in its flat, one-level form.
  //
  // chunk_candidate_buffer_ holds the lists back to back; chunk_candidate_
  // range_buffer_ says where each entry's list starts and how long it is,
  // with a count of -1 meaning "too many to list, evaluate everything" so
  // an overflowing chunk degrades in speed rather than in correctness.
  std::optional<VulkanBuffer> chunk_candidate_buffer_;
  std::optional<VulkanBuffer> chunk_candidate_range_buffer_;
  // Parallel to chunk_candidate_buffer_, one vec4 per entry: xyz = the
  // local-space offset of the repetition instance that entry names, w != 0
  // if it names one at all. See ChunkCandidateOffsetBuffer in Builtin.
  // ChunkVoxelize.comp.glsl and PrimitiveBound's comment for why resolving
  // instances per chunk is worth a whole extra buffer.
  std::optional<VulkanBuffer> chunk_candidate_offset_buffer_;
  // One i32 per cell of every batch entry: -1 to bake, else the local cell
  // index this cell is a bit-identical copy of. See ChunkCellAliasBuffer in
  // Builtin.ChunkVoxelize.comp.glsl and build_cell_alias_map().
  std::optional<VulkanBuffer> chunk_cell_alias_buffer_;
  // World-space bounds of every registered primitive, cached by
  // rebuild_static_scene() so building the lists above costs no snapshot
  // walk per chunk. packed_index is the primitive's own index with its
  // layer index in the high 16 bits -- the shader needs both to fold it.
  struct PrimitiveBound {
    glm::vec3 position{0.0f};
    // The whole primitive's reach, farthest repeated copy included -- what
    // the cheap "can this primitive touch this chunk at all" test uses.
    f32 radius = 0.0f;
    i32 packed_index = 0;

    // --- Domain repetition, resolved per chunk (see voxelize_chunk_batch()).
    //
    // A bounding SPHERE is a hopeless bound for a repeated primitive: 20x20
    // copies on a 10-unit cell give radius ~134 world units, which is wider
    // than the entire streamed field. Such a primitive is therefore in every
    // chunk's candidate list, never culled anywhere, and costs eight
    // evaluate_primitive_at() calls per sample because repeat_limited() has
    // to check every neighbouring tile. On a scene built out of repeated
    // architecture -- which is most of what repetition exists for -- that is
    // the dominant cost of the entire bake.
    //
    // These let the CPU ask the far more useful question instead: which
    // INSTANCES actually reach this chunk? Usually exactly one, whose local
    // -space offset is then handed to the shader directly, so it evaluates a
    // plain unrepeated primitive and the other 399 copies cost nothing.
    u32 repeat_mode = 0; // RepetitionMode, 0 = None
    glm::vec3 repeat_cell{0.0f};
    glm::vec3 repeat_count{0.0f};
    // The reach of ONE instance -- radius above minus the repetition spread.
    f32 instance_radius = 0.0f;
    // Rotation taking the primitive's local space to world space; its
    // inverse maps a chunk's world-space box back into the space repetition
    // is expressed in.
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    // Whichever layer's smoothness this primitive folds with. Instances may
    // only be enumerated separately when this is zero: the fold is a
    // smooth_union, and only at k = 0 does it degenerate to the exact min()
    // that repeat_limited() takes over its candidates. With a real blend
    // radius, folding two instances one after another would blend them into
    // each other instead, which is a different shape.
    f32 layer_smoothness = 0.0f;
    // Whether this primitive's LAYER repeats (Geometry::
    // layer_repetition_mode). Its spread is already inside `radius`; this
    // flag is what keeps the two optimisations that reason about
    // repetition -- per-chunk instance resolution and the cell alias map --
    // away from a primitive whose fold they don't model. See their uses in
    // build_chunk_candidates()/build_cell_alias_map().
    bool layer_repeated = false;
  };
  std::vector<PrimitiveBound> primitive_bounds_;

  std::optional<VulkanBuffer> chunk_visible_cluster_buffer_;
  std::optional<VulkanBuffer> chunk_cull_args_buffer_;
  // Step 5: the imperfect shadow maps. shadow_pair_buffer_ holds this
  // frame's (cluster, light) work list and shadow_args_buffer_ its indirect
  // dispatch arguments plus the pair count (same four-uint convention as
  // the cull's); shadow_atlas_buffer_ is kIsmCount tiles of kIsmResolution^2
  // distances, atomicMin'd by the splat and read by the shading pass.
  std::optional<VulkanBuffer> chunk_shadow_pair_buffer_;
  std::optional<VulkanBuffer> chunk_shadow_args_buffer_;
  std::optional<VulkanBuffer> shadow_atlas_buffer_;
  VulkanShaderModule chunk_shadow_splat_stage_;
  VkDescriptorSetLayout chunk_shadow_splat_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet chunk_shadow_splat_set_ = VK_NULL_HANDLE;
  std::optional<VulkanComputePipeline> chunk_shadow_splat_pipeline_;
  b8 ism_enabled_ = true;

  // Step 6: the ambient-occlusion pass and the structure it traces against.
  // resident_cluster_buffer_ is the cull's third list (every resident
  // cluster, frustum or not -- occluders behind the camera still occlude);
  // voxel_cascade_buffer_ is kVoxelCascadeCount cascades of
  // kVoxelCascadeDim^3 BITS, rebuilt from the point cloud every frame;
  // ao_image_ is the per-pixel visibility the shading pass multiplies its
  // indirect term by.
  std::optional<VulkanBuffer> chunk_resident_cluster_buffer_;
  std::optional<VulkanBuffer> chunk_resident_args_buffer_;
  // kNumLevels * kMaxResidentChunks u32s: 1 where that chunk slot holds a
  // completed, published bake, 0 otherwise. Host-visible and written by the
  // CPU exactly like chunk_table_buffer_ (the two are always updated
  // together -- see write_chunk_table_entry()), and read by Builtin.Chunk
  // ClusterCull.comp.glsl, which is the one pass that walks the cluster
  // pool flat rather than resolving through the table and so has no other
  // way to know whether a page's owning chunk is finished. See that
  // shader's ChunkSlotPublishedBuffer comment for why this is what lets the
  // graphics queue stop waiting on the bake.
  std::optional<VulkanBuffer> chunk_slot_published_buffer_;
  std::optional<VulkanBuffer> voxel_cascade_buffer_;
  VulkanImage ao_image_{};
  VulkanShaderModule chunk_voxel_cascade_stage_;
  VkDescriptorSetLayout chunk_voxel_cascade_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet chunk_voxel_cascade_set_ = VK_NULL_HANDLE;
  std::optional<VulkanComputePipeline> chunk_voxel_cascade_pipeline_;
  VulkanShaderModule stochastic_ao_stage_;
  VkDescriptorSetLayout stochastic_ao_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet stochastic_ao_set_ = VK_NULL_HANDLE;
  std::optional<VulkanComputePipeline> stochastic_ao_pipeline_;
  b8 ao_enabled_ = true;
  // The splat visibility buffer (render_width_ x render_height_ u64s --
  // depth in the high half, winning point id in the low half, ~0 = no
  // splat; see Builtin.ChunkPointSplat.comp.glsl's header comment).
  // Cleared via vkCmdFillBuffer and rewritten by the splat dispatch every
  // render_to() call that runs it, then read by Builtin.RaymarchShader.
  // comp.glsl's binding 20. One buffer serves both SplatMode::Prime and
  // ::Visibility -- what's written is identical, only how the render pass
  // reads it differs. Recreated alongside the render target images
  // (recreate_render_target_images()) since it's sized to match them.
  std::optional<VulkanBuffer> splat_visibility_buffer_;
  // The splat pass's conservative per-tile near bound -- one u32 (a float
  // distance as raw bits) per kSplatTileSize-pixel tile of the render
  // target, so it is recreated alongside splat_visibility_buffer_ and for
  // the same reason. A brick that Visibility mode declines to splat
  // atomicMins its own distance into the tiles it covers here, and
  // Builtin.RaymarchShader.comp.glsl refuses any splat farther than its
  // pixel's bound -- without that, geometry behind a declined brick
  // splats straight through it (see Builtin.ChunkPointSplat.comp.glsl's
  // DECLINED BRICKS STILL OCCLUDE note). Cleared to ~0 ("nothing
  // declined") by the same per-frame vkCmdFillBuffer, hence TRANSFER_DST.
  std::optional<VulkanBuffer> splat_tile_bound_buffer_;

  // TAA (step 1). depth_image_ is pass 3's per-pixel hit distance, which is
  // what lets the resolve pass turn a pixel back into a world position
  // without a velocity buffer; history_images_ ping-pong because a pixel
  // reads history at its REPROJECTED position and would otherwise race
  // with another invocation's write; taa_output_image_ is what the post-
  // process chain reads, so bloom/composite bind one static image whether
  // TAA is on or off. All three are sized to the render target and are
  // recreated with it.
  // --- GPU timing (pass-level timestamps). ---
  //
  // The frame is a chain of ten-odd compute dispatches across two queues,
  // and until this existed there was no way to attribute a millisecond to
  // any of them -- which turned every performance question into an argument
  // from structure. Timestamps are cheap (two writes per pass) and are read
  // back from a frame old enough to be finished, so measuring costs no
  // stall of its own.
  //
  // Graphics passes are timed at boundaries that ALWAYS execute, never
  // inside a conditional block: an unwritten query reads back as
  // unavailable and would poison the whole frame's results.
  enum GraphicsTimestamp : u32 {
    kTsFrameBegin = 0,
    // The prepass, broken into its parts. It was one span covering all of
  // them, which is enough to know it is expensive and useless for knowing
  // WHICH of five dispatches to attack -- and guessing wrong there costs a
  // day. Kept split permanently; a timestamp is a few cycles.
  kTsPrepassClears,    // vkCmdFillBuffer over the per-frame scratch buffers
  kTsPrepassCull,      // cluster cull over the whole pool
  kTsPrepassCascade,   // binary voxel cascade build
  kTsPrepassShadow,    // imperfect shadow map splat
  kTsStreamingPrepass, // the point splat itself
    kTsVisibility,       // the G-buffer pass
    kTsAmbientOcclusion,
    kTsDeferredShade,
    kTsTaaResolve,
    kTsBloom,
    kTsPostComposite,
    kGraphicsTimestampCount,
  };
  // Frames of query pool to rotate through. Reading the pool written two
  // frames ago is what keeps the readback non-blocking.
  static constexpr u32 kTimestampFrames = 3;
  // Reads back a finished frame's pass timestamps, accumulates them, and
  // logs a report every kTimestampReportInterval frames. Never waits: a
  // frame whose results aren't ready yet is simply skipped.
  // ensure_fixed_brick_pool() -- grew brick_pool_buffer_ from its
  // placeholder to full size the first time the fixed-cube field was
  // baked -- was removed along with voxelize() (M0 CSG deletion pass):
  // nothing bakes that field anymore, so fixed_brick_pool_ready_ below
  // just stays false forever and brick_pool_buffer_ stays at its tiny
  // placeholder size, which is correct now that nothing ever needs it
  // larger.

  void collect_frame_timings();

  // See the comment above -- permanently false now.
  b8 fixed_brick_pool_ready_ = false;

  VkQueryPool graphics_timestamp_pool_ = VK_NULL_HANDLE;
  // Two per async ring slot (bake begin/end), read when that slot's fence
  // is waited on -- by which point the results are guaranteed available.
  VkQueryPool async_timestamp_pool_ = VK_NULL_HANDLE;
  // Nanoseconds per timestamp tick, from the device's own limits.
  f32 timestamp_period_ns_ = 0.0f;
  b8 timestamps_supported_ = false;
  u32 timestamp_frame_ = 0;
  // Rolling accumulation, reported every kTimestampReportInterval frames.
  static constexpr u32 kTimestampReportInterval = 120;
  u32 timestamp_samples_ = 0;
  f64 graphics_pass_ms_[kGraphicsTimestampCount]{};
  // Bake timings are what a stutter on a chunk boundary is made of, so they
  // carry a peak alongside the mean -- an average hides the one 40ms bake
  // that caused the hitch.
  f64 async_bake_ms_total_ = 0.0;
  f64 async_bake_ms_peak_ = 0.0;
  u32 async_bake_samples_ = 0;
  // How many CHUNKS each ring slot's in-flight batch covers, and the total
  // across the reporting interval. The ring timestamps bracket a whole
  // batch submission, so on their own they say "a bake took 2.1 seconds"
  // without saying whether that was one pathological chunk or sixteen
  // ordinary ones -- which is exactly the distinction any work on the
  // per-chunk cost needs to see. Recorded by voxelize_chunk_batch(),
  // consumed alongside the timestamps in update_streaming().
  u32 async_bake_chunks_[kAsyncRingDepth]{};
  f64 async_bake_chunks_total_ = 0.0;
  // How many chunks were baked from a candidate list versus abandoned to
  // the whole-scene scene_map() path -- see voxelize_chunk_batch()'s own
  // comment. That fallback costs roughly an order of magnitude per chunk
  // and had no reporting at all, which is how a single unbounded primitive
  // silently disabling the lists scene-wide stayed invisible.
  u64 candidate_chunks_total_ = 0;
  u64 candidate_chunks_fallback_ = 0;
  // --- "Bake this geometry once, ever": the persistent alias cache.
  //
  // build_cell_alias_map() keys a cell by the function the voxelizer would
  // actually fold there -- every candidate that can reach it, and this
  // cell's position relative to each, reduced modulo repetition. That key
  // is a property of the SCENE, not of the chunk: two cells anywhere, in
  // any chunk, baked at any time, whose keys agree bake to bit-identical
  // bricks.
  //
  // So the map from key to representative is kept across frames rather than
  // rebuilt per batch. A cell whose key was seen before copies that brick
  // instead of evaluating ~2,000 scene samples -- whether the
  // representative was baked seconds ago in another chunk (deduplication)
  // or is the very chunk being reloaded after an eviction (caching). They
  // are the same mechanism; the only difference is how far back the
  // representative was baked.
  //
  // An entry is only trustworthy while the slot it names still holds the
  // bake that produced it, which slot_content_generation_ tracks -- see
  // invalidate_slot_content().
  struct AliasCacheEntry {
    u32 global_cell = 0; // slot * kChunkCellCount + local cell
    u64 slot_generation = 0;
  };
  std::unordered_map<u64, AliasCacheEntry> alias_cache_;
  // Bumped every time a slot's content is replaced or freed. An alias cache
  // entry is valid only while this still matches what it recorded.
  std::vector<u64> slot_content_generation_;
  u64 next_slot_content_generation_ = 1;
  // Ceiling on alias_cache_ so a long session cannot grow it without bound
  // (it is cleared wholesale when exceeded -- entries are pure optimisation,
  // so losing them costs bake time, never correctness).
  static constexpr size_t kMaxAliasCacheEntries = 1u << 20;
  // How many aliased cells came from a representative baked in an EARLIER
  // batch rather than this one -- i.e. how much of the win is caching
  // across time rather than deduplication within a batch. Reported with the
  // bake timings.
  u64 alias_cells_cached_ = 0;
  // Why deduplication declined, when it declines for every chunk -- see the
  // report line. "no periodic candidate" means the CPU resolved every
  // repeated primitive down to individual instances (so nothing looks
  // periodic any more); "no cell step" means this level's cell size never
  // lands on a whole repetition period within one chunk.
  u64 alias_bail_no_periodic_ = 0;
  u64 alias_bail_level_ = 0;

  // Cells the alias map found to be copies, against cells considered --
  // the one number that says whether brick deduplication is doing anything
  // on the content being streamed. Reported with the bake timings.
  u64 alias_cells_total_ = 0;
  u64 alias_cells_aliased_ = 0;
  // Whether each ring slot has a bake recorded whose timestamps are worth
  // reading -- without it the first lap reads queries that were never
  // written.
  b8 async_bake_recorded_[kAsyncRingDepth]{};

  VulkanImage depth_image_{};
  // The other half of the G-buffer (step 4): xyz = surface normal, w =
  // material index, written by the visibility pass and consumed by the
  // deferred shading pass. rgba16f so a material index up to
  // kMaxScenePrimitives stays exactly representable alongside a normal.
  VulkanImage normal_material_image_{};
  VulkanImage history_images_[2]{};
  VulkanImage taa_output_image_{};
  // Deferred shading (step 4) -- reads the G-buffer the visibility pass
  // writes and produces the actual image. Shares render_set_layout_ (and
  // render_set_) with the visibility pass: the two shaders read the same
  // scene, and one descriptor set for both is what keeps their views of it
  // from drifting.
  VulkanShaderModule deferred_shade_stage_;
  std::optional<VulkanComputePipeline> deferred_shade_pipeline_;
  VulkanShaderModule taa_resolve_stage_;
  VkDescriptorSetLayout taa_resolve_set_layout_ = VK_NULL_HANDLE;
  // One set per ping-pong parity: set N reads history_images_[N] and writes
  // history_images_[1-N].
  VkDescriptorSet taa_resolve_sets_[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
  std::optional<VulkanComputePipeline> taa_resolve_pipeline_;
  // Which history image this frame READS. Flipped at the end of every
  // render_to() that ran the resolve.
  u32 taa_history_parity_ = 0;
  // Frame counter driving the sub-pixel jitter sequence -- see
  // kTaaJitterCount in the .cpp.
  u32 taa_frame_ = 0;
  // Seed for every pass's per-frame stochastic decisions (the AO ray, the
  // ISM tap rotation, dithers). Deliberately NOT taa_frame_: that cycles
  // modulo kTaaJitterCount (8), and seeding the noise from it meant every
  // pixel only ever saw 8 distinct sample patterns -- the temporal average
  // then converges to a fixed 8-sample estimate whose error is permanent,
  // static grain no amount of accumulation removes. This one free-runs (see
  // kNoiseFrameWrap) so successive frames draw fresh, uncorrelated samples.
  u32 noise_frame_ = 0;
  b8 taa_enabled_ = true;
  // False until a full frame of history exists to blend against (startup,
  // resize, origin recenter) -- see invalidate_temporal_history().
  b8 history_valid_ = false;
  // The camera basis the PREVIOUS frame rendered with, in render space.
  // Only meaningful while history_valid_ is true.
  glm::vec3 prev_camera_position_{0.0f};
  glm::vec3 prev_camera_forward_{0.0f, 0.0f, 1.0f};
  glm::vec3 prev_camera_right_{1.0f, 0.0f, 0.0f};
  glm::vec3 prev_camera_up_{0.0f, 1.0f, 0.0f};
  // 12 bytes (float dist, float skip_dist, int material_index) -- see
  // query_chunked_field()/debug_verify_chunked_field().
  std::optional<VulkanBuffer> chunk_debug_query_output_buffer_;
  // Phase 5: the chunked field's GI cascade, double-buffered exactly like
  // probe_buffer_a_/probe_buffer_b_ above (same ping-pong bounce scheme --
  // see bake_gi_cascade()). Fully separate objects from those, matching
  // the "own everything" separation every other chunked-field buffer here
  // already follows.
  std::optional<VulkanBuffer> chunk_gi_probe_buffer_a_;
  std::optional<VulkanBuffer> chunk_gi_probe_buffer_b_;

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

  // Four loose per-material scalars per static primitive, parallel to
  // primitive_colour_buffer_ -- the odds and ends that don't each justify
  // a buffer and binding of their own, and that the two existing
  // per-primitive vec4 buffers (primitive_colour_buffer_'s rgb+
  // texture_scale, tex_transform_buffer_'s offset+rotation) have no free
  // component left to carry:
  //   x = Material::pixelation_exempt (1/0) -- written into output_image_'s
  //       alpha channel by the render pass for the post-process pass's
  //       pixelation step to read.
  //   y = Material::bump_strength -- scales the bump-map normal
  //       perturbation in Builtin.DeferredShade.comp.glsl.
  //   z = Material::casts_shadow (1/0) -- a 0 makes the shadow paths treat
  //       this primitive's surface as if it weren't there (see
  //       SHADOW_CASTER_TEST in Builtin.BakedFieldCommon.inc.glsl).
  //   w = unused padding (a vec4 is what std430 gives this array anyway).
  // Every slot defaults to "not exempt, ordinary bump, casts shadows", so
  // a slot no registered primitive claims behaves like a plain material.
  std::optional<VulkanBuffer> material_scalar_buffer_;
  // One vec4 per primitive index: originally xyz = the per-channel
  // Beer-Lambert absorption coefficient, w = index of refraction (0 =
  // opaque) -- but the whole glass/transmission shading block that used to
  // READ this buffer (Builtin.DeferredShade.comp.glsl's old transmissive_
  // march()/transmissive_normal() family) was deleted in the M0 CSG
  // deletion pass, and nothing has read any component of it since --
  // confirmed by grepping that shader for "transmission[" outside its own
  // declaration. M5 (Finishes) repurposes component .x as a "waxy" flag
  // (1.0/0.0) for exactly that reason: it was genuinely dead weight, not
  // slack borrowed from a still-live opaque-material code path. See
  // VulkanRaymarchShader::set_material()'s own header comment for the
  // write side and the shader's WAXY_LOBE_* constants for the read side.
  // .y/.z/.w remain unused (0) for every slot, same as before.
  //
  // A separate buffer rather than more slots on material_scalar_buffer_
  // because that vec4 is full (pixelation-exempt, bump strength, shadow
  // casting, roughness), and rather than push constants because the
  // shading pass's push block is at 124 of Vulkan's guaranteed 128 bytes.
  std::optional<VulkanBuffer> transmission_buffer_;
  // The transmissive tail range in primitive_buffer_ -- appended
  // immediately after the volumetric one, so its start is derivable
  // shader-side as volumetric_start + volumetric_count and only the count
  // has to be pushed. See rebuild_static_scene().
  i32 transmissive_start_ = 0;
  i32 transmissive_count_ = 0;

  // Each static (and volumetric) primitive's effective texture offset/
  // rotation -- xyz = material->def.uv_offset * geometry.
  // texture_offset_scale (world units), w = material->def.uv_rotation
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

  // selected_primitive_index_/dynamic_primitive_name_/
  // dynamic_primitive_index_/dynamic_primitive_resync_name_ -- the
  // selection-outline and interactive-primitive-drag state -- were removed
  // in the M0 CSG deletion pass along with the API that set them.

  // See set_grid_visible() above.
  b8 grid_visible_ = false;

  // See set_chunked_field_enabled() above.
  b8 chunked_field_enabled_ = false;

  // See set_splat_mode()/SplatMode above.
  SplatMode splat_mode_ = SplatMode::Prime;

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

  // layer_count_/max_smoothness_/volumetric_start_/volumetric_count_ --
  // all set only by rebuild_static_scene()/voxelize()/voxelize_chunk(),
  // all removed -- were removed along with them (M0 CSG deletion pass).

  // Seconds since construction, accumulated every render_to() call -- see
  // PushConstants::time in vulkan_raymarch_shader.cpp, which drives
  // accumulate_volumetrics()'s scrolling texture animation.
  f32 elapsed_time_ = 0.0f;

  // See set_skybox()/disable_skybox() above. skybox_texture_name_ is only
  // meaningful while skybox_enabled_ is true -- it's what disable_skybox()
  // releases through TextureSystem.
  bool skybox_enabled_ = false;
  std::string skybox_texture_name_;

  // primitive_gpu_index_by_name_/refined_bound_radius_by_name_/
  // previous_refined_bound_radius_by_name_/refined_bounding_radius()/
  // previous_refined_bounding_radius() -- all rebuilt-primitive-bound
  // bookkeeping keyed by Geometry, which no longer exists -- were removed
  // in the M0 CSG deletion pass.

  bool valid_ = false;
};
