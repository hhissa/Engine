#pragma once
#include "vulkan_types.inl"

// Same RAII treatment as VulkanRenderpass and VulkanCommandBuffer.
// VkFence is a driver-side resource with a paired create/destroy — the
// class makes that pairing automatic and the moved-from/null state explicit.
class VulkanFence {
public:
  VulkanFence(VulkanContext &context, b8 create_signaled);
  ~VulkanFence();

  VulkanFence(const VulkanFence &) = delete;
  VulkanFence &operator=(const VulkanFence &) = delete;
  VulkanFence(VulkanFence &&other) noexcept;
  VulkanFence &operator=(VulkanFence &&other) noexcept;

  // Returns TRUE if the fence was already signaled or became signaled
  // within timeout_ns. Returns FALSE on timeout or device error.
  b8 wait(u64 timeout_ns);
  void reset();

  VkFence handle() const noexcept { return handle_; }
  b8 is_signaled() const noexcept { return is_signaled_; }

private:
  VulkanContext *context_;
  VkFence handle_ = VK_NULL_HANDLE;
  b8 is_signaled_ = FALSE;
};
