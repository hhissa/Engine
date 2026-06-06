#include "vulkan_device.h"
#include "../../core/logger.h"

#include <cstring>
#include <string>
#include <vector>

struct VulkanPhysicalDeviceRequirements {
  bool graphics = true;
  bool present = true;
  bool compute = true;
  bool transfer = true;
  std::vector<const char *> device_extension_names;
  bool sampler_anisotropy = true;
  bool discrete_gpu = true;
};

struct VulkanPhysicalDeviceQueueFamilyInfo {
  u32 graphics_family_index = static_cast<u32>(-1);
  u32 present_family_index = static_cast<u32>(-1);
  u32 compute_family_index = static_cast<u32>(-1);
  u32 transfer_family_index = static_cast<u32>(-1);
};

static b8 physical_device_meets_requirements(
    VkPhysicalDevice device, VkSurfaceKHR surface,
    const VkPhysicalDeviceProperties &properties,
    const VkPhysicalDeviceFeatures &features,
    const VulkanPhysicalDeviceRequirements &requirements,
    VulkanPhysicalDeviceQueueFamilyInfo &out_queue_info,
    VulkanSwapchainSupportInfo &out_swapchain_support);

static b8 select_physical_device(VulkanContext &context) {
  u32 physical_device_count = 0;
  VK_CHECK(vkEnumeratePhysicalDevices(context.instance, &physical_device_count,
                                      nullptr));
  if (physical_device_count == 0) {
    KFATAL("No devices which support Vulkan were found.");
    return FALSE;
  }

  std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
  VK_CHECK(vkEnumeratePhysicalDevices(context.instance, &physical_device_count,
                                      physical_devices.data()));

  for (VkPhysicalDevice device : physical_devices) {
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(device, &properties);

    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(device, &features);

    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(device, &memory);

    VulkanPhysicalDeviceRequirements requirements;
    requirements.discrete_gpu = true;
    requirements.device_extension_names.push_back(
        VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    VulkanPhysicalDeviceQueueFamilyInfo queue_info;
    b8 result = physical_device_meets_requirements(
        device, context.surface, properties, features, requirements, queue_info,
        context.device.swapchain_support);

    if (result) {
      KINFO("Selected device: '{}'.", properties.deviceName);
      switch (properties.deviceType) {
      default:
      case VK_PHYSICAL_DEVICE_TYPE_OTHER:
        KINFO("GPU type is Unknown.");
        break;
      case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        KINFO("GPU type is Integrated.");
        break;
      case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        KINFO("GPU type is Discrete.");
        break;
      case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        KINFO("GPU type is Virtual.");
        break;
      case VK_PHYSICAL_DEVICE_TYPE_CPU:
        KINFO("GPU type is CPU.");
        break;
      }

      context.device.physical_device = device;
      context.device.graphics_queue_index =
          static_cast<i32>(queue_info.graphics_family_index);
      context.device.present_queue_index =
          static_cast<i32>(queue_info.present_family_index);
      context.device.transfer_queue_index =
          static_cast<i32>(queue_info.transfer_family_index);
      context.device.properties = properties;
      context.device.features = features;
      context.device.memory = memory;
      break;
    }
  }

  if (context.device.physical_device == VK_NULL_HANDLE) {
    KERROR("No physical devices were found which meet the requirements.");
    return FALSE;
  }

  KINFO("Physical device selected.");
  return TRUE;
}

b8 vulkan_device_create(VulkanContext &context) {
  if (!select_physical_device(context)) {
    return FALSE;
  }
  return TRUE;
}

void vulkan_device_destroy(VulkanContext &context) {
  context.device.physical_device = VK_NULL_HANDLE;
}

void vulkan_device_query_swapchain_support(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface,
    VulkanSwapchainSupportInfo &out_support_info) {

  VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
      physical_device, surface, &out_support_info.capabilities));

  u32 format_count = 0;
  VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface,
                                                &format_count, nullptr));
  if (format_count != 0) {
    out_support_info.formats.resize(format_count);
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(
        physical_device, surface, &format_count,
        out_support_info.formats.data()));
  }

  u32 present_mode_count = 0;
  VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(
      physical_device, surface, &present_mode_count, nullptr));
  if (present_mode_count != 0) {
    out_support_info.present_modes.resize(present_mode_count);
    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(
        physical_device, surface, &present_mode_count,
        out_support_info.present_modes.data()));
  }
}

static b8 physical_device_meets_requirements(
    VkPhysicalDevice device, VkSurfaceKHR surface,
    const VkPhysicalDeviceProperties &properties,
    const VkPhysicalDeviceFeatures &features,
    const VulkanPhysicalDeviceRequirements &requirements,
    VulkanPhysicalDeviceQueueFamilyInfo &out_queue_info,
    VulkanSwapchainSupportInfo &out_swapchain_support) {

  if (requirements.discrete_gpu &&
      properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
    KINFO("Device is not a discrete GPU, and one is required. Skipping.");
    return FALSE;
  }

  u32 queue_family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count,
                                           nullptr);
  std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count,
                                           queue_families.data());

  u8 min_transfer_score = 255;
  for (u32 i = 0; i < queue_family_count; ++i) {
    u8 current_transfer_score = 0;

    if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      out_queue_info.graphics_family_index = i;
      ++current_transfer_score;
    }
    if (queue_families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
      out_queue_info.compute_family_index = i;
      ++current_transfer_score;
    }
    if (queue_families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) {
      if (current_transfer_score <= min_transfer_score) {
        min_transfer_score = current_transfer_score;
        out_queue_info.transfer_family_index = i;
      }
    }

    VkBool32 supports_present = VK_FALSE;
    VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface,
                                                  &supports_present));
    if (supports_present) {
      out_queue_info.present_family_index = i;
    }
  }

  if ((!requirements.graphics ||
       out_queue_info.graphics_family_index != static_cast<u32>(-1)) &&
      (!requirements.present ||
       out_queue_info.present_family_index != static_cast<u32>(-1)) &&
      (!requirements.compute ||
       out_queue_info.compute_family_index != static_cast<u32>(-1)) &&
      (!requirements.transfer ||
       out_queue_info.transfer_family_index != static_cast<u32>(-1))) {

    vulkan_device_query_swapchain_support(device, surface,
                                          out_swapchain_support);

    if (out_swapchain_support.formats.empty() ||
        out_swapchain_support.present_modes.empty()) {
      KINFO("Required swapchain support not present, skipping device.");
      return FALSE;
    }

    // Device extension check.
    u32 available_extension_count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr,
                                         &available_extension_count, nullptr);
    if (available_extension_count != 0) {
      std::vector<VkExtensionProperties> available(available_extension_count);
      vkEnumerateDeviceExtensionProperties(
          device, nullptr, &available_extension_count, available.data());
      for (const char *required : requirements.device_extension_names) {
        bool found = false;
        for (const auto &ext : available) {
          if (std::strcmp(required, ext.extensionName) == 0) {
            found = true;
            break;
          }
        }
        if (!found) {
          KINFO("Required device extension not found: '{}', skipping.",
                required);
          return FALSE;
        }
      }
    }

    if (requirements.sampler_anisotropy && !features.samplerAnisotropy) {
      KINFO("Device does not support samplerAnisotropy, skipping.");
      return FALSE;
    }

    return TRUE;
  }

  return FALSE;
}
