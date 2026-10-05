#include "vulkan_pipeline_cache.h"
#include "../../core/logger.h"
#include "vulkan_utils.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Empty means "no cache this run" -- see vulkan_pipeline_cache_create().
std::string pipeline_cache_path() {
  if (std::getenv("KENGINE_NO_PIPELINE_CACHE")) {
    return {};
  }
  if (const char *explicit_path = std::getenv("KENGINE_PIPELINE_CACHE")) {
    return explicit_path;
  }
  std::filesystem::path dir;
  if (const char *xdg = std::getenv("XDG_CACHE_HOME")) {
    dir = std::filesystem::path(xdg) / "kengine";
  } else if (const char *home = std::getenv("HOME")) {
    dir = std::filesystem::path(home) / ".cache" / "kengine";
  } else {
    return {}; // nowhere sensible to put it -- run without one
  }
  return (dir / "pipeline_cache.bin").string();
}

// The 32-byte header every VkPipelineCache blob starts with (Vulkan spec,
// "Pipeline Cache Header"): length, version, vendorID, deviceID, then a
// 16-byte UUID. Feeding a blob from a different device or driver build to
// vkCreatePipelineCache is allowed (the driver must ignore it), but drivers
// have historically been fragile about it, and checking is four
// comparisons.
bool header_matches_device(const std::vector<char> &blob,
                          const VkPhysicalDeviceProperties &properties) {
  constexpr size_t kHeaderSize = 32;
  if (blob.size() < kHeaderSize) {
    return false;
  }
  u32 fields[4];
  std::memcpy(fields, blob.data(), sizeof(fields));
  if (fields[0] != kHeaderSize ||
      fields[1] != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
      fields[2] != properties.vendorID || fields[3] != properties.deviceID) {
    return false;
  }
  return std::memcmp(blob.data() + 16, properties.pipelineCacheUUID,
                    VK_UUID_SIZE) == 0;
}

} // namespace

void vulkan_pipeline_cache_create(VulkanContext &context) {
  context.pipeline_cache_path = pipeline_cache_path();

  std::vector<char> blob;
  if (!context.pipeline_cache_path.empty()) {
    std::ifstream file(context.pipeline_cache_path,
                      std::ios::binary | std::ios::ate);
    if (file) {
      const std::streamsize size = file.tellg();
      if (size > 0) {
        blob.resize(static_cast<size_t>(size));
        file.seekg(0);
        file.read(blob.data(), size);
      }
    }
    if (!blob.empty() && !header_matches_device(blob, context.device.properties)) {
      // Overwhelmingly this means a driver update, which invalidates every
      // compiled binary in it. Say so, because the next start is the slow
      // one and that is otherwise mystifying.
      KINFO("Pipeline cache at '{}' is for a different device/driver -- "
           "discarding it; this start recompiles pipelines from scratch.",
           context.pipeline_cache_path);
      blob.clear();
    }
  }

  VkPipelineCacheCreateInfo create_info{
      VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  create_info.initialDataSize = blob.size();
  create_info.pInitialData = blob.empty() ? nullptr : blob.data();

  VkResult result =
      vkCreatePipelineCache(context.device.logical_device, &create_info,
                           context.allocator, &context.pipeline_cache);
  if (!vulkan_result_is_success(result)) {
    // Not fatal: every pipeline creation takes VK_NULL_HANDLE happily, it
    // just compiles everything every time.
    KERROR("vkCreatePipelineCache failed with {} -- pipelines will compile "
          "from scratch every run.",
          vulkan_result_string(result, TRUE));
    context.pipeline_cache = VK_NULL_HANDLE;
    return;
  }

  if (context.pipeline_cache_path.empty()) {
    KINFO("Pipeline cache in memory only (KENGINE_NO_PIPELINE_CACHE) -- "
         "pipelines recompile from scratch every run.");
  } else if (blob.empty()) {
    KINFO("Pipeline cache '{}' is empty or missing -- this start compiles "
         "pipelines from scratch (minutes); later starts reuse them.",
         context.pipeline_cache_path);
  } else {
    KINFO("Pipeline cache loaded from '{}' ({} KB).",
         context.pipeline_cache_path, blob.size() / 1024);
  }
}

void vulkan_pipeline_cache_destroy(VulkanContext &context) {
  if (context.pipeline_cache == VK_NULL_HANDLE) {
    return;
  }

  if (!context.pipeline_cache_path.empty()) {
    size_t size = 0;
    VkResult result = vkGetPipelineCacheData(context.device.logical_device,
                                            context.pipeline_cache, &size,
                                            nullptr);
    std::vector<char> blob(size);
    if (vulkan_result_is_success(result) && size > 0) {
      result = vkGetPipelineCacheData(context.device.logical_device,
                                     context.pipeline_cache, &size, blob.data());
    }
    if (!vulkan_result_is_success(result) || size == 0) {
      KWARN("Could not read pipeline cache data back ({}) -- not saving.",
           vulkan_result_string(result, TRUE));
    } else {
      std::filesystem::path path(context.pipeline_cache_path);
      std::error_code ec;
      if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
      }
      // Written to a sibling temp file and renamed, so a crash (or a second
      // instance shutting down at the same moment) can never leave a
      // half-written blob where a whole one is expected. Rename within a
      // directory is atomic on every filesystem this runs on.
      std::filesystem::path temp = path;
      temp += ".tmp";
      {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (file) {
          file.write(blob.data(), static_cast<std::streamsize>(size));
        }
      }
      std::filesystem::rename(temp, path, ec);
      if (ec) {
        KWARN("Failed to save pipeline cache to '{}': {}",
             context.pipeline_cache_path, ec.message());
        std::filesystem::remove(temp, ec);
      } else {
        KINFO("Pipeline cache saved to '{}' ({} KB).",
             context.pipeline_cache_path, size / 1024);
      }
    }
  }

  vkDestroyPipelineCache(context.device.logical_device, context.pipeline_cache,
                        context.allocator);
  context.pipeline_cache = VK_NULL_HANDLE;
}
