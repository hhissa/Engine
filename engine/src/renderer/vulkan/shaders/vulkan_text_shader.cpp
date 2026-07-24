#include "vulkan_text_shader.h"
#include "../../../core/logger.h"
#include "../../../systems/material_system.h"
#include "../../../systems/shader_system.h"
#include "../vulkan_commandbuffer.h"
#include "../vulkan_renderpass.h"

#include <glm/gtc/matrix_transform.hpp>

namespace {
constexpr std::string_view kShaderName = "Shader.Builtin.Text";
constexpr std::string_view kMaterialName = "default_text_material";
constexpr std::string_view kFontName = "DejaVuSans";
constexpr f32 kFontPixelHeight = 28.0f;

struct TextVertex {
  f32 position[2];
  f32 texcoord[2];
};

ShaderConfig build_text_shader_config(VulkanRenderpass &ui_renderpass) {
  ShaderConfig config;
  config.name = std::string(kShaderName);
  config.stage_file_stem = "Builtin.TextShader";
  config.renderpass = &ui_renderpass;
  config.depth_test_enabled = false;
  config.attributes = {
      {"in_position", ShaderUniformType::Vec2},
      {"in_texcoord", ShaderUniformType::Vec2},
  };
  // No instance-scope "diffuse_colour" here (unlike Shader.Builtin.UI):
  // text's per-draw colour must stay a Local-scope push constant (see
  // "colour" below) since draw_text() can be queued multiple times per
  // frame with different colours -- an instance UBO write would race
  // between those calls (see this shader's header comment / the port
  // plan's design notes for the full reasoning). MaterialSystem's generic
  // apply_instance() simply skips diffuse_colour for materials bound to a
  // shader that doesn't declare it, so no special-casing is needed here.
  config.uniforms = {
      {"projection", ShaderUniformType::Mat4, ShaderUniformScope::Global},
      {"view", ShaderUniformType::Mat4, ShaderUniformScope::Global},
      {"diffuse_texture", ShaderUniformType::Sampler,
       ShaderUniformScope::Instance},
      {"colour", ShaderUniformType::Vec4, ShaderUniformScope::Local},
  };
  return config;
}
} // namespace

