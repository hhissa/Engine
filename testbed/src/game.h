#pragma once
#include <defines.h>
#include <game_types.h>
#include <renderer/camera.h>
#include <renderer/renderer_types.inl>

class TestbedGame : public Game {
public:
  TestbedGame();
  ~TestbedGame() override = default;

  b8 initialize() override;
  b8 update(f32 delta_time) override;
  b8 render(f32 delta_time) override;
  void on_resize(u32 width, u32 height) override;

private:
  f32 delta_time = 0.0f;
  Camera camera_;

  // Handle for the demo scene loaded in initialize() (see renderer_load_
  // scene()) -- kInvalidSceneHandle once removed via the 'R'/'C' demo
  // keybinds in update(), so a second press is a harmless no-op.
  SceneHandle demo_scene_ = kInvalidSceneHandle;

  // Adjusted by the Minus/Plus demo keybinds in update() -- see
  // renderer_set_render_scale(). Mirrors what the renderer's already been
  // told, so repeated presses at the clamped ends are harmless no-ops
  // instead of redundant device-idle-waiting calls every frame held.
  f32 render_scale_ = 1.0f;
};
