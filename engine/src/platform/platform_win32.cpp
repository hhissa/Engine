// platform_win32.cpp
#include "platform.h"

#if KPLATFORM_WINDOWS

// NOGDI keeps wingdi.h's `#define ERROR 0` out -- it would turn every
// KERROR (log_level::ERROR) below into a syntax error. Nothing here draws
// through GDI; Vulkan owns the window's contents.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef NOGDI
#define NOGDI
#endif
#include <windows.h>

// Gamepad support: XInput, the standard Win32 controller API (the direct
// counterpart of the Linux layer's raw joystick API) -- Xinput9_1_0.lib is
// the variant available all the way back to Vista, which is as safe a
// default as this codebase's other Win32 dependencies.
#include <Xinput.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include "../renderer/vulkan/vulkan_types.inl"
#include <vulkan/vulkan.h>

#include "../core/input.h"
#include "../core/logger.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <string_view>
#include <thread>

namespace {

constexpr const wchar_t *kWindowClassName = L"kengine_window_class";

// ─── Per-window state, hidden inside the .cpp ──────────────────────────────
struct win32_state {
  HINSTANCE instance;
  HWND hwnd;
  // Set by WM_CLOSE, reported (and cleared) by platform_pump_messages() --
  // the same "the user asked to close" signal X11's WM_DELETE_WINDOW gives
  // on Linux. The window itself is only destroyed by ~PlatformLayer.
  b8 quit_requested;
  // False when constructed from a BorrowedWindowHandle -- the destructor
  // must not destroy a window it doesn't own.
  b8 owns_window;
};

std::wstring widen(std::string_view utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                         static_cast<int>(utf8.size()),
                                         nullptr, 0);
  std::wstring wide(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                      wide.data(), length);
  return wide;
}

