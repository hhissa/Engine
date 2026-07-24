#include "game.h"
#include <core/input.h>
#include <core/logger.h>
#include <renderer/camera.h>
#include <renderer/renderer_frontend.h>

#include <glm/glm.hpp>

#include <format>
#include <optional>

namespace {

// A world-space point plus how big an area (world units) a censor box
// should cover around it -- see draw_censor_box() below.
struct CensorTarget {
  glm::vec3 world_position;
  f32 world_half_size; // half-width/height of the covered area, world units
};

// Draws an opaque black box over target, sized so it visually covers
// world_half_size of the model regardless of how far away or zoomed in
// the current camera is: projects both the center and a same-distance
// point offset by world_half_size along the camera's right vector, and
// uses the on-screen gap between the two as the box's screen-space half-
// size -- a fixed pixel size wouldn't shrink/grow correctly as the camera
// moves between stations or zooms (see CameraSystem's scroll-to-zoom).
// No-op if target is behind the camera (Camera::project_to_screen()
// returns nullopt then -- e.g. the free-fly debug camera flew past it).
void draw_censor_box(const Camera &camera, const CensorTarget &target,
                     u32 width, u32 height) {
  std::optional<glm::vec2> center =
      camera.project_to_screen(target.world_position, width, height);
  std::optional<glm::vec2> edge = camera.project_to_screen(
      target.world_position + camera.right() * target.world_half_size, width,
      height);
  if (!center || !edge) {
    return;
  }
  f32 half_size_px = glm::length(*edge - *center);
  renderer_draw_solid_quad(*center - glm::vec2(half_size_px),
                           glm::vec2(half_size_px * 2.0f),
                           glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
}

} // namespace

SHGame::SHGame() {
  app_config.start_pos_x = 100;
  app_config.start_pos_y = 100;
  app_config.start_width = 1280;
  app_config.start_height = 720;
  app_config.name = "SH";
  width_ = app_config.start_width;
  height_ = app_config.start_height;
}

b8 SHGame::initialize() {
  KDEBUG("SHGame::initialize() called!");

  room = renderer_load_scene("assets/scenes/room.sdf")
             .scale(3.0)
             .translate(glm::vec3(0.0, 0.0, 0.0));

  scene_ = renderer_load_scene("assets/scenes/man.sdf")
               .scale(0.35)
               .translate(glm::vec3(0.0, -2.8, 0.0));

  light1_ = renderer_load_scene("assets/scenes/light.sdf")
                .rotate(glm::vec3(0.0f, glm::radians(45.0f), 0.0f))
                .scale(0.3)
                .translate(glm::vec3(2.0, -1.5, -1.0));

  light2_ = renderer_load_scene("assets/scenes/light.sdf")
                .rotate(glm::vec3(0.0f, glm::radians(135.0f), 0.0f))
                .scale(0.3)
                .translate(glm::vec3(-2.0, -1.5, -1.0));

  overheadLights_ = renderer_load_scene("assets/scenes/overhead lights.sdf")
                        .scale(2.0f)
                        .translate(glm::vec3(0.0, -4.0, 0.0));

  // Equirectangular skybox behind the room -- resolves to
  // assets/textures/skybox_to_equirect_2.png (TextureSystem's usual name ->
  // assets/textures/<name>.png convention, same as every material's
  // diffuse_map_name).
  renderer_enable_sky_box("skybox_to_equirect_2");

  // Camera stations around the man -- Tab cycles, mouse pans within each
  // pose's max_pan (see CameraSystem).
  cameras_.add_pose(
      {glm::vec3(3.0f, -3.0f, -4.0f), glm::radians(-45.0f), 0.0f, 0.1f});
  cameras_.add_pose({glm::vec3(0.0f, -3.0f, -6.0f), glm::radians(0.0f),
                     glm::radians(-0.0f), 0.5f});
  cameras_.add_pose({glm::vec3(-3.3f, -3.0f, -4.5f), glm::radians(35.0f),
                     glm::radians(20.0f), 0.1f});

  // The question block -- selecting a question darkens it permanently and
  // plays its answer lines one Enter press at a time (see QASystem).
  // Dialogue content itself lives in a .conversation file (see
  // conversation.h for the format), not hardcoded here -- including
  // whichever follow-up sub-questions appear once a question's answer
  // finishes (the list returns to the top level automatically once every
  // follow-up at the current level has been asked).
  dialogue_ =
      qa_.load_conversation("assets/conversations/sh_dialogue.conversation");

  renderer_set_pixelation_enabled(true);
  renderer_set_pixelation_block_size(3);

  return true;
}

b8 SHGame::update(f32 dt) {
  delta_time_ = dt;

  // The 0 key toggles the free-fly debug camera (see CameraSystem's class
  // comment for the controls) -- not Escape, which already quits the
  // application engine-side.
  if (input::is_key_down(input::Key::Zero) &&
      !input::was_key_down(input::Key::Zero)) {
    cameras_.toggle_debug();
  }

  // Tab cycles camera stations; the Q&A system reads Up/Down/Enter itself.
  // Both are gameplay-only: while the debug camera flies, cycle() is a
  // no-op (CameraSystem guards it) and the Q&A block is frozen so Enter/
  // arrow presses made while inspecting the scene can't advance dialogue
  // behind your back.
  if (input::is_key_down(input::Key::Tab) &&
      !input::was_key_down(input::Key::Tab)) {
    cameras_.cycle();
  }

  if (!cameras_.debug_active()) {
    qa_.update();
  }
  cameras_.update(width_, height_, dt);

  return true;
}

b8 SHGame::render(f32 dt) {
  // See current_camera()'s own comment -- the exact Camera update() just
  // submitted this frame, whichever one that is (a posed station or the
  // free-fly debug camera). Fetched once up top since both the debug HUD's
  // position readout below and the censor boxes further down need it.
  const Camera &camera = cameras_.current_camera();

  renderer_draw_text("SH", glm::vec2(32.0f, 32.0f),
                     glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
  if (cameras_.debug_active()) {
    glm::vec3 pos = camera.position();
    renderer_draw_text(
        std::format("[DEBUG CAM]  WASD move  Q/E down/up  RMB-drag look  "
                    "Shift fast  [0] exit  pos=({:.2f}, {:.2f}, {:.2f})",
                    pos.x, pos.y, pos.z),
        glm::vec2(32.0f, 64.0f), glm::vec4(1.0f, 0.8f, 0.3f, 1.0f));
  }
  static f32 elapsed = 0.0f;
  elapsed += dt;
  renderer_draw_camera_overlay(
      glm::vec2(0.0f, 0.0f),
      glm::vec2(static_cast<f32>(width_), static_cast<f32>(height_)), elapsed,
      "");
  qa_.render(height_);

  // Face/crotch censor boxes. Positions are estimates in man.sdf's own
  // authored local space (approximate -- nudge kManFaceLocal/
  // kManCrotchLocal, and each CensorTarget's world_half_size, once you can
  // see exactly where they land), transformed by kManScale/kManTranslate
  // -- which MUST match the .scale()/.translate() the man.sdf load below
  // uses, since that's the actual transform putting the model in the
  // world -- then projected through whichever camera is live *this*
  // frame (a posed station, mid-pan/zoom, or the free-fly debug camera),
  // so the boxes track the model correctly no matter which one that is.
  constexpr glm::vec3 kManFaceLocal(0.0f, 0.0f, 0.35f);
  constexpr glm::vec3 kManCrotchLocal(0.0f, 2.75f, 0.15f);
  constexpr f32 kManScale = 0.35f;
  const glm::vec3 kManTranslate(0.0f, -2.8f, 0.0f);
  draw_censor_box(camera, {kManFaceLocal * kManScale + kManTranslate, 0.15f},
                  width_, height_);
  draw_censor_box(camera, {kManCrotchLocal * kManScale + kManTranslate, 0.15f},
                  width_, height_);

  return true;
}

void SHGame::on_resize(u32 width, u32 height) {
  KDEBUG("Resized to {}x{}", width, height);
  width_ = width;
  height_ = height;
}
