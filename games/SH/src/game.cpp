// SHGame's core lifecycle: construction, boot sequence, and the per-frame
// update()/render() dispatch to whichever AppScreen is currently showing.
// Each screen's actual behavior lives in its own file -- see game.h's
// per-method comments for exactly which:
//   game_scene_state.cpp -- apply_scene_state()/register_scene_states()
//   game_menus.cpp       -- Title/ChapterSelect/Settings/Intertitle
//   game_playing.cpp     -- AppScreen::Playing (the actual gameplay loop)
//   game_save.cpp        -- save file / settings file persistence

#include "game.h"
#include <core/input.h>
#include <core/logger.h>
#include <renderer/camera.h>
#include <renderer/renderer_frontend.h>

#include <glm/glm.hpp>

#include <cstdlib>
#include <format>
#include <string>

SHGame::SHGame() {
  app_config.start_pos_x = 100;
  app_config.start_pos_y = 100;
  app_config.start_width = 1280;
  app_config.start_height = 720;
  app_config.name = "SH";
  width_ = app_config.start_width;
  height_ = app_config.start_height;

  // Keep baked room chunks on disk between runs, so a room this machine
  // has shown before streams back in from a file instead of being baked
  // again -- the difference between a room cut that's seamless and one
  // that visibly fills in. The engine leaves where such files go to its
  // host (see VulkanRaymarchShader's KENGINE_CHUNK_CACHE_DIR handling), and
  // reads it before the renderer starts, which is after this constructor.
  // An explicit KENGINE_CHUNK_CACHE_DIR still wins.
  if (!std::getenv("KENGINE_CHUNK_CACHE_DIR")) {
    std::string cache_dir;
    if (const char *xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
      cache_dir = std::string(xdg) + "/kengine/sh_chunks";
    } else if (const char *home = std::getenv("HOME")) {
      cache_dir = std::string(home) + "/.cache/kengine/sh_chunks";
    }
    if (!cache_dir.empty()) {
      setenv("KENGINE_CHUNK_CACHE_DIR", cache_dir.c_str(), /*overwrite=*/0);
    }
  }
}

b8 SHGame::initialize() {
  KDEBUG("SHGame::initialize() called!");

  // Equirectangular skybox behind the room -- resolves to
  // assets/textures/skybox_to_equirect_2.png (TextureSystem's usual name ->
  // assets/textures/<name>.png convention, same as every material's
  // diffuse_map_name). Also doubles as the title/menu screens' backdrop
  // until setup_menus() below replaces it with an actual 3D background.
  renderer_enable_sky_box("black");

  // Dialogue content itself lives in .conversation files (see
  // resources/conversation.h for the format), not hardcoded here --
  // nothing is loaded yet at this point, though; that only happens once a
  // chapter is actually chosen (Start/Continue/Chapter Select), via
  // start_chapter() (game_menus.cpp). register_scene_states() just wires
  // each SceneState's tag= name to an apply_scene_state() call -- pure
  // name->callback registration, independent of which (if any) chapter is
  // currently loaded, so it's safe to set up now regardless.
  register_scene_states();

  // Autosave -- Game has no shutdown hook to save-on-quit from instead (see
  // game_types.h), so this fires save_progress() on every single bit of
  // progress the player makes instead (see QASystem::set_on_any_asked()'s
  // own comment). Registered once here rather than per-chapter in
  // start_chapter(), same as register_scene_states() above -- the callback
  // itself doesn't care which conversation is currently loaded.
  qa_.set_on_any_asked([this] { save_progress(); });

  load_settings(); // leaves render_scale_ at its 0.75 default if no
                   // sh_settings.txt exists yet (e.g. first launch)
  renderer_set_render_scale(render_scale_);

  // Rooms are swapped on every cut, and pre-warming blocks until the whole
  // scene is in the disk cache -- so it must not run on each of those
  // loads. Each room is warmed once instead, behind the Preparing screen
  // (see prepare_speaker_rooms()).
  renderer_set_cache_prewarm_on_load(false);

  // Sample the chunked/clipmap field rather than the fixed-cube one.
  // OFF by default (see set_chunked_field_enabled()), and sdf_editor opts
  // in while this didn't -- which is the whole reason the game and the
  // modeller stopped looking alike:
  //
  //   - the fixed-cube field's voxel is COARSE_CELL_SIZE/BRICK_DIM =
  //     0.25/8 = 0.03125 world units; the chunked field's level 0 is
  //     0.25/16 = 0.015625, i.e. exactly twice as fine near the camera;
  //   - stochastic AO, the imperfect shadow maps and the whole point-splat
  //     path are each gated on this flag as well (see the ao_active/
  //     kRenderFlagIsm/splat_active lines in the raymarch shader), so with
  //     it off the game silently ran with no AO and no ISM shadows at all.
  //
  // It costs almost nothing to turn on here, because the work was already
  // happening: VulkanRendererBackend::begin_frame() drives
  // update_streaming() from the camera unconditionally, so this game has
  // been baking and streaming chunks every frame and then not sampling
  // them. The trade is that the clipmap is finer near the camera but falls
  // off with distance, where the fixed cube was a uniform 0.03125 out to
  // BOUNDS=16 -- the right way round for a game of close interviews in
  // small rooms.
  renderer_set_chunked_field_enabled(true);

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

  // Chapter/settings menus, the title screen's background scene, and the
  // initial title_menu_ build -- see game_menus.cpp.
  setup_menus();

  return true;
}

