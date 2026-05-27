// platform_linux.cpp
#include "platform.h"

#if KPLATFORM_LINUX

#include "../core/logger.h"

#include <X11/XKBlib.h>
#include <X11/Xlib-xcb.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <sys/time.h>
#include <xcb/xcb.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <string_view>
#include <thread>

// ─── Per-window state, hidden inside the .cpp ──────────────────────────────
struct linux_state {
  Display *display;
  xcb_connection_t *connection;
  xcb_window_t window;
  xcb_screen_t *screen;
  xcb_atom_t wm_protocols;
  xcb_atom_t wm_delete_win;
};

// ─── PlatformLayer: owns the X window ──────────────────────────────────────
PlatformLayer::PlatformLayer(std::string_view application_name, i32 x, i32 y,
                             i32 width, i32 height)
    : application_name(application_name), x(x), y(y), width(width),
      height(height), internal_state(new linux_state{}) {
  auto *state = static_cast<linux_state *>(internal_state);

  state->display = XOpenDisplay(nullptr);
  if (!state->display) {
    KFATAL("Could not open X display. Is $DISPLAY set?");
  }
  XAutoRepeatOff(state->display);

  state->connection = XGetXCBConnection(state->display);
  if (xcb_connection_has_error(state->connection)) {
    KFATAL("Failed to connect to X server via XCB.");
  }

  const xcb_setup_t *setup = xcb_get_setup(state->connection);
  xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
  state->screen = it.data;

  state->window = xcb_generate_id(state->connection);

  constexpr u32 event_values =
      XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
      XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
      XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_POINTER_MOTION |
      XCB_EVENT_MASK_STRUCTURE_NOTIFY;

  constexpr u32 event_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
  const u32 value_list[] = {state->screen->black_pixel, event_values};

  xcb_create_window(state->connection, XCB_COPY_FROM_PARENT, state->window,
                    state->screen->root, x, y, width, height, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, state->screen->root_visual,
                    event_mask, value_list);

  xcb_change_property(state->connection, XCB_PROP_MODE_REPLACE, state->window,
                      XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                      application_name.size(), application_name.data());

  constexpr std::string_view DELETE_WIN_NAME = "WM_DELETE_WINDOW";
  constexpr std::string_view PROTOCOLS_NAME = "WM_PROTOCOLS";

  auto wm_delete_cookie = xcb_intern_atom(
      state->connection, 0, DELETE_WIN_NAME.size(), DELETE_WIN_NAME.data());
  auto wm_protocols_cookie = xcb_intern_atom(
      state->connection, 0, PROTOCOLS_NAME.size(), PROTOCOLS_NAME.data());

  auto *wm_delete_reply =
      xcb_intern_atom_reply(state->connection, wm_delete_cookie, nullptr);
  auto *wm_protocols_reply =
      xcb_intern_atom_reply(state->connection, wm_protocols_cookie, nullptr);
  if (!wm_delete_reply || !wm_protocols_reply) {
    KFATAL("Could not intern WM atoms.");
  }
  state->wm_delete_win = wm_delete_reply->atom;
  state->wm_protocols = wm_protocols_reply->atom;

  xcb_change_property(state->connection, XCB_PROP_MODE_REPLACE, state->window,
                      wm_protocols_reply->atom, XCB_ATOM_ATOM, 32, 1,
                      &wm_delete_reply->atom);

  std::free(wm_delete_reply);
  std::free(wm_protocols_reply);

  xcb_map_window(state->connection, state->window);

  if (i32 stream_result = xcb_flush(state->connection); stream_result <= 0) {
    KFATAL("An error occurred when flushing the stream: {}", stream_result);
  }
}

PlatformLayer::~PlatformLayer() {
  auto *state = static_cast<linux_state *>(internal_state);

  XAutoRepeatOn(state->display);
  xcb_destroy_window(state->connection, state->window);
  XCloseDisplay(state->display);

  delete state;
  internal_state = nullptr;
}

b8 PlatformLayer::platform_pump_messages() {
  auto *state = static_cast<linux_state *>(internal_state);
  b8 quit_flagged = false;

  xcb_generic_event_t *event = nullptr;
  while ((event = xcb_poll_for_event(state->connection)) != nullptr) {
    switch (event->response_type & ~0x80) {
    case XCB_KEY_PRESS:
    case XCB_KEY_RELEASE: {
      // TODO: key handling
      break;
    }
    case XCB_BUTTON_PRESS:
    case XCB_BUTTON_RELEASE: {
      // TODO: mouse buttons
      break;
    }
    case XCB_MOTION_NOTIFY: {
      // TODO: mouse movement
      break;
    }
    case XCB_CONFIGURE_NOTIFY: {
      // TODO: resize
      break;
    }
    case XCB_CLIENT_MESSAGE: {
      auto *cm = reinterpret_cast<xcb_client_message_event_t *>(event);
      if (cm->data.data32[0] == state->wm_delete_win) {
        quit_flagged = true;
      }
      break;
    }
    default:
      break;
    }
    std::free(event);
  }

  return !quit_flagged;
}

// ─── Platform namespace: stateless OS utilities ────────────────────────────
namespace Platform {

void *allocate(u64 size, b8 /*aligned*/) {
  // TODO: alignment
  return std::malloc(size);
}

void free(void *block, b8 /*aligned*/) {
  // TODO: alignment
  std::free(block);
}

void *zero_memory(void *block, u64 size) { return std::memset(block, 0, size); }

void *copy_memory(void *dest, const void *source, u64 size) {
  return std::memcpy(dest, source, size);
}

void *set_memory(void *dest, i32 value, u64 size) {
  return std::memset(dest, value, size);
}

namespace {
constexpr std::array<std::string_view, 6> COLOUR_CODES = {
    "0;41", // FATAL — white on red
    "1;31", // ERROR — bright red
    "1;33", // WARN  — bright yellow
    "1;32", // INFO  — bright green
    "1;34", // DEBUG — bright blue
    "1;30"  // TRACE — gray
};
}

void console_write(std::string_view message, u8 colour) {
  std::cout << std::format("\033[{}m{}\033[0m", COLOUR_CODES[colour], message);
}

void console_write_error(std::string_view message, u8 colour) {
  std::cerr << std::format("\033[{}m{}\033[0m", COLOUR_CODES[colour], message);
}

f64 get_absolute_time() {
  using namespace std::chrono;
  auto now = steady_clock::now().time_since_epoch();
  return duration<f64>(now).count();
}

void sleep(u64 ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

} // namespace Platform

#endif // KPLATFORM_LINUX
