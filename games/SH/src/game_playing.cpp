// AppScreen::Playing -- the actual gameplay loop (camera stations, the
// dialogue system, censor boxes), unchanged from before the title screen
// existed, just split out into its own file -- see game.h's
// "game_playing.cpp" section.

#include "game.h"
#include "text_wrap.h"

#include <core/input.h>
#include <core/logger.h>
#include <renderer/camera.h>
#include <renderer/renderer_frontend.h>

#include <glm/glm.hpp>

#include <cstdlib>
#include <optional>
#include <string>

namespace {

// See SHGame::cut_cover_. The backlog is only refreshed by the renderer's
// next frame, so the first frames after a cut still report the old
// camera's (settled) field -- wait this many before trusting a 0.
constexpr u32 kCutCoverMinFrames = 3;
// Upper bound on the cover, so a field that never fully settles (a pool
// running dry, say) costs a moment of black rather than the whole scene.
constexpr f32 kCutCoverMaxSeconds = 1.5f;

// Draws an opaque black box over target, sized so it visually covers
// world_half_size of the model regardless of how far away or zoomed in
// the current camera is: projects both the center and a same-distance
// point offset by world_half_size along the camera's right vector, and
// uses the on-screen gap between the two as the box's screen-space half-
// size -- a fixed pixel size wouldn't shrink/grow correctly as the camera
// moves between stations or zooms (see CameraSystem's scroll-to-zoom).
// No-op if target is behind the camera (Camera::project_to_screen()
// returns nullopt then -- e.g. the free-fly debug camera flew past it).
//
// A backdrop quad, not a solid one: it covers the scene but sits under
// every line of text, so a box over a face never hides the dialogue or
// the speaker's name when the two overlap on screen.
void draw_censor_box(const Camera &camera, const CensorPoint &target,
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
  renderer_draw_backdrop_quad(*center - glm::vec2(half_size_px),
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
    // [L] on a [link] line (see script_line.h) -- e.g. a song
    // recommendation -- opens it.
    const ScriptLine *line = qa_.current_line();
    if (line && line->link && input::is_key_down(input::Key::L) &&
        !input::was_key_down(input::Key::L)) {
      open_link(*line->link);
    }
  }
  cameras_.update(width_, height_, delta_time_);

  if (cut_cover_) {
    ++cut_cover_frames_;
    cut_cover_seconds_ += delta_time_;
    if ((cut_cover_frames_ >= kCutCoverMinFrames &&
         renderer_streaming_backlog() == 0) ||
        cut_cover_seconds_ >= kCutCoverMaxSeconds) {
      cut_cover_ = false;
    }
  }
}

void SHGame::open_link(const std::string &url) const {
  // Single quotes can't appear inside a single-quoted shell word, so drop
  // any rather than let a .conversation file's URL break out of it.
  std::string quoted;
  for (char c : url) {
    if (c != '\'') {
      quoted += c;
    }
  }
#if defined(_WIN32)
  std::string command = "start \"\" \"" + quoted + "\"";
#else
  std::string command = "xdg-open '" + quoted + "' >/dev/null 2>&1 &";
#endif
  if (std::system(command.c_str()) != 0) {
    KWARN("Couldn't open link '{}'.", url);
  }
}

void SHGame::render_playing() const {
  // See current_camera()'s own comment -- the exact Camera update() just
  // submitted this frame, whichever one that is (a posed station or the
  // free-fly debug camera). Fetched once up top since both the debug HUD's
  // position readout below and the censor boxes further down need it.
  const Camera &camera = cameras_.current_camera();

  render_debug_camera_hud();

  // The viewfinder belongs to the shot, not to a [title] card -- a card is
  // the black intertitle screen of an old film, with nothing but its text
  // on it.
  const ScriptLine *shown_line = qa_.current_line();
  static f32 elapsed = 0.0f;
  elapsed += delta_time_;
  if (!(shown_line && shown_line->title)) {
    renderer_draw_camera_overlay(
        glm::vec2(0.0f, 0.0f),
        glm::vec2(static_cast<f32>(width_), static_cast<f32>(height_)),
        elapsed, "");
  }
  // A cutaway or photo the script calls for (see on_script_line()):
  // neither exists yet, so the shot is a placeholder card naming it,
  // sitting under the dialogue like the scene it stands in for would.
  const bool placeholder_up = shot_.kind == Shot::Kind::Cutaway ||
                              shot_.kind == Shot::Kind::Photo;
  if (cut_cover_ && !placeholder_up) {
    renderer_draw_backdrop_quad(
        glm::vec2(0.0f),
        glm::vec2(static_cast<f32>(width_), static_cast<f32>(height_)),
        glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
  }
  if (placeholder_up) {
    const f32 width = static_cast<f32>(width_);
    const f32 height = static_cast<f32>(height_);
    renderer_draw_backdrop_quad(glm::vec2(0.0f), glm::vec2(width, height),
                                glm::vec4(0.08f, 0.08f, 0.1f, 1.0f));
    const std::string label =
        std::string(shot_.kind == Shot::Kind::Cutaway ? "PLACEHOLDER SCENE: "
                                                      : "PLACEHOLDER PHOTO: ") +
        shot_.asset;
    renderer_draw_text(
        label,
        glm::vec2((width - static_cast<f32>(label.size()) * kAverageCharWidth) /
                      2.0f,
                  height * 0.4f),
        glm::vec4(0.9f, 0.3f, 0.3f, 1.0f));
  }

  qa_.render(width_, height_);

  // Face/lap censor boxes for whoever the current state has staged. The
  // points come from that chapter's own kStaging entry
  // (game_scene_state.cpp) rather than being hardcoded here, because every
  // chapter puts its subject somewhere different at a different size --
  // Diego's head is a 0.3-radius sphere 3.64 above his office floor, Mel's
  // a 0.06-radius one 0.84 above hers, and the three PhotoStudio chapters
  // get theirs from wherever kStudioFigure stands props/man.sdf -- so
  // there is no one model transform to pin them to. They're world-space
  // already; all that's left is to project each through whichever camera
  // is live *this* frame (a posed station, mid-pan/zoom, or the free-fly
  // debug camera) so the boxes track correctly no matter which one that
  // is.
  //
  // A chapter with no subject staged returns an empty list and draws
  // nothing, as does the moment before the first apply_scene_state() call
  // has run.
  //
  // Not while something covers the room, though -- a censor box is a
  // backdrop quad queued after the title card's/placeholder card's own,
  // so it would land on top of that card rather than the room under it.
  const ScriptLine *line = qa_.current_line();
  const bool room_covered = placeholder_up || cut_cover_ || (line && line->title);
  if (current_state_ && !room_covered) {
    for (const CensorPoint &point : censor_points_for(*current_state_)) {
      draw_censor_box(camera, point, width_, height_);
    }
  }
}
