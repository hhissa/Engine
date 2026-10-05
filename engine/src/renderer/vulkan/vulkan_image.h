#pragma once

#include "vulkan_types.inl"

// Creates a VkImage + its backing VkDeviceMemory (+ optionally a view),
// reporting failure instead of aborting. Returns false -- with out_image
// left fully cleaned up and zeroed, so is_valid()-style checks on it read
// false and vulkan_image_destroy() on it is a no-op -- when the driver
// cannot satisfy the request.
//
// The failure that matters in practice is VK_ERROR_OUT_OF_DEVICE_MEMORY:
// the field's brick pools already reserve well over a gigabyte of
// device-local memory (see kMaxBricks/kMaxChunkBricks), so on a 4GB-class
// card an ordinary 2048x2048 scene texture is genuinely able to be the
// allocation that doesn't fit. That is a recoverable condition -- the
// caller can fall back to a placeholder texture -- so it must not be an
// assertion. Mirrors VulkanBuffer, which has always reported allocation
// failure this way rather than asserting.
bool vulkan_image_try_create(VulkanContext *context, VkImageType image_type,
                             u32 width, u32 height, VkFormat format,
                             VkImageTiling tiling, VkImageUsageFlags usage,
                             VkMemoryPropertyFlags memory_flags, b32 create_view,
                             VkImageAspectFlags view_aspect_flags,
                             VulkanImage *out_image);

// vulkan_image_try_create() for callers with no fallback to fall back to --
// the swapchain's depth buffer and the raymarcher's own render targets,
// none of which the renderer can draw a frame without. Asserts on failure.
void vulkan_image_create(VulkanContext *context, VkImageType image_type,
                         u32 width, u32 height, VkFormat format,
                         VkImageTiling tiling, VkImageUsageFlags usage,
                         VkMemoryPropertyFlags memory_flags, b32 create_view,
                         VkImageAspectFlags view_aspect_flags,
                         VulkanImage *out_image);

bool vulkan_image_view_create(VulkanContext *context, VkFormat format,
                              VulkanImage *image,
                              VkImageAspectFlags aspect_flags);

void vulkan_image_destroy(VulkanContext *context, VulkanImage *image);
