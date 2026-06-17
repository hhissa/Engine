#pragma once

#include "vulkan_types.inl"

void vulkan_image_create(VulkanContext *context, VkImageType image_type,
                         u32 width, u32 height, VkFormat format,
                         VkImageTiling tiling, VkImageUsageFlags usage,
                         VkMemoryPropertyFlags memory_flags, b32 create_view,
                         VkImageAspectFlags view_aspect_flags,
                         VulkanImage *out_image);

void vulkan_image_view_create(VulkanContext *context, VkFormat format,
                              VulkanImage *image,
                              VkImageAspectFlags aspect_flags);

void vulkan_image_destroy(VulkanContext *context, VulkanImage *image);
