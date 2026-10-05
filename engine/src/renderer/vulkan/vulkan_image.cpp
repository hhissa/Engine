#include "vulkan_image.h"

#include "vulkan_device.h"
#include "vulkan_utils.h"

#include "../../core/asserts.h"
#include "../../core/hmemory.h"
#include "../../core/logger.h"

bool vulkan_image_try_create(VulkanContext *context, VkImageType image_type,
                             u32 width, u32 height, VkFormat format,
                             VkImageTiling tiling, VkImageUsageFlags usage,
                             VkMemoryPropertyFlags memory_flags, b32 create_view,
                             VkImageAspectFlags view_aspect_flags,
                             VulkanImage *out_image) {

  // Cleared before anything can fail, so that every early return below
  // leaves the caller holding an all-null image -- one that reads as
  // invalid and that vulkan_image_destroy() can be called on safely.
  *out_image = VulkanImage{};

  // Copy params
  out_image->width = width;
  out_image->height = height;

  // Creation info.
  VkImageCreateInfo image_create_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_create_info.imageType = VK_IMAGE_TYPE_2D;
  image_create_info.extent.width = width;
  image_create_info.extent.height = height;
  image_create_info.extent.depth = 1; // TODO: Support configurable depth.
  // 1, not a mip chain: nothing in this engine ever samples a mip. Every
  // view vulkan_image_view_create() builds is levelCount = 1, every
  // upload writes mip 0 only (VulkanTexture's single vkCmdCopyBufferToImage),
  // and every sampler pins maxLod = 0 -- there is no downsample/blit step
  // anywhere that would fill levels 1+. This was 4, so every image in the
  // renderer, scene textures included, was paying an extra ~33% of its own
  // size for levels no shader could read (a 2048x2048 RGBA texture
  // allocated 21.3MB to use 16MB). Raise this again only alongside actual
  // mip generation, and note that mipLevels must stay
  // <= floor(log2(max(width, height))) + 1.
  image_create_info.mipLevels = 1;    // TODO: Support mip mapping
  image_create_info.arrayLayers =
      1; // TODO: Support number of layers in the image.
  image_create_info.format = format;
  image_create_info.tiling = tiling;
  image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  image_create_info.usage = usage;
  image_create_info.samples =
      VK_SAMPLE_COUNT_1_BIT; // TODO: Configurable sample count.
  image_create_info.sharingMode =
      VK_SHARING_MODE_EXCLUSIVE; // TODO: Configurable sharing mode.

  VkResult result = vkCreateImage(context->device.logical_device,
                                  &image_create_info, context->allocator,
                                  &out_image->handle);
  if (result != VK_SUCCESS) {
    KERROR("Failed to create a {}x{} image: {}.", width, height,
           vulkan_result_string(result, TRUE));
    out_image->handle = 0;
    return false;
  }

  // Query memory requirements.
  VkMemoryRequirements memory_requirements;
  vkGetImageMemoryRequirements(context->device.logical_device,
                               out_image->handle, &memory_requirements);

  i32 memory_type = context->find_memory_index(
      *context, memory_requirements.memoryTypeBits, memory_flags);
  if (memory_type == -1) {
    KERROR("Required memory type not found. Image not valid.");
    vulkan_image_destroy(context, out_image);
    return false;
  }

  // Allocate memory
  VkMemoryAllocateInfo memory_allocate_info = {
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  memory_allocate_info.allocationSize = memory_requirements.size;
  memory_allocate_info.memoryTypeIndex = memory_type;
  result =
      vkAllocateMemory(context->device.logical_device, &memory_allocate_info,
                       context->allocator, &out_image->memory);
  if (result != VK_SUCCESS) {
    KERROR("Failed to allocate {} bytes of device memory (type {}) for a "
           "{}x{} image: {}.",
           static_cast<u64>(memory_requirements.size), memory_type, width,
           height, vulkan_result_string(result, TRUE));
    out_image->memory = 0;
    vulkan_image_destroy(context, out_image);
    return false;
  }

  // Bind the memory
  result =
      vkBindImageMemory(context->device.logical_device, out_image->handle,
                        out_image->memory, 0); // TODO: configurable offset.
  if (result != VK_SUCCESS) {
    KERROR("Failed to bind image memory for a {}x{} image: {}.", width, height,
           vulkan_result_string(result, TRUE));
    vulkan_image_destroy(context, out_image);
    return false;
  }

  // Create view
  if (create_view) {
    out_image->view = 0;
    if (!vulkan_image_view_create(context, format, out_image,
                                  view_aspect_flags)) {
      vulkan_image_destroy(context, out_image);
      return false;
    }
  }
  return true;
}

void vulkan_image_create(VulkanContext *context, VkImageType image_type,
                         u32 width, u32 height, VkFormat format,
                         VkImageTiling tiling, VkImageUsageFlags usage,
                         VkMemoryPropertyFlags memory_flags, b32 create_view,
                         VkImageAspectFlags view_aspect_flags,
                         VulkanImage *out_image) {
  // Called unconditionally and only its RESULT asserted on: KASSERT compiles
  // to nothing when KASSERTIONS_ENABLED is off, so the call itself must not
  // live inside the macro.
  const bool created = vulkan_image_try_create(
      context, image_type, width, height, format, tiling, usage, memory_flags,
      create_view, view_aspect_flags, out_image);
  KASSERT_MSG(created, "vulkan_image_create failed for an image the renderer "
                       "has no fallback for; see the error above.");
}

bool vulkan_image_view_create(VulkanContext *context, VkFormat format,
                              VulkanImage *image,
                              VkImageAspectFlags aspect_flags) {
  VkImageViewCreateInfo view_create_info = {
      VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_create_info.image = image->handle;
  view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D; // TODO: Make configurable.
  view_create_info.format = format;
  view_create_info.subresourceRange.aspectMask = aspect_flags;

  // TODO: Make configurable
  view_create_info.subresourceRange.baseMipLevel = 0;
  view_create_info.subresourceRange.levelCount = 1;
  view_create_info.subresourceRange.baseArrayLayer = 0;
  view_create_info.subresourceRange.layerCount = 1;

  VkResult result = vkCreateImageView(context->device.logical_device,
                                      &view_create_info, context->allocator,
                                      &image->view);
  if (result != VK_SUCCESS) {
    KERROR("Failed to create an image view: {}.",
           vulkan_result_string(result, TRUE));
    image->view = 0;
    return false;
  }
  return true;
}

void vulkan_image_destroy(VulkanContext *context, VulkanImage *image) {
  if (image->view) {
    vkDestroyImageView(context->device.logical_device, image->view,
                       context->allocator);
    image->view = 0;
  }
  if (image->memory) {
    vkFreeMemory(context->device.logical_device, image->memory,
                 context->allocator);
    image->memory = 0;
  }
  if (image->handle) {
    vkDestroyImage(context->device.logical_device, image->handle,
                   context->allocator);
    image->handle = 0;
  }
}
