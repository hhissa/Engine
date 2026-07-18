#pragma once
#include "vulkan_buffer.h"
#include "vulkan_graphics_pipeline.h"
#include "vulkan_shader_module.h"
#include "vulkan_types.inl"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class VulkanCommandBuffer;
class VulkanRenderpass;
class VulkanTexture;

// Scope of a shader uniform: how often its backing storage is expected to
// change. Global -- once per frame (e.g. projection/view); Instance -- once
// per acquired "instance" (e.g. a material's diffuse colour/texture);
// Local -- once per draw call, delivered via push constants rather than a
// UBO, since several draws using the same instance can occur within one
// frame with different local values (e.g. VulkanTextShader's per-call
// colour -- see its wrapper for why that one specifically must stay Local).
enum class ShaderUniformScope : u8 { Global, Instance, Local };

// The set of uniform/attribute value types this engine's two builtin
// rasterized shaders (UI, Text) actually need. Extend as new shaders need
// new types.
enum class ShaderUniformType : u8 { Vec2, Vec4, Mat4, Sampler };

struct ShaderUniformConfig {
  std::string name;
  ShaderUniformType type;
  ShaderUniformScope scope;
};

struct ShaderAttributeConfig {
  std::string name;
  ShaderUniformType type;
};

// Describes a shader to be built by VulkanShader: which SPIR-V stage files
// to load (assets/shaders/<stage_file_stem>.<vert|frag>.spv), which
// renderpass it runs in, and its vertex attribute / uniform layout in
// declaration order (declaration order determines byte offsets within each
// scope's UBO/push-constant block -- see VulkanShader). Built by a small
// static factory function local to each shader's own .cpp (e.g.
// vulkan_ui_shader.cpp's build_ui_shader_config()) rather than loaded from
// a config file: this engine has exactly two consumers today, so a text
// format's parsing/validation cost doesn't pay for itself yet.
struct ShaderConfig {
  std::string name;
  std::string stage_file_stem;
  VulkanRenderpass *renderpass = nullptr;
  bool depth_test_enabled = false;
  std::vector<ShaderAttributeConfig> attributes;
  std::vector<ShaderUniformConfig> uniforms;
};

// A generic, data-driven rasterization shader: builds its descriptor set
// layouts (set 0 = global, set 1 = instance -- present only if the config
// declares at least one instance-scope uniform), its pipeline, and a single
// combined uniform buffer (a global region followed by kMaxShaderInstances
// fixed-stride instance slots) from a ShaderConfig. Replaces what used to
// be hand-written, duplicated per-shader boilerplate -- see
// VulkanUIShader/VulkanTextShader, now thin wrappers around this that keep
// only their genuinely different vertex-building logic.
//
// Deliberately simplified relative to a fully general shader system:
// descriptor sets are written once -- the global set at construction, each
// instance slot's UBO binding at construction, each instance slot's sampler
// binding lazily on its first apply_instance() after a real texture is set
// via set_sampler() -- and never rewritten wholesale afterward; only buffer
// *contents* change per frame/instance via set_uniform/set_instance_uniform.
// This is safe here because nothing ever rewrites a descriptor set that a
// still-in-flight command buffer might reference (this engine's frame loop
// is fully serialized; see VulkanUIShader's header comment for the same
// reasoning applied to its previous hand-rolled single descriptor set).
// Instance slots are a fixed-size array (kMaxShaderInstances) with a
// free-index stack, not a general allocator -- generous headroom over the 1
// instance each builtin shader actually needs today.
class VulkanShader {
public:
  using UniformIndex = u16;
  static constexpr UniformIndex kInvalidUniformIndex = 0xFFFF;
  static constexpr u32 kInvalidInstanceId = 0xFFFFFFFF;
  static constexpr u32 kMaxShaderInstances = 8;
  static constexpr u32 kMaxInstanceSamplers = 4;

  VulkanShader(VulkanContext &context, ShaderConfig config);
  ~VulkanShader();

  VulkanShader(const VulkanShader &) = delete;
  VulkanShader &operator=(const VulkanShader &) = delete;
  VulkanShader(VulkanShader &&) = delete;
  VulkanShader &operator=(VulkanShader &&) = delete;

