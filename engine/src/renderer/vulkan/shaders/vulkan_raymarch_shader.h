#pragma once
#include "../../../systems/chunk_streaming_manager.h"
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

  // Phase 3b: the camera-driven streaming entry point -- call once per
  // frame (see VulkanRendererBackend::begin_frame(), right after
  // maybe_recenter_origin(), before render_to()) with the camera's current
  // render-space position. Drives chunk_streaming_ (tick() + update()),
  // then records up to kMaxChunkBakesPerFrame voxelize_chunk()/
  // evict_chunk() calls (boundary streaming) and up to
  // kMaxForcedRebakesPerFrame dirty-triggered in-place rebakes -- bounded
  // per frame so this never becomes a rebake()-style device-idle stall,
  // unlike rebuild_static_scene()'s voxelize()/bake_probes() path (which
  // sdf_editor's chunked-field-disabled fallback still uses unchanged). A
  // no-op if nothing needs loading/evicting/rebaking this frame (the
  // common case once the camera settles and nothing's being edited).
  //
  // Phase 6: no longer takes a caller-provided command buffer -- any
  // recorded work goes into this call's own slot of async_command_
  // buffers_ (see that member's own comment) and is submitted to
  // VulkanDevice::async_compute_queue right here, not left for the caller
  // to submit alongside the main per-frame graphics work.
  //
  // Returns nothing, and the caller's graphics submission NEVER WAITS ON
  // THE BAKE. It used to: this returned the async submission's semaphore
  // and VulkanRendererBackend::begin_frame() added it as a wait, so
  // render_to()'s chunked-field reads couldn't race a still-in-flight
  // write. Correct, and the single largest source of stutter in the
  // engine -- while the camera moves, chunk work is generated on
  // essentially every frame, so every frame waited for that frame's own
  // bake to finish. Measured bakes ran 600-2100ms mean with a 4.5 SECOND
  // peak on a large scene; those numbers were frame times.
  //
  // The race is now closed by making an in-flight bake INVISIBLE instead
  // of by stalling: nothing a bake writes is reachable until the CPU sees
  // that submission's fence signalled and publishes it (see publish_
  // completed_bakes()). The chunk table doesn't name the slot, and the
  // cluster pool's flat sweep skips it (see chunk_slot_published_
  // buffer_). A slow bake now costs briefly-stale content at the
  // streaming frontier rather than a multi-second freeze.
  void update_streaming(glm::vec3 camera_pos);

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

  // Marks one primitive as INTERACTIVELY MOVING: kept out of every chunk's
  // candidate list, so no bake folds it and moving it invalidates no
  // chunks, and unioned into the marched field analytically instead so it
  // still draws and still moves at frame rate.
  //
  // The trade is that only the marched field knows about it: shadows,
  // ambient occlusion and GI all read baked data, so they keep lighting
  // the space it used to occupy until it is committed. That lag is
  // precisely what makes dragging free.
  //
  // Pass an empty name to commit -- the next scene rebuild puts it back in
  // the bake, and the ordinary dirty sweep re-bakes exactly the chunks its
  // old and new bounds cover.
  //
  // Held by NAME, not index: primitive indices shift on every scene
  // rebuild, and a stale index would silently drag a different primitive.
  // The caller must trigger a scene rebuild after this (the backend sets
  // scene_dirty_): the index is only resolvable while walking the scene,
  // and the bounds list that excludes it is only rebuilt there.
  // Returns whether anything actually changed.
  b8 set_dynamic_primitive(std::string name) {
    if (dynamic_primitive_name_ == name) {
      return false;
    }
    // BOTH transitions need the chunks it currently occupies evicted once,
    // for opposite reasons.
    //
    // Entering: it is still baked where it was, and those chunks are about
    // to stop being invalidated -- without this it draws twice, a ghost at
    // the old position plus the moving copy.
    //
    // Leaving: it must be baked back IN, and nothing else will ask for
    // that. The drag moved it through set_primitive_transform(), which
    // deliberately does not mark_dirty(), so by the time the editor
    // reconciles on release the file and GeometrySystem already agree --
    // no primitive is dirty, no chunk is invalidated, and the primitive
    // simply never comes back. Live-confirmed: it vanished on release.
    //
    // Either way the name to resync is the one that is (or was) dynamic.
    dynamic_primitive_resync_name_ =
        name.empty() ? dynamic_primitive_name_ : name;
    dynamic_primitive_name_ = std::move(name);
    return true;
  }
  const std::string &dynamic_primitive_name() const noexcept {
    return dynamic_primitive_name_;
  }

  // Rewrites one uploaded primitive's position/rotation in place, skipping
  // the whole-scene rebuild and the graphics-queue idle that rebake() does.
  // Only sound for a primitive that is currently dynamic -- see the
  // definition. False means the name is not uploaded and the caller should
  // fall back to a full rebake.
  b8 update_primitive_transform(std::string_view name, glm::vec3 position,
                                glm::vec3 rotation_euler);

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

  // --- Chunked field (Phase 3a of the clipmap streaming work -- see
  // ChunkKey/ChunkRecord, engine/src/systems/chunk_types.h). Deliberately
  // NOT wired into rebuild_static_scene()/render_to() yet (a later phase's
  // ChunkStreamingManager does that) -- these exist purely to prove the new
  // chunk-addressed GPU scheme (toroidal chunk table -> per-chunk
  // indirection sub-block -> shared brick pool via a free-list allocator)
  // in isolation, on buffers fully separate from the working fixed-cube
  // field above, before anything depends on it. ---

  // Mirrors check_brick_overflow() for the chunked field's own, separate
  // brick pool -- see that method's own comment. Reads chunk_brick_
  // demand_buffer_ (see its own member comment for why this was, until
  // now, a write-only diagnostic hook nothing ever consumed). Call only
  // once the voxelize_chunk_batch() dispatch that most recently wrote it
  // is confirmed complete -- update_streaming()'s ensure_async_cmd()
  // lambda is the one safe place this holds (right after fence-waiting
  // for this ring slot's PREVIOUS submission, which -- since every
  // submission to async_compute_queue is ordered FIFO on that one queue
  // -- also guarantees every earlier submission, including whichever one
  // last wrote this counter, has retired too).
  //
  // A per-BATCH (not per-resident-chunk) signal: it catches a single
  // frame's newly-(re)baked chunks demanding more bricks than the WHOLE
  // shared pool holds, but NOT slow cumulative exhaustion from many
  // chunks accumulating residency across many frames with no single
  // frame's demand ever spiking that high. Still the right first
  // diagnostic to add -- the old field's identical check has the same
  // "single bake" framing, which is exact there since it never streams.
  // Bakes every chunk the scene's bounds cover straight into the disk
  // cache, so play never pays a cold bake. See its definition.
  void prewarm_chunk_cache();
  // Brick count of a gathered payload sitting in a staging region; 0 means
  // the chunk holds no surface.
  u32 cached_chunk_brick_count(u32 region);
  // KENGINE_PREWARM_CACHE requested it; and whether it has already run for
  // this scene (it is a one-shot warm-up, not a per-frame concern).
  b8 prewarm_requested_ = false;
  b8 prewarm_done_ = false;
  void check_chunk_brick_overflow();
  // Copies both GPU free-list stack pointers into the host-visible stats
  // buffer -- see the definition for why neither pool running dry was
  // otherwise visible at all.
  void record_pool_gauges(VulkanCommandBuffer &cmd);
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
  void upload_to_device_local(VulkanBuffer &dest, const void *data, u64 size);
  // Non-blocking sweep of the async ring: for every slot whose fence says
  // its bake has finished, makes that bake's chunks reachable (chunk table
  // + published flag) and queues any slots it replaced for retirement.
  // Called at the top of update_streaming(), every frame.
  // Marks which cells of one chunk bake to identical bricks -- see its
  // definition. out_alias must have kChunkCellCount entries.
  u32 build_cell_alias_map(const std::vector<i32> &chunk_candidates,
                           const std::vector<glm::vec4> &chunk_candidate_offsets,
                           u32 candidate_count, glm::vec3 chunk_min,
                           f32 cell_size, f32 max_smoothness, u32 chunk_slot,
                           const std::unordered_set<u32> &excluded_slots,
                           i32 *out_alias);

  // Invalidates everything the alias cache believes about one slot's
  // contents. MUST be called wherever a slot's bricks stop being the ones
  // an earlier bake put there -- a load into it, or an eviction that frees
  // them. A missed call is the one way this cache can be actively wrong:
  // a stale entry would have a later bake copy from a brick that has since
  // been recycled into unrelated geometry.
  void invalidate_slot_content(u32 slot);
  void publish_completed_bakes();
  // Blocking counterpart to publish_completed_bakes(), for the synchronous
  // debug_verify_*() harnesses only -- see its definition.
  void drain_streaming_for_debug();
  // Sets a chunk slot's entry in chunk_slot_published_buffer_. Paired with
  // every write_chunk_table_entry() call -- the two must never disagree.
  void set_chunk_slot_published(u32 gpu_slot, bool published);

  // Resets the chunked field to empty: fills chunk_table_buffer_ with -1
  // (no chunk resident anywhere) and re-seeds chunk_brick_free_list_buffer_/
  // chunk_brick_free_list_top_buffer_ to a full free pool ([0, 1, ...,
  // kMaxChunkBricks-1], top = kMaxChunkBricks). Called once from the
  // constructor; a later phase's ChunkStreamingManager calls this whenever
  // it wants to discard every resident chunk (e.g. a floating-origin
  // recenter large enough to invalidate the whole window).
  void reset_chunked_field();

  // Assigns world_chunk_coord to gpu_slot in the toroidal chunk table
  // (see Builtin.ChunkedFieldCommon.inc.glsl's floor_mod3()) and records a
  // Builtin.ChunkVoxelize.comp.glsl dispatch into cmd that bakes exactly
  // that chunk's CHUNK_COARSE_DIM^3 cells into gpu_slot's indirection
  // sub-block, reading the SAME currently-uploaded primitive_buffer_/
  // layer_buffer_/param_expr_buffer_ the fixed-cube voxelizer reads (see
  // this method's .cpp comment for why sharing those specific three is
  // safe). Does not allocate/submit cmd itself, matching voxelize()'s own
  // convention -- callers batch multiple chunk bakes into one command
  // buffer/submission the same way rebuild_static_scene() does for
  // voxelize()+bake_probes().
  // level selects the clip level (0 = finest -- see chunk_level_world_size()
  // .cpp) this chunk belongs to, both for sizing (a higher level's chunk
  // covers more world space per cell) and for which of chunk_table_buffer_'s
  // per-level sub-ranges gets the table write.
  void voxelize_chunk(VulkanCommandBuffer &cmd, glm::ivec3 world_chunk_coord,
                      u32 level, u32 gpu_slot, u32 layer_count,
                      f32 max_smoothness);

  // Shared by voxelize_chunk()/evict_chunk() -- see its own .cpp comment.
  void write_chunk_table_entry(glm::ivec3 world_chunk_coord, u32 level, i32 value);

  // Phase 6: bakes an entire batch of chunks in ONE dispatch -- see this
  // method's own .cpp comment (and Builtin.ChunkVoxelize.comp.glsl's) for
  // the full design. voxelize_chunk() above is now a single-entry
  // convenience wrapper around this for callers (the debug_verify_*()
  // harness) that only ever bake one chunk at a time; update_streaming()
  // is the real caller for a non-trivial batch.
  // evicting_slots names the slots whose bricks THIS submission frees
  // before the voxelize dispatch runs -- the alias cache must not offer any
  // of them as a copy source. Empty for callers that record no evictions.
  void voxelize_chunk_batch(VulkanCommandBuffer &cmd,
                            const std::vector<GpuChunkBatchEntry> &entries,
                            u32 layer_count, f32 max_smoothness,
                            const std::unordered_set<u32> &evicting_slots = {});

  // --- Disk-backed brick cache (strategy 1). See the .cpp definitions.
  //
  // Records the GPU gather of one freshly baked chunk into a staging
  // region, and the restore of one from a region the CPU has filled.
  void record_cache_gather(VulkanCommandBuffer &cmd, u32 slot, u32 region,
                           glm::vec3 chunk_world_min, f32 cell_size);
  void record_cache_restore(VulkanCommandBuffer &cmd, u32 slot, u32 region,
                            glm::vec3 chunk_world_min, f32 cell_size);
  // Hash of everything about the scene that can affect one chunk's bake --
  // the cache key. Two chunks whose keys agree bake to identical bricks.
  u64 chunk_content_hash(const std::vector<i32> &candidates,
                         const std::vector<glm::vec4> &offsets, u32 count,
                         glm::vec3 chunk_min, f32 cell_size) const;
  // Path of a cache entry, and the read/write halves of the store.
  std::string chunk_cache_path(u64 key) const;
  // Content key for one chunk, built from the candidate list its bake would
  // use. 0 means "not cacheable" (no usable candidate list).
  u64 chunk_key_for(glm::vec3 chunk_world_min, f32 cell_size);
  // Claims a staging region, or kInvalidChunkSlot if all are in flight.
  u32 acquire_cache_region();
  b8 load_cached_chunk(u64 key, u32 region);
  void store_cached_chunk(u64 key, u32 region);

  // Per-primitive content fingerprints, rebuilt with the scene -- the
  // ingredient that makes a chunk key change exactly when the geometry
  // reaching it changes, and not otherwise (so an edit invalidates only the
  // chunks it actually touched).
  std::vector<u64> primitive_content_hash_;
  // The same, per LAYER (only the layer_count_ live ones) -- a layer's op
  // and smoothness pick the fold its primitives take part in, so they can
  // change a bake without any primitive changing.
  std::vector<u64> layer_content_hash_;
  // Where cache entries live, and whether the cache is usable at all.
  std::string chunk_cache_dir_;
  b8 chunk_cache_enabled_ = false;
  // Keys known to be absent on disk, so a miss costs one lookup rather than
  // a failed open every time the chunk is re-planned.
  std::unordered_set<u64> chunk_cache_misses_;
  u64 chunk_cache_hits_ = 0;
  u64 chunk_cache_stores_ = 0;
  // Chunks the gather produced but store_cached_chunk() refused because they
  // held more than kChunkCacheMaxBricks bricks. These are the EXPENSIVE
  // chunks, and a refusal means they re-bake in full on every visit.
  u64 chunk_cache_too_dense_ = 0;
  // Which staging regions are claimed by an in-flight gather or restore.
  // A region is released when the submission that used it has signalled,
  // which publish_completed_bakes() observes through the same ring fences
  // everything else here is paced by.
  b8 chunk_cache_region_busy_[8]{};
  // Gathers to record once this frame's bake dispatch has been issued --
  // they must follow it in the same submission, since they read the brick
  // indices that bake writes.
  struct PendingCacheGather {
    u32 slot = 0;
    u32 region = 0;
    glm::vec3 chunk_world_min{0.0f};
    f32 cell_size = 0.0f;
  };
  std::vector<PendingCacheGather> pending_cache_gathers_;

  // Builds one chunk's primitive candidate list -- see its .cpp comment.
  u32 build_chunk_candidates(glm::vec3 chunk_min, f32 cell_size,
                             f32 max_smoothness, i32 *out_candidates,
                             glm::vec4 *out_offsets);

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

  // Clears world_chunk_coord's entry out of the toroidal chunk table (CPU
  // side, host-visible write -- mirrors voxelize_chunk()'s own table
  // write, just to -1 instead of a slot) and records a Builtin.ChunkEvict.
  // comp.glsl dispatch into cmd that frees gpu_slot's bricks back onto the
  // shared free-list and clears its indirection sub-block -- the GPU-side
  // half of ChunkStreamingManager::commit_evict(). The table write must
  // happen here (not left to the caller): sample_chunked_field() resolves
  // a query through the table *before* ever touching the indirection
  // buffer, so leaving a stale table entry pointing at gpu_slot would
  // silently resolve future queries to whatever unrelated chunk reuses
  // that slot next, rather than "no chunk resident" -- exactly the class
  // of bug this method exists to make impossible to forget FOR A SLOT
  // THAT'S ACTUALLY GOING BACK TO THE FREE POOL. Caller (update_streaming())
  // owns the ring-delay that makes calling this safe in that case. level
  // must be the same clip level world_chunk_coord/gpu_slot were originally
  // voxelize_chunk()'d with.
  //
  // clear_table_entry=false is the one deliberate exception: a dirty-scene
  // in-place re-bake (see update_streaming()'s own comment) immediately
  // follows this same call, in the same command buffer, with a
  // voxelize_chunk() targeting this SAME gpu_slot -- never handing it back
  // to the free pool at all, so nothing else can ever observe it between
  // the free and the reload. Leaving the table entry pointing at the OLD
  // (still fully valid) content until that voxelize_chunk() overwrites it
  // is what lets a moved/edited primitive's chunk stay visible the whole
  // time instead of blanking out for the several frames a real evict/
  // reload round-trip through the free list would otherwise take.
  //
  // insert_barrier=false lets a caller batch several chunks' worth of
  // eviction together: this method's own trailing barrier only exists to
  // make ITS free-list push visible to whatever reads it next, and a
  // barrier is transitive -- one barrier after N calls sees all N calls'
  // writes just as correctly as N barriers would, so a caller evicting a
  // whole batch this same frame (see update_streaming()'s dirty-rebake
  // path) can pass false here for every call and record ONE shared
  // barrier itself once the whole batch is recorded, instead of paying
  // this call's fixed barrier overhead once per chunk.
  void evict_chunk(VulkanCommandBuffer &cmd, glm::ivec3 world_chunk_coord,
                   u32 level, u32 gpu_slot, bool clear_table_entry = true,
                   bool insert_barrier = true);

  // Phase 6: frees a whole batch of chunks' bricks in ONE dispatch -- see
  // this method's own .cpp comment (and Builtin.ChunkEvict.comp.glsl's)
  // for the full design. evict_chunk() above is now a single-entry
  // convenience wrapper around this. Does NOT touch chunk_table_buffer_ --
  // see its own .cpp comment for why that stays each caller's concern.
  void evict_chunk_batch(VulkanCommandBuffer &cmd, const std::vector<i32> &slots,
                         bool insert_barrier = true);

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
  // analytically hand-computed expected results (two points exactly on the
  // sphere's surface, in different baked cells, and a point in a chunk
  // that was never baked at all) and KERRORs on any mismatch, KINFOs on
  // success. A live visual render isn't a reliable verification signal in
  // this engine's actual target/test environments, so this buffer-level
  // check is deliberately the primary one -- see this method's .cpp
  // comment for the exact expected values, tolerance, and why the query
  // points are exactly on the surface rather than merely near it. Not
  // currently called by anything (Phase 3a verified it once, by hand, then
  // removed the constructor's call to avoid paying this on every real
  // startup) -- kept as a diagnostic a later phase can re-invoke on demand
  // if this path's behavior is ever in doubt again.
  void debug_verify_chunked_field();

  // Phase 4 sibling of debug_verify_chunked_field() above: bakes one test
  // sphere in a level-0 chunk and a second, independent test sphere in a
  // level-1 chunk that's outside level 0's streaming window but inside
  // level 1's, then queries via sample_clipmap_field() (not sample_
  // chunked_field()) to confirm level selection actually falls through to
  // level 1 for the second point instead of incorrectly reporting "empty"
  // from level 0's failed lookup. Same one-shot, KERROR-on-mismatch/
  // KINFO-on-success contract, same "not currently called by anything"
  // status as debug_verify_chunked_field() -- see its own comment.
  void debug_verify_multi_level_field();

  // Regression test for the kNumLevels=3->5 fix (see kNumLevels's own
  // comment): bakes one test sphere 40 units from the origin, in a chunk
  // only level 3 or 4's streaming window reaches (levels 0-2 alone, the old
  // config, could not) and queries it via sample_clipmap_field() to confirm
  // the field's camera-relative reach actually extends past ~32 units now,
  // not just that levels 3/4 exist and compile. Same one-shot, KERROR-on-
  // mismatch/KINFO-on-success, "not currently called by anything" contract
  // as debug_verify_chunked_field()/debug_verify_multi_level_field() above.
  void debug_verify_extended_range_field();

  // Regression test for update_streaming()'s dirty-chunk force-eviction fix
  // (see its own comment) -- the actual bug report this fixes: sdf_editor's
  // scene renders fine on first load but stops reflecting further edits
  // while the camera stays put, because an already-Ready chunk was never
  // re-baked just because the primitives inside it changed. Simulates
  // several real frames via single-use command buffers (submitted and
  // waited on synchronously, so ChunkStreamingManager's ring-delay advances
  // exactly like live frames would) rather than a one-shot dispatch, since
  // this bug is inherently about state that only resolves across several
  // frames. Same one-shot, KERROR-on-mismatch/KINFO-on-success, "not
  // currently called by anything" contract as the debug_verify_* methods
  // above.
  void debug_verify_dirty_rebake();

  // TEMPORARY -- investigating the live "moving a large primitive drops
  // bricks" report. Builds a grid of spheres, streams them in, queries a
  // baseline, then triggers the exact brute-force sweep update_streaming()
  // does when surgical=false and re-queries to see if anything broke.
  void debug_verify_bulk_sweep_TEMP();

  // TEMPORARY -- investigating a live "renderer crashes when I move an
  // object" report, following the Baking-chunk dirty-rebake fix. Streams in
  // a large resident set, then simulates a rapid gizmo drag: reconcile_
  // scene() + rebake() EVERY frame (not waiting for prior forced-rebakes to
  // drain), for many frames, to stack up dirty cycles the way real spinbox/
  // gizmo-drag ticks do.
  void debug_verify_rapid_drag_crash_TEMP();

  // Regression test for sample_clipmap_field()'s LOD cross-fade blend --
  // bakes a level-0 chunk and its level-1 neighbor with deliberately
  // different, precisely known values, then queries a point placed exactly
  // in the middle of the blend zone and confirms the result is a genuine
  // mix of both, not a hard cutoff to either side. Only meaningful with
  // LOD_BLEND_FRACTION > 0 in Builtin.ChunkedFieldCommon.inc.glsl -- that
  // constant now defaults to 0 (blending disabled) after a live editor
  // test showed it corrupting fine repeated detail against a coarser
  // level's under-resolved voxels (see its own comment for the full
  // reasoning); with the default, this test correctly reports level 0's
  // pure value and FAILs its own blended-value check, which is expected,
  // not a regression. Same one-shot, KERROR-on-mismatch/KINFO-on-success,
  // "not currently called by anything" contract as the debug_verify_*
  // methods above.
  void debug_verify_lod_blend();

  // Regression test for kChunkBrickDim's fix (see its own comment) -- the
  // actual reported bug: a primitive thinner than a coarse level's own
  // voxel spacing can lose its surface crossing entirely between adjacent
  // samples. Bakes a thin box at the coarsest clip level and queries
  // exactly at its center, where the analytic answer is exactly known.
  // Same one-shot, KERROR-on-mismatch/KINFO-on-success, "not currently
  // called by anything" contract as the debug_verify_* methods above.
  void debug_verify_thin_geometry_at_coarse_level();

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

  // --- Chunked field (Phase 3a) -- see reset_chunked_field()/
  // voxelize_chunk()/debug_verify_chunked_field() above. Own pipelines,
  // own descriptor pool, own buffers -- nothing here is shared with the
  // fixed-cube field's Vulkan objects above except primitive_buffer_/
  // layer_buffer_/param_expr_buffer_ (read-only scene-description inputs
  // to both voxelizers -- see voxelize_chunk()'s comment). ---
  VulkanShaderModule chunk_voxelize_stage_;
  VkDescriptorSetLayout chunk_voxelize_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet chunk_voxelize_set_ = VK_NULL_HANDLE;
  std::optional<VulkanComputePipeline> chunk_voxelize_pipeline_;

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
  // Ends and submits this frame's recorded chunk work, and hands its chunks
  // to the ring slot whose fence will report them finished -- see its
  // definition. Declared here rather than with the other methods because it
  // names PendingChunkPublish, defined just above.
  void submit_async_chunk_work(
      VulkanCommandBuffer *async_cmd,
      std::vector<PendingChunkPublish> &queued_publishes);

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

  // Phase 4: one ChunkStreamingManager per clip level (pure CPU bookkeeping,
  // see chunk_streaming_manager.h -- owns no Vulkan object itself), each
  // driving update_streaming()'s load/evict decisions for its own level
  // against a disjoint slot_offset range of the shared chunk_indirection_
  // buffer_/chunk_table_buffer_ (see kMaxResidentChunks/kNumLevels/
  // kStreamRadiusChunks/kFramesInFlightDelay's own comments, .cpp). Sized
  // to kNumLevels and populated in the constructor body (each entry needs
  // a different slot_offset, not expressible as a single member-
  // initializer-list construction).
  std::vector<ChunkStreamingManager> chunk_streaming_levels_;

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
  // Grows brick_pool_buffer_ from its placeholder to full size the first
  // time the fixed-cube field is actually baked, and re-points every
  // descriptor that references it. See its own comment for why the pool
  // starts tiny.
  void ensure_fixed_brick_pool();

  void collect_frame_timings();

  // False until ensure_fixed_brick_pool() has grown brick_pool_buffer_ to
  // its real size.
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
  // The interactively-moved primitive -- see set_dynamic_primitive(). Held
  // by name because indices shift on every scene rebuild; the index is
  // re-resolved from it in rebuild_static_scene(), and is -1 whenever the
  // name is empty or names nothing in the current scene.
  std::string dynamic_primitive_name_;
  i32 dynamic_primitive_index_ = -1;
  // One-shot: the primitive whose currently-occupied chunks must be
  // evicted once, set on entering AND on leaving the dynamic state -- see
  // set_dynamic_primitive() for why both directions need it. Empty when
  // there is nothing to do.
  std::string dynamic_primitive_resync_name_;

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

  // How many layers rebuild_static_scene() last uploaded -- the render
  // pass's per-pixel material re-evaluation (scene_map() in
  // Builtin.SdfSceneCommon.inc.glsl) folds exactly this many LayerBuffer
  // entries, matching what the voxelize pass baked.
  i32 layer_count_ = 0;
  // The largest smoothness value across those same layers, computed once by
  // rebuild_static_scene() and persisted here (unlike voxelize()'s own copy
  // of this value, a plain local -- rebuild_static_scene() only runs on
  // scene_dirty_, but update_streaming() needs this every frame it bakes a
  // chunk, regardless of whether the scene just changed) so voxelize_chunk()
  // calls from update_streaming() widen their cull_radius by the same
  // amount voxelize()'s own dispatch does -- see Builtin.ChunkVoxelize.
  // comp.glsl's CULL_RADIUS_CELLS-adjacent comment.
  f32 max_smoothness_ = 0.0f;

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
