#include "vulkan_framebuffer.h"
#include "vulkan_renderpass.h"

VulkanFramebuffer::VulkanFramebuffer(VulkanContext &context,
                                     VulkanRenderpass &renderpass, u32 width,
                                     u32 height,
                                     std::vector<VkImageView> attachments)
    : context_(&context), renderpass_(&renderpass),
      attachments_(std::move(attachments)) {

  VkFramebufferCreateInfo framebuffer_create_info{
      VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  framebuffer_create_info.renderPass = renderpass_->handle();
  framebuffer_create_info.attachmentCount =
      static_cast<u32>(attachments_.size());
  framebuffer_create_info.pAttachments = attachments_.data();
  framebuffer_create_info.width = width;
  framebuffer_create_info.height = height;
  framebuffer_create_info.layers = 1;

  VK_CHECK(vkCreateFramebuffer(context_->device.logical_device,
                               &framebuffer_create_info, context_->allocator,
                               &handle_));
}

VulkanFramebuffer::~VulkanFramebuffer() {
  if (handle_ != VK_NULL_HANDLE) {
    vkDestroyFramebuffer(context_->device.logical_device, handle_,
                         context_->allocator);
    handle_ = VK_NULL_HANDLE;
  }
}

VulkanFramebuffer::VulkanFramebuffer(VulkanFramebuffer &&other) noexcept
    : context_(other.context_), handle_(other.handle_),
      attachments_(std::move(other.attachments_)),
      renderpass_(other.renderpass_) {
  other.handle_ = VK_NULL_HANDLE;
  other.renderpass_ = nullptr;
}

VulkanFramebuffer &
VulkanFramebuffer::operator=(VulkanFramebuffer &&other) noexcept {
  if (this != &other) {
    if (handle_ != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(context_->device.logical_device, handle_,
                           context_->allocator);
    }
    context_ = other.context_;
    handle_ = other.handle_;
    attachments_ = std::move(other.attachments_);
    renderpass_ = other.renderpass_;
    other.handle_ = VK_NULL_HANDLE;
    other.renderpass_ = nullptr;
  }
  return *this;
}
