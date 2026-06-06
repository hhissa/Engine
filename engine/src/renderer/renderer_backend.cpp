#include "renderer_types.inl"
#include "vulkan/vulkan_backend.h"

std::unique_ptr<RendererBackend>
renderer_backend_create(renderer_backend_type type, PlatformLayer &plat_state) {
  switch (type) {
  case renderer_backend_type::RENDERER_BACKEND_TYPE_VULKAN:
    return std::make_unique<VulkanRendererBackend>(plat_state);
  default:
    return nullptr;
  }
}
