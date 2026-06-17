#pragma once
#include "../../core/asserts.h"
#include "../../defines.h"

#include <vector>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>

#define VK_CHECK(expr)                                                         \
  {                                                                            \
    KASSERT((expr) == VK_SUCCESS);                                             \
  }

struct VulkanSwapchainSupportInfo {
  VkSurfaceCapabilitiesKHR capabilities{};
  std::vector<VkSurfaceFormatKHR> formats;
  std::vector<VkPresentModeKHR> present_modes;
};

struct VulkanDevice {
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice logical_device = VK_NULL_HANDLE;
  VulkanSwapchainSupportInfo swapchain_support;

  i32 graphics_queue_index = -1;
  i32 present_queue_index = -1;
  i32 transfer_queue_index = -1;

  VkQueue graphics_queue;
  VkQueue present_queue;
  VkQueue transfer_queue;

  VkPhysicalDeviceProperties properties{};
  VkPhysicalDeviceFeatures features{};
  VkPhysicalDeviceMemoryProperties memory{};

  VkFormat depth_format;
};

struct VulkanImage {
  VkImage handle;
  VkDeviceMemory memory;
  VkImageView view;
  u32 width;
  u32 height;
};

struct VulkanSwapchain {
  VkSurfaceFormatKHR image_format;
  u8 max_frames_in_flight;
  VkSwapchainKHR handle;
  u32 image_count;
  VkImage *images;
  VkImageView *views;

  VulkanImage depth_attachment;
};

struct VulkanContext {
  VkInstance instance = VK_NULL_HANDLE;
  VkAllocationCallbacks *allocator = nullptr;
  VkSurfaceKHR surface = VK_NULL_HANDLE;

  u32 framebuffer_width;
  u32 framebuffer_height;

#if defined(_DEBUG)
  VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
#endif

  VulkanDevice device;

  VulkanSwapchain swapchain;
  u32 image_index;
  u32 current_frame;

  b8 recreating_swapchain;

  i32 (*find_memory_index)(VulkanContext context, u32 type_filter,
                           u32 property_flags);
};