// Windows reports Shift, Ctrl and Alt as one generic virtual key each;
// split them into the left/right codes the Linux layer reports (X11 only
// ever sends XK_Shift_L/R etc.), so the game sees the same keys on both.
input::Key translate_keycode(WPARAM vk, LPARAM lparam) {
  const b8 extended = (lparam & (1 << 24)) != 0;
  switch (vk) {
  case VK_SHIFT: {
    const UINT scancode = (lparam >> 16) & 0xFF;
    return MapVirtualKeyW(scancode, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT
               ? input::Key::RShift
               : input::Key::LShift;
  }
  case VK_CONTROL:
    return extended ? input::Key::RControl : input::Key::LControl;
  case VK_MENU:
    return extended ? input::Key::RAlt : input::Key::LAlt;
  default:
    // input::Key's values ARE Windows virtual-key codes (see input.h), so
    // everything else passes straight through.
    return vk < 256 ? static_cast<input::Key>(vk) : input::Key::None;
  }
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam,
                             LPARAM lparam) {
  auto *state =
      reinterpret_cast<win32_state *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  switch (msg) {
  case WM_NCCREATE: {
    auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    break;
  }
  case WM_ERASEBKGND:
    // Vulkan paints every pixel; letting Windows clear first just flickers.
    return 1;
  case WM_CLOSE:
    if (state) {
      state->quit_requested = TRUE;
    }
    return 0;
  case WM_SIZE:
    // A minimise reports 0x0 -- the application/renderer already skip a
    // zero-sized resize (see Application's EVENT_CODE_RESIZED handler).
    input::process_resize(LOWORD(lparam), HIWORD(lparam));
    break;

  case WM_KEYDOWN:
  case WM_SYSKEYDOWN:
  case WM_KEYUP:
  case WM_SYSKEYUP: {
    const b8 pressed = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    // Auto-repeat downs arrive here too; input::process_key() ignores a
    // press for a key that's already down, matching Linux, where the X
    // layer turns auto-repeat off outright.
    input::process_key(translate_keycode(wparam, lparam), pressed);
    // Let Windows see Alt+F4 so it still closes the window. Everything
    // else is swallowed -- passing a bare Alt or F10 on would drop the
    // window into system-menu mode and stall the frame loop's input.
    if (msg == WM_SYSKEYDOWN && wparam == VK_F4) {
      break;
    }
    return 0;
  }

  case WM_MOUSEMOVE:
    input::process_mouse_move(static_cast<i16>(LOWORD(lparam)),
                              static_cast<i16>(HIWORD(lparam)));
    break;
  case WM_MOUSEWHEEL: {
    // One call per notch, like X11's buttons 4/5. High-resolution wheels
    // send fractions of WHEEL_DELTA; each still counts as one step.
    const i16 delta = GET_WHEEL_DELTA_WPARAM(wparam);
    if (delta != 0) {
      input::process_mouse_wheel(delta > 0 ? 1 : -1);
    }
    return 0;
  }
  case WM_LBUTTONDOWN:
  case WM_LBUTTONUP:
  case WM_MBUTTONDOWN:
  case WM_MBUTTONUP:
  case WM_RBUTTONDOWN:
  case WM_RBUTTONUP: {
    const b8 pressed = msg == WM_LBUTTONDOWN || msg == WM_MBUTTONDOWN ||
                       msg == WM_RBUTTONDOWN;
    input::Button button = input::Button::Left;
    if (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP) {
      button = input::Button::Middle;
    } else if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) {
      button = input::Button::Right;
    }
    // Keep receiving the release (and drag motion) if the cursor leaves
    // the window mid-click -- X11 grabs the pointer implicitly; Win32
    // needs asking.
    if (pressed) {
      SetCapture(hwnd);
    } else if ((wparam & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON)) == 0) {
      ReleaseCapture();
    }
    input::process_button(button, pressed);
    return 0;
  }
  default:
    break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// ─── Gamepad polling (XInput) ───────────────────────────────────────────
// XInput is the Win32 counterpart of platform_linux.cpp's raw joystick
// API -- a simple poll-once-per-frame call, no device file/handle to
// manage. NOTE: this half of the engine's gamepad support could not be
// compiled or run against real hardware in the environment this was
// written in (no Windows toolchain available) -- written carefully from
// XInput's well-documented, stable API shape, but flagged here as
// unverified at the symbol/runtime level, unlike the Linux backend.

f32 clamp01(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
f32 clamp11(f32 v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

struct ButtonBit {
  WORD mask;
  input::GamepadButton button;
};

// XInput already reports the d-pad as discrete button bits (unlike
// Linux's raw joystick API, which reports it as two hat axes that
// platform_linux.cpp has to convert itself) -- no sign-convention guessing
// needed here, the Dpad* bits map straight across.
constexpr std::array<ButtonBit, 14> kButtonBits = {{
    {XINPUT_GAMEPAD_A, input::GamepadButton::A},
    {XINPUT_GAMEPAD_B, input::GamepadButton::B},
    {XINPUT_GAMEPAD_X, input::GamepadButton::X},
    {XINPUT_GAMEPAD_Y, input::GamepadButton::Y},
    {XINPUT_GAMEPAD_LEFT_SHOULDER, input::GamepadButton::LeftBumper},
    {XINPUT_GAMEPAD_RIGHT_SHOULDER, input::GamepadButton::RightBumper},
    {XINPUT_GAMEPAD_BACK, input::GamepadButton::Back},
    {XINPUT_GAMEPAD_START, input::GamepadButton::Start},
    // XInput has no bit for the Guide/Xbox button -- Microsoft reserves
    // it for the system shell and never surfaces it through this API.
    // Left unmapped: is_gamepad_button_down(GamepadButton::Guide) will
    // simply never report true on Windows. Linux's raw joystick API does
    // see it (button 8 in the xpad driver's layout) -- a genuine platform
    // asymmetry, not a bug on either side.
    {XINPUT_GAMEPAD_LEFT_THUMB, input::GamepadButton::LeftStick},
    {XINPUT_GAMEPAD_RIGHT_THUMB, input::GamepadButton::RightStick},
    {XINPUT_GAMEPAD_DPAD_UP, input::GamepadButton::DpadUp},
    {XINPUT_GAMEPAD_DPAD_DOWN, input::GamepadButton::DpadDown},
    {XINPUT_GAMEPAD_DPAD_LEFT, input::GamepadButton::DpadLeft},
    {XINPUT_GAMEPAD_DPAD_RIGHT, input::GamepadButton::DpadRight},
}};

// At most one gamepad is tracked (see input.h's doc comment on the
// gamepad API) -- user index 0 is "the first connected controller" in
// practice for a single-pad setup, which is this engine's whole scope
// right now.
constexpr DWORD kGamepadUserIndex = 0;

void poll_gamepad() {
  XINPUT_STATE state{};
  if (XInputGetState(kGamepadUserIndex, &state) != ERROR_SUCCESS) {
    // Normal, silent case: no controller plugged in (or unplugged since
    // the last call) -- process_gamepad_connection() only fires an event
    // on an actual change, so this is cheap to call every frame.
    input::process_gamepad_connection(false);
    return;
  }
  input::process_gamepad_connection(true);

  const XINPUT_GAMEPAD &pad = state.Gamepad;
  for (const ButtonBit &bit : kButtonBits) {
    input::process_gamepad_button(bit.button, (pad.wButtons & bit.mask) != 0);
  }

  // Sticks: XInput's native range is -32768..32767 (one more negative
  // step than Linux's joystick API, which bottoms out at -32767) --
  // dividing by 32767 and clamping saturates that extra step at -1.0f
  // instead of reporting slightly past it.
  input::process_gamepad_axis(input::GamepadAxis::LeftX,
                              clamp11(pad.sThumbLX / 32767.0f));
  input::process_gamepad_axis(input::GamepadAxis::LeftY,
                              clamp11(pad.sThumbLY / 32767.0f));
  input::process_gamepad_axis(input::GamepadAxis::RightX,
                              clamp11(pad.sThumbRX / 32767.0f));
  input::process_gamepad_axis(input::GamepadAxis::RightY,
                              clamp11(pad.sThumbRY / 32767.0f));

  // Triggers: XInput's native range is 0..255, already starting at
  // "released" -- unlike Linux's -32767-centered trigger axis, so this is
  // a plain divide, no offset needed.
  input::process_gamepad_axis(input::GamepadAxis::LeftTrigger,
                              clamp01(pad.bLeftTrigger / 255.0f));
  input::process_gamepad_axis(input::GamepadAxis::RightTrigger,
                              clamp01(pad.bRightTrigger / 255.0f));
}

} // namespace

// ─── PlatformLayer: owns the Win32 window ──────────────────────────────────
PlatformLayer::PlatformLayer(std::string_view application_name, i32 x, i32 y,
                             i32 width, i32 height)
    : application_name(application_name), x(x), y(y), width(width),
      height(height), internal_state(new win32_state{}) {
  auto *state = static_cast<win32_state *>(internal_state);
  state->instance = GetModuleHandleW(nullptr);

  WNDCLASSEXW wc{sizeof(WNDCLASSEXW)};
  wc.style = CS_DBLCLKS;
  wc.lpfnWndProc = window_proc;
  wc.hInstance = state->instance;
  wc.hIcon = LoadIconA(nullptr, IDI_APPLICATION);
  wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
  wc.lpszClassName = kWindowClassName;
  // Re-registering an already-registered class (a second PlatformLayer in
  // one process) fails harmlessly -- only a genuine failure is fatal.
  if (!RegisterClassExW(&wc) &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    KFATAL("Window class registration failed.");
  }

  // width/height are the drawable (client) size, as on Linux -- grow the
  // outer window by the border and title bar so the swapchain gets exactly
  // what was asked for.
  constexpr DWORD style = WS_OVERLAPPEDWINDOW;
  constexpr DWORD ex_style = WS_EX_APPWINDOW;
  RECT rect{0, 0, width, height};
  AdjustWindowRectEx(&rect, style, FALSE, ex_style);

  const std::wstring title = widen(application_name);
  state->hwnd = CreateWindowExW(
      ex_style, kWindowClassName, title.c_str(), style, x, y,
      rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
      state->instance, state);
  if (!state->hwnd) {
    KFATAL("Window creation failed.");
    return;
  }

  ShowWindow(state->hwnd, SW_SHOW);
  state->owns_window = TRUE;
}

// ─── PlatformLayer: wraps a window owned by something else ────────────────
// On Windows, handle.window is the HWND; handle.connection is unused.
PlatformLayer::PlatformLayer(BorrowedWindowHandle handle, i32 width,
                             i32 height)
    : application_name(), x(0), y(0), width(width), height(height),
      internal_state(new win32_state{}) {
  auto *state = static_cast<win32_state *>(internal_state);
  state->instance = GetModuleHandleW(nullptr);
  state->hwnd = reinterpret_cast<HWND>(handle.window);
  state->owns_window = FALSE;
}

PlatformLayer::~PlatformLayer() {
  auto *state = static_cast<win32_state *>(internal_state);
  if (state->owns_window && state->hwnd) {
    DestroyWindow(state->hwnd);
  }
  delete state;
  internal_state = nullptr;
}

b8 PlatformLayer::platform_pump_messages() {
  auto *state = static_cast<win32_state *>(internal_state);

  // Gamepad state is system-wide, not tied to any particular window --
  // polled unconditionally, before the owns_window check below, same as
  // platform_linux.cpp's joystick poll and for the same reason: gating it
  // on window ownership would silently break gamepad input for a
  // borrowed-window consumer for no architectural reason.
  poll_gamepad();

  // Borrowed windows are pumped by whatever owns their event loop.
  if (!state->owns_window) {
    return TRUE;
  }

  MSG message;
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  const b8 quit = state->quit_requested;
  state->quit_requested = FALSE;
  return !quit;
}

void PlatformLayer::get_required_extension_names(
    std::vector<const char *> &names) const {
  names.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
}

b8 PlatformLayer::create_vulkan_surface(VulkanContext &context) {
  auto *state = static_cast<win32_state *>(internal_state);

  VkWin32SurfaceCreateInfoKHR create_info{
      VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
  create_info.hinstance = state->instance;
  create_info.hwnd = state->hwnd;

  VkResult result = vkCreateWin32SurfaceKHR(
      context.instance, &create_info, context.allocator, &context.surface);
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

// Windows 10+ consoles understand the same ANSI colour escapes the Linux
// layer writes, but only once asked to. Log text is UTF-8 source text, so
// switch the code page while we're at it.
void enable_console_colours() {
  static const bool done = [] {
    SetConsoleOutputCP(CP_UTF8);
    for (DWORD id : {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
      HANDLE handle = GetStdHandle(id);
      DWORD mode = 0;
      if (handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode)) {
        SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
      }
    }
    return true;
  }();
  (void)done;
}
} // namespace

void console_write(std::string_view message, u8 colour) {
  enable_console_colours();
  std::cout << std::format("\033[{}m{}\033[0m", COLOUR_CODES[colour], message);
  // Also show up in a debugger's output window (Visual Studio, WinDbg).
  OutputDebugStringA(std::string(message).c_str());
}

void console_write_error(std::string_view message, u8 colour) {
  enable_console_colours();
  std::cerr << std::format("\033[{}m{}\033[0m", COLOUR_CODES[colour], message);
  OutputDebugStringA(std::string(message).c_str());
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
  if (const char *local = std::getenv("LOCALAPPDATA"); local && *local) {
    return local;
  }
  return {};
}

void set_environment_variable(std::string_view name, std::string_view value,
                              b8 overwrite) {
  const std::string name_str(name);
  if (!overwrite && std::getenv(name_str.c_str())) {
    return;
  }
  _putenv_s(name_str.c_str(), std::string(value).c_str());
}

} // namespace Platform

#endif // KPLATFORM_WINDOWS
