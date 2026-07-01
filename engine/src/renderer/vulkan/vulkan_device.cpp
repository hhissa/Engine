#include "vulkan_device.h"
#include "../../core/logger.h"

#include <array>
#include <cstring>
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
  KINFO("Creating logical device...");

  b8 present_shares_graphics_queue =
      context.device.graphics_queue_index == context.device.present_queue_index;
  b8 transfer_shares_graphics_queue = context.device.graphics_queue_index ==
                                      context.device.transfer_queue_index;

  std::array<u32, 3> indices{};
  u32 index_count = 0;
  indices[index_count++] = context.device.graphics_queue_index;
  if (!present_shares_graphics_queue) {
    indices[index_count++] = context.device.present_queue_index;
  }
  if (!transfer_shares_graphics_queue) {
    indices[index_count++] = context.device.transfer_queue_index;
  }

  // Sized to match `indices` (max 3 distinct queue families) rather than a
  // variable-length array on `index_count`, which clang only accepts as a
  // non-standard C++ extension.
  std::array<VkDeviceQueueCreateInfo, 3> queue_create_infos{};
  for (u32 i = 0; i < index_count; ++i) {
    queue_create_infos[i].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_create_infos[i].queueFamilyIndex = indices[i];
    queue_create_infos[i].queueCount = 1;
    if (indices[i] == context.device.graphics_queue_index) {
      queue_create_infos[i].queueCount = 2;
    }
    queue_create_infos[i].flags = 0;
    queue_create_infos[i].pNext = 0;
    f32 queue_priority = 1.0f;
    queue_create_infos[i].pQueuePriorities = &queue_priority;
  }

  // TODO: should be config driven
  VkPhysicalDeviceFeatures device_features = {};
  device_features.samplerAnisotropy = VK_TRUE; // Request anistrophy
  VkDeviceCreateInfo device_create_info = {
      VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  device_create_info.queueCreateInfoCount = index_count;
  device_create_info.pQueueCreateInfos = queue_create_infos.data();
  device_create_info.pEnabledFeatures = &device_features;
  device_create_info.enabledExtensionCount = 1;
  const char *extension_names = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
  device_create_info.ppEnabledExtensionNames = &extension_names;

  // Deprecated and ignored, so pass nothing.
  device_create_info.enabledLayerCount = 0;
  device_create_info.ppEnabledLayerNames = 0;

  // Create the device.
  VK_CHECK(vkCreateDevice(context.device.physical_device, &device_create_info,
                          context.allocator, &context.device.logical_device));

  KINFO("Logical device created.");

  // Get queues.
  vkGetDeviceQueue(context.device.logical_device,
                   context.device.graphics_queue_index, 0,
                   &context.device.graphics_queue);

  vkGetDeviceQueue(context.device.logical_device,
                   context.device.present_queue_index, 0,
                   &context.device.present_queue);

  vkGetDeviceQueue(context.device.logical_device,
                   context.device.transfer_queue_index, 0,
                   &context.device.transfer_queue);

  VkCommandPoolCreateInfo pool_create_info{
      VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_create_info.queueFamilyIndex = context.device.graphics_queue_index;
  pool_create_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VK_CHECK(vkCreateCommandPool(context.device.logical_device, &pool_create_info,
                               context.allocator,
                               &context.device.graphics_command_pool));
  KINFO("Graphics command pool created.");
  KINFO("Queues obtained.");
  return TRUE;
}

void vulkan_device_destroy(VulkanContext &context) {
  context.device.graphics_queue = 0;
  context.device.present_queue = 0;
  context.device.transfer_queue = 0;

  // Command pools are owned by the device, so they must be destroyed while
  // the logical device handle is still valid.
  KINFO("Destroying command pools...");
  vkDestroyCommandPool(context.device.logical_device,
                       context.device.graphics_command_pool, context.allocator);

  KINFO("Destroying logical device...");
  if (context.device.logical_device) {
    vkDestroyDevice(context.device.logical_device, context.allocator);
    context.device.logical_device = 0;
  }
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

b8 vulkan_device_detect_depth_format(VulkanDevice *device) {
  // Format candidates
  const u64 candidate_count = 3;
  VkFormat candidates[3] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                            VK_FORMAT_D24_UNORM_S8_UINT};

  u32 flags = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
  for (u64 i = 0; i < candidate_count; ++i) {
    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(device->physical_device, candidates[i],
                                        &properties);

    if ((properties.linearTilingFeatures & flags) == flags) {
      device->depth_format = candidates[i];
      return TRUE;
    } else if ((properties.optimalTilingFeatures & flags) == flags) {
      device->depth_format = candidates[i];
      return TRUE;
    }
  }

  return FALSE;
}
