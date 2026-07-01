#include "vulkan_raymarch_shader.h"
#include "../../../core/logger.h"
#include "../../../resources/image_loader.h"
#include "../vulkan_commandbuffer.h"
#include "../vulkan_image.h"

#include <cmath>
#include <utility>
#include <vector>

namespace {
constexpr std::string_view BUILTIN_SHADER_NAME_VOXELIZE =
    "Builtin.RaymarchVoxelize";
constexpr std::string_view BUILTIN_SHADER_NAME_RAYMARCH =
    "Builtin.RaymarchShader";

// Matches the `push_constant` block in Builtin.RaymarchShader.comp.glsl.
// std430 (push constants use the same rules as std430, not std140) still
// aligns vec3 to 16 bytes, so both fields are declared as vec4 to make the
// padding explicit instead of implicit. Recorded directly into the command
// buffer each frame (vkCmdPushConstants) rather than a UBO: there's no
// separate buffer memory a still-in-flight previous frame could be reading
// while this frame's value is written, so no per-swapchain-image buffering
// is needed the way a UBO would require.
struct PushConstants {
  f32 camera_position[4]; // xyz + unused pad
  f32 camera_forward[4];  // xyz + unused pad
  f32 camera_right[4];    // xyz + unused pad
  f32 camera_up[4];       // xyz + unused pad
  f32 model_offset[4];    // xyz + unused pad -- see render_to() for why this
                         // is a translation rather than a full model matrix.
};

// The sphere orbits in a circle instead of spinning in place: a pure
// rotation of a sphere about its own center is invisible (it's rotationally
// symmetric), so it wouldn't demonstrate the "move the object without
// re-baking its geometry" mechanism upstream's model matrix shows off with
// its spinning quad. An orbiting translation does the same demonstration
// visibly for this shape.
constexpr f32 kModelOrbitSpeed = 0.6f;
constexpr f32 kModelOrbitRadius = 0.6f;

// Sparse voxel field dimensions. Must match COARSE_DIM/BRICK_DIM/MAX_BRICKS
// in both Builtin.RaymarchVoxelize.comp.glsl and
// Builtin.RaymarchShader.comp.glsl exactly.
constexpr u32 kCoarseDim = 16;
constexpr u32 kBrickDim = 8;
// Each brick stores a 1-voxel apron on every side (evaluated directly from
// the SDF, not copied from a neighbor) so trilinear sampling has continuous
// data to blend into right at brick boundaries — without this, adjacent
// bricks' data disagreed at the seam and produced visible facets.
constexpr u32 kBrickApronDim = kBrickDim + 2;
constexpr u32 kBrickVoxelCount = kBrickApronDim * kBrickApronDim * kBrickApronDim;
constexpr u32 kMaxBricks = 2048;
} // namespace

