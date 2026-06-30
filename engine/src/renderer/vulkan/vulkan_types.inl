#pragma once
#include "../../core/asserts.h"
#include "../../defines.h"

#include <memory>
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

  VkCommandPool graphics_command_pool;

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

class VulkanRenderpass;

class VulkanCommandBuffer;

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
  std::unique_ptr<VulkanRenderpass> main_renderpass;

  std::vector<std::unique_ptr<VulkanCommandBuffer>> graphics_command_buffers;

  // VkSemaphore is an opaque handle (not a class), so vector<VkSemaphore>
  // has no incomplete-type issue and can live here.
  std::vector<VkSemaphore> image_available_semaphores;
  std::vector<VkSemaphore> queue_complete_semaphores;

  u32 image_index;
  u32 current_frame;

  b8 recreating_swapchain;

  i32 (*find_memory_index)(VulkanContext &context, u32 type_filter,
                           u32 property_flags);

  ~VulkanContext();
};
