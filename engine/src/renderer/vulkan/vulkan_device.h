#pragma once
#include "vulkan_types.inl"

b8 vulkan_device_create(VulkanContext &context);
void vulkan_device_destroy(VulkanContext &context);

// Returns FALSE if the surface can no longer be queried (e.g.
// VK_ERROR_SURFACE_LOST_KHR) — callers must treat that as "no swapchain
// support right now" rather than a fatal error.
b8 vulkan_device_query_swapchain_support(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface,
    VulkanSwapchainSupportInfo &out_support_info);

b8 vulkan_device_detect_depth_format(VulkanDevice *device);
