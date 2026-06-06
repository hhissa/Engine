
#include "../game_types.h"
#include "../renderer/renderer_frontend.h"
#include "event.h"
#include "hmemory.h"
#include "input.h"
#include "logger.h"
#include <X11/Xlib.h>
static EventSystem &event_system() {
  static EventSystem instance;
  return instance;
}

Application::Application(Game *game)
    : game(game),
      platform(game->app_config.name, game->app_config.start_pos_x,
               game->app_config.start_pos_y, game->app_config.start_width,
               game->app_config.start_height),
      width(game->app_config.start_width),
      height(game->app_config.start_height) {
  Logger::initialize_logging();
  EventSystem &events = event_system();
  if (!events.initialize()) {
    // already initialized, or some other failure
    KFATAL("failed to initalize event system");
  }

  input::initialize(events);
  KINFO("Application created: {} ({}x{})", game->app_config.name, width,
        height);

  KFATAL("A test message: {}", 3.14f);
  KERROR("A test message: {}", 3.14f);
  KWARN("A test message: {}", 3.14f);
  KINFO("A test message: {}", 3.14f);
  KDEBUG("A test message: {}", 3.14f);
  KTRACE("A test message: {}", 3.14f);

  quit_sub_ =
      events.subscribe(EventCode::EVENT_CODE_APPLICATION_QUIT,
                       [this](EventCode, void *, const EventContext &ctx) {
                         return on_quit(ctx);
                       });

  auto key_cb = [this](EventCode c, void *s, const EventContext &ctx) {
    return on_key(c, s, ctx);
  };
  key_pressed_sub_ =
      events.subscribe(EventCode::EVENT_CODE_KEY_PRESSED, key_cb);
  key_released_sub_ =
      events.subscribe(EventCode::EVENT_CODE_KEY_RELEASED, key_cb);

  if (!renderer_initialize(game->app_config.name, platform)) {
    KFATAL("Failed to initialize renderer. Aborting application.");
  }
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
  clock.start();
  clock.update();
  last_time = clock.elapsed;
  f64 running_time = 0;
  u8 frame_count = 0;
  f64 target_frame_seconds = 1.0f / 60;

  KINFO(Memory::usage_string());
  while (is_running) {
    if (!platform.platform_pump_messages()) {
      is_running = false;
      break;
    }

    if (!is_suspended) {
      clock.update();
      f64 current_time = clock.elapsed;
      f64 delta = current_time - last_time;
      f64 frame_start_time = Platform::get_absolute_time();
      if (!game->update(delta)) {
        KFATAL("Game update failed, shutting down.");
        is_running = false;
        break;
      }
      if (!game->render(delta)) {
        KFATAL("Game render failed, shutting down.");
        is_running = false;
        break;
      }
      // TODO: refacotr packet creation
      render_packet packet;
      packet.delta_time = delta;
      renderer_draw_frame(&packet);

      f64 frame_end_time = Platform::get_absolute_time();
      f64 frame_elapsed_time = frame_end_time - frame_start_time;
      running_time += frame_elapsed_time;
      f64 remaining_seconds = target_frame_seconds - frame_elapsed_time;

      if (remaining_seconds > 0) {
        u64 remaining_ms = remaining_seconds * 1000;
        b8 limit_frames = FALSE;
        if (remaining_ms > 0 && limit_frames) {
          Platform::sleep(remaining_ms - 10);
        }
        frame_count++;
      }

      input::update(delta);
      last_time = current_time;
    }
  }
  input::shutdown();
  event_system().shutdown();
  renderer_shutdown();

  return true;
}

// --- Handlers ---------------------------------------------------------------

bool Application::on_quit(const EventContext & /*ctx*/) {

  KINFO("EVENT_CODE_APPLICATION_QUIT received, shutting down.");
  is_running = false;
  return true;
}

bool Application::on_key(EventCode code, void * /*sender*/,
                         const EventContext &ctx) {
  const auto key = static_cast<input::Key>(ctx.data.u16[0]);
  const char ch = static_cast<char>(ctx.data.u16[0]);

  if (code == EventCode::EVENT_CODE_KEY_PRESSED) {
    if (key == input::Key::Escape) {
      EventContext data{};
      event_system().fire(EventCode::EVENT_CODE_APPLICATION_QUIT, nullptr,
                          data);
      return true;
    }
    if (key == input::Key::A) {
      KDEBUG("Explicit - A key pressed!");
    } else {
      KDEBUG("'{}' key pressed in window.", ch);
    }
  } else if (code == EventCode::EVENT_CODE_KEY_RELEASED) {
    if (key == input::Key::B) {
      KDEBUG("Explicit - B key released!");
    } else {
      KDEBUG("'{}' key released in window.", ch);
    }
  }
  return false;
}
