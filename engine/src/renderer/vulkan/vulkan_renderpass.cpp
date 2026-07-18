// vulkan_renderpass.cpp
#include "vulkan_renderpass.h"
#include <array>
#include <stdexcept>

namespace {

void vk_check(VkResult result, const char *what) {
  if (result != VK_SUCCESS) {
    throw std::runtime_error(what);
  }
}

} // namespace

VulkanRenderpass::VulkanRenderpass(VulkanContext &context, f32 x, f32 y, f32 w,
                                   f32 h, f32 r, f32 g, f32 b, f32 a, f32 depth,
                                   u32 stencil, u8 clear_flags,
                                   bool has_prev_pass, bool has_next_pass)
    : context_(&context), x_(x), y_(y), w_(w), h_(h), r_(r), g_(g), b_(b),
      a_(a), depth_(depth), stencil_(stencil), clear_flags_(clear_flags) {

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;

  u32 attachment_description_count = 0;
  std::array<VkAttachmentDescription, 2> attachment_descriptions{};

  // Color attachment
  bool do_clear_colour =
      (clear_flags_ & RenderpassClearFlags::kColourBuffer) != 0;
  VkAttachmentDescription color_attachment{};
  color_attachment.format = context_->swapchain.image_format.format;
  color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  color_attachment.loadOp = do_clear_colour ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                            : VK_ATTACHMENT_LOAD_OP_LOAD;
  color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color_attachment.initialLayout = has_prev_pass
                                       ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                       : VK_IMAGE_LAYOUT_UNDEFINED;
  color_attachment.finalLayout = has_next_pass
                                     ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                     : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  color_attachment.flags = 0;
  attachment_descriptions[attachment_description_count] = color_attachment;
  ++attachment_description_count;

  VkAttachmentReference color_attachment_reference{};
  color_attachment_reference.attachment = 0;
  color_attachment_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color_attachment_reference;

  // Depth attachment -- only present at all if this pass clears depth (see
  // the header comment: a pass with nothing depth-related to draw, like a
  // 2D UI pass, has no depth attachment rather than loading a stale one).
  bool do_clear_depth = (clear_flags_ & RenderpassClearFlags::kDepthBuffer) != 0;
  VkAttachmentReference depth_attachment_reference{};
  if (do_clear_depth) {
    VkAttachmentDescription depth_attachment{};
    depth_attachment.format = context_->device.depth_format;
    depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_attachment.finalLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    depth_attachment_reference.attachment = attachment_description_count;
    depth_attachment_reference.layout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    attachment_descriptions[attachment_description_count] = depth_attachment;
    ++attachment_description_count;

    subpass.pDepthStencilAttachment = &depth_attachment_reference;
  } else {
    subpass.pDepthStencilAttachment = nullptr;
  }

  // TODO: other attachment types (input, resolve, preserve)
  subpass.inputAttachmentCount = 0;
  subpass.pInputAttachments = nullptr;
  subpass.pResolveAttachments = nullptr;
  subpass.preserveAttachmentCount = 0;
  subpass.pPreserveAttachments = nullptr;

  // Render pass dependency. TODO: make this configurable.
  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.srcAccessMask = 0;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependency.dependencyFlags = 0;

  VkRenderPassCreateInfo render_pass_create_info{
      VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  render_pass_create_info.attachmentCount = attachment_description_count;
  render_pass_create_info.pAttachments = attachment_descriptions.data();
  render_pass_create_info.subpassCount = 1;
  render_pass_create_info.pSubpasses = &subpass;
  render_pass_create_info.dependencyCount = 1;
  render_pass_create_info.pDependencies = &dependency;
  render_pass_create_info.pNext = nullptr;
  render_pass_create_info.flags = 0;

  vk_check(vkCreateRenderPass(context_->device.logical_device,
                              &render_pass_create_info, context_->allocator,
                              &handle_),
           "vkCreateRenderPass failed");

  state_ = VulkanRenderPassState::Ready;
}

VulkanRenderpass::~VulkanRenderpass() {
  if (handle_ != VK_NULL_HANDLE) {
    vkDestroyRenderPass(context_->device.logical_device, handle_,
                        context_->allocator);
    handle_ = VK_NULL_HANDLE;
  }
}

VulkanRenderpass::VulkanRenderpass(VulkanRenderpass &&other) noexcept
    : context_(other.context_), handle_(other.handle_), x_(other.x_),
      y_(other.y_), w_(other.w_), h_(other.h_), r_(other.r_), g_(other.g_),
      b_(other.b_), a_(other.a_), depth_(other.depth_),
      stencil_(other.stencil_), clear_flags_(other.clear_flags_),
      state_(other.state_) {
  other.handle_ = VK_NULL_HANDLE;
  other.state_ = VulkanRenderPassState::NotAllocated;
}

VulkanRenderpass &
VulkanRenderpass::operator=(VulkanRenderpass &&other) noexcept {
  if (this != &other) {
    if (handle_ != VK_NULL_HANDLE) {
      vkDestroyRenderPass(context_->device.logical_device, handle_,
                          context_->allocator);
    }
    context_ = other.context_;
    handle_ = other.handle_;
    x_ = other.x_;
    y_ = other.y_;
    w_ = other.w_;
    h_ = other.h_;
    r_ = other.r_;
    g_ = other.g_;
    b_ = other.b_;
    a_ = other.a_;
    depth_ = other.depth_;
    stencil_ = other.stencil_;
    clear_flags_ = other.clear_flags_;
    state_ = other.state_;

    other.handle_ = VK_NULL_HANDLE;
    other.state_ = VulkanRenderPassState::NotAllocated;
  }
  return *this;
}

void VulkanRenderpass::begin(VulkanCommandBuffer &command_buffer,
                             VkFramebuffer frame_buffer) {
  VkRenderPassBeginInfo begin_info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin_info.renderPass = handle_;
  begin_info.framebuffer = frame_buffer;
  begin_info.renderArea.offset.x = static_cast<i32>(x_);
  begin_info.renderArea.offset.y = static_cast<i32>(y_);
  begin_info.renderArea.extent.width = static_cast<u32>(w_);
  begin_info.renderArea.extent.height = static_cast<u32>(h_);

  std::array<VkClearValue, 2> clear_values{};
  u32 clear_value_count = 0;

  bool do_clear_colour =
      (clear_flags_ & RenderpassClearFlags::kColourBuffer) != 0;
  if (do_clear_colour) {
    clear_values[clear_value_count].color.float32[0] = r_;
    clear_values[clear_value_count].color.float32[1] = g_;
    clear_values[clear_value_count].color.float32[2] = b_;
    clear_values[clear_value_count].color.float32[3] = a_;
    ++clear_value_count;
  }

  bool do_clear_depth = (clear_flags_ & RenderpassClearFlags::kDepthBuffer) != 0;
  if (do_clear_depth) {
    clear_values[clear_value_count].depthStencil.depth = depth_;
    bool do_clear_stencil =
        (clear_flags_ & RenderpassClearFlags::kStencilBuffer) != 0;
    clear_values[clear_value_count].depthStencil.stencil =
        do_clear_stencil ? stencil_ : 0;
    ++clear_value_count;
  }

  begin_info.clearValueCount = clear_value_count;
  begin_info.pClearValues = clear_value_count > 0 ? clear_values.data() : nullptr;

  vkCmdBeginRenderPass(command_buffer.handle(), &begin_info,
                       VK_SUBPASS_CONTENTS_INLINE);
  command_buffer.set_state(CommandBufferState::InRenderPass);
  state_ = VulkanRenderPassState::InRenderPass;
}

void VulkanRenderpass::end(VulkanCommandBuffer &command_buffer) {
  vkCmdEndRenderPass(command_buffer.handle());
  command_buffer.set_state(CommandBufferState::Recording);
  state_ = VulkanRenderPassState::RecordingEnded;
}
