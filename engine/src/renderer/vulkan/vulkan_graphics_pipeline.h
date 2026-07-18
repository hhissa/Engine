#pragma once
#include "vulkan_shader_module.h"
#include "vulkan_types.inl"

#include <vector>

class VulkanCommandBuffer;
class VulkanRenderpass;

// A graphics pipeline: a pipeline layout (descriptor set layouts + an
// optional push-constant range) plus a vertex+fragment shader stage pair,
// vertex input layout, and fixed-function state (rasterization, blending,
// depth test) -- the rasterization counterpart to VulkanComputePipeline.
// Viewport/scissor are always dynamic state (set per-frame via
// vkCmdSetViewport/vkCmdSetScissor, same as VulkanRendererBackend already
// does each frame), so they aren't configured here.
class VulkanGraphicsPipeline {
public:
  // One vertex attribute's format/offset within the single vertex binding
  // this pipeline assumes (binding 0, stride = vertex_stride). location is
  // implicit: attribute i is bound to `layout(location = i)` in the vertex
  // shader.
  struct VertexAttribute {
    VkFormat format;
    u32 offset;
  };

  // push_constant_size may be 0 (no push constants); Vulkan guarantees at
  // least 128 bytes are available if used. The push constant range (if
  // any) covers both vertex and fragment stages, since both may want to
  // read it (matches Builtin.UIShader.{vert,frag}.glsl, which both declare
  // the same PushConstants block).
  VulkanGraphicsPipeline(
      VulkanContext &context, VulkanRenderpass &renderpass,
      const VulkanShaderModule &vertex_stage,
      const VulkanShaderModule &fragment_stage, u32 vertex_stride,
      const std::vector<VertexAttribute> &vertex_attributes,
      const std::vector<VkDescriptorSetLayout> &descriptor_set_layouts,
      u32 push_constant_size, bool depth_test_enabled);
  ~VulkanGraphicsPipeline();

  VulkanGraphicsPipeline(const VulkanGraphicsPipeline &) = delete;
  VulkanGraphicsPipeline &operator=(const VulkanGraphicsPipeline &) = delete;
  VulkanGraphicsPipeline(VulkanGraphicsPipeline &&other) noexcept;
  VulkanGraphicsPipeline &operator=(VulkanGraphicsPipeline &&other) noexcept;

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
