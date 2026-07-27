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

void SHGame::apply_scene_state(SceneState state) {
  if (current_state_ == state) {
    return; // already there -- e.g. re-selecting a dialogue option that
            // maps to the state already active
  }

  // Every case below is a *complete* description of what should be
  // loaded, not a diff from the previous state -- so tear down whatever
  // scenery/model is currently loaded first, unconditionally, rather than
  // trying to work out what changed between the old and new state.
  auto remove_if_loaded = [](SceneHandle &handle) {
    if (handle != kInvalidSceneHandle) {
      renderer_remove_scene(handle);
      handle = kInvalidSceneHandle;
    }
  };
  remove_if_loaded(room);
  remove_if_loaded(scene_);
  remove_if_loaded(light1_);
  remove_if_loaded(light2_);
  remove_if_loaded(overheadLights_);

  switch (state) {
  case SceneState::Normal:
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

    // Camera stations around the man -- Tab cycles, mouse pans within each
    // pose's max_pan (see CameraSystem).
    cameras_.set_poses({
        {glm::vec3(3.0f, -3.0f, -4.0f), glm::radians(-45.0f), 0.0f, 0.1f},
        {glm::vec3(0.0f, -3.0f, -6.0f), glm::radians(0.0f), glm::radians(-0.0f),
         0.5f},
        {glm::vec3(-3.3f, -3.0f, -4.5f), glm::radians(35.0f),
         glm::radians(20.0f), 0.1f},
    });
    break;

  case SceneState::DoorScene:
    // Deliberately loads only scene_ -- room/light1_/light2_/
    // overheadLights_ stay torn down (kInvalidSceneHandle), same as the
    // renderer_clear_scenes() this replaced.
    scene_ = renderer_load_scene("assets/scenes/door_scene.sdf")
                 .scale(0.35)
                 .translate(glm::vec3(0.0, -2.8, 0.0));

    // Placeholder framing -- mirrors Normal's straight-on station since
    // door_scene.sdf uses the same scale/translate as man.sdf above, but
    // nobody's actually eyeballed this against the real geometry yet.
    // Adjust position/yaw/pitch once you can see how it actually frames.
    cameras_.set_poses({
        {glm::vec3(0.0f, -3.0f, -1.0f), glm::radians(0.0f), glm::radians(0.0f),
         0.1f, 0.5f, 0.5f},
    });
    break;
  }

  current_state_ = state;
}

b8 SHGame::initialize() {
  KDEBUG("SHGame::initialize() called!");

  apply_scene_state(SceneState::Normal);

  // Equirectangular skybox behind the room -- resolves to
  // assets/textures/skybox_to_equirect_2.png (TextureSystem's usual name ->
  // assets/textures/<name>.png convention, same as every material's
  // diffuse_map_name).
  renderer_enable_sky_box("skybox_to_equirect_2");

  // The question block -- selecting a question darkens it permanently and
  // plays its answer lines one Enter press at a time (see QASystem).
  // Dialogue content itself lives in a .conversation file (see
  // conversation.h for the format), not hardcoded here -- including
  // whichever follow-up sub-questions appear once a question's answer
  // finishes (the list returns to the top level automatically once every
  // follow-up at the current level has been asked).
  dialogue_ =
      qa_.load_conversation("assets/conversations/chapter1.conversation");

  // "DoorScene" is declared in the .conversation file itself (a `tag=`
  // line on whichever question(s) should trigger it -- currently "Have you
  // ever attempted suicide?" in chapter1.conversation) rather than matched
  // here by literal question text, so content can move/reword/reuse the
  // trigger without a matching game.cpp change.
  qa_.register_scene_state(
      "DoorScene", [this] { apply_scene_state(SceneState::DoorScene); });

  // Base-level questions play out in SceneState::Normal (already the
  // active state at this point -- see apply_scene_state() above); once a
  // follow-up branch is fully explored and the view pops back to the
  // top-level list, put the scene back to that same base state too, so a
  // dialogue-triggered state (like DoorScene above) doesn't linger once
  // the player has moved on from the question that caused it.
  qa_.set_on_returned_to_root(
      [this] { apply_scene_state(SceneState::Normal); });

  renderer_set_pixelation_enabled(true);
  renderer_set_pixelation_block_size(5);

  // Stronger than the engine's subtle defaults -- this scene is a dim
  // attic room lit mainly by one bright window, so the default bloom
  // threshold/intensity and vignette strength/radius read as basically
  // invisible against it.
  renderer_set_bloom_enabled(false);
  renderer_set_bloom_threshold(0.5f);
  renderer_set_bloom_intensity(0.3f);
  renderer_set_vignette_enabled(true);
  renderer_set_vignette_strength(0.5f);
  renderer_set_vignette_radius(0.5f);

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
  // Only SceneState::Normal actually frames the man model these positions
  // are estimated against -- DoorScene swaps in door_scene.sdf instead (see
  // apply_scene_state()), so drawing these there would pin a censor box to
  // coordinates that don't correspond to anything actually on screen.
  if (current_state_ == SceneState::Normal) {
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

  return true;
}

void SHGame::on_resize(u32 width, u32 height) {
  KDEBUG("Resized to {}x{}", width, height);
  width_ = width;
  height_ = height;
}
