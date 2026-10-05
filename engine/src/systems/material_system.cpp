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
  default_mat.def.display_name = "default";
  default_mat.diffuse_texture = &texture_system_->default_texture();
  default_mat.bump_texture = &texture_system_->flat_texture();
  default_material_.emplace(std::move(default_mat));
}

namespace {
// Everything a Material needs beyond its values: the two texture
// references resolved through TextureSystem. Shared by both acquire
// paths so they cannot drift apart on what an empty map name means.
void resolve_textures(Material &material, TextureSystem &textures) {
  material.diffuse_texture =
      material.def.base_map.empty()
          ? &textures.default_texture()
          : &textures.acquire(material.def.base_map, true);
  // A material with no bump map reads a genuinely uniform texture, so the
  // height-difference computation comes out to exactly zero -- bump
  // mapping is opt-in per material rather than derived from whatever
  // diffuse texture happens to be assigned.
  material.bump_texture = material.def.bump_map.empty()
                              ? &textures.flat_texture()
                              : &textures.acquire(material.def.bump_map, true);
}
} // namespace

Material &MaterialSystem::acquire(std::string_view name, bool auto_release) {
  std::string key(name);
  Entry &entry = materials_.try_emplace(key).first->second;

  if (entry.reference_count == 0) {
    entry.auto_release = auto_release;
  }
  ++entry.reference_count;

  if (!entry.material) {
    // material_def_load_kmt() is the ONLY .kmt parser in the codebase --
    // this function used to carry a second copy of it, and the sdf_editor
    // a third, which is how the editor's quantisation and the engine's
    // parse were able to disagree about a value with nothing noticing.
    MaterialDef def;
    if (!material_def_load_kmt(name, def)) {
      KWARN("Material file not found for '{}'; using the default material "
           "in its place.",
           name);
    } else {
      Material material;
      material.name = key;
      material.def = std::move(def);
      resolve_textures(material, *texture_system_);
      entry.material = std::move(material);
      KTRACE("Material '{}' loaded, reference count now {}.", name,
            entry.reference_count);
    }
  }

  return entry.material ? *entry.material : *default_material_;
}

Material &MaterialSystem::acquire_def(const MaterialDef &def,
                                     bool auto_release) {
  const std::string key = material_def_content_key(def);
  Entry &entry = materials_.try_emplace(key).first->second;

  if (entry.reference_count == 0) {
    entry.auto_release = auto_release;
  }
  ++entry.reference_count;

  if (!entry.material) {
    Material material;
    material.name = key;
    material.def = def;
    resolve_textures(material, *texture_system_);
    entry.material = std::move(material);
    KTRACE("Material '{}' resolved from a definition, reference count now "
          "{}.",
          def.display_name.empty() ? material_id_to_string(def.id)
                                   : def.display_name,
          entry.reference_count);
  }

  return *entry.material;
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
      if (!entry.material->def.base_map.empty()) {
        texture_system_->release(entry.material->def.base_map);
      }
      if (!entry.material->def.bump_map.empty()) {
        texture_system_->release(entry.material->def.bump_map);
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
                               &material.def.base_colour);
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
                                         &material.def.base_colour);
  }
  material.shader->apply_instance(command_buffer);
}
