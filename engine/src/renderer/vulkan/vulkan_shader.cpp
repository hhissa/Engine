#include "vulkan_shader.h"
#include "../../core/logger.h"
#include "vulkan_commandbuffer.h"
#include "vulkan_renderpass.h"
#include "vulkan_texture.h"

namespace {
u32 align_up(u32 value, u32 alignment) {
  if (alignment == 0) {
    return value;
  }
  return (value + alignment - 1) & ~(alignment - 1);
}

u32 uniform_type_size(ShaderUniformType type) {
  switch (type) {
  case ShaderUniformType::Vec2:
    return 8;
  case ShaderUniformType::Vec4:
    return 16;
  case ShaderUniformType::Mat4:
    return 64;
  case ShaderUniformType::Sampler:
    return 0;
  }
  return 0;
}
} // namespace

VulkanShader::VulkanShader(VulkanContext &context, ShaderConfig config)
    : context_(&context), name_(config.name),
      vertex_stage_(context, config.stage_file_stem, "vert",
                   VK_SHADER_STAGE_VERTEX_BIT),
      fragment_stage_(context, config.stage_file_stem, "frag",
                     VK_SHADER_STAGE_FRAGMENT_BIT) {
  if (!vertex_stage_.is_valid() || !fragment_stage_.is_valid()) {
    KERROR("VulkanShader '{}': unable to create shader module(s).", name_);
    return;
  }

  if (!config.renderpass) {
    KERROR("VulkanShader '{}': no renderpass provided.", name_);
    return;
  }

  // Vertex attributes: sequential locations/offsets, tightly packed (no
  // per-attribute alignment padding needed -- Vec2/Vec4 are already
  // naturally aligned for how the two builtin shaders lay out their vertex
  // structs).
  u32 vertex_stride = 0;
  std::vector<VulkanGraphicsPipeline::VertexAttribute> vk_attributes;
  vk_attributes.reserve(config.attributes.size());
  for (const auto &attribute : config.attributes) {
    VkFormat format = VK_FORMAT_R32G32_SFLOAT;
    u32 size = 8;
    switch (attribute.type) {
    case ShaderUniformType::Vec2:
      format = VK_FORMAT_R32G32_SFLOAT;
      size = 8;
      break;
    case ShaderUniformType::Vec4:
      format = VK_FORMAT_R32G32B32A32_SFLOAT;
      size = 16;
      break;
    default:
      KERROR("VulkanShader '{}': unsupported vertex attribute type for "
             "'{}'.",
             name_, attribute.name);
      break;
    }
    vk_attributes.push_back({format, vertex_stride});
    vertex_stride += size;
  }

  // Uniforms: in declaration order, computing each uniform's byte offset
  // within its scope's UBO region (global/instance) or push-constant block
  // (local) as a running sum. Samplers don't occupy UBO/push-constant
  // space; instead each gets a sequential slot in the instance sampler
  // array (global-scope samplers aren't supported -- neither builtin
  // shader needs one).
  uniforms_.reserve(config.uniforms.size());
  for (const auto &uniform : config.uniforms) {
    UniformMeta meta{};
    meta.name = uniform.name;
    meta.type = uniform.type;
    meta.scope = uniform.scope;
    meta.offset = 0;
    meta.size = uniform_type_size(uniform.type);
    meta.sampler_slot = 0;

    if (uniform.type == ShaderUniformType::Sampler) {
      meta.size = 0;
      if (uniform.scope == ShaderUniformScope::Instance) {
        if (instance_sampler_count_ >= kMaxInstanceSamplers) {
          KERROR("VulkanShader '{}': too many instance-scope samplers "
                 "(max {}).",
                 name_, kMaxInstanceSamplers);
        } else {
          meta.sampler_slot = instance_sampler_count_++;
        }
      } else {
        KERROR("VulkanShader '{}': sampler '{}' must be Instance-scope "
               "(global/local samplers are not supported).",
               name_, uniform.name);
      }
    } else {
      switch (uniform.scope) {
      case ShaderUniformScope::Global:
        meta.offset = global_ubo_size_;
        global_ubo_size_ += meta.size;
        break;
      case ShaderUniformScope::Instance:
        meta.offset = instance_ubo_size_;
        instance_ubo_size_ += meta.size;
        has_instance_ubo_ = true;
        break;
      case ShaderUniformScope::Local:
        meta.offset = push_constant_size_;
        push_constant_size_ += meta.size;
        break;
      }
    }

    UniformIndex index = static_cast<UniformIndex>(uniforms_.size());
    uniform_lookup_.emplace(uniform.name, index);
    uniforms_.push_back(std::move(meta));
  }

  use_instance_ = has_instance_ubo_ || instance_sampler_count_ > 0;
  instance_sampler_binding_ = has_instance_ubo_ ? 1 : 0;

  // Strides: each scope's UBO region rounded up to the device's minimum
  // uniform buffer offset alignment, so every instance slot starts at a
  // valid descriptor offset.
  u32 min_alignment = static_cast<u32>(
      context_->device.properties.limits.minUniformBufferOffsetAlignment);
  global_ubo_stride_ = align_up(global_ubo_size_, min_alignment);
  instance_ubo_stride_ =
      has_instance_ubo_ ? align_up(instance_ubo_size_, min_alignment) : 0;

  // One combined buffer: the global region, followed by kMaxShaderInstances
  // fixed-stride instance slots (0 bytes each if the shader has no
  // instance-scope UBO fields, e.g. VulkanTextShader).
  u64 buffer_size = static_cast<u64>(global_ubo_stride_) +
                    static_cast<u64>(instance_ubo_stride_) *
                        kMaxShaderInstances;
  uniform_buffer_.emplace(
      *context_, buffer_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!uniform_buffer_->is_valid()) {
    KERROR("VulkanShader '{}': failed to create uniform buffer.", name_);
    return;
  }

  // Global descriptor set layout: always exactly 1 UBO binding (both
  // builtin shaders always have at least projection/view at global scope).
  // stageFlags are a superset (vertex|fragment) rather than tracked
  // per-uniform -- Vulkan doesn't require every stage granted access to a
  // binding to actually reference it, so this is a harmless simplification
  // that avoids needing a per-uniform "which stage(s) read this" field in
  // ShaderConfig.
  VkDescriptorSetLayoutBinding global_binding{};
  global_binding.binding = 0;
  global_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  global_binding.descriptorCount = 1;
  global_binding.stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutCreateInfo global_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  global_layout_info.bindingCount = 1;
  global_layout_info.pBindings = &global_binding;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &global_layout_info,
                                       context_->allocator,
                                       &global_set_layout_));

  // Instance descriptor set layout: only created if the shader declared at
  // least one instance-scope uniform. UBO binding (if any non-sampler
  // instance uniforms exist) always takes binding 0; the sampler binding
  // (if any instance-scope samplers exist) takes the next binding index --
  // 0 if there's no UBO (e.g. VulkanTextShader), 1 otherwise (e.g.
  // VulkanUIShader). This must match instance_sampler_binding_ above.
  if (use_instance_) {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    if (has_instance_ubo_) {
      VkDescriptorSetLayoutBinding binding{};
      binding.binding = 0;
      binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      binding.descriptorCount = 1;
      binding.stageFlags =
          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
      bindings.push_back(binding);
    }
    if (instance_sampler_count_ > 0) {
      VkDescriptorSetLayoutBinding binding{};
      binding.binding = instance_sampler_binding_;
      binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      binding.descriptorCount = instance_sampler_count_;
      binding.stageFlags =
          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
      bindings.push_back(binding);
    }

    VkDescriptorSetLayoutCreateInfo instance_layout_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    instance_layout_info.bindingCount = static_cast<u32>(bindings.size());
    instance_layout_info.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                         &instance_layout_info,
                                         context_->allocator,
                                         &instance_set_layout_));
  }

  // Descriptor pool: sized for exactly 1 global set plus, if the shader
  // uses instances, kMaxShaderInstances instance sets -- all allocated
  // once below, up front, rather than allocated/freed per acquire/release
  // (see class comment: instance slots are reused, never individually
  // freed back to the pool).
  std::vector<VkDescriptorPoolSize> pool_sizes;
  u32 ubo_count = 1 + (has_instance_ubo_ ? kMaxShaderInstances : 0);
  pool_sizes.push_back({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, ubo_count});
  if (instance_sampler_count_ > 0) {
    pool_sizes.push_back({VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                          instance_sampler_count_ * kMaxShaderInstances});
  }

  u32 max_sets = 1 + (use_instance_ ? kMaxShaderInstances : 0);

  VkDescriptorPoolCreateInfo pool_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.poolSizeCount = static_cast<u32>(pool_sizes.size());
  pool_info.pPoolSizes = pool_sizes.data();
  pool_info.maxSets = max_sets;
  VK_CHECK(vkCreateDescriptorPool(context_->device.logical_device, &pool_info,
                                  context_->allocator, &descriptor_pool_));

  // Global set: allocated and written once here. Its UBO binding's
  // contents are refreshed every frame via set_uniform()/load_data(), but
  // the binding itself is never rewritten.
  VkDescriptorSetAllocateInfo global_alloc{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  global_alloc.descriptorPool = descriptor_pool_;
  global_alloc.descriptorSetCount = 1;
  global_alloc.pSetLayouts = &global_set_layout_;
  VK_CHECK(vkAllocateDescriptorSets(context_->device.logical_device,
                                    &global_alloc, &global_set_));

  VkDescriptorBufferInfo global_buffer_info{uniform_buffer_->handle(), 0,
                                            global_ubo_stride_};
  VkWriteDescriptorSet global_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  global_write.dstSet = global_set_;
  global_write.dstBinding = 0;
  global_write.descriptorCount = 1;
  global_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  global_write.pBufferInfo = &global_buffer_info;
  vkUpdateDescriptorSets(context_->device.logical_device, 1, &global_write, 0,
                        nullptr);

  // Instance sets: all kMaxShaderInstances allocated up front. Each slot's
  // UBO binding (if any) is written now, pointing at that slot's fixed
  // offset in the shared buffer -- it never needs rewriting again, since
  // the offset per slot index never changes. Sampler bindings are left
  // unwritten here; they're written lazily by apply_instance() the first
  // time a real texture is set for that slot (see set_sampler()).
  if (use_instance_) {
    std::vector<VkDescriptorSetLayout> layouts(kMaxShaderInstances,
                                               instance_set_layout_);
    std::array<VkDescriptorSet, kMaxShaderInstances> sets{};
    VkDescriptorSetAllocateInfo alloc{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = descriptor_pool_;
    alloc.descriptorSetCount = kMaxShaderInstances;
    alloc.pSetLayouts = layouts.data();
    VK_CHECK(vkAllocateDescriptorSets(context_->device.logical_device,
                                      &alloc, sets.data()));

    for (u32 i = 0; i < kMaxShaderInstances; ++i) {
      instances_[i].set = sets[i];
      if (has_instance_ubo_) {
        VkDescriptorBufferInfo buffer_info{
            uniform_buffer_->handle(),
            global_ubo_stride_ + static_cast<u64>(i) * instance_ubo_stride_,
            instance_ubo_stride_};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = instances_[i].set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &buffer_info;
        vkUpdateDescriptorSets(context_->device.logical_device, 1, &write, 0,
                              nullptr);
      }
    }

    free_instance_slots_.reserve(kMaxShaderInstances);
    for (u32 i = kMaxShaderInstances; i-- > 0;) {
      free_instance_slots_.push_back(i);
    }
  }

  std::vector<VkDescriptorSetLayout> pipeline_layouts{global_set_layout_};
  if (use_instance_) {
    pipeline_layouts.push_back(instance_set_layout_);
  }

  pipeline_.emplace(*context_, *config.renderpass, vertex_stage_,
                    fragment_stage_, vertex_stride, vk_attributes,
                    pipeline_layouts, push_constant_size_,
                    config.depth_test_enabled);
  if (!pipeline_->is_valid()) {
    KERROR("VulkanShader '{}': failed to create graphics pipeline.", name_);
    return;
  }

  valid_ = true;
}

VulkanShader::~VulkanShader() {
  pipeline_.reset();

  if (descriptor_pool_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(context_->device.logical_device, descriptor_pool_,
                            context_->allocator);
    descriptor_pool_ = VK_NULL_HANDLE;
  }
  if (instance_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 instance_set_layout_, context_->allocator);
    instance_set_layout_ = VK_NULL_HANDLE;
  }
  if (global_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 global_set_layout_, context_->allocator);
    global_set_layout_ = VK_NULL_HANDLE;
  }

  uniform_buffer_.reset();
}

