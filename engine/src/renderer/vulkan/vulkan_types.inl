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
  // The family async_compute_queue below comes from. A family exposing
  // COMPUTE without GRAPHICS when the device has one -- that is a physically
  // separate hardware engine, and the only kind of queue that genuinely runs
  // alongside the graphics one. Falls back to graphics_queue_index on a
  // device that only exposes universal families, which is the behaviour this
  // engine had unconditionally before.
  i32 async_compute_queue_index = -1;

  VkQueue graphics_queue;
  VkQueue present_queue;
  VkQueue transfer_queue;
  // A SECOND queue instance from the SAME family as graphics_queue (queue
  // index 1, not a distinct queue family -- see vulkan_device_create()'s
  // queueCount=2 request, present since before this queue was ever
  // actually retrieved/used). Deliberately not a dedicated async-compute
  // family: most GPUs (this one included) only expose one "universal"
  // family with both GRAPHICS_BIT and COMPUTE_BIT set, so a genuinely
  // separate family isn't available to ask for. Staying within the same
  // family means buffers/images shared between graphics_queue and this
  // queue need no queue-family-ownership-transfer barriers (EXCLUSIVE
  // sharing is scoped to a family, not a specific VkQueue) -- only the
  // usual semaphore/fence ordering between the two submissions. Used by
  // VulkanRaymarchShader to submit chunk-streaming bake/evict work
  // independently of the main per-frame graphics submission -- see its
  // async_command_pool_'s own comment.
  VkQueue async_compute_queue;

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

class VulkanSolidQuadShader;

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
  //
  // image_available_semaphores is sized to max_frames_in_flight and indexed
  // by current_frame -- it's only ever waited on/signaled queue-side
  // (acquire signals it, the submit in end_frame() waits on it), and the
  // in-flight fences already serialize reuse of a given frame-in-flight
  // slot, so per-frame-in-flight sizing is sufficient.
  //
  // queue_complete_semaphores must instead be sized to swapchain.image_count
  // and indexed by image_index, NOT current_frame -- it's handed to
  // vkQueuePresentKHR, and the presentation engine's use of it isn't
  // retired by the in-flight fence (that only tracks GPU queue completion,
  // not presentation). image_count and max_frames_in_flight can differ
  // (image_count == max_frames_in_flight + 1, see
  // vulkan_swapchain.cpp's create()), so indexing this one by current_frame
  // let a later frame resignal the same semaphore while an earlier
  // present using a different image was still consuming it --
  // VUID-vkQueueSubmit-pSignalSemaphores-00067. See
  // VulkanRendererBackend::create_queue_complete_semaphores().
  std::vector<VkSemaphore> image_available_semaphores;
  std::vector<VkSemaphore> queue_complete_semaphores;

  // Monotonic timeline semaphore signalled by EVERY graphics submission,
  // with graphics_timeline_value as the value it was last signalled with.
  //
  // It exists so the async compute queue can order its chunk-bake writes
  // after the previous frame's graphics reads of the very same buffers
  // (chunk indirection, brick pool, cluster pool) without the CPU waiting
  // for anything. That hazard used to be handled by a plain
  // vkQueueWaitIdle(graphics_queue) before recording any bake -- correct,
  // but it drains the pipeline: on a frame with chunk work the CPU sits
  // until the GPU has finished the previous frame, and the GPU then sits
  // until the CPU has finished recording. In a small scene chunk work is
  // rare enough not to notice; in a large one, moving the camera means
  // chunk work EVERY frame, and the result is continuous stutter.
  VkSemaphore graphics_timeline = VK_NULL_HANDLE;
  u64 graphics_timeline_value = 0;

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
  std::unique_ptr<VulkanSolidQuadShader> solid_quad_shader;

  i32 (*find_memory_index)(VulkanContext &context, u32 type_filter,
                           u32 property_flags);

  ~VulkanContext();
};