VulkanRaymarchShader::VulkanRaymarchShader(VulkanContext &context)
    : context_(&context),
      voxelize_stage_(context, BUILTIN_SHADER_NAME_VOXELIZE, "comp",
                     VK_SHADER_STAGE_COMPUTE_BIT),
      render_stage_(context, BUILTIN_SHADER_NAME_RAYMARCH, "comp",
                   VK_SHADER_STAGE_COMPUTE_BIT) {
  if (!voxelize_stage_.is_valid() || !render_stage_.is_valid()) {
    KERROR("Unable to create shader module(s) for the raymarch field "
          "pipeline.");
    return;
  }

  // Output storage image that pass 2 writes into.
  vulkan_image_create(context_, VK_IMAGE_TYPE_2D, context_->framebuffer_width,
                      context_->framebuffer_height,
                      context_->swapchain.image_format.format,
                      VK_IMAGE_TILING_OPTIMAL,
                      VK_IMAGE_USAGE_STORAGE_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, TRUE,
                      VK_IMAGE_ASPECT_COLOR_BIT, &output_image_);

  // Sparse voxel field storage.
  const u64 indirection_size =
      static_cast<u64>(kCoarseDim) * kCoarseDim * kCoarseDim * sizeof(i32);
  const u64 brick_pool_size =
      static_cast<u64>(kMaxBricks) * kBrickVoxelCount * sizeof(f32);

  indirection_buffer_.emplace(*context_, indirection_size,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  brick_pool_buffer_.emplace(*context_, brick_pool_size,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  brick_counter_buffer_.emplace(*context_, sizeof(u32),
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  if (!indirection_buffer_->is_valid() || !brick_pool_buffer_->is_valid() ||
      !brick_counter_buffer_->is_valid()) {
    KERROR("Failed to create sparse voxel field buffers.");
    return;
  }

  // Surface texture, triplanar-mapped onto the sphere in the render shader.
  // Prefer a real loaded image; fall back to the procedural checkerboard
  // if the asset is missing (e.g. post-build.sh hasn't copied assets/ into
  // bin/ yet) so the engine still runs rather than failing outright.
  if (auto loaded = load_image("assets/textures/cobblestone.png")) {
    surface_texture_.emplace(*context_, loaded->width, loaded->height,
                             loaded->channel_count, loaded->pixels);
  } else {
    KWARN("Falling back to a procedural checkerboard for the surface "
         "texture.");
    constexpr u32 kCheckerboardSize = 256;
    constexpr u32 kCheckerboardTile = 32;
    surface_texture_.emplace(
        *context_, kCheckerboardSize, kCheckerboardSize, 4,
        generate_checkerboard_pixels(kCheckerboardSize, kCheckerboardTile));
  }
  if (!surface_texture_->is_valid()) {
    KERROR("Failed to create surface texture.");
    return;
  }

  // Descriptor set layout for pass 1 (voxelize): writes indirection, brick
  // pool, and the allocation counter.
  VkDescriptorSetLayoutBinding voxelize_bindings[3]{};
  for (u32 i = 0; i < 3; ++i) {
    voxelize_bindings[i].binding = i;
    voxelize_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    voxelize_bindings[i].descriptorCount = 1;
    voxelize_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }

  VkDescriptorSetLayoutCreateInfo voxelize_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  voxelize_layout_info.bindingCount = 3;
  voxelize_layout_info.pBindings = voxelize_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &voxelize_layout_info,
                                       context_->allocator,
                                       &voxelize_set_layout_));

  // Descriptor set layout for pass 2 (render): the output image, read-only
  // access to indirection and the brick pool, and the surface texture
  // sampler. The camera/model state that used to be a UBO binding here is
  // now a push constant instead (see PushConstants above) — no separate
  // binding needed, and no per-swapchain-image duplication either.
  VkDescriptorSetLayoutBinding render_bindings[4]{};
  render_bindings[0].binding = 0;
  render_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  render_bindings[0].descriptorCount = 1;
  render_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  render_bindings[1].binding = 1;
  render_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  render_bindings[1].descriptorCount = 1;
  render_bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  render_bindings[2].binding = 2;
  render_bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  render_bindings[2].descriptorCount = 1;
  render_bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  render_bindings[3].binding = 3;
  render_bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  render_bindings[3].descriptorCount = 1;
  render_bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutCreateInfo render_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  render_layout_info.bindingCount = 4;
  render_layout_info.pBindings = render_bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context_->device.logical_device,
                                       &render_layout_info, context_->allocator,
                                       &render_set_layout_));

  // One pool backing both sets.
  VkDescriptorPoolSize pool_sizes[3]{};
  pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  pool_sizes[0].descriptorCount = 5; // voxelize's 3 + render's 2
  pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  pool_sizes[1].descriptorCount = 1;
  pool_sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  pool_sizes[2].descriptorCount = 1;

  VkDescriptorPoolCreateInfo pool_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.poolSizeCount = 3;
  pool_info.pPoolSizes = pool_sizes;
  pool_info.maxSets = 2;
  VK_CHECK(vkCreateDescriptorPool(context_->device.logical_device, &pool_info,
                                  context_->allocator, &descriptor_pool_));

  VkDescriptorSetLayout layouts[2] = {voxelize_set_layout_, render_set_layout_};
  VkDescriptorSet sets[2];
  VkDescriptorSetAllocateInfo alloc_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc_info.descriptorPool = descriptor_pool_;
  alloc_info.descriptorSetCount = 2;
  alloc_info.pSetLayouts = layouts;
  VK_CHECK(vkAllocateDescriptorSets(context_->device.logical_device,
                                    &alloc_info, sets));
  voxelize_set_ = sets[0];
  render_set_ = sets[1];

  // Populate voxelize_set_: {indirection, brick_pool, brick_counter}.
  VkDescriptorBufferInfo voxelize_buffer_infos[3] = {
      {indirection_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {brick_pool_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {brick_counter_buffer_->handle(), 0, VK_WHOLE_SIZE},
  };

  VkWriteDescriptorSet voxelize_writes[3]{};
  for (u32 i = 0; i < 3; ++i) {
    voxelize_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    voxelize_writes[i].dstSet = voxelize_set_;
    voxelize_writes[i].dstBinding = i;
    voxelize_writes[i].descriptorCount = 1;
    voxelize_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    voxelize_writes[i].pBufferInfo = &voxelize_buffer_infos[i];
  }
  vkUpdateDescriptorSets(context_->device.logical_device, 3, voxelize_writes,
                        0, nullptr);

  // Populate render_set_: {out_image, indirection, brick_pool, surface_tex}.
  VkDescriptorImageInfo render_image_info{};
  render_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  render_image_info.imageView = output_image_.view;

  VkDescriptorBufferInfo render_buffer_infos[2] = {
      {indirection_buffer_->handle(), 0, VK_WHOLE_SIZE},
      {brick_pool_buffer_->handle(), 0, VK_WHOLE_SIZE},
  };

  VkDescriptorImageInfo surface_tex_info{};
  surface_tex_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  surface_tex_info.imageView = surface_texture_->view();
  surface_tex_info.sampler = surface_texture_->sampler();

  VkWriteDescriptorSet render_writes[4]{};
  render_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  render_writes[0].dstSet = render_set_;
  render_writes[0].dstBinding = 0;
  render_writes[0].descriptorCount = 1;
  render_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  render_writes[0].pImageInfo = &render_image_info;

  render_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  render_writes[1].dstSet = render_set_;
  render_writes[1].dstBinding = 1;
  render_writes[1].descriptorCount = 1;
  render_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  render_writes[1].pBufferInfo = &render_buffer_infos[0];

  render_writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  render_writes[2].dstSet = render_set_;
  render_writes[2].dstBinding = 2;
  render_writes[2].descriptorCount = 1;
  render_writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  render_writes[2].pBufferInfo = &render_buffer_infos[1];

  render_writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  render_writes[3].dstSet = render_set_;
  render_writes[3].dstBinding = 3;
  render_writes[3].descriptorCount = 1;
  render_writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  render_writes[3].pImageInfo = &surface_tex_info;

  vkUpdateDescriptorSets(context_->device.logical_device, 4, render_writes, 0,
                        nullptr);

  voxelize_pipeline_.emplace(
      *context_, voxelize_stage_,
      std::vector<VkDescriptorSetLayout>{voxelize_set_layout_});
  render_pipeline_.emplace(*context_, render_stage_,
                           std::vector<VkDescriptorSetLayout>{render_set_layout_},
                           sizeof(PushConstants));

  if (!voxelize_pipeline_->is_valid() || !render_pipeline_->is_valid()) {
    KERROR("Failed to create compute pipeline(s) for the raymarch field.");
    return;
  }

  valid_ = true;

  // Bake the sparse voxel field once, now that everything above exists.
  voxelize();
}

VulkanRaymarchShader::~VulkanRaymarchShader() {
  // Destroy pipelines before the descriptor set layouts they were created
  // with.
  render_pipeline_.reset();
  voxelize_pipeline_.reset();

  if (descriptor_pool_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(context_->device.logical_device, descriptor_pool_,
                            context_->allocator);
    descriptor_pool_ = VK_NULL_HANDLE;
  }
  if (render_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 render_set_layout_, context_->allocator);
    render_set_layout_ = VK_NULL_HANDLE;
  }
  if (voxelize_set_layout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(context_->device.logical_device,
                                 voxelize_set_layout_, context_->allocator);
    voxelize_set_layout_ = VK_NULL_HANDLE;
  }

  surface_texture_.reset();

  brick_counter_buffer_.reset();
  brick_pool_buffer_.reset();
  indirection_buffer_.reset();

  vulkan_image_destroy(context_, &output_image_);
}

