#include "vulkan_texture.h"
#include "../../core/logger.h"
#include "vulkan_buffer.h"
#include "vulkan_commandbuffer.h"
#include "vulkan_image.h"

#include <utility>

namespace {

void transition_layout(VulkanContext &context, VulkanCommandBuffer &cmd,
                       VkImage image, VkImageLayout old_layout,
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

  vkCmdPipelineBarrier(cmd.handle(), src_stage, dst_stage, 0, 0, nullptr, 0,
                      nullptr, 1, &barrier);
  (void)context;
}

} // namespace

VulkanTexture::VulkanTexture(VulkanContext &context, u32 width, u32 height,
                            u32 channel_count, const std::vector<u8> &pixels)
    : context_(&context) {
  // Expand to RGBA if the source data isn't already 4 channels -- the image
  // itself is always created as R8G8B8A8_UNORM.
  std::vector<u8> rgba;
  const u8 *upload_data = pixels.data();
  if (channel_count != 4) {
    rgba.resize(static_cast<size_t>(width) * height * 4);
    for (u32 i = 0; i < width * height; ++i) {
      for (u32 c = 0; c < 4; ++c) {
        rgba[i * 4 + c] = c < channel_count
                             ? pixels[i * channel_count + c]
                             : (c == 3 ? 255 : rgba[i * 4]);
      }
    }
    upload_data = rgba.data();
  }
  const VkDeviceSize image_size = static_cast<VkDeviceSize>(width) * height * 4;
  constexpr VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;

  VulkanBuffer staging(*context_, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!staging.is_valid()) {
    KERROR("Failed to create staging buffer for texture upload.");
    return;
  }
  staging.load_data(0, image_size, 0, upload_data);

  vulkan_image_create(context_, VK_IMAGE_TYPE_2D, width, height, format,
                      VK_IMAGE_TILING_OPTIMAL,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, TRUE,
                      VK_IMAGE_ASPECT_COLOR_BIT, &image_);

  auto cmd = VulkanCommandBuffer::allocate_and_begin_single_use(
      *context_, context_->device.graphics_command_pool);

  transition_layout(*context_, *cmd, image_.handle, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);

  VkBufferImageCopy region{};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = {width, height, 1};
  vkCmdCopyBufferToImage(cmd->handle(), staging.handle(), image_.handle,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  transition_layout(*context_, *cmd, image_.handle,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  // end_single_use submits and waits (vkQueueWaitIdle), so the staging
  // buffer below is safe to destroy immediately after -- the upload is
  // guaranteed complete by the time this returns.
  VulkanCommandBuffer::end_single_use(*context_,
                                      context_->device.graphics_command_pool,
                                      std::move(cmd),
                                      context_->device.graphics_queue);

  VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.anisotropyEnable = VK_TRUE;
  sampler_info.maxAnisotropy = 16;
  sampler_info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
  sampler_info.unnormalizedCoordinates = VK_FALSE;
  sampler_info.compareEnable = VK_FALSE;
  sampler_info.compareOp = VK_COMPARE_OP_ALWAYS;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sampler_info.mipLodBias = 0.0f;
  sampler_info.minLod = 0.0f;
  sampler_info.maxLod = 0.0f;

  VkResult result = vkCreateSampler(context_->device.logical_device,
                                   &sampler_info, context_->allocator,
                                   &sampler_);
  if (result != VK_SUCCESS) {
    KERROR("Error creating texture sampler.");
  }
}

VulkanTexture::~VulkanTexture() {
  if (sampler_ != VK_NULL_HANDLE) {
    vkDestroySampler(context_->device.logical_device, sampler_,
                    context_->allocator);
    sampler_ = VK_NULL_HANDLE;
  }
  vulkan_image_destroy(context_, &image_);
}

std::vector<u8> generate_checkerboard_pixels(u32 size, u32 tile_size) {
  std::vector<u8> pixels(static_cast<size_t>(size) * size * 4, 255);
  for (u32 y = 0; y < size; ++y) {
    for (u32 x = 0; x < size; ++x) {
      bool dark = ((x / tile_size) + (y / tile_size)) % 2 == 0;
      if (dark) {
        size_t i = (static_cast<size_t>(y) * size + x) * 4;
        pixels[i + 0] = 40;
        pixels[i + 1] = 40;
        pixels[i + 2] = 40;
        // alpha stays 255
      }
    }
  }
  return pixels;
}
