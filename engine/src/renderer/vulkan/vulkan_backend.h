#pragma once
#include "../../core/application.h"
#include "../../systems/geometry_system.h"
#include "../renderer_backend.h"
#include "vulkan_commandbuffer.h"
#include "vulkan_fence.h"
#include "vulkan_framebuffer.h"
#include "vulkan_types.inl"

#include <glm/glm.hpp>
#include <string>
#include <unordered_map>

class PlatformLayer;

class VulkanRendererBackend final : public RendererBackend {
public:
  explicit VulkanRendererBackend(PlatformLayer &plat_state);
  ~VulkanRendererBackend();

  b8 initialize(std::string_view application_name,
                PlatformLayer &plat_state) override;
  void shutdown() override;
  void on_resized(u16 width, u16 height) override;
  b8 begin_frame(f32 delta_time) override;
  void set_camera(const Camera &camera) override;
  void draw_text(std::string_view text, glm::vec2 position,
                glm::vec4 colour) override;
  void draw_ui_quad(glm::vec2 position, glm::vec2 size) override;
  void draw_line(glm::vec2 start, glm::vec2 end, glm::vec4 colour) override;
  void draw_solid_quad(glm::vec2 position, glm::vec2 size,
                      glm::vec4 colour) override;
  SceneHandle load_scene(std::string_view sdf_path) override;
  void translate_scene(SceneHandle handle, glm::vec3 delta) override;
  void rotate_scene(SceneHandle handle, glm::vec3 euler_radians) override;
  void scale_scene(SceneHandle handle, f32 factor) override;
  void remove_scene(SceneHandle handle) override;
  void clear_scenes() override;
  bool reconcile_scene(SceneHandle handle, std::string_view sdf_path) override;
  void set_selected_primitive(i32 index) override;
  i32 primitive_gpu_index(const std::string &name) const override;
  void set_grid_visible(b8 visible) override;
  void set_chunked_field_enabled(b8 enabled) override;
  void set_dynamic_primitive(std::string_view name) override;
  void request_cache_prewarm() override;
  void set_primitive_transform(std::string_view name, glm::vec3 position,
                               glm::vec3 rotation_euler) override;
  void set_splat_mode(RendererSplatMode mode) override;
  void set_taa_enabled(b8 enabled) override;
  void set_ism_enabled(b8 enabled) override;
  void set_ao_enabled(b8 enabled) override;
  void debug_probe_field(glm::vec3 world_pos) override;
  void set_bloom_enabled(b8 enabled) override;
  void set_vignette_enabled(b8 enabled) override;
  void set_pixelation_enabled(b8 enabled) override;
  void set_pixelation_block_size(u32 block_size) override;
  void set_bloom_threshold(f32 threshold) override;
  void set_bloom_intensity(f32 intensity) override;
  void set_vignette_strength(f32 strength) override;
  void set_vignette_radius(f32 radius) override;
  void set_render_scale(f32 scale) override;
  void set_font(std::string_view name, f32 pixel_height) override;
  void set_skybox(std::string_view texture_name) override;
  void disable_skybox() override;
  void set_floating_origin_enabled(b8 enabled) override;
  glm::vec3 consume_origin_shift() override;
  b8 end_frame(f32 delta_time) override;

private:
  struct TextDrawRequest {
    std::string text;
    glm::vec2 position;
    glm::vec4 colour;
  };
  struct UiQuadDrawRequest {
    glm::vec2 position;
    glm::vec2 size;
  };
  struct LineDrawRequest {
    glm::vec2 start;
    glm::vec2 end;
    glm::vec4 colour;
  };
  struct SolidQuadDrawRequest {
    glm::vec2 position;
    glm::vec2 size;
    glm::vec4 colour;
  };

  VulkanContext context_{};
  PlatformLayer *plat_state_ = nullptr;

  // Set via set_camera() each frame (by the game, through the renderer
  // frontend), consumed by end_frame() when it calls the raymarch shader.
  Camera camera_;

  // Queued via draw_text()/draw_ui_quad() (by the game, through the
  // renderer frontend) during its render() step; flushed into actual draw
  // calls and cleared by end_frame(), so nothing draws unless the game
  // queues it again next frame.
  std::vector<TextDrawRequest> queued_text_draws_;
  std::vector<UiQuadDrawRequest> queued_ui_quad_draws_;
  std::vector<LineDrawRequest> queued_line_draws_;
  std::vector<SolidQuadDrawRequest> queued_solid_quad_draws_;

  // Tracks every currently loaded scene (see load_scene()): each handle
  // maps to the names of exactly the primitives/lights that load_scene()
  // call registered with GeometrySystem (see LoadedSceneNames), so
  // remove_scene() can release just those and leave every other
  // concurrently loaded scene untouched.
  std::unordered_map<SceneHandle, LoadedSceneNames> loaded_scenes_;
  SceneHandle next_scene_handle_ = 1; // 0 is kInvalidSceneHandle

