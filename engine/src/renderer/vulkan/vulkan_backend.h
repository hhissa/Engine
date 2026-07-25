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
  void set_selected_primitive(i32 index) override;
  void set_grid_visible(b8 visible) override;
  void set_bloom_enabled(b8 enabled) override;
  void set_vignette_enabled(b8 enabled) override;
  void set_pixelation_enabled(b8 enabled) override;
  void set_pixelation_block_size(u32 block_size) override;
  void set_bloom_threshold(f32 threshold) override;
  void set_bloom_intensity(f32 intensity) override;
  void set_vignette_strength(f32 strength) override;
  void set_vignette_radius(f32 radius) override;
  void set_font(std::string_view name, f32 pixel_height) override;
  void set_skybox(std::string_view texture_name) override;
  void disable_skybox() override;
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
