#include "vulkan_fence.h"
#include "../../core/logger.h"

VulkanFence::VulkanFence(VulkanContext &context, b8 create_signaled)
    : context_(&context), is_signaled_(create_signaled) {

  VkFenceCreateInfo fence_create_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (is_signaled_) {
    fence_create_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  }
  VK_CHECK(vkCreateFence(context_->device.logical_device, &fence_create_info,
                         context_->allocator, &handle_));
}

VulkanFence::~VulkanFence() {
  if (handle_ != VK_NULL_HANDLE) {
    vkDestroyFence(context_->device.logical_device, handle_,
                   context_->allocator);
    handle_ = VK_NULL_HANDLE;
  }
  is_signaled_ = FALSE;
}

VulkanFence::VulkanFence(VulkanFence &&other) noexcept
    : context_(other.context_), handle_(other.handle_),
      is_signaled_(other.is_signaled_) {
  other.handle_ = VK_NULL_HANDLE;
  other.is_signaled_ = FALSE;
}

VulkanFence &VulkanFence::operator=(VulkanFence &&other) noexcept {
  if (this != &other) {
    if (handle_ != VK_NULL_HANDLE) {
      vkDestroyFence(context_->device.logical_device, handle_,
                     context_->allocator);
    }
    context_ = other.context_;
    handle_ = other.handle_;
    is_signaled_ = other.is_signaled_;
    other.handle_ = VK_NULL_HANDLE;
    other.is_signaled_ = FALSE;
  }
  return *this;
}

b8 VulkanFence::wait(u64 timeout_ns) {
  if (is_signaled_) {
    return TRUE;
  }

  VkResult result = vkWaitForFences(context_->device.logical_device, 1,
                                    &handle_, TRUE, timeout_ns);
  switch (result) {
  case VK_SUCCESS:
    is_signaled_ = TRUE;
    return TRUE;
  case VK_TIMEOUT:
    KWARN("vk_fence_wait - Timed out");
    break;
  case VK_ERROR_DEVICE_LOST:
    KERROR("vk_fence_wait - VK_ERROR_DEVICE_LOST.");
    break;
  case VK_ERROR_OUT_OF_HOST_MEMORY:
    KERROR("vk_fence_wait - VK_ERROR_OUT_OF_HOST_MEMORY.");
    break;
  case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    KERROR("vk_fence_wait - VK_ERROR_OUT_OF_DEVICE_MEMORY.");
    break;
  default:
    KERROR("vk_fence_wait - An unknown error has occurred.");
    break;
  }
  return FALSE;
}

void VulkanFence::reset() {
  if (is_signaled_) {
    VK_CHECK(vkResetFences(context_->device.logical_device, 1, &handle_));
    is_signaled_ = FALSE;
  }
}
