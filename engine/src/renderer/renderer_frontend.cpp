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

void renderer_on_resized(u16 width, u16 height) {
  if (backend) {
    backend->on_resized(width, height);
  } else {
    KWARN("renderer backend does not exist to accept resize: {} {}", width,
         height);
  }
}

void renderer_set_camera(const Camera &camera) {
  if (backend) {
    backend->set_camera(camera);
  } else {
    KWARN("renderer backend does not exist to accept a camera.");
  }
}

void renderer_draw_text(std::string_view text, glm::vec2 position,
                       glm::vec4 colour) {
  if (backend) {
    backend->draw_text(text, position, colour);
  } else {
    KWARN("renderer backend does not exist to accept a text draw request.");
  }
}

void renderer_draw_ui_quad(glm::vec2 position, glm::vec2 size) {
  if (backend) {
    backend->draw_ui_quad(position, size);
  } else {
    KWARN(
        "renderer backend does not exist to accept a UI quad draw request.");
  }
}

void renderer_draw_line(glm::vec2 start, glm::vec2 end, glm::vec4 colour) {
  if (backend) {
    backend->draw_line(start, end, colour);
  } else {
    KWARN("renderer backend does not exist to accept a line draw request.");
  }
}

SceneRef renderer_load_scene(std::string_view sdf_path) {
  if (!backend) {
    KWARN("renderer backend does not exist to accept a scene load request.");
    return SceneRef(kInvalidSceneHandle);
  }
  return SceneRef(backend->load_scene(sdf_path));
}

SceneRef &SceneRef::translate(glm::vec3 delta) {
  if (!backend) {
    KWARN("renderer backend does not exist to accept a scene translate "
         "request.");
  } else if (handle_ != kInvalidSceneHandle) {
    // Invalid handle: quiet no-op -- the failed load already logged why.
    backend->translate_scene(handle_, delta);
  }
  return *this;
}

SceneRef &SceneRef::rotate(glm::vec3 euler_radians) {
  if (!backend) {
    KWARN("renderer backend does not exist to accept a scene rotate request.");
  } else if (handle_ != kInvalidSceneHandle) {
    backend->rotate_scene(handle_, euler_radians);
  }
  return *this;
}

SceneRef &SceneRef::scale(f32 factor) {
  if (!backend) {
    KWARN("renderer backend does not exist to accept a scene scale request.");
  } else if (handle_ != kInvalidSceneHandle) {
    backend->scale_scene(handle_, factor);
  }
  return *this;
}

void renderer_remove_scene(SceneHandle handle) {
  if (backend) {
    backend->remove_scene(handle);
  } else {
    KWARN("renderer backend does not exist to accept a scene remove request.");
  }
}

void renderer_clear_scenes() {
  if (backend) {
    backend->clear_scenes();
  } else {
    KWARN("renderer backend does not exist to accept a scene clear request.");
  }
}

void renderer_set_selected_primitive(i32 index) {
  if (backend) {
    backend->set_selected_primitive(index);
  } else {
    KWARN("renderer backend does not exist to accept a selected-primitive "
         "request.");
  }
}

void renderer_set_grid_visible(b8 visible) {
  if (backend) {
    backend->set_grid_visible(visible);
  } else {
    KWARN("renderer backend does not exist to accept a grid-visibility "
         "request.");
  }
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
