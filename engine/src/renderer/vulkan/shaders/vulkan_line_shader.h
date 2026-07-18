#pragma once
#include "../vulkan_buffer.h"
#include "../vulkan_shader.h"
#include "../vulkan_types.inl"

#include <glm/glm.hpp>
#include <optional>

class VulkanCommandBuffer;
class VulkanRenderpass;

// Draws a single solid-colour line segment (screen pixels) on top of
// whatever the world pass already rendered -- same UI renderpass/
// LOAD_OP_LOAD sequencing as VulkanUIShader (see
// VulkanRendererBackend::end_frame()). Added specifically so a tool
// embedding this engine's rendering (e.g. tools/sdf_editor's
// SceneViewport, drawing its move-gizmo's axis lines) can draw 2D
// overlay content *inside the same native window* the engine already
// owns -- a separate widget/window stacked on top would not reliably
// composite above it (native child windows -- which is what a raw
// QWindow wrapped via QWidget::createWindowContainer() is -- always draw
// over ordinary sibling widgets on X11, regardless of declared widget
// stacking order).
//
// A thin wrapper around VulkanShader (Shader.Builtin.Line): reuses
// VulkanUIShader's exact unit-quad vertex/index buffer (4 vertices, 6
// indices) but reinterprets the quad's local [0,1]^2 coordinates as (t
// along the line, which side of it) in the vertex shader instead of a
// literal screen position -- see Builtin.LineShader.vert.glsl. No
// material/texture/instance uniforms at all: colour comes from a push
// constant directly, the same way VulkanRaymarchShader drives its own
// push constants rather than through MaterialSystem.
class VulkanLineShader {
public:
  VulkanLineShader(VulkanContext &context, VulkanRenderpass &ui_renderpass);
  ~VulkanLineShader();

  VulkanLineShader(const VulkanLineShader &) = delete;
  VulkanLineShader &operator=(const VulkanLineShader &) = delete;
  VulkanLineShader(VulkanLineShader &&) = delete;
  VulkanLineShader &operator=(VulkanLineShader &&) = delete;

  bool is_valid() const noexcept { return valid_; }
  explicit operator bool() const noexcept { return valid_; }

  // Draws one line segment from start to end (screen pixels, (0,0) at the
  // top-left) in colour, at a fixed on-screen thickness (see
  // Builtin.LineShader.vert.glsl's THICKNESS constant). command_buffer
  // must currently be inside the UI renderpass.
  void render_to(VulkanCommandBuffer &command_buffer, u32 width, u32 height,
                glm::vec2 start, glm::vec2 end, glm::vec4 colour);

private:
  VulkanContext *context_;

  // Non-owning -- registered with and owned by ShaderSystem.
  VulkanShader *shader_ = nullptr;
  VulkanShader::UniformIndex projection_uniform_ =
      VulkanShader::kInvalidUniformIndex;
  VulkanShader::UniformIndex view_uniform_ =
      VulkanShader::kInvalidUniformIndex;

  // A single unit quad (local space [0,1]^2) -- render_to() reinterprets
  // it via push constants (start/end/colour) rather than a model matrix.
  std::optional<VulkanBuffer> vertex_buffer_;
  std::optional<VulkanBuffer> index_buffer_;

  bool valid_ = false;
};
