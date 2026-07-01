
#pragma once
#include "../defines.h"
#include "../platform/platform.h"
#include "clock.h"
#include "event.h"
#include <memory>
#include <string>

class Game; // forward declaration

struct ApplicationConfig {
  i16 start_pos_x = 100;
  i16 start_pos_y = 100;
  i16 start_width = 1280;
  i16 start_height = 720;
  std::string name = "Engine Application";
};

class KAPI Application {
public:
  Application(Game *game);
  ~Application();

  // No copies — there's only ever one application.
  Application(const Application &) = delete;
  Application &operator=(const Application &) = delete;

  b8 application_start();
  void application_get_framebuffer_size(u32 *width, u32 *height);

private:
  Game *game;
  PlatformLayer platform;
  b8 is_running = false;
  b8 is_suspended = false;
  i16 width;
  i16 height;
  f64 last_time = 0.0;
  Clock clock;
  // Handlers
  bool on_key(EventCode code, void *sender, const EventContext &ctx);
  bool on_quit(const EventContext &ctx);
  bool on_resized(EventCode code, void *sender, const EventContext &ctx);

  // RAII subscriptions — auto-unregister when Application is destroyed.
  // Declared AFTER any members the handlers touch, so they tear down first.
  EventSubscription quit_sub_;
  EventSubscription key_pressed_sub_;
  EventSubscription key_released_sub_;
  EventSubscription resized_sub_;
};

// Free-function accessor for the live Application instance, so renderer code
// (which doesn't hold an Application reference) can query framebuffer size.
void application_get_framebuffer_size(u32 *width, u32 *height);
