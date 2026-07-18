#include "shader_system.h"
#include "../core/logger.h"

ShaderSystem::ShaderSystem(VulkanContext &context) : context_(&context) {}

VulkanShader &ShaderSystem::create(ShaderConfig config) {
  auto it = shaders_.find(config.name);
  if (it != shaders_.end()) {
    KERROR("ShaderSystem::create called for an already-registered shader "
          "'{}'; returning the existing instance.",
          config.name);
    return *it->second;
  }

  std::string name = config.name;
  auto shader = std::make_unique<VulkanShader>(*context_, std::move(config));
  VulkanShader &result = *shader;
  shaders_.emplace(std::move(name), std::move(shader));
  return result;
}

VulkanShader *ShaderSystem::get(std::string_view name) {
  auto it = shaders_.find(std::string(name));
  if (it == shaders_.end()) {
    return nullptr;
  }
  return it->second.get();
}
