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

// Bitmask controlling which attachments a renderpass clears on begin() --
// lets several renderpasses chain against the same framebuffer within one
// frame (see has_prev_pass/has_next_pass below): the first pass clears
// everything, later ones LOAD instead of CLEAR so they draw on top of
// whatever the earlier passes already produced instead of erasing it.
namespace RenderpassClearFlags {
constexpr u8 kNone = 0x0;
constexpr u8 kColourBuffer = 0x1;
constexpr u8 kDepthBuffer = 0x2;
constexpr u8 kStencilBuffer = 0x4;
} // namespace RenderpassClearFlags

class VulkanRenderpass {
public:
  // has_prev_pass/has_next_pass control the colour attachment's
  // initial/final layout so this pass can chain with others against the
  // same swapchain image within one frame: has_prev_pass expects the image
  // is already VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL (rather than
  // UNDEFINED) on entry, and has_next_pass leaves it in that same layout on
  // exit (rather than transitioning straight to VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
  // so the next pass's LOAD_OP_LOAD picks up exactly what this one wrote.
  // The depth attachment only exists at all if clear_flags requests
  // clearing it -- a pass that doesn't want to clear depth (e.g. a 2D UI
  // pass with nothing depth-related to draw) simply has no depth
  // attachment, rather than loading a stale one it has no use for.
  VulkanRenderpass(VulkanContext &context, f32 x, f32 y, f32 w, f32 h, f32 r,
                   f32 g, f32 b, f32 a, f32 depth, u32 stencil,
                   u8 clear_flags, bool has_prev_pass, bool has_next_pass);
  ~VulkanRenderpass();

  VulkanRenderpass(const VulkanRenderpass &) = delete;
  VulkanRenderpass &operator=(const VulkanRenderpass &) = delete;
  VulkanRenderpass(VulkanRenderpass &&other) noexcept;
  VulkanRenderpass &operator=(VulkanRenderpass &&other) noexcept;

  void begin(VulkanCommandBuffer &command_buffer, VkFramebuffer frame_buffer);
  void end(VulkanCommandBuffer &command_buffer);

  // Updates the render area, e.g. after the framebuffer has been resized.
  void set_render_area(f32 x, f32 y, f32 w, f32 h) noexcept {
    x_ = x;
    y_ = y;
    w_ = w;
    h_ = h;
  }

  VkRenderPass handle() const noexcept { return handle_; }
  VulkanRenderPassState state() const noexcept { return state_; }

private:
  VulkanContext *context_;

  VkRenderPass handle_ = VK_NULL_HANDLE;

  f32 x_, y_, w_, h_;
  f32 r_, g_, b_, a_;
  f32 depth_;
  u32 stencil_;
  u8 clear_flags_;

  VulkanRenderPassState state_ = VulkanRenderPassState::NotAllocated;
};
