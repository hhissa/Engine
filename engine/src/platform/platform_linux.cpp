// platform_linux.cpp
#include "platform.h"
#include <cstdlib>

// Linux platform layer.
#if KPLATFORM_LINUX

#include "../core/logger.h"

#include <X11/XKBlib.h>   // sudo apt-get install libx11-dev
#include <X11/Xlib-xcb.h> // sudo apt-get install libxkbcommon-x11-dev
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <sys/time.h>
#include <xcb/xcb.h>

#include <array> // std::array
#include <chrono>
#include <format>   // std::format
#include <iostream> // std::cout, std::cerr
#include <stdlib.h>
#include <string.h>
#include <string_view> // std::string_view
#include <thread>

struct linux_state {
  Display *display;
  xcb_connection_t *connection;
  xcb_window_t window;
  xcb_screen_t *screen;
  xcb_atom_t wm_protocols;
  xcb_atom_t wm_delete_win;
} internal_state;

PlatformLayer::PlatformLayer(std::string_view application_name, i32 x, i32 y,
                             i32 width, i32 height)
    : application_name(application_name), x(x), y(y), width(width),
      height(height), internal_state(new linux_state{}) {
  auto *state = static_cast<linux_state *>(internal_state);
  state->display = XOpenDisplay(nullptr);
  // turn off key repeats
  XAutoRepeatOff(state->display);

  state->connection = XGetXCBConnection(state->display);

  if (xcb_connection_has_error(state->connection)) {
    KFATAL("Failed to connect to X server via XCB.");
  }

  // Get data from the X server
  const struct xcb_setup_t *setup = xcb_get_setup(state->connection);

  // Loop through screens using iterator
  xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
  int screen_p = 0;
  for (i32 s = screen_p; s > 0; s--) {
    xcb_screen_next(&it);
  }

  // After screens have been looped through, assign it.
  state->screen = it.data;

  // Allocate a XID for the window to be created.
  state->window = xcb_generate_id(state->connection);

  // Register event types.
  // XCB_CW_BACK_PIXEL = filling then window bg with a single colour
  // XCB_CW_EVENT_MASK is required.

  u32 event_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;

  // Listen for keyboard and mouse buttons
  /* these flags mean XCB_EVENT_MASK_BUTTON_PRESS: mouse press
                       XCB_EVENT_MASK_BUTTON_RELEASE: mouse release |
     XCB_EVENT_MASK_KEY_PRESS: keyboard | XCB_EVENT_MASK_KEY_RELEASE keyboard |
     XCB_EVENT_MASK_EXPOSURE visibility of window (minimized/visible)|
                       XCB_EVENT_MASK_POINTER_MOTION mouse movement|
                       XCB_EVENT_MASK_STRUCTURE_NOTIFY closure events;
  */

  u32 event_values = XCB_EVENT_MASK_BUTTON_PRESS |
                     XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_KEY_PRESS |
                     XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_EXPOSURE |
                     XCB_EVENT_MASK_POINTER_MOTION |
                     XCB_EVENT_MASK_STRUCTURE_NOTIFY;

  // Values to be sent over XCB (bg colour, events)
  u32 value_list[] = {state->screen->black_pixel, event_values};

  // Create the window
  xcb_void_cookie_t cookie =
      xcb_create_window(state->connection,
                        XCB_COPY_FROM_PARENT, // depth
                        state->window,
                        state->screen->root,           // parent
                        x,                             // x
                        y,                             // y
                        width,                         // width
                        height,                        // height
                        0,                             // No border
                        XCB_WINDOW_CLASS_INPUT_OUTPUT, // class
                        state->screen->root_visual, event_mask, value_list);

  // Change the title
  xcb_change_property(state->connection, XCB_PROP_MODE_REPLACE, state->window,
                      XCB_ATOM_WM_NAME, XCB_ATOM_STRING,
                      8, // data should be viewed 8 bits at a time
                      application_name.size(), application_name.data());

  // Tell the server to notify when the window manager
  // attempts to destroy the window.

  constexpr std::string_view DELETE_WIN_NAME = "WM_DELETE_WINDOW";
  constexpr std::string_view PROTOCOLS_NAME = "WM_PROTOCOLS";

  // listen to delete window and protocol message and retrieve function pointers
  // to methods
  auto wm_delete_cookie = xcb_intern_atom(
      state->connection, 0, DELETE_WIN_NAME.size(), DELETE_WIN_NAME.data());
  auto wm_protocols_cookie = xcb_intern_atom(
      state->connection, 0, PROTOCOLS_NAME.size(), PROTOCOLS_NAME.data());
  xcb_intern_atom_reply_t *wm_delete_reply =
      xcb_intern_atom_reply(state->connection, wm_delete_cookie, nullptr);
  xcb_intern_atom_reply_t *wm_protocols_reply =
      xcb_intern_atom_reply(state->connection, wm_protocols_cookie, nullptr);
  state->wm_delete_win = wm_delete_reply->atom;
  state->wm_protocols = wm_protocols_reply->atom;

  // For this window, the list of WM protocols I support is [WM_DELETE_WINDOW]
  xcb_change_property(state->connection, XCB_PROP_MODE_REPLACE, state->window,
                      wm_protocols_reply->atom, 4, 32, 1,
                      &wm_delete_reply->atom);

  // Map the window to the screen
  xcb_map_window(state->connection, state->window);

  // Flush the stream
  i32 stream_result = xcb_flush(state->connection);
  if (stream_result <= 0) {
    KFATAL("An error occurred when flusing the stream: {}", stream_result);
  }
}

