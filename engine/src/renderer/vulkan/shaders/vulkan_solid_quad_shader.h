#pragma once
#include "../vulkan_buffer.h"
#include "../vulkan_shader.h"
#include "../vulkan_types.inl"

#include <glm/glm.hpp>
#include <optional>

class VulkanCommandBuffer;
class VulkanRenderpass;

// Draws a single solid-colour, axis-aligned rectangle (screen pixels) on
// top of whatever the world pass already rendered -- same UI renderpass/
// LOAD_OP_LOAD sequencing as VulkanUIShader/VulkanLineShader (see
// VulkanRendererBackend::end_frame()). Added for games that need a flat
// UI box with an arbitrary colour (e.g. SH's face/crotch censor bars) --
// VulkanUIShader always samples a fixed demo texture, and VulkanLineShader
// only draws thin segments, neither of which fit a "solid black
// rectangle" need.
//
// A thin wrapper around VulkanShader (Shader.Builtin.SolidQuad): reuses
// VulkanUIShader's exact unit-quad vertex/index buffer (4 vertices, 6
// indices), mapped directly onto push.position/size in the vertex shader
// (see Builtin.SolidQuadShader.vert.glsl) -- no material/texture/instance
// uniforms at all, colour comes from a push constant directly, the same
// idiom VulkanLineShader already uses.
class VulkanSolidQuadShader {
public:
  VulkanSolidQuadShader(VulkanContext &context, VulkanRenderpass &ui_renderpass);
  ~VulkanSolidQuadShader();

  VulkanSolidQuadShader(const VulkanSolidQuadShader &) = delete;
  VulkanSolidQuadShader &operator=(const VulkanSolidQuadShader &) = delete;
  VulkanSolidQuadShader(VulkanSolidQuadShader &&) = delete;
  VulkanSolidQuadShader &operator=(VulkanSolidQuadShader &&) = delete;

  bool is_valid() const noexcept { return valid_; }
  explicit operator bool() const noexcept { return valid_; }

  // Draws one solid-colour rectangle at position/size (screen pixels,
  // (0,0) at the top-left) in colour (alpha included -- e.g. a
  // translucent box, not just opaque ones). command_buffer must currently
  // be inside the UI renderpass.
  void render_to(VulkanCommandBuffer &command_buffer, u32 width, u32 height,
                glm::vec2 position, glm::vec2 size, glm::vec4 colour);

private:
  VulkanContext *context_;

  // Non-owning -- registered with and owned by ShaderSystem.
  VulkanShader *shader_ = nullptr;
  VulkanShader::UniformIndex projection_uniform_ =
      VulkanShader::kInvalidUniformIndex;
  VulkanShader::UniformIndex view_uniform_ =
      VulkanShader::kInvalidUniformIndex;

  // A single unit quad (local space [0,1]^2) -- render_to() positions and
  // sizes it via push constants rather than a model matrix.
  std::optional<VulkanBuffer> vertex_buffer_;
  std::optional<VulkanBuffer> index_buffer_;

  bool valid_ = false;
};
