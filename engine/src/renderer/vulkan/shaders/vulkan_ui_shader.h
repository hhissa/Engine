#pragma once
#include "../vulkan_buffer.h"
#include "../vulkan_shader.h"
#include "../vulkan_types.inl"

#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

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

  // Draws the image at path (a texture name, e.g. "photos/cloud_mel.jpg" --
  // see TextureSystem's path rule) at position/size, same conventions as
  // render_to(). Each distinct image gets its own material, made and bound
  // to this shader on its first draw and released again once it hasn't
  // been drawn for a few frames -- see release_idle_images() -- so only
  // the images actually on screen hold video memory.
  void render_image_to(VulkanCommandBuffer &command_buffer, u32 width,
                       u32 height, std::string_view path, glm::vec2 position,
                       glm::vec2 size);

  // Releases the material (and so the GPU texture) of every image that
  // hasn't been drawn for kImageIdleFrames frames. Called once per frame,
  // before that frame's draws are recorded. The wait is what makes the
  // release safe without a device idle: every command buffer that could
  // still reference the texture has long since completed.
  void release_idle_images();

private:
  // The image material for path, made and bound on first use.
  Material &image_material(std::string_view path);

  // The draw both render_to() and render_image_to() come down to.
  void draw_quad(VulkanCommandBuffer &command_buffer, u32 width, u32 height,
                 Material &material, glm::vec2 position, glm::vec2 size);

  // One material per image path currently in use -- see
  // render_image_to(). Released (by Material::name, its content key) once
  // idle, see release_idle_images(), and in the destructor. last_drawn is
  // frame_ at its latest draw.
  struct ImageMaterial {
    Material *material = nullptr;
    u64 last_drawn = 0;
  };
  std::unordered_map<std::string, ImageMaterial> image_materials_;
  // Counts release_idle_images() calls, i.e. frames.
  u64 frame_ = 0;
  // More than the swapchain has images in flight (max_frames_in_flight + 1).
  static constexpr u64 kImageIdleFrames = 8;

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
