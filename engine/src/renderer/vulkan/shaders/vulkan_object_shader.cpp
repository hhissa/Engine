#include "vulkan_object_shader.h"
#include "../../../core/logger.h"

namespace {
constexpr std::string_view BUILTIN_SHADER_NAME_OBJECT = "Builtin.ObjectShader";
}

VulkanObjectShader::VulkanObjectShader(VulkanContext &context)
    : context_(&context) {
  stages_.reserve(2);
  stages_.emplace_back(*context_, BUILTIN_SHADER_NAME_OBJECT, "vert",
                       VK_SHADER_STAGE_VERTEX_BIT);
  stages_.emplace_back(*context_, BUILTIN_SHADER_NAME_OBJECT, "frag",
                       VK_SHADER_STAGE_FRAGMENT_BIT);

  for (const auto &stage : stages_) {
    if (!stage.is_valid()) {
      KERROR("Unable to create shader module for '{}'.",
            BUILTIN_SHADER_NAME_OBJECT);
      return;
    }
  }

  // Descriptors and pipeline creation land in a later commit.

  valid_ = true;
}

void VulkanObjectShader::use() {
  // TODO: bind the pipeline once one exists.
}
