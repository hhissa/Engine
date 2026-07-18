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
};
