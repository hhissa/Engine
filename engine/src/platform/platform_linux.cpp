// platform_linux.cpp
#include "platform.h"
#include <xcb/xproto.h>

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
      xcb_key_press_event_t *kb_event = (xcb_key_press_event_t *)event;
      b8 pressed = event->response_type == XCB_KEY_PRESS;
      xcb_keycode_t code = kb_event->detail;
      KeySym key_sym = XkbKeycodeToKeysym(state->display,
                                          (KeyCode)code, // event.xkey.keycode,
                                          0, code & ShiftMask ? 1 : 0);

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

} // namespace Platform
input::Key translate_keycode(u32 x_keycode) {
  using input::Key;

  switch (x_keycode) {
  case XK_BackSpace:
    return Key::Backspace;
  case XK_Return:
    return Key::Enter;
  case XK_Tab:
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
