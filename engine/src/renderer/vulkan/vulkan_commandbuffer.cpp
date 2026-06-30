#include "vulkan_commandbuffer.h"

VulkanCommandBuffer::VulkanCommandBuffer(VulkanContext &context,
                                         VkCommandPool pool, b8 is_primary)
    : context_(&context), pool_(pool) {

  VkCommandBufferAllocateInfo allocate_info{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate_info.commandPool = pool_;
  allocate_info.level = is_primary ? VK_COMMAND_BUFFER_LEVEL_PRIMARY
                                   : VK_COMMAND_BUFFER_LEVEL_SECONDARY;
  allocate_info.commandBufferCount = 1;
  allocate_info.pNext = nullptr;

  state_ = CommandBufferState::NotAllocated;
  VK_CHECK(vkAllocateCommandBuffers(context_->device.logical_device,
                                    &allocate_info, &handle_));
  state_ = CommandBufferState::Ready;
}

VulkanCommandBuffer::~VulkanCommandBuffer() {
  // Guards the moved-from case the same way VulkanRenderpass's destructor
  // does — a default-constructed/zeroed instance can't exist (no default
  // ctor), so only "moved from" needs checking here.
  if (handle_ != VK_NULL_HANDLE) {
    vkFreeCommandBuffers(context_->device.logical_device, pool_, 1, &handle_);
    handle_ = VK_NULL_HANDLE;
    state_ = CommandBufferState::NotAllocated;
  }
}

VulkanCommandBuffer::VulkanCommandBuffer(VulkanCommandBuffer &&other) noexcept
    : context_(other.context_), pool_(other.pool_), handle_(other.handle_),
      state_(other.state_) {
  other.handle_ = VK_NULL_HANDLE;
  other.state_ = CommandBufferState::NotAllocated;
}

VulkanCommandBuffer &
VulkanCommandBuffer::operator=(VulkanCommandBuffer &&other) noexcept {
  if (this != &other) {
    if (handle_ != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(context_->device.logical_device, pool_, 1, &handle_);
    }
    context_ = other.context_;
    pool_ = other.pool_;
    handle_ = other.handle_;
    state_ = other.state_;

    other.handle_ = VK_NULL_HANDLE;
    other.state_ = CommandBufferState::NotAllocated;
  }
  return *this;
}

void VulkanCommandBuffer::begin(b8 is_single_use, b8 is_renderpass_continue,
                                b8 is_simultaneous_use) {
  VkCommandBufferBeginInfo begin_info{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_info.flags = 0;
  if (is_single_use) {
    begin_info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  }
  if (is_renderpass_continue) {
    begin_info.flags |= VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
  }
  if (is_simultaneous_use) {
    begin_info.flags |= VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
  }

  VK_CHECK(vkBeginCommandBuffer(handle_, &begin_info));
  state_ = CommandBufferState::Recording;
}

void VulkanCommandBuffer::end() {
  VK_CHECK(vkEndCommandBuffer(handle_));
  state_ = CommandBufferState::RecordingEnded;
}

void VulkanCommandBuffer::update_submitted() {
  state_ = CommandBufferState::Submitted;
}

void VulkanCommandBuffer::reset() { state_ = CommandBufferState::Ready; }

std::unique_ptr<VulkanCommandBuffer>
VulkanCommandBuffer::allocate_and_begin_single_use(VulkanContext &context,
                                                   VkCommandPool pool) {
  auto command_buffer =
      std::make_unique<VulkanCommandBuffer>(context, pool, TRUE);
  command_buffer->begin(TRUE, FALSE, FALSE);
  return command_buffer;
}

void VulkanCommandBuffer::end_single_use(
    VulkanContext &context, VkCommandPool pool,
    std::unique_ptr<VulkanCommandBuffer> command_buffer, VkQueue queue) {
  command_buffer->end();

  VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  VkCommandBuffer raw_handle = command_buffer->handle();
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &raw_handle;
  VK_CHECK(vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE));

  VK_CHECK(vkQueueWaitIdle(queue));

  // command_buffer goes out of scope here — ~VulkanCommandBuffer() runs,
  // which is the free() call. No explicit free needed; that's the point
  // of taking ownership by value instead of by pointer.
}
