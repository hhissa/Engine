#pragma once
#include "../../core/application.h"
#include "../renderer_backend.h"
#include "vulkan_commandbuffer.h"
#include "vulkan_fence.h"
#include "vulkan_framebuffer.h"
#include "vulkan_types.inl"

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
  b8 end_frame(f32 delta_time) override;

private:
  VulkanContext context_{};
  PlatformLayer *plat_state_ = nullptr;

  // Set via set_camera() each frame (by the game, through the renderer
  // frontend), consumed by end_frame() when it calls the raymarch shader.
  Camera camera_;

  u32 cached_framebuffer_width_ = 0;
  u32 cached_framebuffer_height_ = 0;

  std::vector<VulkanCommandBuffer> graphics_command_buffers_;

  // Owned fences, one per frame in flight.
  std::vector<VulkanFence> in_flight_fences_;
  // Non-owning pointers into in_flight_fences_, one per swapchain image.
  // Null means no frame is currently using that image.
  std::vector<VulkanFence *> images_in_flight_;

  std::vector<VulkanFramebuffer> framebuffers_;

  void create_commandbuffer();
  void regenerate_framebuffers();
  b8 recreate_swapchain();
};
