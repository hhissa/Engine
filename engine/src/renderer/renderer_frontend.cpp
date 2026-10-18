#include "renderer_frontend.h"
#include "renderer_backend.h"

#include "../core/logger.h"

#include <algorithm>
#include <cmath>
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
                       glm::vec4 colour, f32 scale) {
  if (backend) {
    backend->draw_text(text, position, colour, scale);
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

void renderer_draw_solid_quad(glm::vec2 position, glm::vec2 size,
                             glm::vec4 colour) {
  if (backend) {
    backend->draw_solid_quad(position, size, colour);
  } else {
    KWARN("renderer backend does not exist to accept a solid quad draw "
         "request.");
  }
}

void renderer_draw_backdrop_quad(glm::vec2 position, glm::vec2 size,
                                glm::vec4 colour) {
  if (backend) {
    backend->draw_backdrop_quad(position, size, colour);
  } else {
    KWARN("renderer backend does not exist to accept a backdrop quad draw "
         "request.");
  }
}

namespace {
// Draws one viewfinder corner bracket (an "L" opening toward the rect's
// center) at corner, with each arm reaching toward inward (a unit vector
// pointing from that corner toward the rect's center) by arm_length.
void draw_corner_bracket(glm::vec2 corner, glm::vec2 inward, f32 arm_length,
                        glm::vec4 colour) {
  glm::vec2 horizontal_end = corner + glm::vec2(inward.x * arm_length, 0.0f);
  glm::vec2 vertical_end = corner + glm::vec2(0.0f, inward.y * arm_length);
  renderer_draw_line(corner, horizontal_end, colour);
  renderer_draw_line(corner, vertical_end, colour);
}
} // namespace

void renderer_draw_camera_overlay(glm::vec2 position, glm::vec2 size,
                                  f32 blink_time_seconds,
                                  std::string_view caption) {
  const glm::vec4 kHudColour(0.85f, 0.9f, 0.85f, 0.6f);
  const glm::vec4 kRecColour(0.95f, 0.15f, 0.15f, 1.0f);

  glm::vec2 top_left = position;
  glm::vec2 bottom_right = position + size;

  // Rule of thirds: 2 vertical + 2 horizontal lines, dim -- a compositing
  // aid, not meant to dominate the frame.
  const glm::vec4 kThirdsColour(kHudColour.r, kHudColour.g, kHudColour.b, 0.25f);
  for (int i = 1; i <= 2; ++i) {
    f32 x = position.x + size.x * (static_cast<f32>(i) / 3.0f);
    renderer_draw_line(glm::vec2(x, top_left.y), glm::vec2(x, bottom_right.y),
                       kThirdsColour);
    f32 y = position.y + size.y * (static_cast<f32>(i) / 3.0f);
    renderer_draw_line(glm::vec2(top_left.x, y), glm::vec2(bottom_right.x, y),
                       kThirdsColour);
  }

  // Corner brackets -- the classic viewfinder framing cue. Arm length is a
  // fraction of the rect's shorter side, so it scales sensibly across
  // very wide or very tall viewports instead of always being a fixed
  // pixel size.
  f32 arm_length = std::min(size.x, size.y) * 0.06f;
  draw_corner_bracket(top_left, glm::vec2(1.0f, 1.0f), arm_length, kHudColour);
  draw_corner_bracket(glm::vec2(bottom_right.x, top_left.y),
                     glm::vec2(-1.0f, 1.0f), arm_length, kHudColour);
  draw_corner_bracket(glm::vec2(top_left.x, bottom_right.y),
                     glm::vec2(1.0f, -1.0f), arm_length, kHudColour);
  draw_corner_bracket(bottom_right, glm::vec2(-1.0f, -1.0f), arm_length,
                     kHudColour);

  // REC indicator: blinks once per second (solid for the first half-second
  // of each cycle) -- built from a tiny cluster of short line segments
  // radiating from a center point, since draw_line() has no dedicated
  // "filled circle" primitive; at this size (a few pixels) it reads as a
  // small solid dot, not individual strokes.
  f32 blink_phase = std::fmod(std::max(blink_time_seconds, 0.0f), 1.0f);
  if (blink_phase < 0.5f) {
    // Placed just inside the top-left corner bracket -- positive offsets
    // in both axes, since screen space has its origin at the top-left
    // with y increasing downward.
    glm::vec2 dot_center = top_left + glm::vec2(arm_length * 1.8f, arm_length * 1.8f);
    constexpr f32 kDotRadius = 4.0f;
    constexpr int kDotSpokes = 8;
    for (int i = 0; i < kDotSpokes; ++i) {
      f32 angle = (static_cast<f32>(i) / static_cast<f32>(kDotSpokes)) * 6.2831853f;
      glm::vec2 spoke_end =
          dot_center + glm::vec2(std::cos(angle), std::sin(angle)) * kDotRadius;
      renderer_draw_line(dot_center, spoke_end, kRecColour);
    }
    renderer_draw_text("REC", dot_center + glm::vec2(10.0f, -8.0f), kRecColour);
  }

  if (!caption.empty()) {
    renderer_draw_text(caption, glm::vec2(top_left.x + arm_length * 1.4f,
                                         bottom_right.y - 20.0f),
                       kHudColour);
  }
}

void renderer_draw_image(std::string_view path, glm::vec2 position,
                         glm::vec2 size) {
  if (backend) {
    backend->draw_image(path, position, size);
  }
}

glm::vec2 renderer_preload_image(std::string_view path) {
  return backend ? backend->preload_image(path) : glm::vec2(0.0f);
}

void renderer_set_grid_visible(b8 visible) {
  if (backend) {
    backend->set_grid_visible(visible);
  } else {
    KWARN("renderer backend does not exist to accept a grid-visibility "
         "request.");
  }
}

void renderer_set_chunked_field_enabled(b8 enabled) {
  if (backend) {
    backend->set_chunked_field_enabled(enabled);
  } else {
    KWARN("renderer backend does not exist to accept a chunked-field "
         "request.");
  }
}

void renderer_set_taa_enabled(b8 enabled) {
  if (backend) {
    backend->set_taa_enabled(enabled);
  }
}

void renderer_set_ism_enabled(b8 enabled) {
  if (backend) {
    backend->set_ism_enabled(enabled);
  }
}

void renderer_set_ao_enabled(b8 enabled) {
  if (backend) {
    backend->set_ao_enabled(enabled);
  }
}

void renderer_debug_probe_field(glm::vec3 world_pos) {
  if (backend) {
    backend->debug_probe_field(world_pos);
  }
}

void renderer_sculpt_set_material(i32 slot, glm::vec3 colour, f32 roughness,
                                  f32 metallic, bool waxy, i32 fleck_pattern,
                                  f32 fleck_strength, f32 fleck_scale) {
  if (backend) {
    backend->set_material(slot, colour, roughness, metallic, waxy,
                          fleck_pattern, fleck_strength, fleck_scale);
  } else {
    KWARN("renderer backend does not exist to accept a sculpt-set-material "
         "request.");
  }
}

i32 renderer_add_directional_light(glm::vec3 direction, glm::vec3 colour,
                                   f32 intensity) {
  if (backend) {
    return backend->add_directional_light(direction, colour, intensity);
  }
  KWARN("renderer backend does not exist to accept an add-directional-light "
       "request.");
  return -1;
}

i32 renderer_add_point_light(glm::vec3 position, glm::vec3 colour,
                             f32 intensity) {
  if (backend) {
    return backend->add_point_light(position, colour, intensity);
  }
  KWARN("renderer backend does not exist to accept an add-point-light "
       "request.");
  return -1;
}

void renderer_sculpt_stamp_shape(glm::vec3 center,
                            SculptBrushShape shape_type, glm::vec4 params,
                            f32 smoothness, SculptBrushOp shape_op,
                            i32 material_slot, f32 looseness) {
  if (backend) {
    backend->stamp_shape(center, shape_type, params, smoothness, shape_op,
                         material_slot, looseness);
  } else {
    KWARN("renderer backend does not exist to accept a sculpt-stamp "
         "request.");
  }
}

void renderer_sculpt_stamp_sphere(glm::vec3 center, f32 radius,
                             i32 material_slot) {
  renderer_sculpt_stamp_shape(center, SculptBrushShape::Sphere,
                              glm::vec4(radius, 0.0f, 0.0f, 0.0f), 0.0f,
                              SculptBrushOp::Union, material_slot);
}

bool renderer_sculpt_undo() {
  if (!backend) {
    KWARN("renderer backend does not exist to accept a sculpt-undo request.");
    return false;
  }
  return backend->undo_stamp();
}

bool renderer_sculpt_redo() {
  if (!backend) {
    KWARN("renderer backend does not exist to accept a sculpt-redo request.");
    return false;
  }
  return backend->redo_stamp();
}

i32 renderer_sculpt_spray(glm::vec3 center, SculptBrushShape shape_type,
                          glm::vec4 params, f32 opacity_per_pass) {
  if (!backend) {
    KWARN("renderer backend does not exist to accept a spraypaint request.");
    return 0;
  }
  return backend->spray_paint(center, shape_type, params, opacity_per_pass);
}

bool renderer_sculpt_delete_edit(i32 index) {
  if (!backend) {
    KWARN("renderer backend does not exist to accept a sculpt-delete-edit "
         "request.");
    return false;
  }
  return backend->delete_edit(index);
}

bool renderer_sculpt_move_edit(i32 index, glm::vec3 new_center) {
  if (!backend) {
    KWARN("renderer backend does not exist to accept a sculpt-move-edit "
         "request.");
    return false;
  }
  return backend->move_edit(index, new_center);
}

i32 renderer_sculpt_crop(glm::vec3 center, SculptBrushShape shape_type,
                         glm::vec4 params, f32 smoothness) {
  if (!backend) {
    KWARN("renderer backend does not exist to accept a sculpt-crop request.");
    return 0;
  }
  return backend->crop_volume(center, shape_type, params, smoothness);
}

bool renderer_sculpt_cutout(glm::vec3 center, SculptBrushShape shape_type,
                            glm::vec4 params, f32 smoothness,
                            const std::string &removed_path) {
  if (!backend) {
    KWARN("renderer backend does not exist to accept a sculpt-cutout "
         "request.");
    return false;
  }
  return backend->cutout_volume(center, shape_type, params, smoothness,
                                removed_path);
}

bool renderer_sculpt_load_cutout(const std::string &path) {
  if (!backend) {
    KWARN("renderer backend does not exist to accept a sculpt-load-cutout "
         "request.");
    return false;
  }
  return backend->load_cutout_file(path);
}

void renderer_set_splat_mode(RendererSplatMode mode) {
  if (backend) {
    backend->set_splat_mode(mode);
  } else {
    KWARN("renderer backend does not exist to accept a splat-mode request.");
  }
}

void renderer_set_bloom_enabled(b8 enabled) {
  if (backend) {
    backend->set_bloom_enabled(enabled);
  } else {
    KWARN("renderer backend does not exist to accept a bloom-enabled "
         "request.");
  }
}

void renderer_set_vignette_enabled(b8 enabled) {
  if (backend) {
    backend->set_vignette_enabled(enabled);
  } else {
    KWARN("renderer backend does not exist to accept a vignette-enabled "
         "request.");
  }
}

void renderer_set_pixelation_enabled(b8 enabled) {
  if (backend) {
    backend->set_pixelation_enabled(enabled);
  } else {
    KWARN("renderer backend does not exist to accept a pixelation-enabled "
         "request.");
  }
}

void renderer_set_pixelation_block_size(u32 block_size) {
  if (backend) {
    backend->set_pixelation_block_size(block_size);
  } else {
    KWARN("renderer backend does not exist to accept a pixelation-block-"
         "size request.");
  }
}

void renderer_set_bloom_threshold(f32 threshold) {
  if (backend) {
    backend->set_bloom_threshold(threshold);
  } else {
    KWARN("renderer backend does not exist to accept a bloom-threshold "
         "request.");
  }
}

void renderer_set_bloom_intensity(f32 intensity) {
  if (backend) {
    backend->set_bloom_intensity(intensity);
  } else {
    KWARN("renderer backend does not exist to accept a bloom-intensity "
         "request.");
  }
}

void renderer_set_vignette_strength(f32 strength) {
  if (backend) {
    backend->set_vignette_strength(strength);
  } else {
    KWARN("renderer backend does not exist to accept a vignette-strength "
         "request.");
  }
}

void renderer_set_vignette_radius(f32 radius) {
  if (backend) {
    backend->set_vignette_radius(radius);
  } else {
    KWARN("renderer backend does not exist to accept a vignette-radius "
         "request.");
  }
}

void renderer_set_render_scale(f32 scale) {
  if (backend) {
    backend->set_render_scale(scale);
  } else {
    KWARN("renderer backend does not exist to accept a render-scale "
         "request.");
  }
}

void renderer_set_font(std::string_view name, f32 pixel_height) {
  if (backend) {
    backend->set_font(name, pixel_height);
  } else {
    KWARN("renderer backend does not exist to accept a font request.");
  }
}

void renderer_enable_sky_box(std::string_view texture_name) {
  if (backend) {
    backend->set_skybox(texture_name);
  } else {
    KWARN("renderer backend does not exist to accept a skybox request.");
  }
}

void renderer_disable_sky_box() {
  if (backend) {
    backend->disable_skybox();
  } else {
    KWARN("renderer backend does not exist to accept a skybox request.");
  }
}

void renderer_set_floating_origin_enabled(b8 enabled) {
  if (backend) {
    backend->set_floating_origin_enabled(enabled);
  } else {
    KWARN("renderer backend does not exist to accept a floating-origin "
         "request.");
  }
}

glm::vec3 renderer_consume_origin_shift() {
  return backend ? backend->consume_origin_shift() : glm::vec3(0.0f);
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
