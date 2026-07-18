#include "vulkan_graphics_pipeline.h"
#include "../../core/logger.h"
#include "vulkan_commandbuffer.h"
#include "vulkan_renderpass.h"
#include "vulkan_utils.h"

VulkanGraphicsPipeline::VulkanGraphicsPipeline(
    VulkanContext &context, VulkanRenderpass &renderpass,
    const VulkanShaderModule &vertex_stage,
    const VulkanShaderModule &fragment_stage, u32 vertex_stride,
    const std::vector<VertexAttribute> &vertex_attributes,
    const std::vector<VkDescriptorSetLayout> &descriptor_set_layouts,
    u32 push_constant_size, bool depth_test_enabled)
    : context_(&context) {
  VkPipelineLayoutCreateInfo layout_create_info{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_create_info.setLayoutCount =
      static_cast<u32>(descriptor_set_layouts.size());
  layout_create_info.pSetLayouts = descriptor_set_layouts.data();

  VkPushConstantRange push_constant_range{};
  if (push_constant_size > 0) {
    push_constant_range.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_constant_range.offset = 0;
    push_constant_range.size = push_constant_size;
    layout_create_info.pushConstantRangeCount = 1;
    layout_create_info.pPushConstantRanges = &push_constant_range;
  }

  VK_CHECK(vkCreatePipelineLayout(context_->device.logical_device,
                                  &layout_create_info, context_->allocator,
                                  &layout_));

  VkVertexInputBindingDescription binding_description{};
  binding_description.binding = 0;
  binding_description.stride = vertex_stride;
  binding_description.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

  std::vector<VkVertexInputAttributeDescription> attribute_descriptions(
      vertex_attributes.size());
  for (size_t i = 0; i < vertex_attributes.size(); ++i) {
    attribute_descriptions[i].location = static_cast<u32>(i);
    attribute_descriptions[i].binding = 0;
    attribute_descriptions[i].format = vertex_attributes[i].format;
    attribute_descriptions[i].offset = vertex_attributes[i].offset;
  }

  VkPipelineVertexInputStateCreateInfo vertex_input_info{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input_info.vertexBindingDescriptionCount = 1;
  vertex_input_info.pVertexBindingDescriptions = &binding_description;
  vertex_input_info.vertexAttributeDescriptionCount =
      static_cast<u32>(attribute_descriptions.size());
  vertex_input_info.pVertexAttributeDescriptions =
      attribute_descriptions.data();

  VkPipelineInputAssemblyStateCreateInfo input_assembly{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  input_assembly.primitiveRestartEnable = VK_FALSE;

  // Viewport/scissor are dynamic state (set per-frame in begin_frame()), so
  // only the counts matter here -- the actual VkViewport/VkRect2D values
  // are irrelevant at pipeline creation time.
  VkPipelineViewportStateCreateInfo viewport_state{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport_state.viewportCount = 1;
  viewport_state.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rasterizer{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rasterizer.depthClampEnable = VK_FALSE;
  rasterizer.rasterizerDiscardEnable = VK_FALSE;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.lineWidth = 1.0f;
  // No culling: 2D UI quads have no consistent winding relative to a
  // camera the way 3D geometry does, so there's no meaningful "back face"
  // to discard here.
  rasterizer.cullMode = VK_CULL_MODE_NONE;
  rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterizer.depthBiasEnable = VK_FALSE;

  VkPipelineMultisampleStateCreateInfo multisampling{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisampling.sampleShadingEnable = VK_FALSE;
  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  // Ignored by Vulkan when the renderpass's subpass has no depth/stencil
  // attachment (e.g. the UI renderpass) -- always constructing a valid,
  // consistent struct is simpler than conditionally omitting it.
  VkPipelineDepthStencilStateCreateInfo depth_stencil{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth_stencil.depthTestEnable = depth_test_enabled ? VK_TRUE : VK_FALSE;
  depth_stencil.depthWriteEnable = depth_test_enabled ? VK_TRUE : VK_FALSE;
  depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;
  depth_stencil.depthBoundsTestEnable = VK_FALSE;
  depth_stencil.stencilTestEnable = VK_FALSE;

  // Standard alpha blending -- lets a UI texture's alpha channel composite
  // over whatever an earlier renderpass already drew (e.g. the raymarched
  // scene) instead of overwriting it outright.
  VkPipelineColorBlendAttachmentState color_blend_attachment{};
  color_blend_attachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  color_blend_attachment.blendEnable = VK_TRUE;
  color_blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  color_blend_attachment.dstColorBlendFactor =
      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  color_blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
  color_blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  color_blend_attachment.dstAlphaBlendFactor =
      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  color_blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;

  VkPipelineColorBlendStateCreateInfo color_blending{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  color_blending.logicOpEnable = VK_FALSE;
  color_blending.attachmentCount = 1;
  color_blending.pAttachments = &color_blend_attachment;

  VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                     VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic_state{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic_state.dynamicStateCount = 2;
  dynamic_state.pDynamicStates = dynamic_states;

  VkPipelineShaderStageCreateInfo stages[2] = {
      vertex_stage.stage_create_info(), fragment_stage.stage_create_info()};

  VkGraphicsPipelineCreateInfo pipeline_create_info{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pipeline_create_info.stageCount = 2;
  pipeline_create_info.pStages = stages;
  pipeline_create_info.pVertexInputState = &vertex_input_info;
  pipeline_create_info.pInputAssemblyState = &input_assembly;
  pipeline_create_info.pViewportState = &viewport_state;
  pipeline_create_info.pRasterizationState = &rasterizer;
  pipeline_create_info.pMultisampleState = &multisampling;
  pipeline_create_info.pDepthStencilState = &depth_stencil;
  pipeline_create_info.pColorBlendState = &color_blending;
  pipeline_create_info.pDynamicState = &dynamic_state;
  pipeline_create_info.layout = layout_;
  pipeline_create_info.renderPass = renderpass.handle();
  pipeline_create_info.subpass = 0;
  pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_create_info.basePipelineIndex = -1;

  VkResult result = vkCreateGraphicsPipelines(
      context_->device.logical_device, VK_NULL_HANDLE, 1,
      &pipeline_create_info, context_->allocator, &handle_);

  if (vulkan_result_is_success(result)) {
    KDEBUG("Graphics pipeline created!");
  } else {
    KERROR("vkCreateGraphicsPipelines failed with {}.",
          vulkan_result_string(result, TRUE));
  }
}

VulkanGraphicsPipeline::~VulkanGraphicsPipeline() {
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

VulkanGraphicsPipeline::VulkanGraphicsPipeline(
    VulkanGraphicsPipeline &&other) noexcept
    : context_(other.context_), handle_(other.handle_),
      layout_(other.layout_) {
  other.handle_ = VK_NULL_HANDLE;
  other.layout_ = VK_NULL_HANDLE;
}

VulkanGraphicsPipeline &
VulkanGraphicsPipeline::operator=(VulkanGraphicsPipeline &&other) noexcept {
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

void VulkanGraphicsPipeline::bind(VulkanCommandBuffer &command_buffer) {
  vkCmdBindPipeline(command_buffer.handle(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                    handle_);
}