void VulkanRaymarchShader::on_resized(u32 width, u32 height) {
  if (!valid_) {
    return;
  }

  vulkan_image_destroy(context_, &output_image_);
  vulkan_image_create(context_, VK_IMAGE_TYPE_2D, width, height,
                      context_->swapchain.image_format.format,
                      VK_IMAGE_TILING_OPTIMAL,
                      VK_IMAGE_USAGE_STORAGE_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, TRUE,
                      VK_IMAGE_ASPECT_COLOR_BIT, &output_image_);

  // Re-point the render descriptor set's storage-image binding at the new
  // image view -- the old view no longer exists (vulkan_image_destroy just
  // destroyed it), so the descriptor set would otherwise reference a
  // dangling handle.
  VkDescriptorImageInfo image_info{};
  image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  image_info.imageView = output_image_.view;

  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = render_set_;
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  write.pImageInfo = &image_info;

  vkUpdateDescriptorSets(context_->device.logical_device, 1, &write, 0,
                        nullptr);
}

void VulkanRaymarchShader::voxelize() {
  auto cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
      *context_, context_->device.graphics_command_pool);

  // Zero the brick allocation counter before the shader's atomicAdd calls.
  vkCmdFillBuffer(cmd->handle(), brick_counter_buffer_->handle(), 0,
                 sizeof(u32), 0);

  VkBufferMemoryBarrier fill_barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  fill_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  fill_barrier.dstAccessMask =
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  fill_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  fill_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  fill_barrier.buffer = brick_counter_buffer_->handle();
  fill_barrier.offset = 0;
  fill_barrier.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(cmd->handle(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                      &fill_barrier, 0, nullptr);

  voxelize_pipeline_->bind(*cmd);
  vkCmdBindDescriptorSets(cmd->handle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                         voxelize_pipeline_->layout(), 0, 1, &voxelize_set_, 0,
                         nullptr);

  constexpr u32 local_size = 4; // must match local_size_x/y/z in the shader
  u32 groups = (kCoarseDim + local_size - 1) / local_size;
  vkCmdDispatch(cmd->handle(), groups, groups, groups);

  // end_single_use submits and calls vkQueueWaitIdle, so once this returns
  // the field is fully baked and visible — no extra barrier needed for a
  // dependency that's about to be enforced by a full queue idle anyway.
  VulkanCommandBuffer::end_single_use(*context_,
                                      context_->device.graphics_command_pool,
                                      std::move(cmd),
                                      context_->device.graphics_queue);
}

