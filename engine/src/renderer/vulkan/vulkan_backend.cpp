#include "vulkan_backend.h"
#include "../../core/logger.h"

#include "../../platform/platform.h"
#include "vulkan_commandbuffer.h"
#include "vulkan_device.h"
#include "vulkan_renderpass.h"
#include "vulkan_swapchain.h"
#include <vulkan/vulkan.h>

#include <string_view>
#include <vector>

static VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
    VkDebugUtilsMessageTypeFlagsEXT message_types,
    const VkDebugUtilsMessengerCallbackDataEXT *callback_data, void *user_data);

i32 find_memory_index(VulkanContext &context, u32 type_filter,
                      u32 property_flags);

VulkanRendererBackend::VulkanRendererBackend(PlatformLayer &plat_state)
    : plat_state_(&plat_state) {}

VulkanRendererBackend::~VulkanRendererBackend() = default;

b8 VulkanRendererBackend::initialize(std::string_view application_name,
                                     PlatformLayer &plat_state) {
  context_.find_memory_index = find_memory_index;

  // TODO: custom allocator.
  context_.allocator = nullptr;

  application_get_framebuffer_size(&cached_framebuffer_width_,
                                   &cached_framebuffer_height_);
  context_.framebuffer_width =
      (cached_framebuffer_width_ != 0) ? cached_framebuffer_width_ : 800;
  context_.framebuffer_height =
      (cached_framebuffer_height_ != 0) ? cached_framebuffer_height_ : 600;
  cached_framebuffer_width_ = 0;
  cached_framebuffer_height_ = 0;

  VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app_info.apiVersion = VK_API_VERSION_1_2;
  app_info.pApplicationName = application_name.data();
  app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  app_info.pEngineName = "NOAI Engine";
  app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);

  VkInstanceCreateInfo create_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  create_info.pApplicationInfo = &app_info;

  // Required extensions.
  std::vector<const char *> required_extensions;
  required_extensions.push_back(
      VK_KHR_SURFACE_EXTENSION_NAME); // generic surface
  plat_state.get_required_extension_names(
      required_extensions); // platform-specific
#if defined(_DEBUG)
  required_extensions.push_back(
      VK_EXT_DEBUG_UTILS_EXTENSION_NAME); // debug utilities

  KDEBUG("Required extensions:");
  for (const char *ext : required_extensions) {
    KDEBUG("{}", ext);
  }
#endif

  create_info.enabledExtensionCount =
      static_cast<u32>(required_extensions.size());
  create_info.ppEnabledExtensionNames = required_extensions.data();

  // Validation layers.
  std::vector<const char *> required_validation_layers;

  // Only enable validation on non-release builds, after verifying availability.
#if defined(_DEBUG)
  KINFO("Validation layers enabled. Enumerating...");

  required_validation_layers.push_back("VK_LAYER_KHRONOS_validation");

  u32 available_layer_count = 0;
  VK_CHECK(vkEnumerateInstanceLayerProperties(&available_layer_count, nullptr));
  std::vector<VkLayerProperties> available_layers(available_layer_count);
  VK_CHECK(vkEnumerateInstanceLayerProperties(&available_layer_count,
                                              available_layers.data()));

  for (const char *required : required_validation_layers) {
    KINFO("Searching for layer: {}...", required);
    b8 found = FALSE;
    for (const auto &available : available_layers) {
      if (std::string_view(required) == available.layerName) {
        found = TRUE;
        KINFO("Found.");
        break;
      }
    }
    if (!found) {
      KFATAL("Required validation layer is missing: {}", required);
      return FALSE;
    }
  }
  KINFO("All required validation layers are present.");
#endif

  create_info.enabledLayerCount =
      static_cast<u32>(required_validation_layers.size());
  create_info.ppEnabledLayerNames = required_validation_layers.data();

  VK_CHECK(
      vkCreateInstance(&create_info, context_.allocator, &context_.instance));
  KINFO("Vulkan Instance created.");

  // Debugger.
