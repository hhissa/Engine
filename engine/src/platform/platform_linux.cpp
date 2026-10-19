// platform_linux.cpp
#include "platform.h"

#if KPLATFORM_LINUX

#include "../core/event.h"
#include "../core/input.h"
#include "../core/logger.h"

#include <X11/XKBlib.h>
#include <X11/Xlib-xcb.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <sys/time.h>
#include <xcb/xcb.h>

#undef None // get rid of X11's macro
#undef Bool // and these while you're at it
#undef Status
#undef True
#undef False
#undef Success
#undef Always

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <string_view>
#include <thread>

// Gamepad support: the raw Linux joystick API, not evdev directly -- "give
// me normalized axis/button events" is exactly what's needed here, and it
// needs no extra library (unlike SDL2, whose headers aren't available in
// this build environment).
#include <cerrno>
#include <fcntl.h>
#include <linux/joystick.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define VK_USE_PLATFORM_XCB_KHR
#include "../renderer/vulkan/vulkan_types.inl"
#include <vulkan/vulkan.h>

input::Key translate_keycode(u32 x_keycode);
// ─── Per-window state, hidden inside the .cpp ──────────────────────────────
struct linux_state {
  Display *display;
  xcb_connection_t *connection;
  xcb_window_t window;
  xcb_screen_t *screen;
  xcb_atom_t wm_protocols;
  xcb_atom_t wm_delete_win;
  // False when constructed from a BorrowedWindowHandle -- display/screen/
  // wm_* are unused (left zero-initialized) in that case, and the
  // destructor must not destroy a window/connection/display it doesn't own.
  b8 owns_window;

  // Gamepad device (see poll_gamepad() below). -1 when no device is
  // currently open -- either none has ever been found, or the last one
  // open'd was unplugged. Independent of owns_window: a joystick device
  // node is a system-wide resource, not tied to this window/connection.
  i32 gamepad_fd = -1;
  // Throttles the open() retry when no device is currently open, so an
  // unplugged controller doesn't cost a filesystem stat() every single
  // platform_pump_messages() call (cheap, but pointless at render-loop
  // frequency). Counts calls, not time -- simpler, and "every ~2 seconds
  // at 60fps" is plenty responsive for noticing a hot-plug.
  i32 gamepad_reopen_countdown = 0;
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

  state->owns_window = true;
}

// ─── PlatformLayer: wraps a window owned by something else ────────────────
PlatformLayer::PlatformLayer(BorrowedWindowHandle handle, i32 width,
                             i32 height)
    : application_name(), x(0), y(0), width(width), height(height),
      internal_state(new linux_state{}) {
  auto *state = static_cast<linux_state *>(internal_state);
  state->connection = static_cast<xcb_connection_t *>(handle.connection);
  state->window = static_cast<xcb_window_t>(handle.window);
  state->owns_window = false;
}

PlatformLayer::~PlatformLayer() {
  auto *state = static_cast<linux_state *>(internal_state);

  if (state->gamepad_fd >= 0) {
    close(state->gamepad_fd);
  }

  if (state->owns_window) {
    XAutoRepeatOn(state->display);
    xcb_destroy_window(state->connection, state->window);
    XCloseDisplay(state->display);
  }

  delete state;
  internal_state = nullptr;
}

