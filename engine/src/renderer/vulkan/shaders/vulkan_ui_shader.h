#pragma once
#include "../vulkan_buffer.h"
#include "../vulkan_shader.h"
#include "../vulkan_types.inl"

#include <glm/glm.hpp>
#include <optional>

class VulkanCommandBuffer;
class VulkanRenderpass;
struct Material;

// Draws a 2D textured quad on top of whatever the world pass already
// rendered: the UI renderpass this runs in uses LOAD_OP_LOAD (see
// VulkanRenderpass's has_prev_pass), so nothing here erases the raymarched
// scene behind it -- see VulkanRendererBackend::end_frame() for the exact
// sequencing.
//
// A thin wrapper around VulkanShader (Shader.Builtin.UI): owns only what's
// genuinely specific to this shader -- the static unit-quad vertex/index
// buffers and its acquired Material -- and delegates everything else
// (descriptor sets, pipeline, uniform buffer) to the shared generic
// implementation.
class VulkanUIShader {
public:
  VulkanUIShader(VulkanContext &context, VulkanRenderpass &ui_renderpass);
  ~VulkanUIShader();

  VulkanUIShader(const VulkanUIShader &) = delete;
  VulkanUIShader &operator=(const VulkanUIShader &) = delete;
  VulkanUIShader(VulkanUIShader &&) = delete;
  VulkanUIShader &operator=(VulkanUIShader &&) = delete;

  bool is_valid() const noexcept { return valid_; }
  explicit operator bool() const noexcept { return valid_; }

  // Draws the demo quad at position/size (screen pixels, (0,0) at the
  // top-left). command_buffer must currently be inside the UI renderpass
  // (see VulkanRendererBackend::end_frame(), which calls this once per
  // application-queued draw_ui_quad() request). width/height size the
  // orthographic projection to match the current framebuffer.
  void render_to(VulkanCommandBuffer &command_buffer, u32 width, u32 height,
                glm::vec2 position, glm::vec2 size);

private:
  VulkanContext *context_;

  // Non-owning -- registered with and owned by ShaderSystem.
  VulkanShader *shader_ = nullptr;
  VulkanShader::UniformIndex projection_uniform_ =
      VulkanShader::kInvalidUniformIndex;
  VulkanShader::UniformIndex view_uniform_ =
      VulkanShader::kInvalidUniformIndex;

  // A single unit quad (local space [0,1]^2) -- render_to() positions and
  // sizes it on screen via the model matrix push constant, rather than
  // baking a specific screen position into the vertex data itself.
  std::optional<VulkanBuffer> vertex_buffer_;
  std::optional<VulkanBuffer> index_buffer_;

  // The demo quad's material (diffuse tint + texture). Acquired from
  // MaterialSystem in the constructor, released in the destructor -- same
  // discipline as VulkanRaymarchShader's registered geometry.
  Material *material_ = nullptr;

  bool valid_ = false;
};
