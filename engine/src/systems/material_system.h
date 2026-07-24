#pragma once
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
  std::string name;
  glm::vec4 diffuse_colour{1.0f};
  std::string diffuse_map_name; // empty => no diffuse map, use the default.
  // World units per texture tile ("texture_scale=" in the .kmt) -- how far
  // across a surface one full repeat of the diffuse texture spans. Only
  // read by VulkanRaymarchShader's triplanar sampling; the default matches
  // the TEXTURE_SCALE constant that used to be hardcoded in
  // Builtin.RaymarchShader.comp.glsl, so materials that don't set it look
  // exactly as they always did.
  f32 texture_scale = 0.6f;
  // Self-illumination -- "emissive_colour="/"emissive_intensity=" in the
  // .kmt. Any primitive using a material with emissive_intensity > 0
  // becomes a visible light source: it renders at a brightness
  // independent of the scene's actual lighting (see the emissive term in
  // Builtin.RaymarchShader.comp.glsl -- added straight into the shaded
  // colour, unlike diffuse/ambient which depend on incoming light), and
  // VulkanRaymarchShader::rebuild_static_scene() automatically registers
  // one synthesized point light per emissive primitive (positioned at the
  // primitive itself) so it also actually illuminates everything else --
  // an authored "glowing bulb"/"light panel" primitive that is genuinely
  // its own light source, not just a bright-looking prop. Default
  // intensity 0 (off) -- existing materials that don't set this glow
  // exactly as they always did.
  glm::vec3 emissive_colour{1.0f};
  f32 emissive_intensity = 0.0f;
  // "pixelation_exempt=true" in the .kmt -- excludes any primitive using
  // this material from the screen-space pixelation post-process (see
  // Builtin.PostComposite.comp.glsl), so it stays crisp/full-resolution
  // even while everything else on screen pixelates. Off by default (this
  // primitive pixelates normally, like everything else).
  bool pixelation_exempt = false;
  VulkanTexture *diffuse_texture =
      nullptr; // Non-owning -- owned by TextureSystem.

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
  // result and just bump its reference count), resolving diffuse_map_name
  // through TextureSystem. Falls back to default_material() if the file is
  // missing, so callers can always dereference the result.
  Material &acquire(std::string_view name, bool auto_release);

  // Mirrors TextureSystem::release: every acquire() call must be paired
  // with exactly one release() call for the same name. Also releases the
  // material's diffuse texture reference, if it had one, and (if the
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