  // Set by load_scene()/translate_scene()/rotate_scene()/scale_scene()/
  // remove_scene()/clear_scenes() instead of rebaking immediately -- a
  // single scene load is typically followed by several chained transform
  // calls (see SceneRef's .translate()/.rotate()/.scale()), and rebake() is
  // a full synchronous re-voxelize + GI probe bake, expensive enough that
  // paying it once per mutation instead of once per batch made scene setup
  // take an order of magnitude longer than necessary. begin_frame() checks
  // this and rebakes at most once before the next frame actually draws.
  b8 scene_dirty_ = false;

  // Floating origin (see set_floating_origin_enabled()/consume_origin_shift()
  // and maybe_recenter_origin()). Off by default -- games/tools that never
  // opt in see zero behavior change. world_origin_offset_ is the absolute
  // correction accumulated so far: for anything that's been shifted,
  // absolute_position == render_space_position + world_origin_offset_,
  // exactly. Double precision even though every render-space position stays
  // f32 -- this offset alone accumulates across a whole session, so it's the
  // one quantity that actually needs to survive far more than f32's ~7
  // significant digits without itself drifting.
  b8 floating_origin_enabled_ = false;
  glm::dvec3 world_origin_offset_{0.0, 0.0, 0.0};
  // Recenter once the camera's render-space position drifts this far from
  // the origin. Derived from the baked field's finest voxel size (currently
  // COARSE_CELL_SIZE/BRICK_DIM = (2*16.0/128)/8 =~ 0.03 world units -- see
  // Builtin.SdfFieldConfig.inc.glsl) times a large safety margin: f32
  // mantissa precision only starts mattering once a position's magnitude
  // approaches a meaningful fraction of the largest value representable
  // with sub-voxel precision, which is comfortably past 1000 for a ~0.03
  // voxel -- kept well under that (not a round guess) so recenters stay
  // rare relative to how often a free-roaming camera might cross this
  // distance, while precision is never remotely at risk.
  f32 floating_origin_recenter_threshold_ = 96.0f;
  // Accumulates each recenter's render-space shift until a caller consumes
  // it (see consume_origin_shift()) -- the game must apply this same delta
  // to any Camera position it owns itself (see RendererBackend::
  // consume_origin_shift()'s comment for why this can't happen
  // automatically). Left to accumulate rather than assuming exactly one
  // recenter happens between consecutive consume_origin_shift() calls, so a
  // caller that skips a frame (or never calls it while the flag happens to
  // be off) can't silently lose part of a shift.
  glm::vec3 pending_origin_shift_{0.0f};

  // Shifts every render-space position (registered geometry/lights/
  // volumetrics, plus camera_) by -camera_.position() once the camera has
  // drifted floating_origin_recenter_threshold_ units from the render-space
  // origin, accumulating the opposite correction into world_origin_offset_
  // so nothing's true absolute position actually changes -- see the class
  // comment above camera_/world_origin_offset_. Called at the very top of
  // begin_frame(), before the scene_dirty_ check, so a recenter this frame
  // (which marks scene_dirty_ itself -- see its own comment) gets folded
  // into the very same rebake as any other pending scene mutation instead
  // of forcing a second one.
  void maybe_recenter_origin();

  u32 cached_framebuffer_width_ = 0;
  u32 cached_framebuffer_height_ = 0;

  std::vector<VulkanCommandBuffer> graphics_command_buffers_;

  // Owned fences, one per frame in flight.
  std::vector<VulkanFence> in_flight_fences_;
  // Non-owning pointers into in_flight_fences_, one per swapchain image.
  // Null means no frame is currently using that image.
  std::vector<VulkanFence *> images_in_flight_;

  // Backs main_renderpass -- color + depth, one per swapchain image.
  std::vector<VulkanFramebuffer> framebuffers_;
  // Backs ui_renderpass -- color only (no depth attachment; see
  // VulkanRenderpass's has_prev_pass/clear_flags), one per swapchain image.
  std::vector<VulkanFramebuffer> ui_framebuffers_;

  void create_commandbuffer();
  void regenerate_framebuffers();
  b8 recreate_swapchain();

  // context_.queue_complete_semaphores must be sized/indexed by swapchain
  // image (see its declaration comment in vulkan_types.inl), so both
  // initialize() and recreate_swapchain() (image_count can change across a
  // recreate) need to (re)build the whole array -- factored out here
  // instead of duplicated at each call site.
  void create_queue_complete_semaphores();
  void destroy_queue_complete_semaphores();
};