#if defined(_DEBUG)
  KDEBUG("Creating Vulkan debugger...");
  u32 log_severity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
  //                 | VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;

  VkDebugUtilsMessengerCreateInfoEXT debug_create_info{
      VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  debug_create_info.messageSeverity = log_severity;
  debug_create_info.messageType =
      VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
  debug_create_info.pfnUserCallback = vk_debug_callback;

  auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
      vkGetInstanceProcAddr(context_.instance,
                            "vkCreateDebugUtilsMessengerEXT"));
  KASSERT_MSG(func, "Failed to create debug messenger!");
  VK_CHECK(func(context_.instance, &debug_create_info, context_.allocator,
                &context_.debug_messenger));
  KDEBUG("Vulkan debugger created.");
#endif
  KDEBUG("Creating Vulkan surface...");
  if (!plat_state_->create_vulkan_surface(context_)) {
    KERROR("Failed to create platform surface!");
    return FALSE;
  }
  KDEBUG("Vulkan surface created.");

  if (!vulkan_device_create(context_)) {
    KERROR("failed to create device");
    return FALSE;
  }

  KINFO("Vulkan renderer initialized successfully.");
  vulkan_swapchain_create(&context_, context_.framebuffer_width,
                          context_.framebuffer_height, &context_.swapchain);
  context_.main_renderpass = std::make_unique<VulkanRenderpass>(
      context_, 0.0f, 0.0f, static_cast<f32>(context_.framebuffer_width),
      static_cast<f32>(context_.framebuffer_height), 0.0f, 0.0f, 0.2f, 1.0f,
      1.0f, 0);
  regenerate_framebuffers();
  create_commandbuffer();

  // Sync objects — one semaphore pair and one fence per frame in flight.
  context_.image_available_semaphores.resize(
      context_.swapchain.max_frames_in_flight);
  context_.queue_complete_semaphores.resize(
      context_.swapchain.max_frames_in_flight);
  in_flight_fences_.reserve(context_.swapchain.max_frames_in_flight);

  for (u8 i = 0; i < context_.swapchain.max_frames_in_flight; ++i) {
    VkSemaphoreCreateInfo semaphore_create_info{
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(context_.device.logical_device, &semaphore_create_info,
                      context_.allocator,
                      &context_.image_available_semaphores[i]);
    vkCreateSemaphore(context_.device.logical_device, &semaphore_create_info,
                      context_.allocator,
                      &context_.queue_complete_semaphores[i]);

    // Created signaled so the first frame doesn't wait indefinitely for a
    // "previous" frame that never existed.
    in_flight_fences_.emplace_back(context_, TRUE);
  }

  // images_in_flight are non-owning pointers — null means the image is
  // not currently held by any in-flight frame.
  images_in_flight_.assign(context_.swapchain.image_count, nullptr);

  KINFO("Vulkan renderer initialized successfully.");
  return TRUE;
}

void VulkanRendererBackend::shutdown() {
  vkDeviceWaitIdle(context_.device.logical_device);

  // Sync objects — destroying VulkanFence runs vkDestroyFence automatically.
  for (u8 i = 0; i < context_.swapchain.max_frames_in_flight; ++i) {
    if (context_.image_available_semaphores[i]) {
      vkDestroySemaphore(context_.device.logical_device,
                         context_.image_available_semaphores[i],
                         context_.allocator);
    }
    if (context_.queue_complete_semaphores[i]) {
      vkDestroySemaphore(context_.device.logical_device,
                         context_.queue_complete_semaphores[i],
                         context_.allocator);
    }
  }
  context_.image_available_semaphores.clear();
  context_.queue_complete_semaphores.clear();
  in_flight_fences_.clear();
  images_in_flight_.clear();

  framebuffers_.clear(); // Destroy in reverse order of creation.
  context_.graphics_command_buffers.clear();
  context_.main_renderpass.reset();

  KDEBUG("Destroying Vulkan swapchain...");
  vulkan_swapchain_destroy(&context_, &context_.swapchain);

  KDEBUG("Destroying Vulkan device...");
  vulkan_device_destroy(context_);

  KDEBUG("Destroying Vulkan surface...");
  if (context_.surface) {
    vkDestroySurfaceKHR(context_.instance, context_.surface,
                        context_.allocator);
    context_.surface = nullptr;
  }

#if defined(_DEBUG)
  KDEBUG("Destroying Vulkan debugger...");
  if (context_.debug_messenger) {
    auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(context_.instance,
                              "vkDestroyDebugUtilsMessengerEXT"));
    func(context_.instance, context_.debug_messenger, context_.allocator);
  }