namespace {

// Candidate joystick device nodes to probe, in order. The index isn't
// stable across reboots/replugs -- this session observed the connected
// Xbox 360 pad at /dev/input/js1 with js0 being an unrelated keyboard
// consumer-control node, but that's just this session's snapshot, not
// something to hardcode. Probing a short fixed list and keeping the first
// one that both opens AND answers JSIOCGNAME (confirms it's a real
// joystick node, not some other js-numbered device) is simple and good
// enough; a udev-based "watch for new joystick nodes" approach would be
// more correct for exotic multi-pad setups but is overkill for "does this
// engine see a controller at all".
constexpr std::array<const char *, 4> kGamepadDevicePaths = {
    "/dev/input/js0", "/dev/input/js1", "/dev/input/js2", "/dev/input/js3"};

// How many platform_pump_messages() calls to wait between retrying open()
// while no gamepad is connected. O_NONBLOCK opens are cheap, but there's
// no reason to stat() four device nodes every single frame when nothing's
// plugged in -- once every 120 calls is ~2 seconds at 60fps, plenty
// responsive for noticing a hot-plug without being wasteful.
constexpr i32 kGamepadReopenInterval = 120;

// Tries each candidate path; returns an open, confirmed-joystick fd, or -1
// if none worked.
// Opens the first of `paths` that is a joystick device; -1 if none is.
template <typename Paths> i32 open_first_gamepad(const Paths &paths) {
  for (const char *path : paths) {
    i32 fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
      continue;
    }
    char name[128] = {};
    if (ioctl(fd, JSIOCGNAME(sizeof(name)), name) < 0) {
      // Opened, but doesn't answer the joystick ioctl -- not actually a
      // joystick device (js-numbered nodes aren't guaranteed to all be
      // joysticks). Skip it.
      close(fd);
      continue;
    }
    KINFO("Gamepad found: {} ({})", path, name);
    return fd;
  }
  return -1;
}

i32 try_open_gamepad() {
  // KENGINE_GAMEPAD_DEVICE picks one specific joystick node (e.g.
  // /dev/input/js1) instead of the first one found -- for when more than
  // one pad is connected, or to drive the engine from a virtual pad.
  const char *forced = std::getenv("KENGINE_GAMEPAD_DEVICE");
  if (forced && *forced) {
    return open_first_gamepad(std::array<const char *, 1>{forced});
  }
  return open_first_gamepad(kGamepadDevicePaths);
}

// Button 0-10 -> GamepadButton, in the standard xpad driver ordering for
// an Xbox 360 pad: A, B, X, Y, LB, RB, Back, Start, Guide, L3, R3. This is
// well-documented/stable for this family of controller, not a guess.
constexpr std::array<input::GamepadButton, 11> kButtonMap = {
    input::GamepadButton::A,          input::GamepadButton::B,
    input::GamepadButton::X,          input::GamepadButton::Y,
    input::GamepadButton::LeftBumper, input::GamepadButton::RightBumper,
    input::GamepadButton::Back,       input::GamepadButton::Start,
    input::GamepadButton::Guide,      input::GamepadButton::LeftStick,
    input::GamepadButton::RightStick,
};

