#include "vulkan_compute_pipeline.h"
#include "../../core/logger.h"
#include "vulkan_commandbuffer.h"
#include "vulkan_utils.h"

VulkanComputePipeline::VulkanComputePipeline(
    VulkanContext &context, const VulkanShaderModule &compute_stage,
    const std::vector<VkDescriptorSetLayout> &descriptor_set_layouts,
    u32 push_constant_size)
    : context_(&context) {
  VkPipelineLayoutCreateInfo layout_create_info{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_create_info.setLayoutCount =
      static_cast<u32>(descriptor_set_layouts.size());
  layout_create_info.pSetLayouts = descriptor_set_layouts.data();

  VkPushConstantRange push_constant_range{};
  if (push_constant_size > 0) {
    push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_constant_range.offset = 0;
    push_constant_range.size = push_constant_size;
    layout_create_info.pushConstantRangeCount = 1;
    layout_create_info.pPushConstantRanges = &push_constant_range;
  }

  VK_CHECK(vkCreatePipelineLayout(context_->device.logical_device,
                                  &layout_create_info, context_->allocator,
                                  &layout_));

  VkComputePipelineCreateInfo pipeline_create_info{
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline_create_info.stage = compute_stage.stage_create_info();
  pipeline_create_info.layout = layout_;
  pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_create_info.basePipelineIndex = -1;

  VkResult result =
      vkCreateComputePipelines(context_->device.logical_device, VK_NULL_HANDLE,
                               1, &pipeline_create_info, context_->allocator,
                               &handle_);

  if (vulkan_result_is_success(result)) {
    KDEBUG("Compute pipeline created!");
  } else {
    KERROR("vkCreateComputePipelines failed with {}.",
          vulkan_result_string(result, TRUE));
  }
}

VulkanComputePipeline::~VulkanComputePipeline() {
  if (handle_ != VK_NULL_HANDLE) {
    vkDestroyPipeline(context_->device.logical_device, handle_,
                      context_->allocator);
    handle_ = VK_NULL_HANDLE;
  }
  if (layout_ != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(context_->device.logical_device, layout_,
                            context_->allocator);
    layout_ = VK_NULL_HANDLE;
  }
}

VulkanComputePipeline::VulkanComputePipeline(
    VulkanComputePipeline &&other) noexcept
    : context_(other.context_), handle_(other.handle_),
      layout_(other.layout_) {
  other.handle_ = VK_NULL_HANDLE;
  other.layout_ = VK_NULL_HANDLE;
}

VulkanComputePipeline &
VulkanComputePipeline::operator=(VulkanComputePipeline &&other) noexcept {
  if (this != &other) {
    if (handle_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(context_->device.logical_device, handle_,
                        context_->allocator);
    }
    if (layout_ != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(context_->device.logical_device, layout_,
                              context_->allocator);
    }
    context_ = other.context_;
    handle_ = other.handle_;
    layout_ = other.layout_;

    other.handle_ = VK_NULL_HANDLE;
    other.layout_ = VK_NULL_HANDLE;
  }
  return *this;
}

void VulkanComputePipeline::bind(VulkanCommandBuffer &command_buffer) {
  vkCmdBindPipeline(command_buffer.handle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                    handle_);
}
