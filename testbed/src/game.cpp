#include "game.h"
#include <core/input.h>
#include <core/logger.h>
#include <renderer/renderer_frontend.h>

#include <cmath>
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
  camera_.set_position(glm::vec3(0.0f, 1.1f, -2.6f));

  // Example scene load -- see renderer_load_scene()'s doc comment.
  // Multiple scenes can be loaded concurrently; each renderer_load_scene()
  // call returns its own independent handle.
  demo_scene_ = renderer_load_scene("assets/scenes/man_sitting.sdf");

#ifdef TESTBED_STREAM_BENCH
  renderer_set_chunked_field_enabled(true);
  demo_scene_ = renderer_load_scene("assets/scenes/DiegosOffice.sdf");
  camera_.set_position(glm::vec3(0.0f, -2.0f, 0.0f));
#endif

  return true;
}

b8 TestbedGame::update(f32 dt) {
  delta_time = dt;

#ifdef TESTBED_STREAM_BENCH
  {
    static f32 bench_t = 0.0f;
    bench_t += dt;
    {
      static f32 window = 0.0f, worst = 0.0f, total = 0.0f;
      static int frames = 0, o100 = 0, o50 = 0, o33 = 0, o20 = 0;
      if (bench_t > 3.0f) {
        window += dt; total += dt;
        worst = worst > dt ? worst : dt;
        ++frames;
        if (dt > 0.100f) ++o100;
        if (dt > 0.050f) ++o50;
        if (dt > 0.033f) ++o33;
        if (dt > 0.020f) ++o20;
        if (window >= 10.0f) {
          KINFO("WALLCLOCK {} frames: mean {:.1f} ms WORST {:.1f} ms | "
               ">20ms {} >33ms {} >50ms {} >100ms {}",
               frames, 1000.0f * total / static_cast<f32>(frames),
               1000.0f * worst, o20, o33, o50, o100);
          window = 0.0f; worst = 0.0f; total = 0.0f; frames = 0;
          o100 = o50 = o33 = o20 = 0;
        }
      }
    }
    // Radius 25 -- INSIDE the +-45 unit tiling this time.
    const f32 x = 25.0f * std::sin(bench_t * 0.15f);
    const f32 z = 25.0f * std::cos(bench_t * 0.11f);
    camera_.set_position(glm::vec3(x, -2.0f, z));
    renderer_set_camera(camera_);
    return true;
  }
#endif

  // Must run before camera_ is used/submitted below -- a floating-origin
  // recenter (see RendererBackend::consume_origin_shift()'s comment) only
  // shifts the renderer's own transient copy, since the renderer doesn't
  // own this Camera. A no-op ((0,0,0)) whenever floating origin is
  // disabled (the default), so this is safe to call unconditionally.
  camera_.shift(renderer_consume_origin_shift());

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

  // Example remove/clear calls -- edge-detected (is_key_down() &&
  // !was_key_down()) since each one triggers a full scene rebake (a
  // device-idle wait), so holding the key down must not repeat it.
  if (input::is_key_down(input::Key::R) && !input::was_key_down(input::Key::R)) {
    renderer_remove_scene(demo_scene_);
    demo_scene_ = kInvalidSceneHandle;
  }
  if (input::is_key_down(input::Key::C) && !input::was_key_down(input::Key::C)) {
    renderer_clear_scenes();
    demo_scene_ = kInvalidSceneHandle;
  }

  // Render-scale demo keybinds -- Minus/Plus step render_scale_ by 0.1
  // (clamped to [0.25, 1.0]) and push it to the renderer. Edge-detected
  // like R/C above: set_render_scale() waits for the device to go idle to
  // safely recreate the render targets, so it must fire once per press,
  // not every frame the key is held.
  if (input::is_key_down(input::Key::Minus) &&
      !input::was_key_down(input::Key::Minus)) {
    render_scale_ = glm::clamp(render_scale_ - 0.1f, 0.25f, 1.0f);
    renderer_set_render_scale(render_scale_);
    KDEBUG("Render scale: {}", render_scale_);
  }
  if (input::is_key_down(input::Key::Plus) &&
      !input::was_key_down(input::Key::Plus)) {
    render_scale_ = glm::clamp(render_scale_ + 0.1f, 0.25f, 1.0f);
    renderer_set_render_scale(render_scale_);
    KDEBUG("Render scale: {}", render_scale_);
  }

  return true;
}

b8 TestbedGame::render(f32 dt) {
  renderer_draw_text("Hello, engine", glm::vec2(32.0f, 32.0f),
                     glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
  return true;
}

void TestbedGame::on_resize(u32 width, u32 height) {
  KDEBUG("Resized to {}x{}", width, height);
}
