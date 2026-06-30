#pragma once
#include "vulkan_types.inl"
#include <vector>

class VulkanRenderpass;

class VulkanFramebuffer {
public:
  // Takes attachments by value — the C version manually kallocate/kfree'd
  // a copy of the array. std::vector copy in the constructor replaces that
  // entirely: same ownership semantics, no manual free needed.
  VulkanFramebuffer(VulkanContext &context, VulkanRenderpass &renderpass,
                    u32 width, u32 height,
                    std::vector<VkImageView> attachments);
  ~VulkanFramebuffer();

  VulkanFramebuffer(const VulkanFramebuffer &) = delete;
  VulkanFramebuffer &operator=(const VulkanFramebuffer &) = delete;
  VulkanFramebuffer(VulkanFramebuffer &&other) noexcept;
  VulkanFramebuffer &operator=(VulkanFramebuffer &&other) noexcept;

  VkFramebuffer handle() const noexcept { return handle_; }

private:
  VulkanContext *context_;
  VkFramebuffer handle_ = VK_NULL_HANDLE;
  // Replaces the kallocate'd VkImageView* in the C struct.
  std::vector<VkImageView> attachments_;
  // Non-owning — the renderpass outlives the framebuffer in all call sites.
  VulkanRenderpass *renderpass_;
};