f32 clamp01(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
f32 clamp11(f32 v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

// Drains every pending js_event on state->gamepad_fd this frame, translates
// each into the matching input::process_gamepad_*() call, and handles
// lazy-open/hot-plug/disconnect of the device itself. See this function's
// call site in platform_pump_messages() for why this runs unconditionally,
// independent of owns_window.
void poll_gamepad(linux_state *state) {
  if (state->gamepad_fd < 0) {
    // No device currently open -- throttle how often we retry, see
    // kGamepadReopenInterval.
    if (state->gamepad_reopen_countdown > 0) {
      --state->gamepad_reopen_countdown;
      return;
    }
    state->gamepad_reopen_countdown = kGamepadReopenInterval;

    state->gamepad_fd = try_open_gamepad();
    if (state->gamepad_fd < 0) {
      // Normal, silent case: no controller plugged in. Only fires an
      // event the first time (process_gamepad_connection() diffs against
      // current state), not every retry.
      input::process_gamepad_connection(false);
      return;
    }
    input::process_gamepad_connection(true);
  }

  js_event ev;
  for (;;) {
    ssize_t n = read(state->gamepad_fd, &ev, sizeof(ev));
    if (n == static_cast<ssize_t>(sizeof(ev))) {
      // JS_EVENT_INIT is ORed into type for the synthetic "here's the
      // current state" events the kernel sends right after open -- these
      // are real initial state, not noise to discard, so mask it off and
      // handle them exactly like a live event.
      u8 type = ev.type & ~JS_EVENT_INIT;

      if (type == JS_EVENT_BUTTON) {
        if (ev.number < kButtonMap.size()) {
          input::process_gamepad_button(kButtonMap[ev.number], ev.value != 0);
        }
        continue;
      }

      if (type != JS_EVENT_AXIS) {
        continue;
      }

      switch (ev.number) {
      case 0: // Left stick X
        input::process_gamepad_axis(input::GamepadAxis::LeftX,
                                    clamp11(ev.value / 32767.0f));
        break;
      case 1: // Left stick Y
        input::process_gamepad_axis(input::GamepadAxis::LeftY,
                                    clamp11(ev.value / 32767.0f));
        break;
      case 3: // Right stick X
        input::process_gamepad_axis(input::GamepadAxis::RightX,
                                    clamp11(ev.value / 32767.0f));
        break;
      case 4: // Right stick Y
        input::process_gamepad_axis(input::GamepadAxis::RightY,
                                    clamp11(ev.value / 32767.0f));
        break;
      case 2: // Left trigger -- hardware range is -32767 (released) ..
              // 32767 (fully pressed), unlike the stick axes' centered
              // range.
        input::process_gamepad_axis(
            input::GamepadAxis::LeftTrigger,
            clamp01((ev.value + 32767) / 65534.0f));
        break;
      case 5: // Right trigger, same mapping as case 2.
        input::process_gamepad_axis(
            input::GamepadAxis::RightTrigger,
            clamp01((ev.value + 32767) / 65534.0f));
        break;
      case 6: // D-pad X (hat, effectively 3-state: -32767/0/32767).
              // Reported as buttons, not an axis -- see GamepadButton's
              // doc comment in input.h for why.
        input::process_gamepad_button(input::GamepadButton::DpadLeft,
                                      ev.value < 0);
        input::process_gamepad_button(input::GamepadButton::DpadRight,
                                      ev.value > 0);
        break;
      case 7: // D-pad Y (hat). Sign convention ASSUMED, not interactively
              // verified against real hardware this session: negative =
              // up, matching stick-Y's own convention (axes 1/4) and the
              // most common Linux joystick hat convention. If a future
              // session with hands-on access finds the d-pad up/down feel
              // inverted, flip this.
        input::process_gamepad_button(input::GamepadButton::DpadUp,
                                      ev.value < 0);
        input::process_gamepad_button(input::GamepadButton::DpadDown,
                                      ev.value > 0);
        break;
      default:
        break;
      }
      continue;
    }

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // No more events pending this frame -- normal, done.
      return;
    }

    // n == 0 (EOF) or any other read error (e.g. ENODEV) means the device
    // is gone. Close it and let the next call's lazy-open retry find it
    // again if replugged.
    close(state->gamepad_fd);
    state->gamepad_fd = -1;
    state->gamepad_reopen_countdown = 0; // retry immediately next call
    input::process_gamepad_connection(false);
    return;
  }
}

} // namespace