// Destructor
PlatformLayer::~PlatformLayer() {
  // cleanup
  auto *state = static_cast<linux_state *>(internal_state);

  // Restore keyboard auto-repeat — global setting, MUST be undone.
  XAutoRepeatOn(state->display);

  // Destroy the window (tell the X server to free it).
  xcb_destroy_window(state->connection, state->window);

  // Close the display, which also closes the XCB connection
  // (XGetXCBConnection shares ownership with the Display).
  XCloseDisplay(state->display);

  // Free our heap-allocated state struct (paired with `new linux_state{}`).
  delete state;
  internal_state = nullptr;
}

// Methods: ReturnType ClassName::method_name(params)
b8 PlatformLayer::platform_pump_messages() {
  auto *state = static_cast<linux_state *>(internal_state);
  b8 quit_flagged = false;

  xcb_generic_event_t *event = nullptr;
  while ((event = xcb_poll_for_event(state->connection)) != nullptr) {
    switch (event->response_type & ~0x80) {
    case XCB_KEY_PRESS:
    case XCB_KEY_RELEASE: {
      // TODO: Key presses and releases
      break;
    }
    case XCB_BUTTON_PRESS:
    case XCB_BUTTON_RELEASE: {
      // TODO: Mouse button presses and releases
      break;
    }
    case XCB_MOTION_NOTIFY: {
      // TODO: mouse movement
      break;
    }
    case XCB_CONFIGURE_NOTIFY: {
      // TODO: Resizing
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
void *PlatformLayer::platform_allocate(u64 size, b8 aligned) {
  // your allocator
  return malloc(size);
}

void PlatformLayer::platform_free(void *block, b8 aligned) {
  // free it
  free(block);
}

void *PlatformLayer::platform_zero_memory(void *block, u64 size) {
  return memset(block, 0, size);
}

void *PlatformLayer::platform_copy_memory(void *dest, const void *source,
                                          u64 size) {
  return memcpy(dest, source, size);
}

void *PlatformLayer::platform_set_memory(void *dest, i32 value, u64 size) {
  return memset(dest, value, size);
}

void PlatformLayer::platform_console_write(std::string_view message,
                                           u8 colour) {
  constexpr std::array<std::string_view, 6> colour_codes = {
      "0;41", "1;31", "1;33", "1;32", "1;34", "1;30"};
  std::cout << std::format("\033[{}m{}\033[0m", colour_codes[colour], message);
}

void PlatformLayer::platform_console_write_error(std::string_view message,
                                                 u8 colour) {
  constexpr std::array<std::string_view, 6> colour_codes = {
      "0;41", "1;31", "1;33", "1;32", "1;34", "1;30"};
  std::cerr << std::format("\033[{}m{}\033[0m", colour_codes[colour], message);
}

f64 PlatformLayer::platform_get_absolute_time() {
  using namespace std::chrono;
  auto now = steady_clock::now().time_since_epoch();
  return duration<f64>(now).count();
}

void PlatformLayer::platform_sleep(u64 ms) {
  // nanosleep / usleep / std::this_thread::sleep_for
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
#endif