  bool is_valid() const noexcept { return valid_; }
  explicit operator bool() const noexcept { return valid_; }

  // Returns kInvalidUniformIndex if no uniform with this name was declared
  // in the shader's config.
  UniformIndex uniform_index(std::string_view name) const;

  // Binds the pipeline. Call once per draw sequence using this shader,
  // before any of the methods below.
  void use(VulkanCommandBuffer &command_buffer);

  // Writes value into the shader's global-scope UBO region. index must
  // refer to a Global-scope, non-sampler uniform.
  void set_uniform(UniformIndex index, const void *value);
  // Intentionally empty -- kept for symmetry with apply_globals()/
  // bind_instance(). The global descriptor set is written once at
  // construction and never swapped for another, so there's nothing to
  // select here.
  void bind_globals();
  // Binds the global descriptor set (set 0).
  void apply_globals(VulkanCommandBuffer &command_buffer);

  // Allocates an instance slot (one of kMaxShaderInstances pre-allocated
  // descriptor sets) for e.g. one material. Returns kInvalidInstanceId if
  // all slots are currently in use.
  u32 acquire_instance_resources();
  void release_instance_resources(u32 instance_id);
  // Selects which instance's resources subsequent set_instance_uniform() /
  // set_sampler() / apply_instance() calls act on.
  void bind_instance(u32 instance_id);
  // Writes value into the bound instance's UBO region. index must refer to
  // an Instance-scope, non-sampler uniform.
  void set_instance_uniform(UniformIndex index, const void *value);
  // Sets the texture for the bound instance's sampler uniform at index.
  // Only triggers a real vkUpdateDescriptorSets call on the next
  // apply_instance() if the texture pointer actually changed since the
  // last one.
  void set_sampler(UniformIndex index, VulkanTexture &texture);
  // Writes any dirty instance descriptor state (sampler bindings whose
  // texture changed since the last call) and binds the instance descriptor
  // set (set 1). No-op-safe to call even if the shader has no instance
  // uniforms at all, aside from requiring bind_instance() to have been
  // called first.
  void apply_instance(VulkanCommandBuffer &command_buffer);

  // Pushes size bytes of Local-scope uniform data (e.g. a model matrix)
  // directly into the command buffer via vkCmdPushConstants, at offset 0.
  void push_local(VulkanCommandBuffer &command_buffer, const void *data,
                  u32 size);

  VkPipelineLayout pipeline_layout() const noexcept;

private:
  struct UniformMeta {
    std::string name;
    ShaderUniformType type;
    ShaderUniformScope scope;
    u32 offset;       // Byte offset within its scope's UBO/push-constant block.
    u32 size;         // Byte size; 0 for samplers.
    u32 sampler_slot; // Index within the instance sampler array; only meaningful for Instance-scope samplers.
  };

  struct InstanceState {
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::array<VulkanTexture *, kMaxInstanceSamplers> bound_textures{};
    std::array<VulkanTexture *, kMaxInstanceSamplers> desired_textures{};
  };

  VulkanContext *context_ = nullptr;
  std::string name_;

  VulkanShaderModule vertex_stage_;
  VulkanShaderModule fragment_stage_;
  std::optional<VulkanGraphicsPipeline> pipeline_;

  std::vector<UniformMeta> uniforms_;
  std::unordered_map<std::string, UniformIndex> uniform_lookup_;

  u32 global_ubo_size_ = 0;
  u32 global_ubo_stride_ = 0;
  u32 instance_ubo_size_ = 0;
  u32 instance_ubo_stride_ = 0;
  bool has_instance_ubo_ = false;
  u32 instance_sampler_count_ = 0;
  u32 instance_sampler_binding_ = 0;
  bool use_instance_ = false;
  u32 push_constant_size_ = 0;

  std::optional<VulkanBuffer> uniform_buffer_;

  VkDescriptorSetLayout global_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout instance_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet global_set_ = VK_NULL_HANDLE;

  std::array<InstanceState, kMaxShaderInstances> instances_;
  std::vector<u32> free_instance_slots_;
  u32 bound_instance_id_ = kInvalidInstanceId;

  bool valid_ = false;
};