VulkanShader::UniformIndex
VulkanShader::uniform_index(std::string_view name) const {
  auto it = uniform_lookup_.find(std::string(name));
  if (it == uniform_lookup_.end()) {
    return kInvalidUniformIndex;
  }
  return it->second;
}

void VulkanShader::use(VulkanCommandBuffer &command_buffer) {
  if (!valid_) {
    KWARN("VulkanShader '{}': use() called on an invalid shader.", name_);
    return;
  }
  pipeline_->bind(command_buffer);
}

void VulkanShader::set_uniform(UniformIndex index, const void *value) {
  if (index >= uniforms_.size()) {
    KERROR("VulkanShader '{}': set_uniform called with invalid index {}.",
          name_, index);
    return;
  }
  const UniformMeta &meta = uniforms_[index];
  if (meta.scope != ShaderUniformScope::Global ||
      meta.type == ShaderUniformType::Sampler) {
    KERROR("VulkanShader '{}': set_uniform requires a Global-scope, "
           "non-sampler uniform ('{}').",
           name_, meta.name);
    return;
  }
  uniform_buffer_->load_data(meta.offset, meta.size, 0, value);
}

void VulkanShader::bind_globals() {
  // Intentionally empty -- see class/header comment.
}

void VulkanShader::apply_globals(VulkanCommandBuffer &command_buffer) {
  VkCommandBuffer cmd = command_buffer.handle();
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                         pipeline_->layout(), 0, 1, &global_set_, 0, nullptr);
}

