#include "vulkan_backend.h"
#include "../../core/logger.h"

#include "../../platform/platform.h"
#include "vulkan_commandbuffer.h"
#include "vulkan_device.h"
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
#include "../../resources/sdf_scene.h"
#include <glm/gtc/quaternion.hpp>
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

  // Phase 3b: camera-driven chunk streaming -- see VulkanRaymarchShader::
  // update_streaming()'s own comment for why this replaces rebake()'s
  // vkDeviceWaitIdle for the chunked field instead of stalling, and for
  // why it no longer records into this frame's OWN command buffer at all
  // (it submits its own work to async_compute_queue instead). A no-op
  // call (cheap: just tick()+update() with an empty Plan, returning
  // VK_NULL_HANDLE) whenever nothing needs loading/evicting/rebaking this
  // frame, so this is safe to call unconditionally every frame regardless
  // of scene_dirty_ above. Nothing is stashed for end_frame() any more:
  // this frame's graphics submission deliberately does NOT wait on whatever
  // chunk work was just queued -- see update_streaming()'s own comment for
  // how an in-flight bake is kept invisible instead of being waited out.
  context_.raymarch_shader->update_streaming(camera_.position());

  // Phase 5: cascade GI recenter/rebake, recorded AFTER update_streaming()
  // above -- a cascade bake gathers against the chunked field, so it must
  // see this frame's chunk load/evict budget already recorded (same
  // ordering reasoning as update_gi_cascade()'s own comment). A no-op call
  // most frames, same as update_streaming() above.
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
                                      glm::vec2 position, glm::vec4 colour) {
  queued_text_draws_.push_back({std::string(text), position, colour});
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

SceneHandle VulkanRendererBackend::load_scene(std::string_view sdf_path) {
  auto scene = load_sdf_scene(sdf_path);
  if (!scene) {
    // load_sdf_scene() already logged why (missing file).
    return kInvalidSceneHandle;
  }

  // Namespace every registered name under this load's handle -- two .sdf
  // files authored by the editor both contain "layer0/layer0_primitive"
  // etc., and un-prefixed those would collide in GeometrySystem (see
  // load_scene()'s name_prefix doc comment for what that corrupts).
  SceneHandle handle = next_scene_handle_++;
  std::string name_prefix = "scene" + std::to_string(handle) + "/";
  LoadedSceneNames names = context_.geometry_system->load_scene(
      *scene, /*auto_release=*/true, name_prefix);

  loaded_scenes_.emplace(handle, std::move(names));

  // Deferred to begin_frame() -- see scene_dirty_'s comment.
  scene_dirty_ = true;
  return handle;
}

bool VulkanRendererBackend::reconcile_scene(SceneHandle handle,
                                            std::string_view sdf_path) {
  auto it = loaded_scenes_.find(handle);
  if (it == loaded_scenes_.end()) {
    KWARN("VulkanRendererBackend::reconcile_scene called with a handle that "
         "isn't currently loaded: {}. Call load_scene() first.",
         handle);
    return false;
  }

  auto scene = load_sdf_scene(sdf_path);
  if (!scene) {
    // load_sdf_scene() already logged why (missing/malformed file).
    return false;
  }

  // Same hazard remove_scene()/clear_scenes() guard against (see their own
  // comment): GeometrySystem::reconcile_scene() below can release() a
  // primitive/light/volumetric that's gone, or swap a still-present one's
  // material -- either can cascade into MaterialSystem/TextureSystem
  // dropping a texture's refcount to zero and destroying its VkImage/
  // VkSampler immediately, synchronously, no wait of its own.
  //
  // The wait used to be an unconditional vkDeviceWaitIdle up front, which
  // made EVERY editor edit -- including a pure add or an in-place gizmo
  // drag, which destroy nothing -- drain the whole device. Worse than it
  // sounds: "the whole device" includes the async compute queue, where
  // chunk bakes now run in submissions that can span many frames, so a
  // trivial edit could block for the tail of a multi-hundred-ms bake it
  // had no data dependency on. Deferred instead into reconcile_scene()
  // itself, fired at most once, only immediately before the first actually-
  // destructive operation (see before_first_destructive's doc comment) --
  // and it drains only the GRAPHICS queue: textures are sampled exclusively
  // by graphics-submitted passes (render/deferred-shade/GI bounces); the
  // async compute voxelizer evaluates the analytic SDF from plain buffers
  // and never binds a scene texture, so it cannot be holding the VkImage
  // being destroyed.
  auto destructive_wait = [this]() {
    vkQueueWaitIdle(context_.device.graphics_queue);
  };

  // Same per-handle name_prefix load_scene() used for this handle -- MUST
  // match exactly, or reconcile_scene() (engine-side) would diff against
  // the wrong names entirely (see its own comment).
  std::string name_prefix = "scene" + std::to_string(handle) + "/";
  bool changed = context_.geometry_system->reconcile_scene(
      *scene, it->second, /*auto_release=*/true, name_prefix,
      destructive_wait);

  if (changed) {
    // Deferred to begin_frame() -- see scene_dirty_'s comment.
    scene_dirty_ = true;
  }
  return changed;
}

void VulkanRendererBackend::translate_scene(SceneHandle handle,
                                            glm::vec3 delta) {
  auto it = loaded_scenes_.find(handle);
  if (it == loaded_scenes_.end()) {
    KWARN("VulkanRendererBackend::translate_scene called with a handle that "
         "isn't currently loaded: {}.",
         handle);
    return;
  }

  for (const std::string &name : it->second.primitive_names) {
    Geometry *geometry = context_.geometry_system->find(name);
    if (!geometry) {
      continue;
    }
    if (geometry->type == PrimitiveType::Plane) {
      // A plane has no position -- it's always the horizontal y=height
      // plane, with height in params.x -- so only vertical translation
      // means anything for it.
      geometry->params.x += delta.y;
    } else {
      geometry->position += delta;
    }
    // See GeometrySystem::mark_dirty()'s comment -- a mutation made through
    // find()'s returned pointer, like this one, is invisible to
    // GeometrySystem unless the caller (here) reports it explicitly.
    context_.geometry_system->mark_dirty(name);
  }
  for (const std::string &name : it->second.volumetric_names) {
    Volumetric *volumetric = context_.geometry_system->find_volumetric(name);
    if (!volumetric) {
      continue;
    }
    volumetric->position += delta;
  }
  for (const std::string &name : it->second.light_names) {
    Light *light = context_.geometry_system->find_light(name);
    if (!light) {
      continue;
    }
    // Only a Point light's vector is a world-space position -- a
    // Directional light's is a pure direction (see LightConfig::vector),
    // unaffected by translation.
    if (light->type == LightType::Point) {
      light->vector += delta;
    }
  }

  // Deferred to begin_frame() -- see scene_dirty_'s comment.
  scene_dirty_ = true;
}

void VulkanRendererBackend::rotate_scene(SceneHandle handle,
                                         glm::vec3 euler_radians) {
  auto it = loaded_scenes_.find(handle);
  if (it == loaded_scenes_.end()) {
    KWARN("VulkanRendererBackend::rotate_scene called with a handle that "
         "isn't currently loaded: {}.",
         handle);
    return;
  }

  // Compose in quaternion space rather than adding Euler angles --
  // glm::quat(vec3)/glm::eulerAngles() are exact inverses of each other,
  // and glm::quat(vec3) is also exactly how rebuild_static_scene() turns
  // Geometry::rotation into the quaternion the voxelize shader consumes,
  // so the round-trip stays consistent with what the GPU sees.
  glm::quat scene_rotation(euler_radians);

  for (const std::string &name : it->second.primitive_names) {
    Geometry *geometry = context_.geometry_system->find(name);
    if (!geometry) {
      continue;
    }
    if (geometry->type == PrimitiveType::Plane) {
      KWARN("rotate_scene: plane '{}' skipped -- a plane is always the "
           "horizontal y=height plane and can't tilt.",
           name);
      continue;
    }
    geometry->position = scene_rotation * geometry->position;
    geometry->rotation =
        glm::eulerAngles(scene_rotation * glm::quat(geometry->rotation));
    context_.geometry_system->mark_dirty(name);
  }
  for (const std::string &name : it->second.volumetric_names) {
    Volumetric *volumetric = context_.geometry_system->find_volumetric(name);
    if (!volumetric) {
      continue;
    }
    volumetric->position = scene_rotation * volumetric->position;
    volumetric->rotation =
        glm::eulerAngles(scene_rotation * glm::quat(volumetric->rotation));
  }
  for (const std::string &name : it->second.light_names) {
    Light *light = context_.geometry_system->find_light(name);
    if (!light) {
      continue;
    }
    // A Point light's vector is a position (rotates like a primitive's --
    // see translate_scene() above); a Directional light's is a direction,
    // which also needs to turn with the scene, just without the position
    // semantics (no pivot-relative orbit -- a direction has no origin).
    light->vector = scene_rotation * light->vector;
  }

  // Deferred to begin_frame() -- see scene_dirty_'s comment.
  scene_dirty_ = true;
}

void VulkanRendererBackend::scale_scene(SceneHandle handle, f32 factor) {
  auto it = loaded_scenes_.find(handle);
  if (it == loaded_scenes_.end()) {
    KWARN("VulkanRendererBackend::scale_scene called with a handle that "
         "isn't currently loaded: {}.",
         handle);
    return;
  }
  if (factor <= 0.0f) {
    KWARN("scale_scene called with a non-positive factor ({}). Ignoring.",
         factor);
    return;
  }

  // Every layer this scene's primitives combine under also needs its
  // smoothness (a blend-radius length, exactly like the shapes it blends)
  // scaled in lockstep -- collected into a set first so a layer holding
  // several of this scene's primitives only gets scaled once, not once per
  // primitive (scale_layer_smoothness() isn't idempotent -- a second call
  // would double-apply the factor).
  std::unordered_set<u32> touched_layers;

  for (const std::string &name : it->second.primitive_names) {
    Geometry *geometry = context_.geometry_system->find(name);
    if (!geometry) {
      continue;
    }
    // Every primitive type's params slots are lengths (radii, half-extents,
    // heights -- see SdfPrimitiveDef::params), as is extra_param (corner
    // radius / edge thickness), so a uniform scale multiplies all of them.
    // This also covers Plane: its height lives in params.x.
    geometry->position *= factor;
    geometry->params *= factor;
    geometry->extra_param *= factor;
    // twist/bend/displace_frequency are rates (radians, or a sin()
    // argument multiplier, per world-unit) -- dividing by factor keeps the
    // same *visual* twist/bend/ripple density as the primitive grows or
    // shrinks, exactly like param_expr_scale below does for a
    // parametric-attribute formula's own per-world-unit rate.
    // displace_amplitude is a length (an added distance), so it scales up
    // like params/extra_param above instead.
    geometry->twist /= factor;
    geometry->bend /= factor;
    geometry->displace_amplitude *= factor;
    geometry->displace_frequency /= factor;
    // Slots driven by a param_expression formula ignore the plain constant
    // scaled above -- their scale accumulates here instead, applied GPU-side
    // as s*f(p/s) (see Geometry::param_expr_scale). Without this, scaling a
    // scene left every formula-driven length at its authored size while the
    // rest of the model scaled -- visibly mangling anything authored with
    // parametric attributes (e.g. man.sdf's tapered capsule limbs).
    geometry->param_expr_scale *= factor;
    // repetition_cell is a length (cell spacing) too, so it must shrink/grow
    // in lockstep with the primitive -- left alone, scaling a scene down
    // would leave repeated instances at their old, now-relatively-larger
    // spacing. repetition_count is a plain instance count and never scales.
    geometry->repetition_cell *= factor;
    // The texture's own world-space tiling frequency is a length too, and
    // needs to shrink/grow with the primitive the same way -- accumulated
    // per-Geometry rather than scaling geometry->material->texture_scale
    // directly, since Material is shared/reference-counted (see
    // Geometry::texture_scale_factor's comment for why mutating it here
    // would leak into every other primitive using that same material).
    geometry->texture_scale_factor *= factor;
    // The texture's world-space offset is a length too (see
    // Geometry::texture_offset_scale's comment) -- same accumulation
    // reasoning as texture_scale_factor immediately above. Rotation needs
    // no equivalent line here: an angle is scale-invariant.
    geometry->texture_offset_scale *= factor;

    context_.geometry_system->mark_dirty(name);
    touched_layers.insert(geometry->layer);
  }
  for (const std::string &name : it->second.volumetric_names) {
    Volumetric *volumetric = context_.geometry_system->find_volumetric(name);
    if (!volumetric) {
      continue;
    }
    // Same reasoning as the opaque primitives above -- position/params/
    // extra_param are all lengths. density is a per-world-unit accumulation
    // rate, not a length, so it's deliberately left alone: a bigger shaft
    // reads brighter simply because the ray now travels further through it
    // at the same density, exactly like a thicker fog bank looks denser
    // without its extinction coefficient changing.
    volumetric->position *= factor;
    volumetric->params *= factor;
    volumetric->extra_param *= factor;
  }
  for (const std::string &name : it->second.light_names) {
    Light *light = context_.geometry_system->find_light(name);
    if (!light) {
      continue;
    }
    // Only a Point light's vector is a position (a length, scales like any
    // other -- see the primitive loop above); a Directional light's is a
    // direction, whose magnitude is meaningless, so scaling it would be a
    // silent no-op at best -- skip it instead of pretending it matters.
    if (light->type == LightType::Point) {
      light->vector *= factor;
      // A Point light's brightness at a given surface falls off as
      // intensity/distance^2 (see the render shader's attenuation term).
      // Scaling position without also scaling intensity by factor^2 changes
      // how bright the light reads at every surface around it: growing a
      // scene (factor > 1) pushes the light farther from everything it was
      // authored to illuminate, so its apparent brightness silently drops
      // by factor^2 (e.g. a 3x scale makes it 9x dimmer -- easily enough to
      // read as "the light doesn't work at all"); shrinking a scene
      // (factor < 1) does the reverse, making it implausibly bright.
      // Scaling intensity by factor^2 here exactly cancels the extra
      // distance^2 in the denominator, so the light reads the same
      // brightness at every correspondingly-scaled point it did before.
      light->intensity *= factor * factor;
    }
  }

  for (u32 layer_index : touched_layers) {
    context_.geometry_system->scale_layer_smoothness(layer_index, factor);
  }

  // Deferred to begin_frame() -- see scene_dirty_'s comment.
  scene_dirty_ = true;
}

void VulkanRendererBackend::remove_scene(SceneHandle handle) {
  auto it = loaded_scenes_.find(handle);
  if (it == loaded_scenes_.end()) {
    KWARN("VulkanRendererBackend::remove_scene called with a handle that "
         "isn't currently loaded: {}.",
         handle);
    return;
  }

  // Wait for the device to go idle *before* releasing anything below --
  // release() can cascade into MaterialSystem/TextureSystem dropping a
  // texture's refcount to zero and destroying its VkImage/VkSampler
  // immediately (synchronously, no wait of its own). rebake()'s own
  // device-idle wait happens too late for that: a still-in-flight (or
  // even currently executing) dispatch could still be reading that exact
  // sampler through the render descriptor set, which is exactly what
  // produced VUID-vkDestroySampler-sampler-01082 here.
  vkDeviceWaitIdle(context_.device.logical_device);

  for (const std::string &name : it->second.primitive_names) {
    context_.geometry_system->release(name);
  }
  for (const std::string &name : it->second.light_names) {
    context_.geometry_system->release_light(name);
  }
  for (const std::string &name : it->second.volumetric_names) {
    context_.geometry_system->release_volumetric(name);
  }
  loaded_scenes_.erase(it);

  // Deferred to begin_frame() -- see scene_dirty_'s comment. The
  // vkDeviceWaitIdle above already happened (required before release()),
  // so this is only about not paying for the re-voxelize/GI-bake twice if
  // more scene calls follow before the next frame.
  scene_dirty_ = true;
}

void VulkanRendererBackend::clear_scenes() {
  // See remove_scene()'s comment -- must happen before any release() below.
  vkDeviceWaitIdle(context_.device.logical_device);

  for (auto &[handle, names] : loaded_scenes_) {
    for (const std::string &name : names.primitive_names) {
      context_.geometry_system->release(name);
    }
    for (const std::string &name : names.light_names) {
      context_.geometry_system->release_light(name);
    }
    for (const std::string &name : names.volumetric_names) {
      context_.geometry_system->release_volumetric(name);
    }
  }
  loaded_scenes_.clear();

  // Deferred to begin_frame() -- see scene_dirty_'s comment.
  scene_dirty_ = true;
}

void VulkanRendererBackend::set_selected_primitive(i32 index) {
  context_.raymarch_shader->set_selected_primitive(index);
}

i32 VulkanRendererBackend::primitive_gpu_index(const std::string &name) const {
  return context_.raymarch_shader->gpu_index_for_primitive(name);
}

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
        request.colour);
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
