#include "material_system.h"
#include "../core/logger.h"
#include "../renderer/vulkan/vulkan_commandbuffer.h"

#include <fstream>
#include <sstream>

namespace {
std::string material_path(std::string_view name) {
  return "assets/materials/" + std::string(name) + ".kmt";
}

std::string trim(const std::string &s) {
  constexpr const char *kWhitespace = " \t\r\n";
  auto start = s.find_first_not_of(kWhitespace);
  if (start == std::string::npos) {
    return "";
  }
  auto end = s.find_last_not_of(kWhitespace);
  return s.substr(start, end - start + 1);
}
} // namespace

MaterialSystem::MaterialSystem(TextureSystem &texture_system)
    : texture_system_(&texture_system) {
  Material default_mat;
  default_mat.name = "default";
  default_mat.diffuse_colour = glm::vec4(1.0f);
  default_mat.diffuse_texture = &texture_system_->default_texture();
  default_material_.emplace(std::move(default_mat));
}

Material &MaterialSystem::acquire(std::string_view name, bool auto_release) {
  std::string key(name);
  Entry &entry = materials_.try_emplace(key).first->second;

  if (entry.reference_count == 0) {
    entry.auto_release = auto_release;
  }
  ++entry.reference_count;

  if (!entry.material) {
    std::ifstream file(material_path(name));
    if (!file.is_open()) {
      KWARN("Material file not found for '{}'; using the default material "
           "in its place.",
           name);
    } else {
      Material material;
      material.name = key;

      std::string line;
      while (std::getline(file, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
          continue;
        }

        auto eq = trimmed.find('=');
        if (eq == std::string::npos) {
          KWARN("Potential formatting issue in material file '{}': '=' "
               "token not found. Skipping line: '{}'",
               name, trimmed);
          continue;
        }

        std::string var_name = trim(trimmed.substr(0, eq));
        std::string value = trim(trimmed.substr(eq + 1));

        if (var_name == "name") {
          material.name = value;
        } else if (var_name == "diffuse_map_name") {
          material.diffuse_map_name = value;
        } else if (var_name == "diffuse_colour") {
          std::istringstream iss(value);
          glm::vec4 colour(1.0f);
          iss >> colour.x >> colour.y >> colour.z >> colour.w;
          if (iss.fail()) {
            KWARN("Error parsing diffuse_colour in file '{}'. Using "
                 "default of white instead.",
                 name);
            colour = glm::vec4(1.0f);
          }
          material.diffuse_colour = colour;
        } else if (var_name == "texture_scale") {
          std::istringstream iss(value);
          f32 scale = 0.0f;
          iss >> scale;
          if (iss.fail() || scale <= 0.0f) {
            KWARN("Error parsing texture_scale in file '{}'. Using the "
                 "default of {} instead.",
                 name, material.texture_scale);
          } else {
            material.texture_scale = scale;
          }
        }
        // "version" is intentionally ignored -- nothing yet depends on it.
      }

      material.diffuse_texture =
          material.diffuse_map_name.empty()
              ? &texture_system_->default_texture()
              : &texture_system_->acquire(material.diffuse_map_name, true);

      entry.material = std::move(material);
      KTRACE("Material '{}' loaded, reference count now {}.", name,
            entry.reference_count);
    }
  }

  return entry.material ? *entry.material : *default_material_;
}

void MaterialSystem::release(std::string_view name) {
  std::string key(name);
  auto it = materials_.find(key);
  if (it == materials_.end() || it->second.reference_count == 0) {
    KWARN("MaterialSystem::release called for a material with no "
         "outstanding references: '{}'.",
         name);
    return;
  }

  Entry &entry = it->second;
  --entry.reference_count;
  if (entry.reference_count == 0 && entry.auto_release) {
    if (entry.material) {
      if (!entry.material->diffuse_map_name.empty()) {
        texture_system_->release(entry.material->diffuse_map_name);
      }
      if (entry.material->shader && entry.material->shader_instance_id !=
                                        VulkanShader::kInvalidInstanceId) {
        entry.material->shader->release_instance_resources(
            entry.material->shader_instance_id);
      }
    }
    materials_.erase(it);
  }
}

void MaterialSystem::bind_to_shader(Material &material, VulkanShader &shader) {
  material.shader = &shader;
  material.shader_instance_id = shader.acquire_instance_resources();
  material.diffuse_colour_uniform = shader.uniform_index("diffuse_colour");
  material.diffuse_texture_uniform = shader.uniform_index("diffuse_texture");

  if (material.shader_instance_id == VulkanShader::kInvalidInstanceId) {
    KERROR("MaterialSystem::bind_to_shader: failed to acquire instance "
          "resources for material '{}'.",
          material.name);
    return;
  }

  shader.bind_instance(material.shader_instance_id);
  if (material.diffuse_colour_uniform != VulkanShader::kInvalidUniformIndex) {
    shader.set_instance_uniform(material.diffuse_colour_uniform,
                               &material.diffuse_colour);
  }
  if (material.diffuse_texture_uniform != VulkanShader::kInvalidUniformIndex &&
      material.diffuse_texture) {
    shader.set_sampler(material.diffuse_texture_uniform,
                       *material.diffuse_texture);
  }
}

void MaterialSystem::set_diffuse_texture(Material &material,
                                         VulkanTexture &texture) {
  material.diffuse_texture = &texture;
  if (material.shader &&
      material.diffuse_texture_uniform != VulkanShader::kInvalidUniformIndex) {
    material.shader->bind_instance(material.shader_instance_id);
    material.shader->set_sampler(material.diffuse_texture_uniform, texture);
  }
}

void MaterialSystem::apply_instance(Material &material,
                                    VulkanCommandBuffer &command_buffer) {
  if (!material.shader ||
      material.shader_instance_id == VulkanShader::kInvalidInstanceId) {
    return;
  }
  material.shader->bind_instance(material.shader_instance_id);
  if (material.diffuse_colour_uniform != VulkanShader::kInvalidUniformIndex) {
    material.shader->set_instance_uniform(material.diffuse_colour_uniform,
                                         &material.diffuse_colour);
  }
  material.shader->apply_instance(command_buffer);
}
