#include "game.h"
#include <core/input.h>
#include <core/logger.h>
#include <renderer/renderer_frontend.h>

#include <glm/glm.hpp>

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

  // Camera stations around the man -- Tab cycles, mouse pans within each
  // pose's max_pan (see CameraSystem).
  cameras_.add_pose({glm::vec3(0.0f, 1.0f, 0.0f), 0.0f, 0.0f, 0.15f});
  cameras_.add_pose({glm::vec3(2.5f, 0.5f, -1.5f), glm::radians(-55.0f),
                     glm::radians(-8.0f), 0.2f});
  cameras_.add_pose({glm::vec3(-1.5f, 1.8f, -2.0f), glm::radians(35.0f),
                     glm::radians(-25.0f), 0.1f});

  // The question block -- selecting a question darkens it permanently and
  // plays its answer lines one Enter press at a time (see QASystem).
  qa_.add_entry("Who are you?",
                {"...", "I don't remember.", "I've been here a long time."});
  qa_.add_entry("Where is this place?",
                {"Somewhere between.", "It doesn't have a name anymore."});
  qa_.add_entry("What is the light?", {"Don't look at it too long."});
  qa_.add_entry("Can I leave?", {"..."});

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
  renderer_draw_text("SH", glm::vec2(32.0f, 32.0f),
                     glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
  if (cameras_.debug_active()) {
    renderer_draw_text("[DEBUG CAM]  WASD move  Q/E down/up  RMB-drag look  "
                       "Shift fast  [0] exit",
                       glm::vec2(32.0f, 64.0f),
                       glm::vec4(1.0f, 0.8f, 0.3f, 1.0f));
  }
  qa_.render();
  return true;
}

void SHGame::on_resize(u32 width, u32 height) {
  KDEBUG("Resized to {}x{}", width, height);
  width_ = width;
  height_ = height;
}
