#pragma once
#include <defines.h>
#include <game_types.h>

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
};
