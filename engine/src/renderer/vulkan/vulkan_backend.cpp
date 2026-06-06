#include "vulkan_backend.h"
#include "../../core/logger.h"

#include "../../platform/platform.h"
#include "vulkan_device.h"
#include <vulkan/vulkan.h>

#include <string>
#include <string_view>
#include <vector>

static VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
    VkDebugUtilsMessageTypeFlagsEXT message_types,
    const VkDebugUtilsMessengerCallbackDataEXT *callback_data, void *user_data);

VulkanRendererBackend::VulkanRendererBackend(PlatformLayer &plat_state)
    : plat_state_(&plat_state) {}

b8 VulkanRendererBackend::initialize(std::string_view application_name,
                                     PlatformLayer &plat_state) {
  // TODO: custom allocator.
  context_.allocator = nullptr;

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
  return TRUE;
}

void VulkanRendererBackend::shutdown() {
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
