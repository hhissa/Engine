#include "vulkan_shader_module.h"
#include "../../core/logger.h"
#include "../../platform/filesystem.h"

#include <format>

VulkanShaderModule::VulkanShaderModule(VulkanContext &context,
                                       std::string_view name,
                                       std::string_view type_str,
                                       VkShaderStageFlagBits stage)
    : context_(&context) {
  std::string file_name = std::format("assets/shaders/{}.{}.spv", name, type_str);

  FileHandle handle(file_name, FileMode::Read, /*binary=*/true);
  if (!handle) {
    KERROR("Unable to read shader module: {}.", file_name);
    return;
  }

  auto bytes = handle.read_all_bytes();
  if (!bytes) {
    KERROR("Unable to binary read shader module: {}.", file_name);
    return;
  }

  VkShaderModuleCreateInfo create_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  create_info.codeSize = bytes->size();
  create_info.pCode = reinterpret_cast<const u32 *>(bytes->data());

  VK_CHECK(vkCreateShaderModule(context_->device.logical_device, &create_info,
                                context_->allocator, &handle_));

  stage_create_info_.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stage_create_info_.stage = stage;
  stage_create_info_.module = handle_;
  stage_create_info_.pName = "main";
}

VulkanShaderModule::~VulkanShaderModule() {
  if (handle_ != VK_NULL_HANDLE) {
    vkDestroyShaderModule(context_->device.logical_device, handle_,
                          context_->allocator);
    handle_ = VK_NULL_HANDLE;
  }
}

VulkanShaderModule::VulkanShaderModule(VulkanShaderModule &&other) noexcept
    : context_(other.context_), handle_(other.handle_),
      stage_create_info_(other.stage_create_info_) {
  other.handle_ = VK_NULL_HANDLE;
}

VulkanShaderModule &
VulkanShaderModule::operator=(VulkanShaderModule &&other) noexcept {
  if (this != &other) {
    if (handle_ != VK_NULL_HANDLE) {
      vkDestroyShaderModule(context_->device.logical_device, handle_,
                            context_->allocator);
    }
    context_ = other.context_;
    handle_ = other.handle_;
    stage_create_info_ = other.stage_create_info_;

    other.handle_ = VK_NULL_HANDLE;
  }
  return *this;
}
