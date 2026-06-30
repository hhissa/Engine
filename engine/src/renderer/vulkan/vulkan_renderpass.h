// vulkan_renderpass.h
#pragma once
#include "./vulkan_commandbuffer.h"
#include "./vulkan_types.inl"

enum class VulkanRenderPassState {
  Ready,
  Recording,
  InRenderPass,
  RecordingEnded,
  Submitted,
  NotAllocated
};

class VulkanRenderpass {
public:
  VulkanRenderpass(VulkanContext &context, f32 x, f32 y, f32 w, f32 h, f32 r,
                   f32 g, f32 b, f32 a, f32 depth, u32 stencil);
  ~VulkanRenderpass();

  VulkanRenderpass(const VulkanRenderpass &) = delete;
  VulkanRenderpass &operator=(const VulkanRenderpass &) = delete;
  VulkanRenderpass(VulkanRenderpass &&other) noexcept;
  VulkanRenderpass &operator=(VulkanRenderpass &&other) noexcept;

  void begin(VulkanCommandBuffer &command_buffer, VkFramebuffer frame_buffer);
  void end(VulkanCommandBuffer &command_buffer);

  VkRenderPass handle() const noexcept { return handle_; }
  VulkanRenderPassState state() const noexcept { return state_; }

private:
  VulkanContext *context_;

  VkRenderPass handle_ = VK_NULL_HANDLE;

  f32 x_, y_, w_, h_;
  f32 r_, g_, b_, a_;
  f32 depth_;
  u32 stencil_;

  VulkanRenderPassState state_ = VulkanRenderPassState::NotAllocated;
};
