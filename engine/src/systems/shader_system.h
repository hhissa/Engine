#pragma once
#include "../renderer/vulkan/vulkan_shader.h"
#include "../renderer/vulkan/vulkan_types.inl"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

// A name-keyed registry of VulkanShader instances -- structurally parallel
// to TextureSystem/MaterialSystem (see their header comments), rather than
// a shader_create/_use/... entry added to RendererBackend's virtual
// interface the way kohi's shader_system routes through its backend
// vtable: this engine already established the pattern of Vulkan-typed
// systems living directly in engine/src/systems/, constructed by
// VulkanRendererBackend and stored on VulkanContext, with exactly one
// backend implementation -- there's no seam here that needs abstracting.
//
// Shaders are held by unique_ptr (VulkanShader is non-movable, like
// VulkanTexture/VulkanCommandBuffer) so callers can hold a stable
// VulkanShader* across the shader's lifetime (see MaterialSystem, which
// caches one per acquired material).
class ShaderSystem {
public:
  explicit ShaderSystem(VulkanContext &context);
  ~ShaderSystem() = default;

  ShaderSystem(const ShaderSystem &) = delete;
  ShaderSystem &operator=(const ShaderSystem &) = delete;

  // Builds and registers a new shader under config.name. If a shader with
  // that name is already registered, logs an error and returns the
  // existing one instead of building a duplicate.
  VulkanShader &create(ShaderConfig config);

  // Returns the shader registered under name, or nullptr if none exists.
  VulkanShader *get(std::string_view name);

private:
  VulkanContext *context_;
  std::unordered_map<std::string, std::unique_ptr<VulkanShader>> shaders_;
};
