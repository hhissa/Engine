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
  SceneHandle load_scene(std::string_view sdf_path) override;
  void translate_scene(SceneHandle handle, glm::vec3 delta) override;
  void rotate_scene(SceneHandle handle, glm::vec3 euler_radians) override;
  void scale_scene(SceneHandle handle, f32 factor) override;
  void remove_scene(SceneHandle handle) override;
  void clear_scenes() override;
  void set_selected_primitive(i32 index) override;
  void set_grid_visible(b8 visible) override;
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

  // Tracks every currently loaded scene (see load_scene()): each handle
  // maps to the names of exactly the primitives/lights that load_scene()
  // call registered with GeometrySystem (see LoadedSceneNames), so
  // remove_scene() can release just those and leave every other
  // concurrently loaded scene untouched.
  std::unordered_map<SceneHandle, LoadedSceneNames> loaded_scenes_;
  SceneHandle next_scene_handle_ = 1; // 0 is kInvalidSceneHandle

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
};
