#include "application.h"
#include "../game_types.h"
#include "hmemory.h"
#include "logger.h"

Application::Application(Game *game)
    : game(game),
      platform(game->app_config.name, game->app_config.start_pos_x,
               game->app_config.start_pos_y, game->app_config.start_width,
               game->app_config.start_height),
      width(game->app_config.start_width),
      height(game->app_config.start_height) {
  Logger::initialize_logging();
  KINFO("Application created: {} ({}x{})", game->app_config.name, width,
        height);

  KFATAL("A test message: {}", 3.14f);
  KERROR("A test message: {}", 3.14f);
  KWARN("A test message: {}", 3.14f);
  KINFO("A test message: {}", 3.14f);
  KDEBUG("A test message: {}", 3.14f);
  KTRACE("A test message: {}", 3.14f);

  if (!game->initialize()) {
    KFATAL("Game failed to initialize.");
  }

  game->on_resize(width, height);
}

Application::~Application() {
  KINFO("Application shutting down.");
  // PlatformLayer destructor runs automatically.
}

b8 Application::application_start() {
  is_running = true;
  KINFO(Memory::usage_string());
  while (is_running) {
    if (!platform.platform_pump_messages()) {
      is_running = false;
      break;
    }

    if (!is_suspended) {
      if (!game->update(0.0f)) {
        KFATAL("Game update failed, shutting down.");
        is_running = false;
        break;
      }
      if (!game->render(0.0f)) {
        KFATAL("Game render failed, shutting down.");
        is_running = false;
        break;
      }
    }
  }

  return true;
}