u32 VulkanShader::acquire_instance_resources() {
  if (free_instance_slots_.empty()) {
    KERROR("VulkanShader '{}': no free instance slots (max {}).", name_,
          kMaxShaderInstances);
    return kInvalidInstanceId;
  }
  u32 id = free_instance_slots_.back();
  free_instance_slots_.pop_back();
  return id;
}

void VulkanShader::release_instance_resources(u32 instance_id) {
  if (instance_id >= kMaxShaderInstances) {
    KERROR("VulkanShader '{}': release_instance_resources called with "
           "invalid id {}.",
           name_, instance_id);
    return;
  }
  instances_[instance_id].bound_textures.fill(nullptr);
  instances_[instance_id].desired_textures.fill(nullptr);
  if (bound_instance_id_ == instance_id) {
    bound_instance_id_ = kInvalidInstanceId;
  }
  free_instance_slots_.push_back(instance_id);
}

void VulkanShader::bind_instance(u32 instance_id) {
  bound_instance_id_ = instance_id;
}

void VulkanShader::set_instance_uniform(UniformIndex index,
                                        const void *value) {
  if (bound_instance_id_ == kInvalidInstanceId) {
    KERROR("VulkanShader '{}': set_instance_uniform called with no "
           "instance bound.",
           name_);
    return;
  }
  if (index >= uniforms_.size()) {
    KERROR("VulkanShader '{}': set_instance_uniform called with invalid "
           "index {}.",
           name_, index);
    return;
  }
  const UniformMeta &meta = uniforms_[index];
  if (meta.scope != ShaderUniformScope::Instance ||
      meta.type == ShaderUniformType::Sampler) {
    KERROR("VulkanShader '{}': set_instance_uniform requires an "
           "Instance-scope, non-sampler uniform ('{}').",
           name_, meta.name);
    return;
  }
  u64 offset = global_ubo_stride_ +
              static_cast<u64>(bound_instance_id_) * instance_ubo_stride_ +
              meta.offset;
  uniform_buffer_->load_data(offset, meta.size, 0, value);
}

