#pragma once
#include "core/application.h"
#include "defines.h"

class KAPI Game {
public:
  Game() = default;
  virtual ~Game() = default;

  Game(const Game &) = delete;
  Game &operator=(const Game &) = delete;

  virtual b8 initialize() = 0;
  virtual b8 update(f32 delta_time) = 0;
  virtual b8 render(f32 delta_time) = 0;
  virtual void on_resize(u32 width, u32 height) = 0;

  ApplicationConfig app_config;
};
