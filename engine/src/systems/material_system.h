#pragma once
#include "../resources/material_def.h"
#include "../renderer/vulkan/vulkan_shader.h"
#include "../renderer/vulkan/vulkan_texture.h"
#include "texture_system.h"

#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

class VulkanCommandBuffer;

// A tint colour plus an optional diffuse texture, parsed from a plain-text
// .kmt config file (assets/materials/<name>.kmt) -- kohi's format, kept
// as-is:
//
//   #material file
//   version=0.1
//   name=test_material
//   diffuse_colour=1.0 1.0 1.0 1.0
//   diffuse_map_name=cobblestone
//
// Originally ported without kohi's per-object descriptor-set/instance-state
// plumbing (vulkan_material_shader's acquire_resources et al.), since
// nothing needed per-object GPU binding at the time. VulkanUIShader/
// VulkanTextShader's move to the generic VulkanShader system now needs
// exactly that, so it's back -- see MaterialSystem::bind_to_shader()/
// apply_instance() below. Materials that are never bound to a shader (e.g.
// the ones VulkanRaymarchShader reads purely as colour+texture data for its
// compute dispatch) simply leave shader==nullptr and are unaffected.
struct Material {
  // The key MaterialSystem cached this under. For a material acquired
  // from a MaterialDef that is the def's CONTENT key (see
  // material_def_content_key()) -- deliberately, so two identically-valued
  // materials resolve to one runtime entry and one set of texture
  // references, however they were authored. For a legacy acquire(name) it
  // is the .kmt filename.
  //
  // Not a display name and not an authoring identity: see MaterialDef::id
  // and MaterialDef::display_name for those.
  std::string name;

  // The authored values, in ONE place.
  //
  // These used to be a dozen flat fields on this struct, duplicated from
  // the .kmt parser, duplicated again in the sdf_editor's filename
  // encoder, and duplicated a third time in the editor's reader. Adding a
  // property meant touching all of them and hoping they agreed about
  // rounding. Holding the def itself means a new property is one line in
  // for_each_material_property() and zero lines here.
  MaterialDef def;

  VulkanTexture *diffuse_texture =
      nullptr; // Non-owning -- owned by TextureSystem.
  VulkanTexture *bump_texture =
      nullptr; // Non-owning -- owned by TextureSystem. See
               // MaterialDef::bump_map.

  // GPU instance resources for the shader this material is bound to via
  // MaterialSystem::bind_to_shader(). Left at their defaults (shader ==
  // nullptr) for materials that are never drawn through a VulkanShader.
  VulkanShader *shader = nullptr;
  u32 shader_instance_id = VulkanShader::kInvalidInstanceId;
  // Cached uniform indices for this material's shader -- either may be
  // VulkanShader::kInvalidUniformIndex if that shader doesn't declare the
  // corresponding uniform (e.g. Shader.Builtin.Text has no instance-scope
  // diffuse_colour, only diffuse_texture; apply_instance() below skips
  // setting whichever index is invalid).
  VulkanShader::UniformIndex diffuse_colour_uniform =
      VulkanShader::kInvalidUniformIndex;
  VulkanShader::UniformIndex diffuse_texture_uniform =
      VulkanShader::kInvalidUniformIndex;
};

class MaterialSystem {
public:
  explicit MaterialSystem(TextureSystem &texture_system);
  ~MaterialSystem() = default;

  MaterialSystem(const MaterialSystem &) = delete;
  MaterialSystem &operator=(const MaterialSystem &) = delete;

  // Loads and parses assets/materials/<name>.kmt the first time a given
  // name is requested (subsequent calls for the same name reuse the cached
  // result and just bump its reference count), resolving diffuse_map_name/
  // bump_map_name through TextureSystem. Falls back to default_material()
  // if the file is missing, so callers can always dereference the result.
  Material &acquire(std::string_view name, bool auto_release);

  // Acquires the runtime material for an already-resolved MaterialDef --
  // the path everything scene-driven now takes, with acquire(name) above
  // left for the legacy .kmt form.
  //
  // Keyed by material_def_content_key(def), NOT by the def's id or display
  // name. That is the one place content addressing belongs: two primitives
  // whose resolved materials would render identically share one entry and
  // one pair of texture references, whether they reference the same
  // library material, two identical ones, or the same one with identical
  // overrides.
  //
  // The corollary matters for callers: the key to pass to release() is
  // Material::name (which holds exactly that content key), not anything
  // the author typed.
  Material &acquire_def(const MaterialDef &def, bool auto_release);

  // Mirrors TextureSystem::release: every acquire() call must be paired
  // with exactly one release() call for the same name. Also releases the
  // material's diffuse/bump texture references, if it had any, and (if the
  // material was bound to a shader via bind_to_shader()) its shader
  // instance resources.
  void release(std::string_view name);

  // A plain white material using the default checkerboard texture, always
  // resident for the lifetime of the system.
  Material &default_material() noexcept { return *default_material_; }

  // Acquires GPU instance resources for material from shader and caches
  // the shader's "diffuse_colour"/"diffuse_texture" uniform indices on the
  // material for later apply_instance() calls. Call once per material,
  // right after acquiring it, from the shader wrapper that owns it (see
  // VulkanUIShader/VulkanTextShader).
  void bind_to_shader(Material &material, VulkanShader &shader);

  // Updates a material's diffuse texture, both the cached Material field
  // and (if the material is already bound to a shader) the shader's
  // pending instance sampler state. Used by VulkanTextShader to point its
  // material at the baked font atlas instead of an on-disk texture, after
  // bind_to_shader() has already run.
  void set_diffuse_texture(Material &material, VulkanTexture &texture);

  // Refreshes material's bound shader instance (diffuse colour contents,
  // plus the sampler binding if the texture changed) and binds it (set 1).
  // No-op if material.shader is null (material was never bound to a
  // shader).
  void apply_instance(Material &material, VulkanCommandBuffer &command_buffer);

private:
  struct Entry {
    std::optional<Material> material;
    u32 reference_count = 0;
    bool auto_release = false;
  };

  TextureSystem *texture_system_;
  std::unordered_map<std::string, Entry> materials_;
  std::optional<Material> default_material_;
};
