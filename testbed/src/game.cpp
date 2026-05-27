#include "game.h"
#include <core/logger.h>

TestbedGame::TestbedGame() {
  app_config.start_pos_x = 100;
  app_config.start_pos_y = 100;
  app_config.start_width = 1280;
  app_config.start_height = 720;
  app_config.name = "Bingus Engine Testbed";
}

b8 TestbedGame::initialize() {
  KDEBUG("TestbedGame::initialize() called!");
  return true;
}

b8 TestbedGame::update(f32 dt) {
  delta_time = dt;
  return true;
}

b8 TestbedGame::render(f32 dt) { return true; }

void TestbedGame::on_resize(u32 width, u32 height) {
  KDEBUG("Resized to {}x{}", width, height);
}
