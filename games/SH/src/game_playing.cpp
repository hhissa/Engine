// AppScreen::Playing -- the actual gameplay loop (camera stations, the
// dialogue system, censor boxes), unchanged from before the title screen
// existed, just split out into its own file -- see game.h's
// "game_playing.cpp" section.

#include "game.h"

#include <core/input.h>
#include <core/logger.h>
#include <renderer/camera.h>
#include <renderer/renderer_frontend.h>

#include <glm/glm.hpp>

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

void SHGame::update_playing() {
  toggle_debug_camera_if_pressed();

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
  cameras_.update(width_, height_, delta_time_);
}

void SHGame::render_playing() const {
  // See current_camera()'s own comment -- the exact Camera update() just
  // submitted this frame, whichever one that is (a posed station or the
  // free-fly debug camera). Fetched once up top since both the debug HUD's
  // position readout below and the censor boxes further down need it.
  const Camera &camera = cameras_.current_camera();

  render_debug_camera_hud();

  static f32 elapsed = 0.0f;
  elapsed += delta_time_;
  renderer_draw_camera_overlay(
      glm::vec2(0.0f, 0.0f),
      glm::vec2(static_cast<f32>(width_), static_cast<f32>(height_)), elapsed,
      "");
  qa_.render(width_, height_);

  // Face/crotch censor boxes. Positions are estimates in man.sdf's own
  // authored local space (approximate -- nudge kManFaceLocal/
  // kManCrotchLocal, and each CensorTarget's world_half_size, once you can
  // see exactly where they land), transformed by kManScale/kManTranslate
  // -- which MUST match the .scale()/.translate() the man.sdf load in
  // game_scene_state.cpp's apply_scene_state() uses, since that's the
  // actual transform putting the model in the world -- then projected
  // through whichever camera is live *this* frame (a posed station, mid-
  // pan/zoom, or the free-fly debug camera), so the boxes track the model
  // correctly no matter which one that is. Unconditional (no per-state
  // gate) since every one of the 40 SceneState values currently loads
  // man.sdf at this exact same transform -- see apply_scene_state()'s
  // switch; re-add a gate here if a future state ever swaps in a different
  // model/transform instead.
  constexpr glm::vec3 kManFaceLocal(0.0f, 0.0f, 0.0f);
  constexpr glm::vec3 kManCrotchLocal(0.0f, 2.75f, 0.0f);
  constexpr f32 kManScale = 0.35f;
  const glm::vec3 kManTranslate(0.0f, -2.8f, 0.0f);
  draw_censor_box(camera, {kManFaceLocal * kManScale + kManTranslate, 0.15f},
                  width_, height_);
  draw_censor_box(camera,
                  {kManCrotchLocal * kManScale + kManTranslate, 0.15f},
                  width_, height_);
}
