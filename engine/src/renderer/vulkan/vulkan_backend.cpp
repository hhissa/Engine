#include "vulkan_backend.h"
#include "../../core/logger.h"

#include "../../platform/platform.h"
#include "vulkan_commandbuffer.h"
#include "vulkan_device.h"
#include "vulkan_renderpass.h"
#include "vulkan_swapchain.h"
#include "vulkan_utils.h"
#include "shaders/vulkan_object_shader.h"
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
      static_cast<f32>(context_.framebuffer_height), 1.0f, 0.0f, 0.2f, 1.0f,
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

  // Create builtin shaders.
  context_.object_shader = std::make_unique<VulkanObjectShader>(context_);
  if (!context_.object_shader->is_valid()) {
    KERROR("Error loading built-in object shader.");
    return FALSE;
  }

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

  KDEBUG("Destroying shaders...");
  context_.object_shader.reset();

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

void VulkanRendererBackend::on_resized(u16 width, u16 height) {
  // Update the "framebuffer size generation", a counter which indicates
  // when the framebuffer size has been updated.
  cached_framebuffer_height_ = height;
  cached_framebuffer_width_ = width;
  context_.framebuffer_size_generation++;

  KINFO("Vulkan renderer backend->resized: w/h/gen: {}/{}/{}", width, height,
        context_.framebuffer_size_generation);
}

