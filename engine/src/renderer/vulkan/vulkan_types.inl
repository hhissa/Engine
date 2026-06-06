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

  VkPhysicalDeviceProperties properties{};
  VkPhysicalDeviceFeatures features{};
  VkPhysicalDeviceMemoryProperties memory{};
};

struct VulkanContext {
  VkInstance instance = VK_NULL_HANDLE;
  VkAllocationCallbacks *allocator = nullptr;
  VkSurfaceKHR surface = VK_NULL_HANDLE;

#if defined(_DEBUG)
  VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
#endif

  VulkanDevice device;
};
