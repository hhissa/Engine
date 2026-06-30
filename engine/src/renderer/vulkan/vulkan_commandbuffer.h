#pragma once
#include "vulkan_types.inl"
#include <memory>

enum class CommandBufferState {
  Ready,
  Recording,
  InRenderPass,
  RecordingEnded,
  Submitted,
  NotAllocated
};

// Same shape as VulkanRenderpass: the C version split allocation/free into
// vulkan_command_buffer_allocate()/_free(), free functions someone has to
// pair correctly by hand. Tying allocate to the constructor and free to the
// destructor makes a forgotten _free() call (exactly the kind of bug we hit
// with the swapchain destroy earlier) impossible instead of just unlikely.
class VulkanCommandBuffer {
public:
  VulkanCommandBuffer(VulkanContext &context, VkCommandPool pool,
                      b8 is_primary);
  ~VulkanCommandBuffer();

  // One VkCommandBuffer handle, one owner — same reasoning as VulkanRenderpass.
  VulkanCommandBuffer(const VulkanCommandBuffer &) = delete;
  VulkanCommandBuffer &operator=(const VulkanCommandBuffer &) = delete;
  VulkanCommandBuffer(VulkanCommandBuffer &&other) noexcept;
  VulkanCommandBuffer &operator=(VulkanCommandBuffer &&other) noexcept;

  void begin(b8 is_single_use, b8 is_renderpass_continue,
             b8 is_simultaneous_use);
  void end();
  void update_submitted();
  void reset();

  VkCommandBuffer handle() const noexcept { return handle_; }
  CommandBufferState state() const noexcept { return state_; }
  void set_state(CommandBufferState state) noexcept { state_ = state; }

  // Replaces vulkan_command_buffer_allocate_and_begin_single_use(). Returns
  // ownership via unique_ptr rather than writing into an out-param, since
  // the caller now genuinely owns the lifetime (and must eventually pass it
  // to end_single_use, or it leaks the C++ way: a destructor that never runs
  // because the unique_ptr itself was dropped — visible at the type level,
  // unlike a raw out-param the caller could just forget to free).
  static std::unique_ptr<VulkanCommandBuffer>
  allocate_and_begin_single_use(VulkanContext &context, VkCommandPool pool);

  // Takes ownership of the buffer (by value unique_ptr) rather than a
  // pointer/reference: ending a single-use buffer always frees it
  // afterward in the original code, so "this function consumes the
  // buffer" is the real contract. Letting the parameter go out of scope
  // at the end of the function body runs ~VulkanCommandBuffer()
  // automatically — that *is* the free() call, no separate line needed.
  static void
  end_single_use(VulkanContext &context, VkCommandPool pool,
                 std::unique_ptr<VulkanCommandBuffer> command_buffer,
                 VkQueue queue);

private:
  VulkanContext *context_;
  VkCommandPool pool_;
  VkCommandBuffer handle_ = VK_NULL_HANDLE;
  CommandBufferState state_ = CommandBufferState::NotAllocated;
};
