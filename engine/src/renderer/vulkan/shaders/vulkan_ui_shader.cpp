#include "vulkan_ui_shader.h"
#include "../../../core/logger.h"
#include "../../../systems/material_system.h"
#include "../../../systems/shader_system.h"
#include "../vulkan_commandbuffer.h"
#include "../vulkan_renderpass.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace {
constexpr std::string_view kShaderName = "Shader.Builtin.UI";
constexpr std::string_view kMaterialName = "test_ui_material";

struct UIVertex {
  f32 position[2];
  f32 texcoord[2];
};

ShaderConfig build_ui_shader_config(VulkanRenderpass &ui_renderpass) {
  ShaderConfig config;
  config.name = std::string(kShaderName);
  config.stage_file_stem = "Builtin.UIShader";
  config.renderpass = &ui_renderpass;
  config.depth_test_enabled = false;
  config.attributes = {
      {"in_position", ShaderUniformType::Vec2},
      {"in_texcoord", ShaderUniformType::Vec2},
  };
  // Declaration order matters -- see VulkanShader: it determines byte
  // offsets within the global UBO, instance UBO, and push-constant block
  // respectively. Matches Builtin.UIShader.{vert,frag}.glsl exactly.
  config.uniforms = {
      {"projection", ShaderUniformType::Mat4, ShaderUniformScope::Global},
      {"view", ShaderUniformType::Mat4, ShaderUniformScope::Global},
      {"diffuse_colour", ShaderUniformType::Vec4, ShaderUniformScope::Instance},
      {"diffuse_texture", ShaderUniformType::Sampler,
       ShaderUniformScope::Instance},
      {"model", ShaderUniformType::Mat4, ShaderUniformScope::Local},
  };
  return config;
}
} // namespace

VulkanUIShader::VulkanUIShader(VulkanContext &context,
                               VulkanRenderpass &ui_renderpass)
    : context_(&context) {
  shader_ = &context_->shader_system->create(build_ui_shader_config(ui_renderpass));
  if (!shader_->is_valid()) {
    KERROR("Unable to create the '{}' shader.", kShaderName);
    return;
  }

  projection_uniform_ = shader_->uniform_index("projection");
  view_uniform_ = shader_->uniform_index("view");

  material_ = &context_->material_system->acquire(kMaterialName, true);
  context_->material_system->bind_to_shader(*material_, *shader_);

  // A unit quad (local space [0,1]^2, top-left origin) -- render_to()
  // positions/sizes it on screen via the model matrix push constant rather
  // than baking a screen position into the vertex data itself.
  UIVertex vertices[4] = {
      {{0.0f, 0.0f}, {0.0f, 0.0f}},
      {{1.0f, 0.0f}, {1.0f, 0.0f}},
      {{1.0f, 1.0f}, {1.0f, 1.0f}},
      {{0.0f, 1.0f}, {0.0f, 1.0f}},
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
    KERROR("Failed to create UI shader buffers.");
    return;
  }

  valid_ = true;
}

VulkanUIShader::~VulkanUIShader() {
  if (material_) {
    context_->material_system->release(kMaterialName);
    material_ = nullptr;
  }

  index_buffer_.reset();
  vertex_buffer_.reset();

  // shader_ is owned by ShaderSystem, not this wrapper -- nothing to
  // destroy here.
}

void VulkanUIShader::render_to(VulkanCommandBuffer &command_buffer, u32 width,
                               u32 height, glm::vec2 position, glm::vec2 size) {
  if (!valid_) {
    KWARN("VulkanUIShader::render_to called on an invalid shader.");
    return;
  }

  shader_->use(command_buffer);

  // Screen-pixel orthographic projection: (0,0) at the top-left, matching
  // the vertex data's own [0,1]^2 top-left-origin convention. bottom/top
  // are swapped (height, 0) relative to a typical GL-style ortho matrix so
  // Y increases downward, same as screen pixel coordinates.
  glm::mat4 projection = glm::ortho(0.0f, static_cast<f32>(width),
                                    static_cast<f32>(height), 0.0f, -1.0f,
                                    1.0f);
  glm::mat4 view(1.0f);
  shader_->set_uniform(projection_uniform_, &projection);
  shader_->set_uniform(view_uniform_, &view);
  shader_->bind_globals();
  shader_->apply_globals(command_buffer);

  context_->material_system->apply_instance(*material_, command_buffer);

  glm::mat4 model =
      glm::translate(glm::mat4(1.0f), glm::vec3(position, 0.0f)) *
      glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
  shader_->push_local(command_buffer, &model, sizeof(model));

  VkCommandBuffer cmd = command_buffer.handle();
  VkDeviceSize offsets[1] = {0};
  VkBuffer vertex_buffer_handle = vertex_buffer_->handle();
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_handle, offsets);
  vkCmdBindIndexBuffer(cmd, index_buffer_->handle(), 0, VK_INDEX_TYPE_UINT32);

  vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
}
