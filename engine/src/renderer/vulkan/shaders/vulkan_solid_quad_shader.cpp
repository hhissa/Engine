#include "vulkan_solid_quad_shader.h"
#include "../../../core/logger.h"
#include "../../../systems/shader_system.h"
#include "../vulkan_commandbuffer.h"
#include "../vulkan_renderpass.h"

#include <glm/gtc/matrix_transform.hpp>

namespace {
constexpr std::string_view kShaderName = "Shader.Builtin.SolidQuad";

struct SolidQuadVertex {
  f32 position[2]; // local [0,1]^2 -- see Builtin.SolidQuadShader.vert.glsl
};

// Matches Builtin.SolidQuadShader.vert.glsl's push_constant block exactly
// (declaration order determines byte offsets: position=0, size=8,
// colour=16).
struct SolidQuadPushConstants {
  f32 position[2];
  f32 size[2];
  f32 colour[4];
};

ShaderConfig build_solid_quad_shader_config(VulkanRenderpass &ui_renderpass) {
  ShaderConfig config;
  config.name = std::string(kShaderName);
  config.stage_file_stem = "Builtin.SolidQuadShader";
  config.renderpass = &ui_renderpass;
  config.depth_test_enabled = false;
  config.attributes = {
      {"in_position", ShaderUniformType::Vec2},
  };
  config.uniforms = {
      {"projection", ShaderUniformType::Mat4, ShaderUniformScope::Global},
      {"view", ShaderUniformType::Mat4, ShaderUniformScope::Global},
      {"position", ShaderUniformType::Vec2, ShaderUniformScope::Local},
      {"size", ShaderUniformType::Vec2, ShaderUniformScope::Local},
      {"colour", ShaderUniformType::Vec4, ShaderUniformScope::Local},
  };
  return config;
}
} // namespace

VulkanSolidQuadShader::VulkanSolidQuadShader(VulkanContext &context,
                                            VulkanRenderpass &ui_renderpass)
    : context_(&context) {
  shader_ = &context_->shader_system->create(
      build_solid_quad_shader_config(ui_renderpass));
  if (!shader_->is_valid()) {
    KERROR("Unable to create the '{}' shader.", kShaderName);
    return;
  }

  projection_uniform_ = shader_->uniform_index("projection");
  view_uniform_ = shader_->uniform_index("view");

  SolidQuadVertex vertices[4] = {
      {{0.0f, 0.0f}},
      {{1.0f, 0.0f}},
      {{1.0f, 1.0f}},
      {{0.0f, 1.0f}},
  };
  u32 indices[6] = {0, 1, 2, 2, 3, 0};

  vertex_buffer_.emplace(
      *context_, sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  vertex_buffer_->load_data(0, sizeof(vertices), 0, vertices);

  index_buffer_.emplace(
      *context_, sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  index_buffer_->load_data(0, sizeof(indices), 0, indices);

  if (!vertex_buffer_->is_valid() || !index_buffer_->is_valid()) {
    KERROR("Failed to create solid quad shader buffers.");
    return;
  }

  valid_ = true;
}

VulkanSolidQuadShader::~VulkanSolidQuadShader() {
  index_buffer_.reset();
  vertex_buffer_.reset();
  // shader_ is owned by ShaderSystem, not this wrapper -- nothing to
  // destroy here.
}

void VulkanSolidQuadShader::render_to(VulkanCommandBuffer &command_buffer,
                                     u32 width, u32 height, glm::vec2 position,
                                     glm::vec2 size, glm::vec4 colour) {
  if (!valid_) {
    KWARN("VulkanSolidQuadShader::render_to called on an invalid shader.");
    return;
  }

  shader_->use(command_buffer);

  glm::mat4 projection = glm::ortho(0.0f, static_cast<f32>(width),
                                    static_cast<f32>(height), 0.0f, -1.0f,
                                    1.0f);
  glm::mat4 view(1.0f);
  shader_->set_uniform(projection_uniform_, &projection);
  shader_->set_uniform(view_uniform_, &view);
  shader_->bind_globals();
  shader_->apply_globals(command_buffer);

  SolidQuadPushConstants push{};
  push.position[0] = position.x;
  push.position[1] = position.y;
  push.size[0] = size.x;
  push.size[1] = size.y;
  push.colour[0] = colour.r;
  push.colour[1] = colour.g;
  push.colour[2] = colour.b;
  push.colour[3] = colour.a;
  shader_->push_local(command_buffer, &push, sizeof(push));

  VkCommandBuffer cmd = command_buffer.handle();
  VkDeviceSize offsets[1] = {0};
  VkBuffer vertex_buffer_handle = vertex_buffer_->handle();
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_handle, offsets);
  vkCmdBindIndexBuffer(cmd, index_buffer_->handle(), 0, VK_INDEX_TYPE_UINT32);

  vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
}