b8 SHGame::update(f32 dt) {
  delta_time_ = dt;

  // Must run before any camera-position state below is used -- a floating-
  // origin recenter (see RendererBackend::consume_origin_shift()'s comment)
  // only shifts the renderer's own transient copy, since the renderer
  // doesn't own this game's cameras_. A no-op ((0,0,0)) whenever floating
  // origin is disabled (the default -- SH's camera never currently
  // free-roams far enough to need it, see CameraPose's own comment), so
  // this is safe to call unconditionally every frame regardless of screen_.
  cameras_.shift_all(renderer_consume_origin_shift());

  switch (screen_) {
  case AppScreen::Title:
    update_title_screen();
    break;
  case AppScreen::ChapterSelect:
    update_chapter_select();
    break;
  case AppScreen::Settings:
    update_settings_screen();
    break;
  case AppScreen::Intertitle:
    update_intertitle();
    break;
  case AppScreen::Preparing:
    update_preparing();
    break;
  case AppScreen::Playing:
    update_playing();
    break;
  }

  return true;
}

b8 SHGame::render(f32 dt) {
  renderer_draw_text("SH", glm::vec2(32.0f, 32.0f),
                     glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));

  switch (screen_) {
  case AppScreen::Title:
    render_title_screen();
    break;
  case AppScreen::ChapterSelect:
    render_chapter_select();
    break;
  case AppScreen::Settings:
    render_settings_screen();
    break;
  case AppScreen::Intertitle:
    render_intertitle();
    break;
  case AppScreen::Preparing:
    render_preparing();
    break;
  case AppScreen::Playing:
    render_playing();
    break;
  }

  return true;
}

void SHGame::toggle_debug_camera_if_pressed() {
  if (input::is_key_down(input::Key::Zero) &&
      !input::was_key_down(input::Key::Zero)) {
    cameras_.toggle_debug();
  }
}

void SHGame::render_debug_camera_hud() const {
  if (!cameras_.debug_active()) {
    return;
  }
  const Camera &camera = cameras_.current_camera();
  glm::vec3 pos = camera.position();
  renderer_draw_text(
      std::format("[DEBUG CAM]  WASD move  Q/E down/up  RMB-drag look  "
                  "Shift fast  [0] exit  pos=({:.2f}, {:.2f}, {:.2f})  "
                  "yaw={:.2f} pitch={:.2f}",
                  pos.x, pos.y, pos.z, camera.yaw(), camera.pitch()),
      glm::vec2(32.0f, 64.0f), glm::vec4(1.0f, 0.8f, 0.3f, 1.0f));
}

void SHGame::on_resize(u32 width, u32 height) {
  KDEBUG("Resized to {}x{}", width, height);
  width_ = width;
  height_ = height;
}
