#pragma once
#include "../vulkan_shader_module.h"
#include "../vulkan_types.inl"

#include <vector>

// The built-in "Builtin.ObjectShader" vertex+fragment pair. This is a
// placeholder rasterization shader (see assets/shaders/Builtin.ObjectShader
// .{vert,frag}.glsl); pipeline creation/binding is added in a later commit.
class VulkanObjectShader {
public:
  explicit VulkanObjectShader(VulkanContext &context);

  VulkanObjectShader(const VulkanObjectShader &) = delete;
  VulkanObjectShader &operator=(const VulkanObjectShader &) = delete;
  VulkanObjectShader(VulkanObjectShader &&) = delete;
  VulkanObjectShader &operator=(VulkanObjectShader &&) = delete;

  // True only if every stage module loaded/compiled successfully.
  bool is_valid() const noexcept { return valid_; }
  explicit operator bool() const noexcept { return valid_; }

  void use();

private:
  VulkanContext *context_;
  std::vector<VulkanShaderModule> stages_;
  VulkanPipeline pipeline_;
  bool valid_ = false;
};
