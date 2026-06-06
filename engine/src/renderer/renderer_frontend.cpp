#include "renderer_frontend.h"
#include "renderer_backend.h"

#include "../core/logger.h"

#include <memory>

namespace {

// Backend render context.
std::unique_ptr<RendererBackend> backend;

b8 renderer_begin_frame(f32 delta_time) {
  return backend->begin_frame(delta_time);
}

b8 renderer_end_frame(f32 delta_time) {
  b8 result = backend->end_frame(delta_time);
  ++backend->frame_number;
  return result;
}

} // namespace

b8 renderer_initialize(std::string_view application_name,
                       PlatformLayer &plat_state) {
  // TODO: make this configurable.
  backend = renderer_backend_create(
      renderer_backend_type::RENDERER_BACKEND_TYPE_VULKAN, plat_state);
  backend->frame_number = 0;

  if (!backend->initialize(application_name, plat_state)) {
    KFATAL("Renderer backend failed to initialize. Shutting down.");
    return FALSE;
  }
  return TRUE;
}

void renderer_shutdown() {
  backend->shutdown();
  backend.reset(); // replaces kfree + renderer_backend_destroy
}

b8 renderer_draw_frame(render_packet *packet) {
  // If the begin frame returned successfully, mid-frame operations may
  // continue.
  if (renderer_begin_frame(packet->delta_time)) {
    // End the frame. If this fails, it is likely unrecoverable.
    b8 result = renderer_end_frame(packet->delta_time);
    if (!result) {
      KERROR("renderer_end_frame failed. Application shutting down...");
      return FALSE;
    }
  }
  return TRUE;
}
