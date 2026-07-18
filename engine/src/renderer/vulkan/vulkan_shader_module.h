#pragma once
#include "vulkan_types.inl"

#include <string_view>

// A single compiled Vulkan shader stage (vertex, fragment, compute, ...),
// loaded from assets/shaders/<name>.<type_str>.spv.
//
// Deliberately stage-agnostic: a compute shader is just one of these
// constructed with VK_SHADER_STAGE_COMPUTE_BIT, no different from a vertex
// or fragment stage. Higher-level shader objects (e.g. VulkanShader) combine
// one or more of these; a compute pipeline can hold a single one.
class VulkanShaderModule {
public:
  VulkanShaderModule(VulkanContext &context, std::string_view name,
                     std::string_view type_str,
                     VkShaderStageFlagBits stage);
  ~VulkanShaderModule();

  VulkanShaderModule(const VulkanShaderModule &) = delete;
  VulkanShaderModule &operator=(const VulkanShaderModule &) = delete;
  VulkanShaderModule(VulkanShaderModule &&other) noexcept;
  VulkanShaderModule &operator=(VulkanShaderModule &&other) noexcept;

  bool is_valid() const noexcept { return handle_ != VK_NULL_HANDLE; }
  explicit operator bool() const noexcept { return is_valid(); }

  VkShaderModule handle() const noexcept { return handle_; }
  const VkPipelineShaderStageCreateInfo &stage_create_info() const noexcept {
    return stage_create_info_;
  }

private:
  VulkanContext *context_;
  VkShaderModule handle_ = VK_NULL_HANDLE;
  VkPipelineShaderStageCreateInfo stage_create_info_{};
};
