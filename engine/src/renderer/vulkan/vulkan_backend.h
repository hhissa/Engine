#pragma once
#include "../renderer_backend.h"
#include "vulkan_types.inl"

class PlatformLayer;

class VulkanRendererBackend final : public RendererBackend {
public:
  explicit VulkanRendererBackend(PlatformLayer &plat_state);
  ~VulkanRendererBackend() override = default;

  b8 initialize(std::string_view application_name,
                PlatformLayer &plat_state) override;
  void shutdown() override;
  void on_resized(u16 width, u16 height) override;
  b8 begin_frame(f32 delta_time) override;
  b8 end_frame(f32 delta_time) override;

private:
  VulkanContext context_{};
  PlatformLayer *plat_state_ = nullptr;
};
