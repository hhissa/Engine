#pragma once
#include "camera_system.h"
#include "qa_system.h"

#include <defines.h>
#include <game_types.h>
#include <renderer/renderer_types.inl>

class SHGame : public Game {
public:
  SHGame();
  ~SHGame() override = default;

  b8 initialize() override;
  b8 update(f32 delta_time) override;
  b8 render(f32 delta_time) override;
  void on_resize(u32 width, u32 height) override;

private:
  f32 delta_time_ = 0.0f;

  QASystem qa_;
  CameraSystem cameras_;

  // Current framebuffer size -- the camera system needs it to normalize
  // the mouse position. Seeded from app_config, kept fresh by on_resize().
  u32 width_ = 0;
  u32 height_ = 0;

  SceneHandle scene_ = kInvalidSceneHandle;
  SceneHandle light1_ = kInvalidSceneHandle;
  SceneHandle light2_ = kInvalidSceneHandle;

  SceneHandle overheadLights_ = kInvalidSceneHandle;

  SceneHandle room = kInvalidSceneHandle;
};
