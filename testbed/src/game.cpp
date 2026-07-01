#include "game.h"
#include <core/input.h>
#include <core/logger.h>
#include <renderer/renderer_frontend.h>

#include <glm/glm.hpp>

TestbedGame::TestbedGame() {
  app_config.start_pos_x = 100;
  app_config.start_pos_y = 100;
  app_config.start_width = 1280;
  app_config.start_height = 720;
  app_config.name = "Bingus Engine Testbed";
}

b8 TestbedGame::initialize() {
  KDEBUG("TestbedGame::initialize() called!");
  camera_.set_position(glm::vec3(0.0f, 0.0f, -3.0f));
  return true;
}

b8 TestbedGame::update(f32 dt) {
  delta_time = dt;

  constexpr f32 kTurnRate = 1.0f;   // radians/sec
  constexpr f32 kMoveSpeed = 1.5f;  // units/sec -- tuned to this scene's
                                    // ~2-unit-radius bounding box, not
                                    // upstream's much larger scene scale.

  if (input::is_key_down(input::Key::A) ||
      input::is_key_down(input::Key::Left)) {
    camera_.yaw(kTurnRate * dt);
  }
  if (input::is_key_down(input::Key::D) ||
      input::is_key_down(input::Key::Right)) {
    camera_.yaw(-kTurnRate * dt);
  }
  if (input::is_key_down(input::Key::Up)) {
    camera_.pitch(kTurnRate * dt);
  }
  if (input::is_key_down(input::Key::Down)) {
    camera_.pitch(-kTurnRate * dt);
  }

  glm::vec3 velocity(0.0f);
  if (input::is_key_down(input::Key::W)) {
    velocity += camera_.forward();
  }
  if (input::is_key_down(input::Key::S)) {
    velocity -= camera_.forward();
  }
  if (input::is_key_down(input::Key::Q)) {
    velocity -= camera_.right();
  }
  if (input::is_key_down(input::Key::E)) {
    velocity += camera_.right();
  }
  if (input::is_key_down(input::Key::Space)) {
    velocity.y += 1.0f;
  }
  if (input::is_key_down(input::Key::X)) {
    velocity.y -= 1.0f;
  }

  if (glm::length(velocity) > 0.0002f) {
    camera_.move(glm::normalize(velocity) * (kMoveSpeed * dt));
  }

  renderer_set_camera(camera_);

  return true;
}

b8 TestbedGame::render(f32 dt) { return true; }

void TestbedGame::on_resize(u32 width, u32 height) {
  KDEBUG("Resized to {}x{}", width, height);
}
