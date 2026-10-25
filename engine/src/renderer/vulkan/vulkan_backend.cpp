#include "vulkan_backend.h"
#include "renderer/renderer_frontend.h" // SculptStampInfo
#include "../../core/logger.h"

#include "../../platform/platform.h"
#include "vulkan_commandbuffer.h"
#include "vulkan_device.h"
#include "vulkan_pipeline_cache.h"
#include "vulkan_renderpass.h"
#include "vulkan_swapchain.h"
#include "vulkan_utils.h"
#include "shaders/vulkan_raymarch_shader.h"
#include "shaders/vulkan_ui_shader.h"
#include "shaders/vulkan_text_shader.h"
#include "shaders/vulkan_line_shader.h"
#include "shaders/vulkan_solid_quad_shader.h"
#include "../../systems/texture_system.h"
#include "../../systems/shader_system.h"
#include "../../systems/material_system.h"
#include "../../systems/geometry_system.h"
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <unordered_map>
#include <cmath>
#include <limits>
#include <vulkan/vulkan.h>

#include <string_view>
#include <unordered_set>
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

  // Before anything creates a pipeline (the shaders below all do, in their
  // constructors) -- a warm cache is what keeps that from taking minutes.
  vulkan_pipeline_cache_create(context_);

  KINFO("Vulkan renderer initialized successfully.");
  if (!vulkan_swapchain_create(&context_, context_.framebuffer_width,
                               context_.framebuffer_height,
                               &context_.swapchain)) {
    KERROR("failed to create swapchain");
    return FALSE;
  }
  // World render pass: clears colour/depth/stencil, and -- since the UI
  // pass runs after it against the same swapchain image -- leaves its
  // colour attachment in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
  // (has_next_pass=true) instead of transitioning straight to
  // VK_IMAGE_LAYOUT_PRESENT_SRC_KHR.
  context_.main_renderpass = std::make_unique<VulkanRenderpass>(
      context_, 0.0f, 0.0f, static_cast<f32>(context_.framebuffer_width),
      static_cast<f32>(context_.framebuffer_height), 0.0f, 0.0f, 0.2f, 1.0f,
      1.0f, 0,
      RenderpassClearFlags::kColourBuffer | RenderpassClearFlags::kDepthBuffer |
          RenderpassClearFlags::kStencilBuffer,
      /*has_prev_pass=*/false, /*has_next_pass=*/true);

  // UI render pass: draws on top of whatever the world pass (and the
  // raymarch shader's copy into the swapchain image, which runs between
  // the two passes -- see end_frame()) already produced, so it clears
  // nothing and expects the image already in
  // VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL (has_prev_pass=true). It's the
  // last pass before present, so it transitions to
  // VK_IMAGE_LAYOUT_PRESENT_SRC_KHR (has_next_pass=false).
  context_.ui_renderpass = std::make_unique<VulkanRenderpass>(
      context_, 0.0f, 0.0f, static_cast<f32>(context_.framebuffer_width),
      static_cast<f32>(context_.framebuffer_height), 0.0f, 0.0f, 0.0f, 0.0f,
      1.0f, 0, RenderpassClearFlags::kNone,
      /*has_prev_pass=*/true, /*has_next_pass=*/false);

  regenerate_framebuffers();
  create_commandbuffer();

  // Sync objects. image_available_semaphores and in_flight_fences_ are one
  // per frame in flight; queue_complete_semaphores is sized/indexed
  // differently -- see its declaration comment in vulkan_types.inl.
  context_.image_available_semaphores.resize(
      context_.swapchain.max_frames_in_flight);
  in_flight_fences_.reserve(context_.swapchain.max_frames_in_flight);

  for (u8 i = 0; i < context_.swapchain.max_frames_in_flight; ++i) {
    VkSemaphoreCreateInfo semaphore_create_info{
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(context_.device.logical_device, &semaphore_create_info,
                      context_.allocator,
                      &context_.image_available_semaphores[i]);

    // Created signaled so the first frame doesn't wait indefinitely for a
    // "previous" frame that never existed.
    in_flight_fences_.emplace_back(context_, TRUE);
  }
  create_queue_complete_semaphores();

  // See VulkanContext::graphics_timeline. Starts at 0, so the very first
  // async submission's wait (for "the previous frame") is satisfied
  // immediately.
  {
    VkSemaphoreTypeCreateInfo timeline_type{
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timeline_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timeline_type.initialValue = 0;
    VkSemaphoreCreateInfo timeline_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    timeline_info.pNext = &timeline_type;
    vkCreateSemaphore(context_.device.logical_device, &timeline_info,
                      context_.allocator, &context_.graphics_timeline);
    context_.graphics_timeline_value = 0;
  }

  // images_in_flight are non-owning pointers — null means the image is
  // not currently held by any in-flight frame.
  images_in_flight_.assign(context_.swapchain.image_count, nullptr);

  // Create builtin shaders.
  context_.texture_system = std::make_unique<TextureSystem>(context_);
  context_.shader_system = std::make_unique<ShaderSystem>(context_);
  context_.material_system =
      std::make_unique<MaterialSystem>(*context_.texture_system);
  context_.geometry_system =
      std::make_unique<GeometrySystem>(*context_.material_system);

  context_.raymarch_shader = std::make_unique<VulkanRaymarchShader>(context_);
  if (!context_.raymarch_shader->is_valid()) {
    KERROR("Error loading built-in raymarch shader.");
    return FALSE;
  }

  context_.ui_shader =
      std::make_unique<VulkanUIShader>(context_, *context_.ui_renderpass);
  if (!context_.ui_shader->is_valid()) {
    KERROR("Error loading built-in UI shader.");
    return FALSE;
  }

  context_.text_shader =
      std::make_unique<VulkanTextShader>(context_, *context_.ui_renderpass);
  if (!context_.text_shader->is_valid()) {
    KERROR("Error loading built-in text shader.");
    return FALSE;
  }

  context_.line_shader =
      std::make_unique<VulkanLineShader>(context_, *context_.ui_renderpass);
  if (!context_.line_shader->is_valid()) {
    KERROR("Error loading built-in line shader.");
    return FALSE;
  }

  context_.solid_quad_shader = std::make_unique<VulkanSolidQuadShader>(
      context_, *context_.ui_renderpass);
  if (!context_.solid_quad_shader->is_valid()) {
    KERROR("Error loading built-in solid quad shader.");
    return FALSE;
  }

  KINFO("Vulkan renderer initialized successfully.");
  return TRUE;
}

void VulkanRendererBackend::shutdown() {
  vkDeviceWaitIdle(context_.device.logical_device);

  KDEBUG("Destroying shaders...");
  context_.solid_quad_shader.reset();
  context_.line_shader.reset();
  context_.text_shader.reset();
  context_.ui_shader.reset();
  context_.raymarch_shader.reset();
  context_.geometry_system.reset();
  context_.material_system.reset();
  context_.shader_system.reset();
  context_.texture_system.reset();

  ui_framebuffers_.clear();
  framebuffers_.clear(); // Destroy in reverse order of creation.
  context_.graphics_command_buffers.clear();
  context_.ui_renderpass.reset();
  context_.main_renderpass.reset();

  KDEBUG("Destroying Vulkan swapchain...");
  vulkan_swapchain_destroy(&context_, &context_.swapchain);

  // Sync objects — destroyed *after* the swapchain, deliberately.
  // vkDeviceWaitIdle() does not retire the presentation engine's use of
  // the semaphores handed to vkQueuePresentKHR (that's the spec gap
  // VK_EXT_swapchain_maintenance1 exists for), so destroying them while
  // the last present still references them trips
  // VUID-vkDestroySemaphore-semaphore-05149. Destroying the swapchain
  // first retires those references. VulkanFence's destructor runs
  // vkDestroyFence automatically.
  for (u8 i = 0; i < context_.swapchain.max_frames_in_flight; ++i) {
    if (context_.image_available_semaphores[i]) {
      vkDestroySemaphore(context_.device.logical_device,
                         context_.image_available_semaphores[i],
                         context_.allocator);
    }
  }
  context_.image_available_semaphores.clear();
  if (context_.graphics_timeline != VK_NULL_HANDLE) {
    vkDestroySemaphore(context_.device.logical_device,
                       context_.graphics_timeline, context_.allocator);
    context_.graphics_timeline = VK_NULL_HANDLE;
  }
  destroy_queue_complete_semaphores();
  in_flight_fences_.clear();
  images_in_flight_.clear();

  // After every pipeline is gone (the shaders were destroyed above) so the
  // cache holds everything this run compiled, and before the device it
  // belongs to.
  vulkan_pipeline_cache_destroy(context_);

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

  maybe_recenter_origin();

  // A batch of scene mutations (load_scene()/translate_scene()/etc. -- see
  // scene_dirty_'s comment) may have piled up since the last frame; rebake
  // at most once here rather than once per call.
  if (scene_dirty_) {
    context_.raymarch_shader->rebake();
    scene_dirty_ = false;
  }
  // A released drag settling back into the world, a slice per frame.
  if (context_.raymarch_shader &&
      context_.raymarch_shader->step_drag_instance_settle()) {
    release_unused_sculpt_chunks();
  }

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

  // Check if the framebuffer has been resized, or if acquire/present
  // reported the swapchain out of date. Either way a new swapchain must
  // be created.
  if (context_.framebuffer_size_generation !=
          context_.framebuffer_size_last_generation ||
      context_.swapchain_out_of_date) {
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

  // VulkanRaymarchShader::update_streaming() -- the camera-driven chunk
  // streaming core -- was removed in the M0 CSG deletion pass (see
  // /home/h-i/.claude/plans/joyful-skipping-giraffe.md): it existed to
  // load/evict/rebake chunks against GeometrySystem's registered CSG
  // primitives, which no longer exist. Nothing currently populates the
  // chunked field at all (a future SculptVolume milestone is expected to
  // give it a new, brush-driven producer), so there is nothing left for a
  // per-frame streaming tick to do.

  // Phase 5: cascade GI recenter/rebake -- a cascade bake gathers against
  // the chunked field, so it must run after any per-frame chunk load/evict
  // budget would have been recorded (same ordering reasoning as
  // update_gi_cascade()'s own comment). A no-op call most frames.
  context_.raymarch_shader->update_gi_cascade(*command_buffer, camera_.position());

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

void VulkanRendererBackend::set_camera(const Camera &camera) {
  camera_ = camera;
}

void VulkanRendererBackend::draw_text(std::string_view text,
                                      glm::vec2 position, glm::vec4 colour,
                                      f32 scale) {
  queued_text_draws_.push_back({std::string(text), position, colour, scale});
}

void VulkanRendererBackend::draw_ui_quad(glm::vec2 position, glm::vec2 size) {
  queued_ui_quad_draws_.push_back({position, size});
}

void VulkanRendererBackend::draw_line(glm::vec2 start, glm::vec2 end,
                                      glm::vec4 colour) {
  queued_line_draws_.push_back({start, end, colour});
}

void VulkanRendererBackend::draw_solid_quad(glm::vec2 position, glm::vec2 size,
                                           glm::vec4 colour) {
  queued_solid_quad_draws_.push_back({position, size, colour});
}

void VulkanRendererBackend::draw_backdrop_quad(glm::vec2 position,
                                              glm::vec2 size,
                                              glm::vec4 colour) {
  queued_backdrop_quad_draws_.push_back({{}, position, size, colour});
}

void VulkanRendererBackend::draw_image(std::string_view path,
                                       glm::vec2 position, glm::vec2 size,
                                       glm::vec4 tint) {
  queued_backdrop_quad_draws_.push_back(
      {std::string(path), position, size, tint});
}

glm::vec2 VulkanRendererBackend::preload_image(std::string_view path) {
  // Decode only -- see TextureSystem::predecode(). The GPU texture is made
  // on the image's first draw and given up again soon after it stops being
  // drawn (see VulkanUIShader::release_idle_images()), so preloading a
  // chapter's worth of photos costs RAM, not video memory.
  if (auto size = context_.texture_system->predecode(path)) {
    return glm::vec2(static_cast<f32>(size->first),
                     static_cast<f32>(size->second));
  }
  return glm::vec2(0.0f);
}

// The CSG-primitive scene-authoring methods that used to live here
// (load_scene()/reconcile_scene()/set_cache_prewarm_on_load()/
// request_cache_prewarm()/set_primitive_transform()/translate_scene()/
// streaming_backlog()/rotate_scene()/scale_scene()/remove_scene()/
// clear_scenes()/set_selected_primitive()/primitive_gpu_index()/
// set_dynamic_primitive()) were removed in the M0 CSG deletion pass (see
// /home/h-i/.claude/plans/joyful-skipping-giraffe.md) along with
// GeometrySystem's primitive-authoring surface and VulkanRaymarchShader's
// matching GPU plumbing.
void VulkanRendererBackend::set_grid_visible(b8 visible) {
  context_.raymarch_shader->set_grid_visible(visible);
}

void VulkanRendererBackend::set_chunked_field_enabled(b8 enabled) {
  context_.raymarch_shader->set_chunked_field_enabled(enabled);
}

// The two enums are declared separately (RendererSplatMode in the shared
// renderer_types.inl so the frontend/backend interface can name it without
// pulling in Vulkan headers; SplatMode alongside the shader that actually
// consumes it) but MUST agree value-for-value, since this is a straight
// cast -- and SplatMode's own values additionally have to match SPLAT_MODE_*
// in Builtin.RaymarchShader.comp.glsl, which nothing in C++ can check.
static_assert(static_cast<i32>(RendererSplatMode::Off) ==
                  static_cast<i32>(SplatMode::Off) &&
              static_cast<i32>(RendererSplatMode::Prime) ==
                  static_cast<i32>(SplatMode::Prime) &&
              static_cast<i32>(RendererSplatMode::Visibility) ==
                  static_cast<i32>(SplatMode::Visibility),
              "RendererSplatMode and SplatMode must stay in lockstep.");

void VulkanRendererBackend::set_taa_enabled(b8 enabled) {
  context_.raymarch_shader->set_taa_enabled(enabled);
}

void VulkanRendererBackend::set_ism_enabled(b8 enabled) {
  context_.raymarch_shader->set_ism_enabled(enabled);
}

void VulkanRendererBackend::set_ao_enabled(b8 enabled) {
  context_.raymarch_shader->set_ao_enabled(enabled);
}

void VulkanRendererBackend::debug_probe_field(glm::vec3 world_pos) {
  context_.raymarch_shader->debug_probe_field(world_pos, camera_.position());
}

// The two pairs of enums below are declared separately (SculptBrushShape/
// SculptBrushOp in the shared renderer_types.inl so the frontend/backend
// interface can name them without pulling in Vulkan headers;
// VulkanRaymarchShader::BrushShape/BrushOp alongside the shader that
// actually consumes them) but MUST agree value-for-value, since both casts
// below are straight casts -- and the Vulkan-side values additionally have
// to match SHAPE_*/OP_* in Builtin.SculptStamp.comp.glsl, which nothing in
// C++ can check. Same pattern as RendererSplatMode/SplatMode's own
// static_assert just above set_chunked_field_enabled().
static_assert(
    static_cast<i32>(SculptBrushShape::Sphere) ==
        static_cast<i32>(VulkanRaymarchShader::BrushShape::Sphere) &&
    static_cast<i32>(SculptBrushShape::Box) ==
        static_cast<i32>(VulkanRaymarchShader::BrushShape::Box) &&
    static_cast<i32>(SculptBrushShape::Plane) ==
        static_cast<i32>(VulkanRaymarchShader::BrushShape::Plane) &&
    static_cast<i32>(SculptBrushShape::Torus) ==
        static_cast<i32>(VulkanRaymarchShader::BrushShape::Torus) &&
    static_cast<i32>(SculptBrushShape::CappedCylinder) ==
        static_cast<i32>(VulkanRaymarchShader::BrushShape::CappedCylinder) &&
    static_cast<i32>(SculptBrushShape::CappedCone) ==
        static_cast<i32>(VulkanRaymarchShader::BrushShape::CappedCone) &&
    static_cast<i32>(SculptBrushShape::RoundBox) ==
        static_cast<i32>(VulkanRaymarchShader::BrushShape::RoundBox) &&
    static_cast<i32>(SculptBrushShape::BoxFrame) ==
        static_cast<i32>(VulkanRaymarchShader::BrushShape::BoxFrame) &&
    static_cast<i32>(SculptBrushShape::Octahedron) ==
        static_cast<i32>(VulkanRaymarchShader::BrushShape::Octahedron) &&
    static_cast<i32>(SculptBrushShape::Pyramid) ==
        static_cast<i32>(VulkanRaymarchShader::BrushShape::Pyramid) &&
    static_cast<i32>(SculptBrushShape::HexPrism) ==
        static_cast<i32>(VulkanRaymarchShader::BrushShape::HexPrism) &&
    static_cast<i32>(SculptBrushShape::RoundCone) ==
        static_cast<i32>(VulkanRaymarchShader::BrushShape::RoundCone) &&
    static_cast<i32>(SculptBrushShape::Capsule) ==
        static_cast<i32>(VulkanRaymarchShader::BrushShape::Capsule) &&
    static_cast<i32>(SculptBrushShape::Link) ==
        static_cast<i32>(VulkanRaymarchShader::BrushShape::Link) &&
    static_cast<i32>(SculptBrushShape::Ellipsoid) ==
        static_cast<i32>(VulkanRaymarchShader::BrushShape::Ellipsoid),
    "SculptBrushShape and VulkanRaymarchShader::BrushShape must stay in "
    "lockstep.");
static_assert(
    static_cast<i32>(SculptBrushOp::Union) ==
        static_cast<i32>(VulkanRaymarchShader::BrushOp::Union) &&
    static_cast<i32>(SculptBrushOp::Subtraction) ==
        static_cast<i32>(VulkanRaymarchShader::BrushOp::Subtraction) &&
    static_cast<i32>(SculptBrushOp::ObjectUnion) ==
        static_cast<i32>(VulkanRaymarchShader::BrushOp::ObjectUnion) &&
    static_cast<i32>(SculptBrushOp::ObjectSubtraction) ==
        static_cast<i32>(VulkanRaymarchShader::BrushOp::ObjectSubtraction) &&
    static_cast<i32>(SculptBrushOp::ObjectIntersection) ==
        static_cast<i32>(VulkanRaymarchShader::BrushOp::ObjectIntersection) &&
    static_cast<i32>(SculptBrushOp::ObjectPaint) ==
        static_cast<i32>(VulkanRaymarchShader::BrushOp::ObjectPaint) &&
    static_cast<i32>(SculptBrushOp::Fleck) ==
        static_cast<i32>(VulkanRaymarchShader::BrushOp::Fleck),
    "SculptBrushOp and VulkanRaymarchShader::BrushOp must stay in "
    "lockstep.");

void VulkanRendererBackend::set_material(i32 slot, glm::vec3 colour,
                                         f32 roughness, f32 metallic,
                                         bool waxy) {
  // Straight pass-through, same "no SculptSystem bookkeeping, no undo
  // involvement" shape as spray_paint()'s own pass-through below -- a
  // material slot's configuration is not an undoable edit, it is a
  // standing piece of renderer state a LATER stamp_shape() call references
  // by index (see VulkanRaymarchShader::set_material()'s own header
  // comment).
  context_.raymarch_shader->set_material(slot, colour, roughness, metallic,
                                         waxy);
}

i32 VulkanRendererBackend::add_directional_light(glm::vec3 direction,
                                                  glm::vec3 colour,
                                                  f32 intensity) {
  return context_.raymarch_shader->add_directional_light(direction, colour,
                                                          intensity);
}

void VulkanRendererBackend::set_point_light(i32 index, glm::vec3 position,
                                            glm::vec3 colour, f32 intensity,
                                            i32 source_material) {
  context_.raymarch_shader->set_point_light(index, position, colour, intensity,
                                            source_material);
}

i32 VulkanRendererBackend::add_point_light(glm::vec3 position,
                                           glm::vec3 colour, f32 intensity) {
  return context_.raymarch_shader->add_point_light(position, colour,
                                                    intensity);
}

void VulkanRendererBackend::apply_stamp_and_record(
    glm::vec3 center, SculptBrushShape shape_type, glm::vec4 params,
    f32 smoothness, SculptBrushOp shape_op, i32 material_slot,
    f32 looseness, glm::quat rotation, i32 object_id) {
  // The GPU-side work (chunk-table-slot/brick allocation, the actual
  // voxel/splat writes, and -- M2b -- its own undo snapshot) and the CPU-
  // side record are driven side by side here rather than one calling the
  // other -- see SculptSystem's own header comment for why it stays
  // backend-agnostic. cells_touched comes straight back from the GPU call
  // so SculptVolume's record can never claim a stamp "happened" more
  // completely than it actually did (e.g. if kMaxResidentChunks ran out
  // partway through). looseness is passed to the GPU call below, which
  // stores it per brick (chunk_brick_looseness_buffer_); nothing shades
  // with it at the moment.
  const auto vulkan_shape =
      static_cast<VulkanRaymarchShader::BrushShape>(shape_type);
  const auto vulkan_op = static_cast<VulkanRaymarchShader::BrushOp>(shape_op);
  if (shape_op != SculptBrushOp::Union &&
      shape_op != SculptBrushOp::Subtraction) {
    // Object-scoped: can't be blended in on its own -- rebuilt from the
    // list (see add_stamps()).
    SculptStampInfo info;
    info.center = center;
    info.shape_type = shape_type;
    info.params = params;
    info.smoothness = smoothness;
    info.op = shape_op;
    info.material_slot = material_slot;
    info.looseness = looseness;
    info.rotation = rotation;
    add_stamps(object_id, &info, 1);
    return;
  }
  const i32 cells_touched = context_.raymarch_shader->stamp_shape(
      center, vulkan_shape, params, smoothness, vulkan_op, material_slot,
      looseness, rotation, object_id);
  sculpt_system_.apply_stamp(center, shape_type, params, smoothness,
                             shape_op, material_slot, cells_touched,
                             looseness, rotation, object_id);

  // M5 (checkpoint-removal redesign): append this stamp onto the GPU
  // stamp-list mirror too -- see VulkanRaymarchShader::sculpt_stamp_list_
  // append()'s own comment for why this, not the (now-deleted) periodic
  // checkpoint, is what delete_edit()/move_edit() reconstruct GPU state
  // from. Covers every call site that lands here: a fresh user stamp,
  // Redo, and a .stamps Load replay (tools/modelling_software_v2/src/
  // main_window.cpp's on_load_triggered(), via replay_stamp() ->
  // renderer_sculpt_stamp_shape() -> stamp_shape() -> this function).
  context_.raymarch_shader->sculpt_stamp_list_append(
      center, vulkan_shape, params, smoothness, vulkan_op, material_slot,
      looseness, rotation, object_id);
}

namespace {
// The cells a recompute must cover for stamps[indices]: each one's own
// footprint, and for an object crop the whole of what it crops -- its
// object and every object welded to it (a sculpture's crops live in its
// carrier, which has no shapes of its own, and act on all its strokes) --
// since it removes everything outside it (and putting one back or taking
// it away changes all of that).
std::vector<VulkanRaymarchShader::RecomputeShape>
stamp_footprints(const std::vector<SculptStamp> &stamps,
                 const std::vector<size_t> &indices,
                 const std::unordered_map<i32, i32> &weld_keys) {
  std::vector<VulkanRaymarchShader::RecomputeShape> footprints;
  std::vector<i32> cropped; // objects whose whole extent must re-run
  auto footprint = [](const SculptStamp &stamp) {
    return VulkanRaymarchShader::RecomputeShape{
        stamp.center,
        static_cast<VulkanRaymarchShader::BrushShape>(stamp.shape_type),
        stamp.params, stamp.smoothness, stamp.rotation,
        sculpt_op_adds(stamp.op)};
  };
  for (size_t i : indices) {
    const SculptStamp &stamp = stamps[i];
    if (stamp.op == SculptBrushOp::Fleck) {
      continue; // no field: nothing to re-run
    }
    footprints.push_back(footprint(stamp));
    if (stamp.op == SculptBrushOp::ObjectIntersection &&
        std::find(cropped.begin(), cropped.end(), stamp.object_id) ==
            cropped.end()) {
      cropped.push_back(stamp.object_id);
    }
  }
  if (cropped.empty()) {
    return footprints;
  }
  // The crops' weld keys: every object under one of them is cropped too.
  std::unordered_set<i32> cropped_keys;
  for (i32 object : cropped) {
    if (auto key = weld_keys.find(object); key != weld_keys.end()) {
      cropped_keys.insert(key->second);
    }
  }
  auto is_cropped = [&](i32 object) {
    if (std::find(cropped.begin(), cropped.end(), object) != cropped.end()) {
      return true;
    }
    auto key = weld_keys.find(object);
    return key != weld_keys.end() && cropped_keys.count(key->second) != 0;
  };
  for (const SculptStamp &stamp : stamps) {
    if (sculpt_op_adds(stamp.op) && is_cropped(stamp.object_id)) {
      footprints.push_back(footprint(stamp));
    }
  }
  return footprints;
}
} // namespace

bool VulkanRendererBackend::add_stamps(i32 object_id,
                                       const SculptStampInfo *infos,
                                       i32 count) {
  end_drag_instance_for(&object_id, 1);
  if (!infos || count <= 0) {
    return false;
  }
  const std::vector<i32> object_ids(static_cast<size_t>(count), object_id);
  return add_stamps_to_objects(object_ids.data(), infos, count);
}

bool VulkanRendererBackend::add_stamps_to_objects(const i32 *object_ids,
                                                  const SculptStampInfo *infos,
                                                  i32 count) {
  end_drag_instance_for(object_ids, count);
  if (!object_ids || !infos || count <= 0) {
    return false;
  }
  std::vector<size_t> added;
  for (i32 k = 0; k < count; ++k) {
    const SculptStampInfo &stamp = infos[k];
    const i32 object_id = object_ids[k];
    const auto shape =
        static_cast<VulkanRaymarchShader::BrushShape>(stamp.shape_type);
    const auto op = static_cast<VulkanRaymarchShader::BrushOp>(stamp.op);
    added.push_back(sculpt_system_.volume().stamps().size());
    sculpt_system_.apply_stamp(stamp.center, stamp.shape_type, stamp.params,
                               stamp.smoothness, stamp.op,
                               stamp.material_slot, /*cells_touched=*/-1,
                               stamp.looseness, stamp.rotation, object_id);
    context_.raymarch_shader->sculpt_stamp_list_append(
        stamp.center, shape, stamp.params, stamp.smoothness, op,
        stamp.material_slot, stamp.looseness, stamp.rotation, object_id);
  }
  context_.raymarch_shader->recompute_shapes(
      stamp_footprints(sculpt_system_.volume().stamps(), added, weld_keys_));
  // Same reasons as transform_objects(): the incremental undo snapshots no
  // longer describe the field.
  context_.raymarch_shader->invalidate_undo_history();
  redo_stack_.clear();
  return true;
}

void VulkanRendererBackend::stamp_shape(glm::vec3 center,
                                        SculptBrushShape shape_type,
                                        glm::vec4 params, f32 smoothness,
                                        SculptBrushOp shape_op,
                                        i32 material_slot, f32 looseness,
                                        glm::quat rotation) {
  end_drag_instance(); // see end_drag_instance()
  apply_stamp_and_record(center, shape_type, params, smoothness, shape_op,
                         material_slot, looseness, rotation,
                         current_object_id_);
  // A fresh, user-driven stamp invalidates any pending redo history -- see
  // redo_stack_'s own comment.
  redo_stack_.clear();
}

bool VulkanRendererBackend::undo_stamp() {
  end_drag_instance(); // see end_drag_instance()
  // GPU first: only drop the CPU-side bookkeeping record (and make it
  // available to Redo) if VulkanRaymarchShader confirms an undo actually
  // happened -- the same "never claim more than actually occurred"
  // discipline apply_stamp_and_record() already follows for a fresh stamp.
  if (!context_.raymarch_shader->undo_stamp()) {
    return false;
  }
  if (std::optional<SculptStamp> popped = sculpt_system_.pop_last_stamp()) {
    redo_stack_.push_back(*popped);
    // M5 (checkpoint-removal redesign): Undo always pops the CPU list's
    // TAIL, so the GPU stamp-list mirror must drop its own tail entry too,
    // or a later delete_edit()/move_edit() would recompute against a
    // mirror that still includes a stamp Undo already reverted GPU-side.
    // The CPU list's size() right now (AFTER the pop above) is exactly the
    // tail index that needs erasing -- see sculpt_stamp_list_erase()'s own
    // comment for why erasing the tail needs no shifting.
    context_.raymarch_shader->sculpt_stamp_list_erase(
        static_cast<i32>(sculpt_system_.volume().stamps().size()));
  }
  return true;
}

i32 VulkanRendererBackend::spray_paint(glm::vec3 center,
                                       SculptBrushShape shape_type,
                                       glm::vec4 params,
                                       f32 opacity_per_pass) {
  end_drag_instance(); // see end_drag_instance()
  // Straight pass-through, same shape-enum cast stamp_shape() uses (the
  // static_assert pinning SculptBrushShape/VulkanRaymarchShader::BrushShape
  // together, above, covers this call too) -- see VulkanRaymarchShader::
  // spray_paint() for the actual mechanics. Deliberately does NOT go
  // through apply_stamp_and_record()/sculpt_system_ or touch redo_stack_:
  // Spraypaint has no SculptSystem bookkeeping/undo entry to keep in step
  // with (see spray_paint()'s own header comment on why it isn't undoable
  // yet), so there is nothing here for a CPU-side record to stay
  // consistent with.
  return context_.raymarch_shader->spray_paint(
      center, static_cast<VulkanRaymarchShader::BrushShape>(shape_type),
      params, opacity_per_pass);
}

void VulkanRendererBackend::preview_brush(bool enabled, glm::vec3 center,
                                          SculptBrushShape shape_type,
                                          glm::vec4 params, SculptBrushOp op,
                                          i32 material_slot,
                                          glm::quat rotation) {
  // Straight pass-through, same shape/op-enum cast spray_paint() above
  // uses (the static_assert pinning SculptBrushShape/SculptBrushOp against
  // VulkanRaymarchShader::BrushShape/BrushOp, above, covers this call too)
  // -- no SculptSystem bookkeeping, no undo involvement: a preview is not
  // an edit, it never touches sculpt_system_ or redo_stack_ at all.
  context_.raymarch_shader->set_brush_preview(
      enabled, center, static_cast<VulkanRaymarchShader::BrushShape>(shape_type),
      params, static_cast<VulkanRaymarchShader::BrushOp>(op), material_slot,
      rotation);
}

bool VulkanRendererBackend::redo_stamp() {
  end_drag_instance(); // see end_drag_instance()
  if (redo_stack_.empty()) {
    return false;
  }
  const SculptStamp stamp = redo_stack_.back();
  redo_stack_.pop_back();
  // Re-invoking the exact same stamp path is safe/deterministic here
  // because undo/redo is a strictly linear stack -- nothing else could
  // have touched the same voxels between this stamp being undone and this
  // redo (see RendererBackend::redo_stamp()'s own comment). Deliberately
  // does NOT go through stamp_shape() (which would clear redo_stack_ and
  // destroy any further pending redos) -- see apply_stamp_and_record()'s
  // own comment.
  apply_stamp_and_record(stamp.center, stamp.shape_type, stamp.params,
                         stamp.smoothness, stamp.op, stamp.material_slot,
                         stamp.looseness, stamp.rotation, stamp.object_id);
  return true;
}

bool VulkanRendererBackend::delete_edit(i32 index) {
  end_drag_instance(); // see end_drag_instance()
  // M5 (checkpoint-removal redesign -- see /home/h-i/.claude/plans/joyful-
  // skipping-giraffe.md's M4 section for the ORIGINAL checkpoint/replay
  // design this REPLACES). The GPU voxel/brick field is now a pure,
  // cheaply-recomputable CACHE of the ordered stamp list, not the
  // authoritative state -- so deleting an arbitrary stamp no longer needs
  // a full-volume checkpoint restore plus a replay of every edit between
  // it and the current end of the list. Only the chunks the deleted
  // stamp's OWN AABB touches can possibly change (nothing else in the
  // sculpture was ever influenced by it), so this:
  //   1. Mutates BOTH the GPU stamp-list mirror AND the CPU SculptVolume
  //      list FIRST (erase-compact, same shape as std::vector::erase on
  //      the CPU side) -- recompute reads the mirror as ground truth, so
  //      it must already reflect the deletion before it runs.
  //   2. Dispatches ONE recompute over just `target`'s own touched cells.
  // No checkpoint snapshot/readback, no per-stamp replay loop.
  const std::vector<SculptStamp> stamps = sculpt_system_.volume().stamps();
  if (index < 0 || static_cast<size_t>(index) >= stamps.size()) {
    return false;
  }
  // Refuse (loudly, via gpu_stamp_mirror_count()'s own KERROR at the
  // point the mirror first overflowed) rather than recompute from an
  // incomplete mirror, which would silently produce wrong geometry -- see
  // that method's own header comment for the documented, non-silent
  // degrade this implies in an extremely long session.
  if (index >= context_.raymarch_shader->gpu_stamp_mirror_count()) {
    KWARN("delete_edit({}) refused -- past the GPU stamp-list mirror's "
         "cap (see VulkanRaymarchShader::gpu_stamp_mirror_count()'s own "
         "comment); this edit can no longer be deleted via the recompute "
         "path.",
         index);
    return false;
  }

  const SculptStamp target = stamps[static_cast<size_t>(index)];

  context_.raymarch_shader->sculpt_stamp_list_erase(index);
  sculpt_system_.remove_stamp_at(static_cast<size_t>(index));

  context_.raymarch_shader->recompute_region(
      target.center, static_cast<VulkanRaymarchShader::BrushShape>(
                         target.shape_type),
      target.params, target.smoothness, target.rotation);

  // The recompute above can free brick indices back onto the shared pool
  // and later hand the SAME index to completely different geometry -- any
  // undo_stack_ entry still referencing one of those indices would corrupt
  // unrelated bricks if a later Ctrl+Z tried to restore it. See
  // VulkanRaymarchShader::invalidate_undo_history()'s own comment (this
  // redesign's one GPU-side behavior the now-deleted checkpoint ring used
  // to provide for free, as an undocumented side effect of always calling
  // reset_chunked_field()).
  context_.raymarch_shader->invalidate_undo_history();

  // A structural edit-list change invalidates any pending redo history --
  // same reasoning stamp_shape() already documents for a fresh stamp (a
  // stale redo_stack_ entry would always re-append at the CURRENT tail via
  // apply_stamp_and_record(), which is no longer where it originally sat
  // relative to the rest of the (now reshuffled) history).
  redo_stack_.clear();
  release_unused_sculpt_chunks();

  return true;
}

namespace {
// Conservative radius of a stamp, as a half-size about its center: params
// .xyz hold each shape's extents (radii, half-extents, heights -- see
// VulkanRaymarchShader::stamp_shape()'s param table), a rotated shape can
// reach sqrt(3) times further along an axis, and smoothness can bulge a
// smooth union past the shape itself.
f32 approximate_stamp_radius(const SculptStamp &stamp) {
  const f32 p0 = std::fabs(stamp.params.x);
  const f32 p1 = std::fabs(stamp.params.y);
  const f32 p2 = std::fabs(stamp.params.z);
  f32 radius = std::max({p0, p1, p2});
  switch (stamp.shape_type) {
  // Shapes whose reach adds up -- already a sphere about the centre, which
  // turning can't widen.
  case SculptBrushShape::Torus:
  case SculptBrushShape::Capsule:
    return p0 + p1 + std::max(stamp.smoothness, 0.0f);
  case SculptBrushShape::Link:
    return p0 + p1 + p2 + std::max(stamp.smoothness, 0.0f);
  case SculptBrushShape::Sphere:
    return p0 + std::max(stamp.smoothness, 0.0f);
  default:
    break;
  }
  if (std::fabs(stamp.rotation.w) < 0.999999f) {
    radius *= 1.7320508f;
  }
  return radius + std::max(stamp.smoothness, 0.0f);
}
} // namespace

template <typename Filter>
bool VulkanRendererBackend::union_stamp_bounds(Filter include,
                                               glm::vec3 *out_min,
                                               glm::vec3 *out_max) const {
  bool any = false;
  glm::vec3 lo(0.0f), hi(0.0f);
  for (const SculptStamp &stamp : sculpt_system_.volume().stamps()) {
    if (!sculpt_op_visible(stamp.op) || !include(stamp)) {
      continue;
    }
    const f32 radius = approximate_stamp_radius(stamp);
    const glm::vec3 stamp_lo = stamp.center - glm::vec3(radius);
    const glm::vec3 stamp_hi = stamp.center + glm::vec3(radius);
    lo = any ? glm::min(lo, stamp_lo) : stamp_lo;
    hi = any ? glm::max(hi, stamp_hi) : stamp_hi;
    any = true;
  }
  if (any) {
    *out_min = lo;
    *out_max = hi;
  }
  return any;
}

bool VulkanRendererBackend::sculpt_bounds(glm::vec3 *out_min,
                                          glm::vec3 *out_max) const {
  return union_stamp_bounds([](const SculptStamp &) { return true; }, out_min,
                            out_max);
}

bool VulkanRendererBackend::object_bounds(i32 object_id, glm::vec3 *out_min,
                                          glm::vec3 *out_max) const {
  glm::vec3 lo, hi;
  if (!union_stamp_bounds(
          [object_id](const SculptStamp &stamp) {
            return stamp.object_id == object_id;
          },
          &lo, &hi)) {
    return false;
  }
  // A crop keeps only what is inside it.
  for (const SculptStamp &stamp : sculpt_system_.volume().stamps()) {
    if (stamp.object_id == object_id &&
        stamp.op == SculptBrushOp::ObjectIntersection) {
      const f32 radius = approximate_stamp_radius(stamp);
      lo = glm::max(lo, stamp.center - glm::vec3(radius));
      hi = glm::min(hi, stamp.center + glm::vec3(radius));
    }
  }
  if (lo.x > hi.x || lo.y > hi.y || lo.z > hi.z) {
    return false; // cropped away to nothing
  }
  *out_min = lo;
  *out_max = hi;
  return true;
}

i32 VulkanRendererBackend::begin_object() {
  current_object_id_ = next_object_id_++;
  return current_object_id_;
}

i32 VulkanRendererBackend::pick_object(glm::vec3 point,
                                       f32 max_distance) const {
  i32 best_object = -1;
  f32 best_distance = max_distance;
  for (const SculptStamp &stamp : sculpt_system_.volume().stamps()) {
    if (!sculpt_op_visible(stamp.op)) {
      continue;
    }
    // Cheap reject before the exact shape distance.
    const f32 to_center = glm::length(point - stamp.center);
    if (to_center - approximate_stamp_radius(stamp) > best_distance) {
      continue;
    }
    const glm::vec3 local =
        glm::conjugate(stamp.rotation) * (point - stamp.center);
    const f32 distance = VulkanRaymarchShader::eval_shape_sdf_cpu(
        static_cast<VulkanRaymarchShader::BrushShape>(stamp.shape_type),
        stamp.params, local);
    if (distance <= best_distance) {
      best_distance = distance;
      best_object = stamp.object_id;
    }
  }
  return best_object;
}

i32 VulkanRendererBackend::raycast_object(glm::vec3 origin,
                                          glm::vec3 direction, f32 max_t,
                                          f32 *out_t) const {
  return raycast_objects(origin, direction, max_t, nullptr, 0, out_t);
}

bool VulkanRendererBackend::sculpt_brick_pool_usage(u32 *out_used,
                                                    u32 *out_total) const {
  u32 free = 0, total = 0;
  if (!context_.raymarch_shader ||
      !context_.raymarch_shader->sculpt_brick_pool(&free, &total) || total == 0) {
    return false;
  }
  *out_used = total - free;
  *out_total = total;
  return true;
}

i32 VulkanRendererBackend::stamp_count() const {
  return static_cast<i32>(sculpt_system_.volume().stamps().size());
}

namespace {
// The adding stamps of `objects`, as they are now (their surface lies inside
// these; carves, crops and paint only act within them).
std::vector<VulkanRaymarchShader::RecomputeShape>
surface_footprints(const std::vector<SculptStamp> &stamps,
                   const std::unordered_set<i32> &objects) {
  std::vector<VulkanRaymarchShader::RecomputeShape> footprints;
  for (const SculptStamp &stamp : stamps) {
    if (sculpt_op_adds(stamp.op) && objects.count(stamp.object_id)) {
      footprints.push_back(
          {stamp.center,
           static_cast<VulkanRaymarchShader::BrushShape>(stamp.shape_type),
           stamp.params, stamp.smoothness, stamp.rotation, false});
    }
  }
  return footprints;
}
} // namespace

void VulkanRendererBackend::end_drag_instance(bool gradual) {
  VulkanRaymarchShader *shader = context_.raymarch_shader.get();
  if (!shader) {
    return;
  }
  if (shader->drag_instance_active()) {
    shader->end_drag_instance(
        surface_footprints(sculpt_system_.volume().stamps(),
                           shader->drag_instance_objects()));
    drag_instance_m_ = glm::mat3(1.0f);
    drag_instance_t_ = glm::vec3(0.0f);
    drag_instance_scale_ = 1.0f;
    shader->invalidate_undo_history();
    redo_stack_.clear();
  }
  if (shader->drag_instance_settling() && !gradual) {
    shader->finish_drag_instance();
  }
  if (!shader->drag_instance_settling()) {
    release_unused_sculpt_chunks();
  }
}

void VulkanRendererBackend::end_drag_instance_for(const i32 *object_ids,
                                                  i32 count) {
  VulkanRaymarchShader *shader = context_.raymarch_shader.get();
  if (!shader || !object_ids ||
      (!shader->drag_instance_active() && !shader->drag_instance_settling())) {
    return;
  }
  for (i32 i = 0; i < count; ++i) {
    if (shader->drag_instance_objects().count(object_ids[i])) {
      end_drag_instance();
      return;
    }
  }
}

void VulkanRendererBackend::set_drag_preview(bool on) {
  if (on == drag_preview_) {
    return;
  }
  drag_preview_ = on;
  if (!on) {
    end_drag_instance(/*gradual=*/true);
  }
  if (on || coarse_objects_.empty()) {
    return;
  }
  // The drag is over: re-sculpt what it moved in full.
  const std::vector<SculptStamp> &stamps = sculpt_system_.volume().stamps();
  std::vector<size_t> moved;
  for (size_t i = 0; i < stamps.size(); ++i) {
    if (coarse_objects_.count(stamps[i].object_id)) {
      moved.push_back(i);
    }
  }
  coarse_objects_.clear();
  if (!moved.empty()) {
    context_.raymarch_shader->recompute_shapes(stamp_footprints(stamps, moved, weld_keys_));
  }
}

void VulkanRendererBackend::set_preview_smoothness(f32 k) {
  context_.raymarch_shader->set_brush_preview_smoothness(k);
}

void VulkanRendererBackend::set_object_weld_keys(const i32 *object_ids,
                                                 const i32 *keys, i32 count) {
  std::unordered_map<i32, i32> next;
  for (i32 i = 0; object_ids && keys && i < count; ++i) {
    if (keys[i] > 0) {
      next[object_ids[i]] = keys[i];
    }
  }
  // The objects whose key changed: their surroundings blend differently.
  std::unordered_set<i32> changed;
  for (const auto &[object, key] : weld_keys_) {
    auto it = next.find(object);
    if (it == next.end() || it->second != key) {
      changed.insert(object);
    }
  }
  for (const auto &[object, key] : next) {
    auto it = weld_keys_.find(object);
    if (it == weld_keys_.end() || it->second != key) {
      changed.insert(object);
    }
  }
  if (!changed.empty()) {
    end_drag_instance(); // the instance was folded with the old keys
    ++weld_generation_;
  }
  weld_keys_ = std::move(next);
  context_.raymarch_shader->set_object_weld_keys(weld_keys_);
  if (changed.empty()) {
    return;
  }
  const std::vector<SculptStamp> &stamps = sculpt_system_.volume().stamps();
  std::vector<size_t> touched;
  for (size_t i = 0; i < stamps.size(); ++i) {
    if (sculpt_op_adds(stamps[i].op) && changed.count(stamps[i].object_id)) {
      touched.push_back(i);
    }
  }
  if (touched.empty()) {
    return; // e.g. a new, still empty stroke object
  }
  context_.raymarch_shader->recompute_shapes(stamp_footprints(stamps, touched, weld_keys_));
  // Same reasons as transform_objects().
  context_.raymarch_shader->invalidate_undo_history();
  redo_stack_.clear();
}

void VulkanRendererBackend::build_raycast_runs() const {
  const SculptVolume &volume = sculpt_system_.volume();
  if (raycast_runs_generation_ == volume.generation() &&
      raycast_runs_weld_generation_ == weld_generation_) {
    return;
  }
  raycast_runs_generation_ = volume.generation();
  raycast_runs_weld_generation_ = weld_generation_;
  raycast_runs_.clear();
  const std::vector<SculptStamp> &stamps = volume.stamps();
  // A run per object, or per weld block (keys offset past every object id).
  std::unordered_map<i64, size_t> run_index;
  constexpr i64 kBlockRun = i64(1) << 32;
  for (size_t i = 0; i < stamps.size(); ++i) {
    const SculptStamp &stamp = stamps[i];
    if (stamp.op == SculptBrushOp::ObjectPaint) {
      continue; // no geometry
    }
    const bool scoped = stamp.op != SculptBrushOp::Union &&
                        stamp.op != SculptBrushOp::Subtraction;
    size_t r;
    if (!scoped) {
      r = raycast_runs_.size();
      raycast_runs_.push_back({});
      raycast_runs_[r].scoped = false;
    } else {
      auto key = weld_keys_.find(stamp.object_id);
      const i64 id = key == weld_keys_.end() ? i64(stamp.object_id)
                                             : kBlockRun + key->second;
      auto [it, added] = run_index.try_emplace(id, raycast_runs_.size());
      if (added) {
        raycast_runs_.push_back({});
      }
      r = it->second;
    }
    RaycastRun &run = raycast_runs_[r];
    const glm::vec3 reach(approximate_stamp_radius(stamp) + 0.01f);
    if (run.stamps.empty()) {
      run.lo = stamp.center - reach;
      run.hi = stamp.center + reach;
    } else {
      run.lo = glm::min(run.lo, stamp.center - reach);
      run.hi = glm::max(run.hi, stamp.center + reach);
    }
    run.stamps.push_back(static_cast<u32>(i));
  }
}

i32 VulkanRendererBackend::raycast_objects(glm::vec3 origin,
                                           glm::vec3 direction, f32 max_t,
                                           const i32 *object_ids, i32 count,
                                           f32 *out_t, i32 stamp_limit) const {
  // object_ids null: every object counts.
  auto included = [&](i32 object_id) {
    return !object_ids ||
           std::find(object_ids, object_ids + count, object_id) !=
               object_ids + count;
  };
  const std::vector<SculptStamp> &stamps = sculpt_system_.volume().stamps();
  using Shape = VulkanRaymarchShader::BrushShape;
  // Distance from p to a stamp's shape -- or, when p is well outside its
  // bounding sphere, to that sphere: never more than the true distance, so
  // a step by it can't overshoot, and far cheaper.
  auto stamp_distance = [](const SculptStamp &stamp, glm::vec3 p) {
    const f32 to_bound =
        glm::length(p - stamp.center) - approximate_stamp_radius(stamp);
    // Exact within its blend's reach, so a smooth blend below is right.
    if (to_bound > 0.05f + std::max(stamp.smoothness, 0.0f)) {
      return to_bound;
    }
    return VulkanRaymarchShader::eval_shape_sdf_cpu(
        static_cast<Shape>(stamp.shape_type), stamp.params,
        glm::conjugate(stamp.rotation) * (p - stamp.center));
  };
  // Sphere tracing against the stamps folded as the sculpt folds them
  // (Builtin.SculptChunkRecompute.comp.glsl): object-scoped adds and
  // carves with their smoothness (so a snap lands on a soft blend's bulge,
  // not inside it), crops; plain Union is a hard min and plain Subtraction
  // carves are ignored (their holes still read as solid), as before.
  // Polynomial smooth min / subtraction, as Builtin.BrushShapes.inc.glsl.
  auto smooth_min = [](f32 a, f32 b, f32 k, bool *a_wins) {
    if (k <= 0.0001f || std::fabs(a - b) >= k) {
      *a_wins = a < b;
      return std::min(a, b);
    }
    const f32 h = std::clamp(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
    *a_wins = h >= 0.5f;
    return b + (a - b) * h - k * h * (1.0f - h);
  };
  auto smooth_cut = [](f32 cutter, f32 base, f32 k) {
    if (k <= 0.0001f || std::fabs(base + cutter) >= k) {
      return std::max(-cutter, base);
    }
    const f32 h = std::clamp(0.5f - 0.5f * (base + cutter) / k, 0.0f, 1.0f);
    return base + (-cutter - base) * h + k * h * (1.0f - h);
  };
  constexpr int kMaxSteps = 256;
  constexpr f32 kHitEpsilon = 0.005f;
  constexpr f32 kEmpty = std::numeric_limits<f32>::max();
  auto is_scoped = [](const SculptStamp &stamp) {
    return stamp.op != SculptBrushOp::Union &&
           stamp.op != SculptBrushOp::Subtraction;
  };

  // Only the stamps the ray passes through can be hit or change what is
  // hit (a crop is the exception: it removes all of its object outside
  // it). Tracing just those, rather than every stamp at every step, is
  // what keeps this cheap enough to run every frame in a big scene --
  // dropping a stamp the ray never reaches can't make a step overshoot
  // anything it does reach. They come in runs (build_raycast_runs()), in
  // fold order: each object's scoped stamps together, a weld block's (a
  // sculpture's strokes and its carves) together -- as the recompute folds
  // them -- and a ray only looks inside the runs whose box it crosses.
  // stamp_limit: the stamps made since don't count (they're appended).
  const size_t stamp_end =
      stamp_limit < 0 ? stamps.size()
                      : std::min(stamps.size(), static_cast<size_t>(stamp_limit));
  build_raycast_runs();
  struct Traced {
    const SculptStamp *stamp;
    i32 run;
  };
  std::vector<Traced> traced;
  const glm::vec3 inv_dir(
      std::fabs(direction.x) > 1e-12f ? 1.0f / direction.x : 1e12f,
      std::fabs(direction.y) > 1e-12f ? 1.0f / direction.y : 1e12f,
      std::fabs(direction.z) > 1e-12f ? 1.0f / direction.z : 1e12f);
  for (size_t r = 0; r < raycast_runs_.size(); ++r) {
    const RaycastRun &run = raycast_runs_[r];
    // Slab test of the ray segment [0, max_t] against the run's box.
    const glm::vec3 t0 = (run.lo - origin) * inv_dir;
    const glm::vec3 t1 = (run.hi - origin) * inv_dir;
    const glm::vec3 near = glm::min(t0, t1);
    const glm::vec3 far = glm::max(t0, t1);
    const f32 enter = std::max({near.x, near.y, near.z, 0.0f});
    const f32 leave = std::min({far.x, far.y, far.z, max_t});
    if (enter > leave) {
      continue;
    }
    const size_t first = traced.size();
    bool any_visible = false;
    for (u32 i : run.stamps) {
      if (i >= stamp_end) {
        continue;
      }
      const SculptStamp &stamp = stamps[i];
      // An object left out still has its carves cut the rest of its
      // sculpture; only its own shapes go.
      const bool counts =
          included(stamp.object_id) ||
          (run.scoped && !sculpt_op_visible(stamp.op) &&
           weld_keys_.count(stamp.object_id));
      bool on_ray = false;
      if (counts) {
        const f32 along = std::clamp(glm::dot(stamp.center - origin, direction),
                                     0.0f, max_t);
        const f32 radius = approximate_stamp_radius(stamp) + kHitEpsilon;
        const glm::vec3 offset = origin + direction * along - stamp.center;
        on_ray = glm::dot(offset, offset) <= radius * radius;
      }
      if (on_ray && (sculpt_op_visible(stamp.op) || !run.scoped)) {
        any_visible = true;
      }
      if (on_ray || (run.scoped && stamp.op == SculptBrushOp::ObjectIntersection &&
                     (included(stamp.object_id) ||
                      weld_keys_.count(stamp.object_id)))) {
        traced.push_back({&stamp, static_cast<i32>(r)});
      }
    }
    if (!any_visible) {
      traced.resize(first); // nothing of this run on the ray to hit
    }
  }
  if (traced.empty()) {
    return -1;
  }

  f32 t = 0.0f;
  for (int step = 0; step < kMaxSteps && t <= max_t; ++step) {
    const glm::vec3 p = origin + direction * t;
    f32 nearest = kEmpty;
    i32 nearest_object = -1;
    bool in_run = false;
    i32 run_id = -1;
    i32 run_object = -1; // the object whose shape is nearest in the run
    f32 run = kEmpty;
    auto end_run = [&] {
      if (in_run && run < nearest) {
        nearest = run;
        nearest_object = run_object;
      }
      in_run = false;
    };
    for (const Traced &entry : traced) {
      const SculptStamp &stamp = *entry.stamp;
      const bool scoped = is_scoped(stamp);
      if (in_run && (!scoped || entry.run != run_id)) {
        end_run();
      }
      if (scoped && !in_run) {
        in_run = true;
        run_id = entry.run;
        run_object = -1;
        run = kEmpty;
      }
      switch (stamp.op) {
      case SculptBrushOp::Union: {
        const f32 d = stamp_distance(stamp, p);
        if (d < nearest) {
          nearest = d;
          nearest_object = stamp.object_id;
        }
        break;
      }
      case SculptBrushOp::ObjectUnion: {
        const f32 d = stamp_distance(stamp, p);
        if (run == kEmpty) {
          run = d;
          run_object = stamp.object_id;
        } else {
          bool wins = false;
          run = smooth_min(d, run, stamp.smoothness, &wins);
          if (wins) {
            run_object = stamp.object_id;
          }
        }
        break;
      }
      case SculptBrushOp::Fleck: { // a small sphere, hard
        const f32 d = stamp_distance(stamp, p);
        if (d < run) {
          run = d;
          run_object = stamp.object_id;
        }
        break;
      }
      case SculptBrushOp::ObjectSubtraction:
        // Only carves where p is inside the cutter (d < 0) or within its
        // rounding; further out the bound is a fine stand-in.
        if (run != kEmpty) {
          run = smooth_cut(stamp_distance(stamp, p), run, stamp.smoothness);
        }
        break;
      case SculptBrushOp::ObjectIntersection:
        if (run != kEmpty) {
          run = std::max(run, stamp_distance(stamp, p));
        }
        break;
      default: // Subtraction (ignored, see above), ObjectPaint (no geometry)
        break;
      }
    }
    end_run();
    if (nearest_object < 0) {
      return -1; // nothing to hit
    }
    if (nearest < kHitEpsilon) {
      if (out_t) {
        *out_t = t;
      }
      return nearest_object;
    }
    t += nearest;
  }
  return -1;
}

// Indices of every stamp belonging to any of `object_ids`, in list order.
// False (with a warning) if one is past the GPU stamp mirror's cap, since
// its region can't be re-run.
bool VulkanRendererBackend::collect_object_stamps(
    const i32 *object_ids, i32 count, const char *caller,
    std::vector<size_t> *out_members) const {
  const std::vector<SculptStamp> &stamps = sculpt_system_.volume().stamps();
  const i32 mirror_count = context_.raymarch_shader->gpu_stamp_mirror_count();
  out_members->clear();
  for (size_t i = 0; i < stamps.size(); ++i) {
    if (std::find(object_ids, object_ids + count, stamps[i].object_id) ==
        object_ids + count) {
      continue;
    }
    if (static_cast<i32>(i) >= mirror_count) {
      KWARN("{} refused -- stamp {} is past the GPU stamp-list mirror's "
            "cap, so it can't be re-run.",
            caller, i);
      return false;
    }
    out_members->push_back(i);
  }
  return true;
}

bool VulkanRendererBackend::transform_object(i32 object_id, glm::vec3 pivot,
                                             glm::quat rotation,
                                             glm::vec3 translation,
                                             f32 scale) {
  return transform_objects(&object_id, 1, pivot, rotation, translation,
                           scale);
}

bool VulkanRendererBackend::transform_objects(const i32 *object_ids,
                                              i32 count, glm::vec3 pivot,
                                              glm::quat rotation,
                                              glm::vec3 translation,
                                              f32 scale) {
  if (!(scale > 0.0f) || !object_ids || count <= 0) { // also rejects NaN
    return false;
  }
  // A copy: the loop below updates the live list.
  const std::vector<SculptStamp> stamps = sculpt_system_.volume().stamps();
  std::vector<size_t> members;
  if (!collect_object_stamps(object_ids, count, "transform_objects()",
                             &members) ||
      members.empty()) {
    return false;
  }

  rotation = glm::normalize(rotation);
  using Shape = VulkanRaymarchShader::BrushShape;
  using Op = VulkanRaymarchShader::BrushOp;
  std::vector<VulkanRaymarchShader::RecomputeShape> footprints;
  footprints.reserve(members.size() * 2);

  // Which moved objects need their cells padded for welded fillets. A
  // fillet lies inside the bounds of the stroke that makes it, so when a
  // sculpture's welded pieces all move together their own footprints
  // already cover every fillet; only a piece moved away from welded
  // neighbours that stay put needs the pad (their fillets onto it change).
  const std::unordered_set<i32> moved(object_ids, object_ids + count);
  std::unordered_set<i32> split_keys; // weld keys with a piece left behind
  for (const auto &[object, key] : weld_keys_) {
    if (!moved.count(object)) {
      split_keys.insert(key);
    }
  }
  auto needs_weld_pad = [&](i32 object) {
    auto it = weld_keys_.find(object);
    return it != weld_keys_.end() && split_keys.count(it->second) != 0;
  };

  // A drag of whole sculptures (nothing welded to them left behind, no
  // scene-wide ops among them) goes through the drag instance: built once,
  // then only its transform changes. Anything else re-sculpts as it moves.
  VulkanRaymarchShader &shader = *context_.raymarch_shader;
  if (shader.drag_instance_settling()) {
    end_drag_instance(); // the last drag's objects back in the world first
  }
  bool instanced = false;
  if (drag_preview_) {
    bool eligible = true;
    for (i32 object : moved) {
      if (needs_weld_pad(object)) {
        eligible = false;
      }
    }
    for (size_t i : members) {
      const SculptBrushOp op = stamps[i].op;
      if (op == SculptBrushOp::Union || op == SculptBrushOp::Subtraction) {
        eligible = false;
      }
    }
    if (shader.drag_instance_active() &&
        (!eligible || shader.drag_instance_objects() != moved)) {
      end_drag_instance();
    }
    if (eligible && !shader.drag_instance_active() &&
        shader.begin_drag_instance(
            moved, surface_footprints(sculpt_system_.volume().stamps(), moved))) {
      drag_instance_m_ = glm::mat3(1.0f);
      drag_instance_t_ = glm::vec3(0.0f);
      drag_instance_scale_ = 1.0f;
    }
    instanced = eligible && shader.drag_instance_active();
  } else {
    end_drag_instance();
  }
  for (size_t i : members) {
    const SculptStamp &stamp = stamps[i];
    const glm::vec3 new_center =
        pivot + scale * (rotation * (stamp.center - pivot)) + translation;
    const glm::quat new_rotation = glm::normalize(rotation * stamp.rotation);
    // Every shape param is a length (radius, half-extent, height, corner
    // radius, edge thickness -- see stamp_shape()'s param table), and so is
    // the smooth-blend radius, so a uniform scale multiplies them all.
    glm::vec4 new_params = stamp.params * scale;
    // ...except a section code (params.w on the round shapes -- see
    // Builtin.BrushShapes.inc.glsl's modifier notes), which is a count.
    const i32 shape_id = static_cast<i32>(stamp.shape_type);
    if (shape_id == 0 || shape_id == 3 || shape_id == 4 || shape_id == 5 ||
        (shape_id >= 8 && shape_id <= 14)) {
      new_params.w = stamp.params.w;
    }
    const f32 new_smoothness = stamp.smoothness * scale;
    const auto shape = static_cast<Shape>(stamp.shape_type);

    context_.raymarch_shader->sculpt_stamp_list_update(
        static_cast<i32>(i), new_center, shape, new_params, new_smoothness,
        static_cast<Op>(stamp.op), stamp.material_slot, stamp.looseness,
        new_rotation, stamp.object_id);
    sculpt_system_.update_stamp_transform(i, new_center, new_rotation,
                                          new_params, new_smoothness,
                                          stamp.cells_touched);

    // Both poses: the old one so the cells it vacated are cleared, the new
    // one so its new cells are filled. (A fleck has no cells.)
    if (stamp.op != SculptBrushOp::Fleck && !instanced) {
      const bool adds =
          sculpt_op_adds(stamp.op) && needs_weld_pad(stamp.object_id);
      footprints.push_back({stamp.center, shape, stamp.params,
                            stamp.smoothness, stamp.rotation, adds});
      footprints.push_back(
          {new_center, shape, new_params, new_smoothness, new_rotation, adds});
    }
  }

  if (instanced) {
    // Drawn moved: this move after the ones before it.
    const glm::mat3 a = scale * glm::mat3_cast(rotation);
    const glm::vec3 b = pivot - a * pivot + translation;
    drag_instance_m_ = a * drag_instance_m_;
    drag_instance_t_ = a * drag_instance_t_ + b;
    drag_instance_scale_ *= scale;
    shader.set_drag_instance_transform(drag_instance_m_, drag_instance_t_,
                                       drag_instance_scale_);
    shader.invalidate_undo_history();
    redo_stack_.clear();
    return true;
  }

  // Mid-drag: half resolution, redone in full when the drag ends.
  if (drag_preview_) {
    coarse_objects_.insert(object_ids, object_ids + count);
  }
  context_.raymarch_shader->set_coarse_recompute(drag_preview_);
  context_.raymarch_shader->recompute_shapes(footprints);
  context_.raymarch_shader->set_coarse_recompute(false);

  // Same as move_edit(): undo snapshots captured before this no longer
  // describe the field, and redo entries would replay stale poses.
  context_.raymarch_shader->invalidate_undo_history();
  redo_stack_.clear();
  release_unused_sculpt_chunks();
  return true;
}

bool VulkanRendererBackend::delete_objects(const i32 *object_ids,
                                           i32 count) {
  end_drag_instance_for(object_ids, count);
  if (!object_ids || count <= 0) {
    return false;
  }
  std::vector<size_t> members;
  if (!collect_object_stamps(object_ids, count, "delete_objects()",
                             &members) ||
      members.empty()) {
    return false;
  }
  // Taken before the erase: a deleted crop gives back what it removed, so
  // the whole of what it cropped is re-run (see stamp_footprints()).
  const std::vector<VulkanRaymarchShader::RecomputeShape> footprints =
      stamp_footprints(sculpt_system_.volume().stamps(), members, weld_keys_);
  // Same order as delete_edit(): both lists first (recompute reads the GPU
  // mirror as ground truth), back to front so earlier indices stay valid.
  for (auto it = members.rbegin(); it != members.rend(); ++it) {
    context_.raymarch_shader->sculpt_stamp_list_erase(static_cast<i32>(*it));
    sculpt_system_.remove_stamp_at(*it);
  }
  context_.raymarch_shader->recompute_shapes(footprints);

  // Same reasons as delete_edit().
  context_.raymarch_shader->invalidate_undo_history();
  redo_stack_.clear();
  release_unused_sculpt_chunks();
  return true;
}

void VulkanRendererBackend::set_outlined_objects(const i32 *object_ids,
                                                 i32 count, glm::vec4 colour,
                                                 const i32 *outline_groups) {
  count = std::max(count, 0);
  outlined_objects_.assign(object_ids, object_ids + count);
  outline_keys_.resize(static_cast<size_t>(count));
  for (i32 i = 0; i < count; ++i) {
    // Without keys every object is its own outline; negative keys can't
    // collide with a caller's own (non-negative) ones.
    outline_keys_[static_cast<size_t>(i)] =
        outline_groups ? outline_groups[i] : -1 - i;
  }
  outline_colour_ = colour;
}

void VulkanRendererBackend::set_scope_objects(const i32 *object_ids,
                                              i32 count) {
  scope_objects_.assign(object_ids, object_ids + std::max(count, 0));
}

bool VulkanRendererBackend::set_object_material(const i32 *object_ids,
                                                i32 count, i32 slot) {
  end_drag_instance_for(object_ids, count);
  std::vector<size_t> members;
  if (!object_ids || count <= 0 ||
      !collect_object_stamps(object_ids, count, "set_object_material()",
                             &members)) {
    return false;
  }
  const std::vector<SculptStamp> &stamps = sculpt_system_.volume().stamps();
  std::vector<VulkanRaymarchShader::RecomputeShape> footprints;
  bool changed_any = false;
  for (size_t i : members) {
    const SculptStamp &stamp = stamps[i];
    if (stamp.material_slot == slot) {
      continue;
    }
    const auto shape =
        static_cast<VulkanRaymarchShader::BrushShape>(stamp.shape_type);
    context_.raymarch_shader->sculpt_stamp_list_update(
        static_cast<i32>(i), stamp.center, shape, stamp.params,
        stamp.smoothness, static_cast<VulkanRaymarchShader::BrushOp>(stamp.op),
        slot, stamp.looseness, stamp.rotation, stamp.object_id);
    if (stamp.op != SculptBrushOp::Fleck) { // a fleck has no cells
      footprints.push_back(
          {stamp.center, shape, stamp.params, stamp.smoothness, stamp.rotation});
    }
    sculpt_system_.set_stamp_material(i, slot);
    changed_any = true;
  }
  if (!changed_any) {
    return false;
  }
  context_.raymarch_shader->recompute_shapes(footprints);
  // Same reasons as transform_object().
  context_.raymarch_shader->invalidate_undo_history();
  redo_stack_.clear();
  return true;
}

bool VulkanRendererBackend::remap_object_materials(const i32 *object_ids,
                                                   i32 count,
                                                   const i32 *from_slots,
                                                   const i32 *to_slots,
                                                   i32 pairs) {
  end_drag_instance_for(object_ids, count);
  std::vector<size_t> members;
  if (!collect_object_stamps(object_ids, count, "remap_object_materials()",
                             &members)) {
    return false;
  }
  const std::vector<SculptStamp> &stamps = sculpt_system_.volume().stamps();
  std::vector<size_t> changed;
  for (size_t i : members) {
    const SculptStamp &stamp = stamps[i];
    const i32 *from = std::find(from_slots, from_slots + pairs,
                                stamp.material_slot);
    if (from == from_slots + pairs) {
      continue;
    }
    const i32 slot = to_slots[from - from_slots];
    if (slot == stamp.material_slot) {
      continue;
    }
    context_.raymarch_shader->sculpt_stamp_list_update(
        static_cast<i32>(i), stamp.center,
        static_cast<VulkanRaymarchShader::BrushShape>(stamp.shape_type),
        stamp.params, stamp.smoothness,
        static_cast<VulkanRaymarchShader::BrushOp>(stamp.op), slot,
        stamp.looseness, stamp.rotation, stamp.object_id);
    sculpt_system_.set_stamp_material(i, slot);
    changed.push_back(i);
  }
  if (changed.empty()) {
    return false;
  }
  context_.raymarch_shader->recompute_shapes(stamp_footprints(stamps, changed, weld_keys_));
  // Same reasons as transform_object().
  context_.raymarch_shader->invalidate_undo_history();
  redo_stack_.clear();
  return true;
}

i32 VulkanRendererBackend::object_stamps(i32 object_id,
                                         SculptStampInfo *out_stamps,
                                         i32 capacity) const {
  i32 count = 0;
  for (const SculptStamp &stamp : sculpt_system_.volume().stamps()) {
    if (stamp.object_id != object_id) {
      continue;
    }
    if (out_stamps && count < capacity) {
      SculptStampInfo &info = out_stamps[count];
      info.center = stamp.center;
      info.shape_type = stamp.shape_type;
      info.params = stamp.params;
      info.smoothness = stamp.smoothness;
      info.op = stamp.op;
      info.material_slot = stamp.material_slot;
      info.looseness = stamp.looseness;
      info.rotation = stamp.rotation;
    }
    ++count;
  }
  return count;
}

i32 VulkanRendererBackend::all_stamps(SculptStampInfo *out_stamps,
                                      i32 *out_objects, i32 capacity) const {
  const std::vector<SculptStamp> &stamps = sculpt_system_.volume().stamps();
  for (i32 i = 0; i < capacity && i < static_cast<i32>(stamps.size()); ++i) {
    const SculptStamp &stamp = stamps[static_cast<size_t>(i)];
    if (out_stamps) {
      SculptStampInfo &info = out_stamps[i];
      info.center = stamp.center;
      info.shape_type = stamp.shape_type;
      info.params = stamp.params;
      info.smoothness = stamp.smoothness;
      info.op = stamp.op;
      info.material_slot = stamp.material_slot;
      info.looseness = stamp.looseness;
      info.rotation = stamp.rotation;
    }
    if (out_objects) {
      out_objects[i] = stamp.object_id;
    }
  }
  return static_cast<i32>(stamps.size());
}

i32 VulkanRendererBackend::object_ids(i32 *out_ids, i32 capacity) const {
  std::vector<i32> ids;
  for (const SculptStamp &stamp : sculpt_system_.volume().stamps()) {
    if (std::find(ids.begin(), ids.end(), stamp.object_id) == ids.end()) {
      ids.push_back(stamp.object_id);
    }
  }
  for (i32 i = 0; out_ids && i < capacity && i < static_cast<i32>(ids.size());
       ++i) {
    out_ids[i] = ids[static_cast<size_t>(i)];
  }
  return static_cast<i32>(ids.size());
}

bool VulkanRendererBackend::set_object_looseness(const i32 *object_ids,
                                                 i32 count, f32 looseness) {
  end_drag_instance_for(object_ids, count);
  std::vector<size_t> members;
  if (!object_ids || count <= 0 ||
      !collect_object_stamps(object_ids, count, "set_object_looseness()",
                             &members)) {
    return false;
  }
  looseness = std::clamp(looseness, 0.0f, 1.0f);
  const std::vector<SculptStamp> &stamps = sculpt_system_.volume().stamps();
  std::vector<VulkanRaymarchShader::RecomputeShape> footprints;
  for (size_t i : members) {
    const SculptStamp &stamp = stamps[i];
    // A paint stamp's looseness is its opacity (see ObjectPaint).
    if (stamp.looseness == looseness || stamp.op == SculptBrushOp::ObjectPaint ||
        stamp.op == SculptBrushOp::Fleck) {
      continue;
    }
    const auto shape =
        static_cast<VulkanRaymarchShader::BrushShape>(stamp.shape_type);
    context_.raymarch_shader->sculpt_stamp_list_update(
        static_cast<i32>(i), stamp.center, shape, stamp.params,
        stamp.smoothness, static_cast<VulkanRaymarchShader::BrushOp>(stamp.op),
        stamp.material_slot, looseness, stamp.rotation, stamp.object_id);
    footprints.push_back(
        {stamp.center, shape, stamp.params, stamp.smoothness, stamp.rotation});
    sculpt_system_.set_stamp_looseness(i, looseness);
  }
  if (footprints.empty()) {
    return false;
  }
  context_.raymarch_shader->recompute_shapes(footprints);
  // Same reasons as transform_object().
  context_.raymarch_shader->invalidate_undo_history();
  redo_stack_.clear();
  return true;
}

void VulkanRendererBackend::set_material_flecks(i32 slot, f32 looseness,
                                                i32 style, f32 comb,
                                                f32 ruffle, f32 impasto,
                                                f32 neatness) {
  context_.raymarch_shader->set_material_flecks(slot, looseness, style, comb,
                                                ruffle, impasto, neatness);
}

void VulkanRendererBackend::set_material_effects(i32 slot, f32 boil, f32 flow,
                                                 f32 wave, f32 evaporate,
                                                 f32 throb) {
  context_.raymarch_shader->set_material_effects(slot, boil, flow, wave,
                                                 evaporate, throb);
}

void VulkanRendererBackend::set_material_glow(i32 slot, f32 glow) {
  context_.raymarch_shader->set_material_glow(slot, glow);
}

void VulkanRendererBackend::update_selection_outline() {
  // Dot spacing 8px, half of it dot, a 2.5px line, 35% alpha where hidden.
  const glm::vec4 style(8.0f, 0.5f, 2.5f, 0.35f);
  // One outline per distinct key, in first-seen order, then the scope set.
  std::vector<i32> keys;
  for (i32 key : outline_keys_) {
    if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
      keys.push_back(key);
    }
  }
  std::vector<VulkanRaymarchShader::OutlineObject> objects(keys.size());
  VulkanRaymarchShader::OutlineObject scope;
  scope.scope_mask = true;
  for (const SculptStamp &stamp : sculpt_system_.volume().stamps()) {
    const VulkanRaymarchShader::OutlineStamp outline_stamp{
        stamp.center,
        static_cast<VulkanRaymarchShader::BrushShape>(stamp.shape_type),
        stamp.params,
        stamp.smoothness,
        stamp.rotation,
        static_cast<VulkanRaymarchShader::BrushOp>(stamp.op),
        approximate_stamp_radius(stamp),
        stamp.object_id};
    const auto it = std::find(outlined_objects_.begin(),
                              outlined_objects_.end(), stamp.object_id);
    if (it != outlined_objects_.end()) {
      const i32 key =
          outline_keys_[static_cast<size_t>(it - outlined_objects_.begin())];
      const size_t index = static_cast<size_t>(
          std::find(keys.begin(), keys.end(), key) - keys.begin());
      objects[index].stamps.push_back(outline_stamp);
    }
    if (std::find(scope_objects_.begin(), scope_objects_.end(),
                  stamp.object_id) != scope_objects_.end()) {
      scope.stamps.push_back(outline_stamp);
    }
  }
  if (!scope_objects_.empty()) {
    // Even with no stamps yet (a new, empty sculpture): then everything is
    // out of focus.
    objects.push_back(std::move(scope));
  }
  context_.raymarch_shader->set_selection_outline(objects, outline_colour_,
                                                  style, scope_objects_);
}

void VulkanRendererBackend::release_unused_sculpt_chunks() {
  std::vector<VulkanRaymarchShader::RecomputeShape> live;
  const std::vector<SculptStamp> &stamps = sculpt_system_.volume().stamps();
  live.reserve(stamps.size());
  for (const SculptStamp &stamp : stamps) {
    live.push_back({stamp.center,
                    static_cast<VulkanRaymarchShader::BrushShape>(stamp.shape_type),
                    stamp.params, stamp.smoothness, stamp.rotation});
  }
  context_.raymarch_shader->release_unused_sculpt_chunks(live);
}

i32 VulkanRendererBackend::clone_object(i32 object_id) {
  std::vector<SculptStampInfo> copies(
      static_cast<size_t>(object_stamps(object_id, nullptr, 0)));
  if (copies.empty()) {
    return -1;
  }
  object_stamps(object_id, copies.data(), static_cast<i32>(copies.size()));
  // Rebuilt from the list rather than blended in stamp by stamp, so an
  // object built from object-scoped stamps copies exactly.
  const i32 clone_id = next_object_id_++;
  add_stamps(clone_id, copies.data(), static_cast<i32>(copies.size()));
  return clone_id;
}

bool VulkanRendererBackend::object_anchor(i32 object_id, glm::vec3 *out_center,
                                          glm::quat *out_rotation) const {
  for (const SculptStamp &stamp : sculpt_system_.volume().stamps()) {
    if (stamp.object_id == object_id) {
      *out_center = stamp.center;
      *out_rotation = stamp.rotation;
      return true;
    }
  }
  return false;
}

bool VulkanRendererBackend::move_edit(i32 index, glm::vec3 new_center) {
  end_drag_instance(); // see end_drag_instance()
  // M5 (checkpoint-removal redesign) -- see delete_edit()'s own comment
  // for the full picture. Only the UNION of the stamp's OLD and NEW AABBs
  // can possibly change: nothing outside the old position ever held this
  // stamp's geometry, and nothing outside the new position will.
  const std::vector<SculptStamp> stamps = sculpt_system_.volume().stamps();
  if (index < 0 || static_cast<size_t>(index) >= stamps.size()) {
    return false;
  }
  if (index >= context_.raymarch_shader->gpu_stamp_mirror_count()) {
    KWARN("move_edit({}) refused -- past the GPU stamp-list mirror's cap "
         "(see VulkanRaymarchShader::gpu_stamp_mirror_count()'s own "
         "comment); this edit can no longer be moved via the recompute "
         "path.",
         index);
    return false;
  }

  // Captured BEFORE any mutation below -- everything except the center
  // (shape_type/params/smoothness/op/material_slot/looseness) is read
  // verbatim from this EXISTING record, matching move_edit()'s own "same
  // shape/params/smoothness/op/material/looseness, only the center
  // changes" contract.
  const SculptStamp original = stamps[static_cast<size_t>(index)];
  const auto vulkan_shape =
      static_cast<VulkanRaymarchShader::BrushShape>(original.shape_type);
  const auto vulkan_op =
      static_cast<VulkanRaymarchShader::BrushOp>(original.op);

  // Mutate the GPU mirror in place FIRST (recompute reads it as ground
  // truth) -- unlike delete_edit()'s erase-compact, this leaves the
  // mirror's size/order untouched, same as SculptVolume::update_stamp_
  // center()'s own CPU-side shape.
  context_.raymarch_shader->sculpt_stamp_list_update(
      index, new_center, vulkan_shape, original.params, original.smoothness,
      vulkan_op, original.material_slot, original.looseness,
      original.rotation, original.object_id);

  // One combined dispatch over the UNION of the old-center and new-center
  // cell sets (deduplicated -- see VulkanRaymarchShader::
  // recompute_moved_region()'s own comment for why a cell both positions
  // happen to share must only be recomputed once, not twice).
  const i32 cells_touched = context_.raymarch_shader->recompute_moved_region(
      original.center, new_center, vulkan_shape, original.params,
      original.smoothness, original.rotation);

  // Only now update the CPU-side record -- same "GPU first" discipline
  // delete_edit()/undo_stamp() already follow. cells_touched here is
  // "however many cells this recompute touched around the move" (which can
  // include cells OTHER stamps also happen to overlap), a slightly
  // different meaning than the live stamp_shape() path's own return value
  // -- harmless, since SculptStamp::cells_touched is documented as purely
  // informational (nothing reads it back).
  sculpt_system_.update_stamp_center(static_cast<size_t>(index), new_center,
                                     cells_touched);

  // Same reasoning as delete_edit()'s own call to this -- the recompute
  // above can churn brick indices, so any undo_stack_ entry still
  // referencing one by number is now unsafe to restore.
  context_.raymarch_shader->invalidate_undo_history();

  // A structural edit-list change invalidates any pending redo history --
  // same reasoning delete_edit()'s own tail comment gives.
  redo_stack_.clear();
  release_unused_sculpt_chunks();

  return true;
}

i32 VulkanRendererBackend::crop_volume(glm::vec3 center,
                                       SculptBrushShape shape_type,
                                       glm::vec4 params, f32 smoothness) {
  end_drag_instance(); // see end_drag_instance()
  // Straight pass-through, same shape-enum cast stamp_shape() uses --
  // deliberately does NOT go through sculpt_system_/redo_stack_ (Crop is
  // not a brush stamp and is not undoable/redoable this pass -- see
  // VulkanRaymarchShader::crop_volume()'s own header comment for the full
  // documented scope decision).
  return context_.raymarch_shader->crop_volume(
      center, static_cast<VulkanRaymarchShader::BrushShape>(shape_type),
      params, smoothness, /*keep_outside=*/false);
}

bool VulkanRendererBackend::cutout_volume(glm::vec3 center,
                                          SculptBrushShape shape_type,
                                          glm::vec4 params, f32 smoothness,
                                          const std::string &removed_path) {
  const auto vulkan_shape =
      static_cast<VulkanRaymarchShader::BrushShape>(shape_type);
  // Capture-and-save the REMOVED portion FIRST, from the pre-crop state --
  // see VulkanRaymarchShader::export_crop_outside()'s own comment for why
  // this must run before the live crop below, not after. If the file write
  // fails, bail WITHOUT touching the live volume at all (same "never claim
  // more than actually happened" discipline every other sculpt-family call
  // in this backend already follows).
  if (!context_.raymarch_shader->export_crop_outside(
          removed_path, center, vulkan_shape, params, smoothness)) {
    return false;
  }
  context_.raymarch_shader->crop_volume(center, vulkan_shape, params,
                                        smoothness, /*keep_outside=*/false);
  return true;
}

bool VulkanRendererBackend::load_cutout_file(const std::string &path) {
  end_drag_instance(); // see end_drag_instance()
  return context_.raymarch_shader->load_crop_outside_file(path);
}

void VulkanRendererBackend::set_splat_mode(RendererSplatMode mode) {
  context_.raymarch_shader->set_splat_mode(static_cast<SplatMode>(mode));
}

void VulkanRendererBackend::set_bloom_enabled(b8 enabled) {
  context_.raymarch_shader->set_bloom_enabled(enabled);
}

void VulkanRendererBackend::set_vignette_enabled(b8 enabled) {
  context_.raymarch_shader->set_vignette_enabled(enabled);
}

void VulkanRendererBackend::set_pixelation_enabled(b8 enabled) {
  context_.raymarch_shader->set_pixelation_enabled(enabled);
}

void VulkanRendererBackend::set_pixelation_block_size(u32 block_size) {
  context_.raymarch_shader->set_pixelation_block_size(block_size);
}

void VulkanRendererBackend::set_bloom_threshold(f32 threshold) {
  context_.raymarch_shader->set_bloom_threshold(threshold);
}

void VulkanRendererBackend::set_bloom_intensity(f32 intensity) {
  context_.raymarch_shader->set_bloom_intensity(intensity);
}

void VulkanRendererBackend::set_vignette_strength(f32 strength) {
  context_.raymarch_shader->set_vignette_strength(strength);
}

void VulkanRendererBackend::set_vignette_radius(f32 radius) {
  context_.raymarch_shader->set_vignette_radius(radius);
}

void VulkanRendererBackend::set_render_scale(f32 scale) {
  context_.raymarch_shader->set_render_scale(scale);
}

void VulkanRendererBackend::set_font(std::string_view name, f32 pixel_height) {
  context_.text_shader->set_font(name, pixel_height);
}

void VulkanRendererBackend::set_skybox(std::string_view texture_name) {
  context_.raymarch_shader->set_skybox(texture_name);
}

void VulkanRendererBackend::disable_skybox() {
  context_.raymarch_shader->disable_skybox();
}

void VulkanRendererBackend::set_floating_origin_enabled(b8 enabled) {
  floating_origin_enabled_ = enabled;
}

glm::vec3 VulkanRendererBackend::consume_origin_shift() {
  glm::vec3 shift = pending_origin_shift_;
  pending_origin_shift_ = glm::vec3(0.0f);
  return shift;
}

void VulkanRendererBackend::maybe_recenter_origin() {
  if (!floating_origin_enabled_) {
    return;
  }

  glm::vec3 camera_pos = camera_.position();
  if (glm::length(camera_pos) < floating_origin_recenter_threshold_) {
    return;
  }

  // Every render-space position moves by -camera_pos, so the camera itself
  // lands exactly at the render-space origin (where the baked field's fixed
  // [-BOUNDS,BOUNDS] cube is centered); world_origin_offset_ absorbs the
  // opposite correction so render_space + world_origin_offset_ (the true
  // absolute position) is unchanged for everything shifted -- a pure change
  // of representation, not a real move.
  glm::vec3 shift = -camera_pos;
  context_.geometry_system->shift_all(shift);
  camera_.shift(shift);
  world_origin_offset_ -= glm::dvec3(camera_pos);
  pending_origin_shift_ += shift;

  // Every render-space position just moved, so last frame's pixels can no
  // longer be reprojected into this frame's view -- see
  // VulkanRaymarchShader::invalidate_temporal_history().
  context_.raymarch_shader->invalidate_temporal_history();

  KINFO("Floating origin recenter: render space shifted by ({}, {}, {}) "
       "(camera had drifted {} units from origin).",
       shift.x, shift.y, shift.z, glm::length(camera_pos));

  // Today's baked field is one fixed-position [-BOUNDS,BOUNDS] cube in
  // render space (see Builtin.RaymarchVoxelize.comp.glsl) -- its already-
  // baked brick contents are tied to every primitive's *old* render-space
  // position, so (unlike the per-chunk field a later phase introduces,
  // where a resident chunk's baked content is translation-invariant and
  // only its placement metadata needs updating) this recenter needs an
  // ordinary full rebake, exactly like any other scene edit, to re-bake the
  // field against everyone's new render-space positions.
  scene_dirty_ = true;
}

b8 VulkanRendererBackend::end_frame(f32 delta_time) {
  VulkanCommandBuffer *command_buffer =
      context_.graphics_command_buffers[context_.image_index].get();

  // End renderpass
  context_.main_renderpass->end(*command_buffer);

  // Raymarch the sphere into the swapchain image. Must happen outside the
  // render pass instance that just ended (barriers/dispatch aren't valid
  // inside one). Leaves the swapchain image in
  // VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL rather than transitioning
  // straight to present, since the UI pass below still needs to draw on
  // top of it.
  update_selection_outline();
  context_.raymarch_shader->render_to(
      *command_buffer, context_.swapchain.images[context_.image_index],
      context_.framebuffer_width, context_.framebuffer_height, delta_time,
      camera_);

  // UI pass: draws on top of the raymarched scene via LOAD_OP_LOAD (see
  // ui_renderpass's construction) instead of clearing it away. What
  // actually gets drawn is entirely up to the application -- see
  // draw_text()/draw_ui_quad(), queued via the renderer frontend during
  // the game's render() step -- the engine has no hardcoded UI content of
  // its own.
  context_.ui_renderpass->begin(
      *command_buffer, ui_framebuffers_[context_.image_index].handle());
  // Images nothing has drawn for a while give their video memory back --
  // see release_idle_images(). Before this frame's draws, so one queued
  // again this frame is simply re-made.
  context_.ui_shader->release_idle_images();
  // Drawn first of all: a backdrop is what everything else in this pass is
  // meant to sit on -- see draw_backdrop_quad().
  for (const BackdropDrawRequest &request : queued_backdrop_quad_draws_) {
    if (!request.image_path.empty()) {
      context_.ui_shader->render_image_to(
          *command_buffer, context_.framebuffer_width,
          context_.framebuffer_height, request.image_path, request.position,
          request.size, request.colour);
      continue;
    }
    context_.solid_quad_shader->render_to(
        *command_buffer, context_.framebuffer_width,
        context_.framebuffer_height, request.position, request.size,
        request.colour);
  }
  for (const UiQuadDrawRequest &request : queued_ui_quad_draws_) {
    context_.ui_shader->render_to(*command_buffer, context_.framebuffer_width,
                                  context_.framebuffer_height,
                                  request.position, request.size);
  }
  context_.text_shader->begin_batch();
  for (const TextDrawRequest &request : queued_text_draws_) {
    context_.text_shader->render_to(
        *command_buffer, context_.framebuffer_width,
        context_.framebuffer_height, request.text, request.position,
        request.colour, request.scale);
  }
  // Drawn on top of quads/text -- matches its intended use as an
  // always-visible overlay (e.g. tools/sdf_editor's move-gizmo).
  for (const LineDrawRequest &request : queued_line_draws_) {
    context_.line_shader->render_to(*command_buffer, context_.framebuffer_width,
                                    context_.framebuffer_height, request.start,
                                    request.end, request.colour);
  }
  // Drawn last of all: a solid quad is meant to fully occlude whatever's
  // beneath it (e.g. a censor box), so nothing else queued this frame --
  // including lines -- should be able to show through it.
  for (const SolidQuadDrawRequest &request : queued_solid_quad_draws_) {
    context_.solid_quad_shader->render_to(
        *command_buffer, context_.framebuffer_width,
        context_.framebuffer_height, request.position, request.size,
        request.colour);
  }
  context_.ui_renderpass->end(*command_buffer);

  queued_ui_quad_draws_.clear();
  queued_text_draws_.clear();
  queued_line_draws_.clear();
  queued_solid_quad_draws_.clear();
  queued_backdrop_quad_draws_.clear();

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

  // The semaphore(s) to be signaled when the queue is complete. Indexed by
  // image_index, not current_frame -- see queue_complete_semaphores'
  // declaration comment in vulkan_types.inl.
  // Two signals: the per-image binary semaphore the presentation engine
  // waits on, and the monotonic timeline the async compute queue orders
  // itself against (see VulkanContext::graphics_timeline). The value array
  // needs one entry per signal semaphore; the entry for a binary semaphore
  // is ignored.
  const u64 timeline_signal_value = context_.graphics_timeline_value + 1;
  VkSemaphore signal_semaphores[2] = {
      context_.queue_complete_semaphores[context_.image_index],
      context_.graphics_timeline};
  const uint64_t signal_values[2] = {0, timeline_signal_value};
  submit_info.signalSemaphoreCount = 2;
  submit_info.pSignalSemaphores = signal_semaphores;

  // One wait semaphore: the swapchain image must be acquired before
  // anything can render into it.
  //
  // THE CHUNK BAKE IS DELIBERATELY NOT WAITED ON. This used to add update_
  // streaming()'s async submission as a second wait, so render_to()'s
  // chunked-field reads couldn't start before that frame's bake finished.
  // Correct, and the engine's single largest source of stutter: moving the
  // camera generates chunk work nearly every frame, so nearly every frame
  // waited out its own bake -- measured at 600-2100ms mean and a 4.5 second
  // peak on a large scene. The dependency is now resolved by visibility
  // rather than by ordering: nothing an in-flight bake writes is reachable
  // (the chunk table doesn't name its slot, and Builtin.ChunkClusterCull.
  // comp.glsl skips its cluster pages) until the CPU observes that
  // submission's fence and publishes it, which happens in the NEXT frame's
  // update_streaming(). The reverse hazard -- a bake overwriting bricks
  // this frame is still reading -- is unchanged and still handled, by the
  // async submission itself waiting on context_.graphics_timeline.
  VkSemaphore wait_semaphores[1] = {
      context_.image_available_semaphores[context_.current_frame]};
  VkPipelineStageFlags wait_stages[1] = {
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
  submit_info.waitSemaphoreCount = 1;
  submit_info.pWaitSemaphores = wait_semaphores;
  submit_info.pWaitDstStageMask = wait_stages;

  // Wait values are all zero: every wait semaphore here is binary, and the
  // spec ignores the value for those. Only the signal side carries a real
  // timeline value.
  const uint64_t wait_values[1] = {0};
  VkTimelineSemaphoreSubmitInfo timeline_submit{
      VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
  timeline_submit.waitSemaphoreValueCount = submit_info.waitSemaphoreCount;
  timeline_submit.pWaitSemaphoreValues = wait_values;
  timeline_submit.signalSemaphoreValueCount = 2;
  timeline_submit.pSignalSemaphoreValues = signal_values;
  submit_info.pNext = &timeline_submit;

  VkResult result =
      vkQueueSubmit(context_.device.graphics_queue, 1, &submit_info,
                    in_flight_fences_[context_.current_frame].handle());
  if (result == VK_SUCCESS) {
    // Only advance once the submission is actually in flight -- a failed
    // submit signals nothing, and a counter that ran ahead of the last real
    // signal would make the next async wait block forever.
    context_.graphics_timeline_value = timeline_signal_value;
  }
  if (result != VK_SUCCESS) {
    KERROR("vkQueueSubmit failed with result: {}",
           vulkan_result_string(result, TRUE));
    return FALSE;
  }

  command_buffer->update_submitted();
  // End queue submission

  // Give the image back to the swapchain. Same image_index-indexed
  // semaphore signaled by the submit above.
  vulkan_swapchain_present(
      &context_, &context_.swapchain, context_.device.graphics_queue,
      context_.device.present_queue,
      context_.queue_complete_semaphores[context_.image_index],
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

void VulkanRendererBackend::create_queue_complete_semaphores() {
  // See queue_complete_semaphores' declaration comment in vulkan_types.inl
  // for why this is sized to image_count and indexed by image_index rather
  // than mirroring image_available_semaphores' per-frame-in-flight sizing.
  context_.queue_complete_semaphores.resize(context_.swapchain.image_count);
  VkSemaphoreCreateInfo semaphore_create_info{
      VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (u32 i = 0; i < context_.swapchain.image_count; ++i) {
    vkCreateSemaphore(context_.device.logical_device, &semaphore_create_info,
                      context_.allocator,
                      &context_.queue_complete_semaphores[i]);
  }
}

void VulkanRendererBackend::destroy_queue_complete_semaphores() {
  for (VkSemaphore semaphore : context_.queue_complete_semaphores) {
    if (semaphore) {
      vkDestroySemaphore(context_.device.logical_device, semaphore,
                         context_.allocator);
    }
  }
  context_.queue_complete_semaphores.clear();
}

void VulkanRendererBackend::regenerate_framebuffers() {
  framebuffers_.clear();
  framebuffers_.reserve(context_.swapchain.image_count);
  ui_framebuffers_.clear();
  ui_framebuffers_.reserve(context_.swapchain.image_count);

  for (u32 i = 0; i < context_.swapchain.image_count; ++i) {
    // TODO: make attachment list dynamic based on configured attachments
    std::vector<VkImageView> attachments = {
        context_.swapchain.views[i], context_.swapchain.depth_attachment.view};

    framebuffers_.emplace_back(
        context_, *context_.main_renderpass, context_.framebuffer_width,
        context_.framebuffer_height, std::move(attachments));

    // ui_renderpass has no depth attachment (see its clear_flags), so its
    // framebuffer only needs the swapchain colour view.
    std::vector<VkImageView> ui_attachments = {context_.swapchain.views[i]};
    ui_framebuffers_.emplace_back(
        context_, *context_.ui_renderpass, context_.framebuffer_width,
        context_.framebuffer_height, std::move(ui_attachments));
  }
}

b8 VulkanRendererBackend::recreate_swapchain() {
  // If already being recreated, do not try again.
  if (context_.recreating_swapchain) {
    KDEBUG("recreate_swapchain called when already recreating. Booting.");
    return FALSE;
  }

  // Figure out the target size. A recreate triggered purely by an
  // out-of-date swapchain (acquire/present) has no pending resize event,
  // so there is no cached size — keep the current dimensions then.
  b8 resize_pending = context_.framebuffer_size_generation !=
                      context_.framebuffer_size_last_generation;
  u32 new_width =
      resize_pending ? cached_framebuffer_width_ : context_.framebuffer_width;
  u32 new_height =
      resize_pending ? cached_framebuffer_height_ : context_.framebuffer_height;

  // Detect if the window is too small to be drawn to (e.g. minimized).
  if (new_width == 0 || new_height == 0) {
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

  // Command buffers and framebuffers reference the old swapchain's image
  // views, so they must be torn down (RAII, via clear()) *before*
  // vulkan_swapchain_recreate() destroys those views — destroying a view a
  // live framebuffer still references trips
  // VUID-vkDestroyImageView-imageView-01026.
  context_.graphics_command_buffers.clear();
  framebuffers_.clear();
  ui_framebuffers_.clear();

  // vulkan_swapchain_recreate() requeries swapchain support and depth
  // format internally as part of create().
  if (!vulkan_swapchain_recreate(&context_, new_width, new_height,
                                 &context_.swapchain)) {
    // Leave the size generation un-synced and the out-of-date flag set so
    // this is retried next frame — the surface may only be transiently
    // unavailable (e.g. mid-resize).
    KERROR("recreate_swapchain: swapchain recreation failed. Retrying next "
           "frame.");
    context_.recreating_swapchain = FALSE;
    return FALSE;
  }

  // create() adopted the extent actually granted into
  // context_.framebuffer_width/height (which can differ from the
  // new_width/new_height request); just clear the cached request.
  cached_framebuffer_width_ = 0;
  cached_framebuffer_height_ = 0;

  // Update framebuffer size generation and clear the out-of-date flag.
  context_.framebuffer_size_last_generation =
      context_.framebuffer_size_generation;
  context_.swapchain_out_of_date = FALSE;

  // The image count can change across a recreate.
  images_in_flight_.assign(context_.swapchain.image_count, nullptr);

  // queue_complete_semaphores is sized to image_count too (see its
  // declaration comment) and must be rebuilt to match. Safe to destroy the
  // old ones now, the same reasoning as shutdown()'s: vulkan_swapchain_
  // recreate() already destroyed the old VkSwapchainKHR above, which
  // retires the presentation engine's references to them.
  destroy_queue_complete_semaphores();
  create_queue_complete_semaphores();

  context_.main_renderpass->set_render_area(
      0.0f, 0.0f, static_cast<f32>(context_.framebuffer_width),
      static_cast<f32>(context_.framebuffer_height));
  context_.ui_renderpass->set_render_area(
      0.0f, 0.0f, static_cast<f32>(context_.framebuffer_width),
      static_cast<f32>(context_.framebuffer_height));

  regenerate_framebuffers();
  create_commandbuffer();

  // The raymarch shader's output image is sized to match the framebuffer,
  // so it needs recreating too -- unlike the sparse voxel field, which
  // represents world-space scene geometry and doesn't depend on screen
  // resolution at all.
  context_.raymarch_shader->on_resized(context_.framebuffer_width,
                                      context_.framebuffer_height);

  // Clear the recreating flag.
  context_.recreating_swapchain = FALSE;

  return TRUE;
}
