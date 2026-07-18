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

class VulkanRaymarchShader;

class TextureSystem;

class ShaderSystem;

class MaterialSystem;

class GeometrySystem;

class VulkanUIShader;

class VulkanTextShader;

class VulkanLineShader;

// VulkanComputePipeline, used by VulkanRaymarchShader, is a separate,
// already-implemented RAII class since compute and graphics pipelines are
// created/bound differently.
struct VulkanPipeline {
  VkPipeline handle = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
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
  u64 framebuffer_size_generation;
  u64 framebuffer_size_last_generation;

#if defined(_DEBUG)
  VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
#endif

  VulkanDevice device;

  VulkanSwapchain swapchain;
  std::unique_ptr<VulkanRenderpass> main_renderpass;
  // Runs after main_renderpass, drawing on top of it via LOAD_OP_LOAD (see
  // VulkanRenderpass's has_prev_pass) instead of clearing -- this is what
  // lets VulkanUIShader's quad appear over the raymarched scene rather
  // than replacing it. See VulkanRendererBackend::end_frame() for the
  // exact per-frame sequencing between the two.
  std::unique_ptr<VulkanRenderpass> ui_renderpass;

  std::vector<std::unique_ptr<VulkanCommandBuffer>> graphics_command_buffers;

  // VkSemaphore is an opaque handle (not a class), so vector<VkSemaphore>
  // has no incomplete-type issue and can live here.
  std::vector<VkSemaphore> image_available_semaphores;
  std::vector<VkSemaphore> queue_complete_semaphores;

  u32 image_index;
  u32 current_frame;

  b8 recreating_swapchain;
  // Set when acquire/present reports VK_ERROR_OUT_OF_DATE_KHR (or
  // suboptimal). The swapchain must NOT be recreated in place at that
  // point: the backend's framebuffers still reference the old image views
  // (VUID-vkDestroyImageView-imageView-01026). Instead this flag routes
  // the recreation through VulkanRendererBackend::recreate_swapchain(),
  // which tears framebuffers/command buffers down first.
  b8 swapchain_out_of_date = FALSE;

  std::unique_ptr<TextureSystem> texture_system;
  std::unique_ptr<ShaderSystem> shader_system;
  std::unique_ptr<MaterialSystem> material_system;
  std::unique_ptr<GeometrySystem> geometry_system;
  std::unique_ptr<VulkanRaymarchShader> raymarch_shader;
  std::unique_ptr<VulkanUIShader> ui_shader;
  std::unique_ptr<VulkanTextShader> text_shader;
  std::unique_ptr<VulkanLineShader> line_shader;

  i32 (*find_memory_index)(VulkanContext &context, u32 type_filter,
                           u32 property_flags);

  ~VulkanContext();
};