#endif

  KDEBUG("Destroying Vulkan instance...");
  vkDestroyInstance(context_.instance, context_.allocator);
}

void VulkanRendererBackend::on_resized(u16 width, u16 height) {}

b8 VulkanRendererBackend::begin_frame(f32 delta_time) { return TRUE; }

b8 VulkanRendererBackend::end_frame(f32 delta_time) { return TRUE; }

static VKAPI_ATTR VkBool32 VKAPI_CALL
vk_debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
                  VkDebugUtilsMessageTypeFlagsEXT message_types,
                  const VkDebugUtilsMessengerCallbackDataEXT *callback_data,
                  void *user_data) {
  switch (message_severity) {
  default:
  case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
    KERROR("{}", callback_data->pMessage);
    break;
  case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
    KWARN("{}", callback_data->pMessage);
    break;
  case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
    KINFO("{}", callback_data->pMessage);
    break;
  case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
    KTRACE("{}", callback_data->pMessage);
    break;
  }
  return VK_FALSE;
}

i32 find_memory_index(VulkanContext &context, u32 type_filter,
                      u32 property_flags) {
  VkPhysicalDeviceMemoryProperties memory_properties;
  vkGetPhysicalDeviceMemoryProperties(context.device.physical_device,
                                      &memory_properties);

  for (u32 i = 0; i < memory_properties.memoryTypeCount; ++i) {
    // Check each memory type to see if its bit is set to 1.
    if (type_filter & (1 << i) &&
        (memory_properties.memoryTypes[i].propertyFlags & property_flags) ==
            property_flags) {
      return i;
    }
  }

  KWARN("Unable to find suitable memory type!");
  return -1;
}

void VulkanRendererBackend::create_commandbuffer() {
  // A vector naturally replaces the darray's lazy-reserve check
  // (`if (!context.graphics_command_buffers)`): .empty() instead of a
  // null-pointer check, .resize() instead of darray_reserve.
  if (context_.graphics_command_buffers.empty()) {
    context_.graphics_command_buffers.resize(context_.swapchain.image_count);
  }

  // Rebuild for the current image count — same logic as the C version:
  // free anything already allocated, then allocate fresh. Resetting each
  // unique_ptr runs the old VulkanCommandBuffer's destructor (the free)
  // before the new one is constructed (the allocate), so this is the same
  // two-step free-then-allocate, just expressed through ownership transfer
  // instead of an explicit free() call followed by a separate allocate() call.
  for (u32 i = 0; i < context_.swapchain.image_count; ++i) {
    context_.graphics_command_buffers[i].reset();
    context_.graphics_command_buffers[i] =
        std::make_unique<VulkanCommandBuffer>(
            context_, context_.device.graphics_command_pool, TRUE);
  }

  KDEBUG("Vulkan command buffers created.");
}

void VulkanRendererBackend::regenerate_framebuffers() {
  framebuffers_.clear();
  framebuffers_.reserve(context_.swapchain.image_count);

  for (u32 i = 0; i < context_.swapchain.image_count; ++i) {
    // TODO: make attachment list dynamic based on configured attachments
    std::vector<VkImageView> attachments = {
        context_.swapchain.views[i], context_.swapchain.depth_attachment.view};

    framebuffers_.emplace_back(
        context_, *context_.main_renderpass, context_.framebuffer_width,
        context_.framebuffer_height, std::move(attachments));
  }
}
