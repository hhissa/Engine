#pragma once
#include "vulkan_shader_module.h"
#include "vulkan_types.inl"

#include <vector>

class VulkanCommandBuffer;

// A compute pipeline: a pipeline layout (descriptor set layouts + an
// optional push-constant range) plus a single compute shader stage. Unlike
// a graphics pipeline, there is no vertex input, rasterizer, viewport/
// scissor, blend, or render pass state to configure — a compute pipeline is
// just "this shader, bound to these descriptor sets (and maybe some push
// constants)."
class VulkanComputePipeline {
public:
  // descriptor_set_layouts may be empty. push_constant_size may be 0 (no
  // push constants); Vulkan guarantees at least 128 bytes are available if
  // used. Push constants are recorded directly into the command buffer
  // (vkCmdPushConstants), unlike a UBO's separate buffer memory — ideal for
  // small values that change every dispatch, since there's no separate
  // memory a still-in-flight frame could be reading while a new value is
  // written for the next one.
  VulkanComputePipeline(
      VulkanContext &context, const VulkanShaderModule &compute_stage,
      const std::vector<VkDescriptorSetLayout> &descriptor_set_layouts = {},
      u32 push_constant_size = 0);
  ~VulkanComputePipeline();

  VulkanComputePipeline(const VulkanComputePipeline &) = delete;
  VulkanComputePipeline &operator=(const VulkanComputePipeline &) = delete;
  VulkanComputePipeline(VulkanComputePipeline &&other) noexcept;
  VulkanComputePipeline &operator=(VulkanComputePipeline &&other) noexcept;

  bool is_valid() const noexcept { return handle_ != VK_NULL_HANDLE; }
  explicit operator bool() const noexcept { return is_valid(); }

  void bind(VulkanCommandBuffer &command_buffer);

  VkPipeline handle() const noexcept { return handle_; }
  VkPipelineLayout layout() const noexcept { return layout_; }

private:
  VulkanContext *context_;
  VkPipeline handle_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
};