void VulkanShader::set_sampler(UniformIndex index, VulkanTexture &texture) {
  if (bound_instance_id_ == kInvalidInstanceId) {
    KERROR("VulkanShader '{}': set_sampler called with no instance bound.",
          name_);
    return;
  }
  if (index >= uniforms_.size()) {
    KERROR("VulkanShader '{}': set_sampler called with invalid index {}.",
          name_, index);
    return;
  }
  const UniformMeta &meta = uniforms_[index];
  if (meta.scope != ShaderUniformScope::Instance ||
      meta.type != ShaderUniformType::Sampler) {
    KERROR("VulkanShader '{}': set_sampler requires an Instance-scope "
           "sampler uniform ('{}').",
           name_, meta.name);
    return;
  }
  instances_[bound_instance_id_].desired_textures[meta.sampler_slot] =
      &texture;
}

void VulkanShader::apply_instance(VulkanCommandBuffer &command_buffer) {
  if (bound_instance_id_ == kInvalidInstanceId) {
    KERROR("VulkanShader '{}': apply_instance called with no instance "
           "bound.",
           name_);
    return;
  }
  InstanceState &state = instances_[bound_instance_id_];

  std::array<VkDescriptorImageInfo, kMaxInstanceSamplers> image_infos{};
  std::array<VkWriteDescriptorSet, kMaxInstanceSamplers> writes{};
  u32 write_count = 0;
  for (u32 slot = 0; slot < instance_sampler_count_; ++slot) {
    if (state.desired_textures[slot] != nullptr &&
        state.desired_textures[slot] != state.bound_textures[slot]) {
      VulkanTexture *texture = state.desired_textures[slot];
      image_infos[write_count].imageLayout =
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      image_infos[write_count].imageView = texture->view();
      image_infos[write_count].sampler = texture->sampler();

      writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[write_count].dstSet = state.set;
      writes[write_count].dstBinding = instance_sampler_binding_;
      writes[write_count].dstArrayElement = slot;
      writes[write_count].descriptorCount = 1;
      writes[write_count].descriptorType =
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[write_count].pImageInfo = &image_infos[write_count];
      ++write_count;

      state.bound_textures[slot] = texture;
    }
  }
  if (write_count > 0) {
    vkUpdateDescriptorSets(context_->device.logical_device, write_count,
                          writes.data(), 0, nullptr);
  }

  VkCommandBuffer cmd = command_buffer.handle();
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                         pipeline_->layout(), 1, 1, &state.set, 0, nullptr);
}

void VulkanShader::push_local(VulkanCommandBuffer &command_buffer,
                              const void *data, u32 size) {
  VkCommandBuffer cmd = command_buffer.handle();
  vkCmdPushConstants(cmd, pipeline_->layout(),
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, size, data);
}

VkPipelineLayout VulkanShader::pipeline_layout() const noexcept {
  return pipeline_ ? pipeline_->layout() : VK_NULL_HANDLE;
}