b8 PlatformLayer::platform_pump_messages() {
  auto *state = static_cast<linux_state *>(internal_state);

  // Gamepad state is system-wide, not tied to any particular window or
  // X11 connection (unlike keyboard/mouse, which legitimately come from
  // this window's XCB event stream) -- so it's polled unconditionally,
  // BEFORE the owns_window check below. A joystick device file has
  // nothing to do with X11 window ownership: gating this on owns_window
  // would silently break gamepad input for every borrowed-window consumer
  // (e.g. a tool embedding this PlatformLayer in a Qt-owned window) for no
  // architectural reason, which runs against the point of this being an
  // engine-level, not tool-specific, capability.
  poll_gamepad(state);

  // Borrowed windows are pumped by whatever owns their event loop (e.g.
  // Qt's XCB platform plugin, already reading from this exact connection)
  // -- polling it again here would race on the same socket, and the
  // key-translation path below dereferences state->display, which is null
  // in this mode.
  if (!state->owns_window) {
    return TRUE;
  }

  b8 quit_flagged = false;

  xcb_generic_event_t *event = nullptr;
  while ((event = xcb_poll_for_event(state->connection)) != nullptr) {
    switch (event->response_type & ~0x80) {
    case XCB_KEY_PRESS:
    case XCB_KEY_RELEASE: {
      xcb_key_press_event_t *kb_event = (xcb_key_press_event_t *)event;
      b8 pressed = event->response_type == XCB_KEY_PRESS;
      xcb_keycode_t code = kb_event->detail;
      // Column 1 is the *shifted* keysym -- select it from the event's
      // modifier state, not the keycode. Testing the keycode's low bit
      // (`code & ShiftMask`) picked the shifted symbol for every
      // odd-keycoded key: Tab (keycode 23) became XK_ISO_Left_Tab, which
      // translate_keycode didn't recognise, so Tab never registered.
      b8 shifted = (kb_event->state & XCB_MOD_MASK_SHIFT) != 0;
      KeySym key_sym = XkbKeycodeToKeysym(state->display, (KeyCode)code, 0,
                                          shifted ? 1 : 0);

      input::Key key = translate_keycode(key_sym);

      // Pass to the input subsystem for processing.
      input::process_key(key, pressed);
      break;
    }
    case XCB_BUTTON_PRESS:
    case XCB_BUTTON_RELEASE: {
      xcb_button_press_event_t *mouse_event = (xcb_button_press_event_t *)event;
      b8 pressed = event->response_type == XCB_BUTTON_PRESS;
      input::Button mouse_button = input::Button::Count;
      switch (mouse_event->detail) {
      case XCB_BUTTON_INDEX_1:
        mouse_button = input::Button::Left;
        break;
      case XCB_BUTTON_INDEX_2:
        mouse_button = input::Button::Middle;
        break;
      case XCB_BUTTON_INDEX_3:
        mouse_button = input::Button::Right;
        break;
      // X11 reports the scroll wheel as button 4 (up/away) and button 5
      // (down/toward), each delivered as an immediate press+release pair
      // per detent -- process_mouse_wheel() only needs the press half, or
      // every scroll tick would be double-counted (once from a press-half
      // of +-1, then again from a spurious release-half also seen here).
      case XCB_BUTTON_INDEX_4:
        if (pressed) {
          input::process_mouse_wheel(1);
        }
        break;
      case XCB_BUTTON_INDEX_5:
        if (pressed) {
          input::process_mouse_wheel(-1);
        }
        break;
      }

      // Pass over to the input subsystem.
      if (mouse_button != input::Button::Count) {
        input::process_button(mouse_button, pressed);
      }
      break;
    }
    case XCB_MOTION_NOTIFY: {
      // TODO: mouse movement
      xcb_motion_notify_event_t *move_event =
          (xcb_motion_notify_event_t *)event;

      // Pass over to the input subsystem.
      input::process_mouse_move(move_event->event_x, move_event->event_y);
      break;
    }
    case XCB_CONFIGURE_NOTIFY: {
      // Resizing - note that this is also triggered by moving the window,
      // but should be passed anyway since a change in the x/y could mean an
      // upper-left resize. The application layer can decide what to do
      // with this.
      xcb_configure_notify_event_t *configure_event =
          (xcb_configure_notify_event_t *)event;

      input::process_resize(configure_event->width, configure_event->height);
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

void PlatformLayer::get_required_extension_names(
    std::vector<const char *> &names) const {
  names.push_back("VK_KHR_xcb_surface"); // VK_KHR_xlib_surface?
}

b8 PlatformLayer::create_vulkan_surface(VulkanContext &context) {
  auto *state = static_cast<linux_state *>(internal_state);

  VkXcbSurfaceCreateInfoKHR create_info{
      VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR};
  create_info.connection = state->connection;
  create_info.window = state->window;

  VkResult result = vkCreateXcbSurfaceKHR(context.instance, &create_info,
                                          context.allocator, &context.surface);
  if (result != VK_SUCCESS) {
    KFATAL("Vulkan surface creation failed.");
    return FALSE;
  }
  return TRUE;
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

std::string user_cache_directory() {
  if (const char *xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
    return xdg;
  }
  if (const char *home = std::getenv("HOME"); home && *home) {
    return std::string(home) + "/.cache";
  }
  return {};
}

void set_environment_variable(std::string_view name, std::string_view value,
                              b8 overwrite) {
  setenv(std::string(name).c_str(), std::string(value).c_str(),
         overwrite ? 1 : 0);
}

} // namespace Platform
input::Key translate_keycode(u32 x_keycode) {
  using input::Key;

  switch (x_keycode) {
  case XK_BackSpace:
    return Key::Backspace;
  case XK_Return:
    return Key::Enter;
  case XK_Tab:
  case XK_ISO_Left_Tab: // Shift+Tab produces this keysym instead of XK_Tab
    return Key::Tab;
    // case XK_Shift:    return Key::Shift;
    // case XK_Control:  return Key::Control;

  case XK_Pause:
    return Key::Pause;
  case XK_Caps_Lock:
    return Key::Capital;

  case XK_Escape:
    return Key::Escape;

    // Not supported
    // case : return Key::Convert;
    // case : return Key::NonConvert;
    // case : return Key::Accept;

  case XK_Mode_switch:
    return Key::ModeChange;

  case XK_space:
    return Key::Space;
  case XK_Prior:
    return Key::Prior;
  case XK_Next:
    return Key::Next;
  case XK_End:
    return Key::End;
  case XK_Home:
    return Key::Home;
  case XK_Left:
    return Key::Left;
  case XK_Up:
    return Key::Up;
  case XK_Right:
    return Key::Right;
  case XK_Down:
    return Key::Down;
  case XK_Select:
    return Key::Select;
  case XK_Print:
    return Key::Print;
  case XK_Execute:
    return Key::Execute;
  // case XK_snapshot: return Key::Snapshot;  // not supported
  case XK_Insert:
    return Key::Insert;
  case XK_Delete:
    return Key::Delete;
  case XK_Help:
    return Key::Help;

  case XK_Meta_L:
    return Key::LWin; // TODO: not sure this is right
  case XK_Meta_R:
    return Key::RWin;
    // case XK_apps:    return Key::Apps;   // not supported
    // case XK_sleep:   return Key::Sleep;  // not supported

  case XK_KP_0:
    return Key::Numpad0;
  case XK_KP_1:
    return Key::Numpad1;
  case XK_KP_2:
    return Key::Numpad2;
  case XK_KP_3:
    return Key::Numpad3;
  case XK_KP_4:
    return Key::Numpad4;
  case XK_KP_5:
    return Key::Numpad5;
  case XK_KP_6:
    return Key::Numpad6;
  case XK_KP_7:
    return Key::Numpad7;
  case XK_KP_8:
    return Key::Numpad8;
  case XK_KP_9:
    return Key::Numpad9;
  case XK_multiply:
    return Key::Multiply;
  case XK_KP_Add:
    return Key::Add;
  case XK_KP_Separator:
    return Key::Separator;
  case XK_KP_Subtract:
    return Key::Subtract;
  case XK_KP_Decimal:
    return Key::Decimal;
  case XK_KP_Divide:
    return Key::Divide;

  case XK_F1:
    return Key::F1;
  case XK_F2:
    return Key::F2;
  case XK_F3:
    return Key::F3;
  case XK_F4:
    return Key::F4;
  case XK_F5:
    return Key::F5;
  case XK_F6:
    return Key::F6;
  case XK_F7:
    return Key::F7;
  case XK_F8:
    return Key::F8;
  case XK_F9:
    return Key::F9;
  case XK_F10:
    return Key::F10;
  case XK_F11:
    return Key::F11;
  case XK_F12:
    return Key::F12;
  case XK_F13:
    return Key::F13;
  case XK_F14:
    return Key::F14;
  case XK_F15:
    return Key::F15;
  case XK_F16:
    return Key::F16;
  case XK_F17:
    return Key::F17;
  case XK_F18:
    return Key::F18;
  case XK_F19:
    return Key::F19;
  case XK_F20:
    return Key::F20;
  case XK_F21:
    return Key::F21;
  case XK_F22:
    return Key::F22;
  case XK_F23:
    return Key::F23;
  case XK_F24:
    return Key::F24;

  case XK_Num_Lock:
    return Key::NumLock;
  case XK_Scroll_Lock:
    return Key::Scroll;

  case XK_KP_Equal:
    return Key::NumpadEqual;

  case XK_Shift_L:
    return Key::LShift;
  case XK_Shift_R:
    return Key::RShift;
  case XK_Control_L:
    return Key::LControl;
  case XK_Control_R:
    return Key::RControl;
  case XK_Alt_L:
    return Key::LAlt;
  case XK_Alt_R:
    return Key::RAlt;

  case XK_0:
    return Key::Zero;
  case XK_1:
    return Key::One;
  case XK_2:
    return Key::Two;
  case XK_3:
    return Key::Three;
  case XK_4:
    return Key::Four;
  case XK_5:
    return Key::Five;
  case XK_6:
    return Key::Six;
  case XK_7:
    return Key::Seven;
  case XK_8:
    return Key::Eight;
  case XK_9:
    return Key::Nine;

  case XK_semicolon:
    return Key::Semicolon;
  case XK_plus:
    return Key::Plus;
  case XK_comma:
    return Key::Comma;
  case XK_minus:
    return Key::Minus;
  case XK_period:
    return Key::Period;
  case XK_slash:
    return Key::Slash;
  case XK_grave:
    return Key::Grave;

  case XK_a:
  case XK_A:
    return Key::A;
  case XK_b:
  case XK_B:
    return Key::B;
  case XK_c:
  case XK_C:
    return Key::C;
  case XK_d:
  case XK_D:
    return Key::D;
  case XK_e:
  case XK_E:
    return Key::E;
  case XK_f:
  case XK_F:
    return Key::F;
  case XK_g:
  case XK_G:
    return Key::G;
  case XK_h:
  case XK_H:
    return Key::H;
  case XK_i:
  case XK_I:
    return Key::I;
  case XK_j:
  case XK_J:
    return Key::J;
  case XK_k:
  case XK_K:
    return Key::K;
  case XK_l:
  case XK_L:
    return Key::L;
  case XK_m:
  case XK_M:
    return Key::M;
  case XK_n:
  case XK_N:
    return Key::N;
  case XK_o:
  case XK_O:
    return Key::O;
  case XK_p:
  case XK_P:
    return Key::P;
  case XK_q:
  case XK_Q:
    return Key::Q;
  case XK_r:
  case XK_R:
    return Key::R;
  case XK_s:
  case XK_S:
    return Key::S;
  case XK_t:
  case XK_T:
    return Key::T;
  case XK_u:
  case XK_U:
    return Key::U;
  case XK_v:
  case XK_V:
    return Key::V;
  case XK_w:
  case XK_W:
    return Key::W;
  case XK_x:
  case XK_X:
    return Key::X;
  case XK_y:
  case XK_Y:
    return Key::Y;
  case XK_z:
  case XK_Z:
    return Key::Z;

  default:
    return Key::None;
  }
}
#endif // KPLATFORM_LINUX