b8 VulkanRendererBackend::begin_frame(f32 delta_time) {
  VulkanDevice &device = context_.device;

  // Check if recreating swap chain and boot out.
  if (context_.recreating_swapchain) {
    VkResult result = vkDeviceWaitIdle(device.logical_device);
    if (!vulkan_result_is_success(result)) {
      KERROR("vulkan_renderer_backend_begin_frame vkDeviceWaitIdle (1) "
             "failed: '{}'",
             vulkan_result_string(result, TRUE));
      return FALSE;
    }
    KINFO("Recreating swapchain, booting.");
    return FALSE;
  }

  // Check if the framebuffer has been resized. If so, a new swapchain must
  // be created.
  if (context_.framebuffer_size_generation !=
      context_.framebuffer_size_last_generation) {
    VkResult result = vkDeviceWaitIdle(device.logical_device);
    if (!vulkan_result_is_success(result)) {
      KERROR("vulkan_renderer_backend_begin_frame vkDeviceWaitIdle (2) "
             "failed: '{}'",
             vulkan_result_string(result, TRUE));
      return FALSE;
    }

    // If the swapchain recreation failed (because, for example, the window
    // was minimized), boot out before unsetting the flag.
    if (!recreate_swapchain()) {
      return FALSE;
    }

    KINFO("Resized, booting.");
    return FALSE;
  }

  // Wait for the execution of the current frame to complete. The fence
  // being free will allow this one to move on.
  if (!in_flight_fences_[context_.current_frame].wait(UINT64_MAX)) {
    KWARN("In-flight fence wait failure!");
    return FALSE;
  }

  // Acquire the next image from the swap chain. Pass along the semaphore
  // that should be signaled when this completes. This same semaphore will
  // later be waited on by the queue submission to ensure this image is
  // available.
  if (!vulkan_swapchain_acquire_next_image_index(
          &context_, &context_.swapchain, UINT64_MAX,
          context_.image_available_semaphores[context_.current_frame],
          VK_NULL_HANDLE, &context_.image_index)) {
    return FALSE;
  }

  // Begin recording commands.
  VulkanCommandBuffer *command_buffer =
      context_.graphics_command_buffers[context_.image_index].get();
  command_buffer->reset();
  command_buffer->begin(FALSE, FALSE, FALSE);

  // Dynamic state
  VkViewport viewport{};
  viewport.x = 0.0f;
  viewport.y = static_cast<f32>(context_.framebuffer_height);
  viewport.width = static_cast<f32>(context_.framebuffer_width);
  viewport.height = -static_cast<f32>(context_.framebuffer_height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;

  // Scissor
  VkRect2D scissor{};
  scissor.offset.x = scissor.offset.y = 0;
  scissor.extent.width = context_.framebuffer_width;
  scissor.extent.height = context_.framebuffer_height;

  vkCmdSetViewport(command_buffer->handle(), 0, 1, &viewport);
  vkCmdSetScissor(command_buffer->handle(), 0, 1, &scissor);

  context_.main_renderpass->set_render_area(
      0.0f, 0.0f, static_cast<f32>(context_.framebuffer_width),
      static_cast<f32>(context_.framebuffer_height));

  // Begin the render pass.
  context_.main_renderpass->begin(*command_buffer,
                                  framebuffers_[context_.image_index].handle());

  return TRUE;
}

b8 VulkanRendererBackend::end_frame(f32 delta_time) {
  VulkanCommandBuffer *command_buffer =
      context_.graphics_command_buffers[context_.image_index].get();

  // End renderpass
  context_.main_renderpass->end(*command_buffer);

  command_buffer->end();

  // Make sure the previous frame is not using this image (i.e. its fence is
  // being waited on).
  if (images_in_flight_[context_.image_index] != nullptr) {
    images_in_flight_[context_.image_index]->wait(UINT64_MAX);
  }

  // Mark the image fence as in-use by this frame.
  images_in_flight_[context_.image_index] =
      &in_flight_fences_[context_.current_frame];

  // Reset the fence for use on the next frame.
  in_flight_fences_[context_.current_frame].reset();

  // Submit the queue and wait for the operation to complete.
  VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};

  // Command buffer(s) to be executed.
  VkCommandBuffer command_buffer_handle = command_buffer->handle();
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &command_buffer_handle;

  // The semaphore(s) to be signaled when the queue is complete.
  submit_info.signalSemaphoreCount = 1;
  submit_info.pSignalSemaphores =
      &context_.queue_complete_semaphores[context_.current_frame];

  // Wait semaphore ensures that the operation cannot begin until the image
  // is available.
  submit_info.waitSemaphoreCount = 1;
  submit_info.pWaitSemaphores =
      &context_.image_available_semaphores[context_.current_frame];

  // Each semaphore waits on the corresponding pipeline stage to complete.
  // 1:1 ratio. VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT prevents
  // subsequent colour attachment writes from executing until the semaphore
  // signals (i.e. one frame is presented at a time).
  VkPipelineStageFlags flags[1] = {
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
  submit_info.pWaitDstStageMask = flags;

  VkResult result =
      vkQueueSubmit(context_.device.graphics_queue, 1, &submit_info,
                    in_flight_fences_[context_.current_frame].handle());
  if (result != VK_SUCCESS) {
    KERROR("vkQueueSubmit failed with result: {}",
           vulkan_result_string(result, TRUE));
    return FALSE;
  }

  command_buffer->update_submitted();
  // End queue submission

  // Give the image back to the swapchain.
  vulkan_swapchain_present(
      &context_, &context_.swapchain, context_.device.graphics_queue,
      context_.device.present_queue,
      context_.queue_complete_semaphores[context_.current_frame],
      context_.image_index);

  return TRUE;
}

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

b8 VulkanRendererBackend::recreate_swapchain() {
  // If already being recreated, do not try again.
  if (context_.recreating_swapchain) {
    KDEBUG("recreate_swapchain called when already recreating. Booting.");
    return FALSE;
  }

  // Detect if the window is too small to be drawn to.
  if (context_.framebuffer_width == 0 || context_.framebuffer_height == 0) {
    KDEBUG("recreate_swapchain called when window is < 1 in a dimension. "
           "Booting.");
    return FALSE;
  }

  // Mark as recreating if the dimensions are valid.
  context_.recreating_swapchain = TRUE;

  // Wait for any operations to complete.
  vkDeviceWaitIdle(context_.device.logical_device);

  // Clear these out just in case.
  for (auto &fence_ptr : images_in_flight_) {
    fence_ptr = nullptr;
  }

  // vulkan_swapchain_recreate() requeries swapchain support and depth
  // format internally as part of create().
  vulkan_swapchain_recreate(&context_, cached_framebuffer_width_,
                            cached_framebuffer_height_, &context_.swapchain);

  // Sync the framebuffer size with the cached sizes.
  context_.framebuffer_width = cached_framebuffer_width_;
  context_.framebuffer_height = cached_framebuffer_height_;
  cached_framebuffer_width_ = 0;
  cached_framebuffer_height_ = 0;

  // Update framebuffer size generation.
  context_.framebuffer_size_last_generation =
      context_.framebuffer_size_generation;

  // Command buffers and framebuffers are tied to the old swapchain images;
  // destroying and recreating them runs through the same RAII teardown as
  // shutdown(), via clear().
  context_.graphics_command_buffers.clear();
  framebuffers_.clear();

  context_.main_renderpass->set_render_area(
      0.0f, 0.0f, static_cast<f32>(context_.framebuffer_width),
      static_cast<f32>(context_.framebuffer_height));

  regenerate_framebuffers();
  create_commandbuffer();

  // Clear the recreating flag.
  context_.recreating_swapchain = FALSE;

  return TRUE;
}