VulkanTextShader::VulkanTextShader(VulkanContext &context,
                                   VulkanRenderpass &ui_renderpass)
    : context_(&context) {
  shader_ =
      &context_->shader_system->create(build_text_shader_config(ui_renderpass));
  if (!shader_->is_valid()) {
    KERROR("Unable to create the '{}' shader.", kShaderName);
    return;
  }

  projection_uniform_ = shader_->uniform_index("projection");
  view_uniform_ = shader_->uniform_index("view");

  font_ = std::make_unique<BitmapFont>(*context_, kFontName, kFontPixelHeight);
  if (!font_->is_valid()) {
    KERROR("Failed to bake bitmap font '{}'.", kFontName);
    return;
  }
  font_pixel_height_ = kFontPixelHeight;

  // Acquire a trivial material purely as the vehicle for this shader's
  // instance resources, then point it at the baked font atlas instead of
  // an on-disk texture (default_text_material.kmt has no diffuse_map_name,
  // so bind_to_shader() below would otherwise leave it on the default
  // checkerboard texture).
  material_ = &context_->material_system->acquire(kMaterialName, true);
  context_->material_system->bind_to_shader(*material_, *shader_);
  context_->material_system->set_diffuse_texture(*material_, font_->atlas());

  vertex_buffer_.emplace(
      *context_, static_cast<u64>(kMaxCharacters) * 4 * sizeof(TextVertex),
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  index_buffer_.emplace(
      *context_, static_cast<u64>(kMaxCharacters) * 6 * sizeof(u32),
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  if (!vertex_buffer_->is_valid() || !index_buffer_->is_valid()) {
    KERROR("Failed to create text shader buffers.");
    return;
  }

  valid_ = true;
}

VulkanTextShader::~VulkanTextShader() {
  if (material_) {
    context_->material_system->release(kMaterialName);
    material_ = nullptr;
  }

  index_buffer_.reset();
  vertex_buffer_.reset();
  font_.reset();

  // shader_ is owned by ShaderSystem, not this wrapper -- nothing to
  // destroy here.
}

void VulkanTextShader::begin_batch() {
  batched_characters_ = 0;
}

void VulkanTextShader::set_font(std::string_view name, f32 pixel_height) {
  if (!valid_) {
    KWARN("VulkanTextShader::set_font called on an invalid shader.");
    return;
  }

  // The old atlas texture (and material_'s descriptor binding pointing at
  // it) could still be read by an in-flight frame's command buffer --
  // same hazard VulkanRaymarchShader::rebake() guards against.
  vkDeviceWaitIdle(context_->device.logical_device);

  auto new_font = std::make_unique<BitmapFont>(*context_, name, pixel_height);
  if (!new_font->is_valid()) {
    KERROR("Failed to bake bitmap font '{}' at size {}; keeping the "
          "current font.",
          name, pixel_height);
    return; // new_font destructs here -- font_ (the working font) is untouched
  }

  font_ = std::move(new_font); // old font destructs here, only now that the new one is confirmed good
  font_pixel_height_ = pixel_height;
  context_->material_system->set_diffuse_texture(*material_, font_->atlas());
}

void VulkanTextShader::render_to(VulkanCommandBuffer &command_buffer,
                                 u32 width, u32 height, std::string_view text,
                                 glm::vec2 origin, glm::vec4 colour) {
  if (!valid_) {
    KWARN("VulkanTextShader::render_to called on an invalid shader.");
    return;
  }

  if (text.size() > kMaxCharacters) {
    text = text.substr(0, kMaxCharacters);
  }

  std::vector<BitmapFont::GlyphQuad> glyphs = font_->layout(text, origin);
  if (glyphs.empty()) {
    return;
  }

  // Clamp to what's left of the shared per-frame glyph budget (see
  // begin_batch()).
  u32 available = kMaxCharacters - batched_characters_;
  if (available == 0) {
    KWARN("VulkanTextShader: out of per-frame glyph budget ({}); '{}' will "
         "not be drawn.",
         kMaxCharacters, text);
    return;
  }
  if (glyphs.size() > available) {
    KWARN("VulkanTextShader: per-frame glyph budget ({}) exceeded; '{}' will "
         "be truncated.",
         kMaxCharacters, text);
    glyphs.resize(available);
  }

  std::vector<TextVertex> vertices(glyphs.size() * 4);
  std::vector<u32> indices(glyphs.size() * 6);
  for (size_t i = 0; i < glyphs.size(); ++i) {
    const BitmapFont::GlyphQuad &g = glyphs[i];
    TextVertex *v = &vertices[i * 4];
    v[0] = {{g.x0, g.y0}, {g.s0, g.t0}}; // top-left
    v[1] = {{g.x1, g.y0}, {g.s1, g.t0}}; // top-right
    v[2] = {{g.x1, g.y1}, {g.s1, g.t1}}; // bottom-right
    v[3] = {{g.x0, g.y1}, {g.s0, g.t1}}; // bottom-left

    u32 base = static_cast<u32>(i * 4);
    u32 *idx = &indices[i * 6];
    idx[0] = base + 0;
    idx[1] = base + 1;
    idx[2] = base + 2;
    idx[3] = base + 2;
    idx[4] = base + 3;
    idx[5] = base + 0;
  }

  // Append at the batch cursor rather than offset 0 -- earlier draws this
  // frame are only *recorded*, so their data must survive in the buffer
  // until the GPU executes the whole frame.
  vertex_buffer_->load_data(
      static_cast<u64>(batched_characters_) * 4 * sizeof(TextVertex),
      vertices.size() * sizeof(TextVertex), 0, vertices.data());
  index_buffer_->load_data(static_cast<u64>(batched_characters_) * 6 *
                               sizeof(u32),
                           indices.size() * sizeof(u32), 0, indices.data());

  shader_->use(command_buffer);

  glm::mat4 projection = glm::ortho(0.0f, static_cast<f32>(width),
                                    static_cast<f32>(height), 0.0f, -1.0f,
                                    1.0f);
  glm::mat4 view(1.0f);
  shader_->set_uniform(projection_uniform_, &projection);
  shader_->set_uniform(view_uniform_, &view);
  shader_->bind_globals();
  shader_->apply_globals(command_buffer);

  context_->material_system->apply_instance(*material_, command_buffer);

  shader_->push_local(command_buffer, &colour, sizeof(colour));

  VkCommandBuffer cmd = command_buffer.handle();
  VkDeviceSize offsets[1] = {0};
  VkBuffer vertex_buffer_handle = vertex_buffer_->handle();
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_handle, offsets);
  vkCmdBindIndexBuffer(cmd, index_buffer_->handle(), 0, VK_INDEX_TYPE_UINT32);

  // firstIndex/vertexOffset select this draw's slice of the shared
  // per-frame buffers (index values stay glyph-local 0..3, vertexOffset
  // rebases them).
  vkCmdDrawIndexed(cmd, static_cast<u32>(indices.size()), 1,
                   batched_characters_ * 6,
                   static_cast<i32>(batched_characters_ * 4), 0);

  batched_characters_ += static_cast<u32>(glyphs.size());
}