void VulkanRaymarchShader::transition_image(
    VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
    VkImageLayout new_layout, VkAccessFlags src_access,
    VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
    VkPipelineStageFlags dst_stage) {
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;

  vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1,
                       &barrier);
}

namespace {
void write_vec3(f32 (&dest)[4], glm::vec3 v) {
  dest[0] = v.x;
  dest[1] = v.y;
  dest[2] = v.z;
  dest[3] = 0.0f;
}
} // namespace

void VulkanRaymarchShader::render_to(VulkanCommandBuffer &command_buffer,
                                    VkImage swapchain_image, u32 width,
                                    u32 height, f32 delta_time,
                                    const Camera &camera) {
  if (!valid_) {
    KWARN("VulkanRaymarchShader::render_to called on an invalid shader.");
    return;
  }

  time_seconds_ += delta_time;

  PushConstants push_constants{};
  write_vec3(push_constants.camera_position, camera.position());
  write_vec3(push_constants.camera_forward, camera.forward());
  write_vec3(push_constants.camera_right, camera.right());
  write_vec3(push_constants.camera_up, camera.up());

  // Orbit the object in the XZ plane. The shader transforms the ray into
  // object-local space (subtracting model_offset) before marching the
  // already-baked field, so this moves the visible sphere without touching
  // the field's contents at all.
  push_constants.model_offset[0] =
      std::cos(time_seconds_ * kModelOrbitSpeed) * kModelOrbitRadius;
  push_constants.model_offset[1] = 0.0f;
  push_constants.model_offset[2] =
      std::sin(time_seconds_ * kModelOrbitSpeed) * kModelOrbitRadius;
  push_constants.model_offset[3] = 0.0f;

  VkCommandBuffer cmd = command_buffer.handle();

  // Storage image -> GENERAL for the compute shader to write into. Old
  // layout is claimed UNDEFINED (discard) since every pixel gets
  // overwritten unconditionally each frame; src stage/access still wait on
  // any in-flight transfer read from the previous frame's copy below.
  transition_image(cmd, output_image_.handle, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  render_pipeline_->bind(command_buffer);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                         render_pipeline_->layout(), 0, 1, &render_set_, 0,
                         nullptr);
  vkCmdPushConstants(cmd, render_pipeline_->layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants),
                    &push_constants);

  constexpr u32 local_size = 16; // must match local_size_x/y in the shader
  u32 group_x = (width + local_size - 1) / local_size;
  u32 group_y = (height + local_size - 1) / local_size;
  vkCmdDispatch(cmd, group_x, group_y, 1);

  // Storage image -> transfer source.
  transition_image(cmd, output_image_.handle, VK_IMAGE_LAYOUT_GENERAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);

  // Swapchain image -> transfer destination. The render pass that ran
  // earlier this frame wrote it via COLOR_ATTACHMENT_OUTPUT; wait for that
  // before overwriting. Old layout is claimed UNDEFINED since the copy
  // below overwrites every pixel, so the swapchain image's prior contents
  // (the render pass's clear) don't need preserving.
  transition_image(cmd, swapchain_image, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);

  VkImageCopy region{};
  region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.srcSubresource.layerCount = 1;
  region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.dstSubresource.layerCount = 1;
  region.extent = {width, height, 1};

  vkCmdCopyImage(cmd, output_image_.handle,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchain_image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  // Swapchain image -> present source, required before vkQueuePresentKHR.
  transition_image(cmd, swapchain_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                   VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}
