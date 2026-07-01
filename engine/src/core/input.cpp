#include "input.h"
#include "event.h"
#include "logger.h"

#include <array>
#include <cstddef>

namespace input {

namespace {

template <typename E> constexpr std::size_t idx(E e) noexcept {
  return static_cast<std::size_t>(e);
}

struct KeyboardState {
  std::array<bool, 256> keys{};
};

struct MouseState {
  i16 x{0};
  i16 y{0};
  std::array<bool, idx(Button::Count)> buttons{};
};

struct InputState {
  KeyboardState keyboard_current;
  KeyboardState keyboard_previous;
  MouseState mouse_current;
  MouseState mouse_previous;
};

bool g_initialized = false;
InputState g_state{};
EventSystem *g_events = nullptr; // non-owning; provided to initialize()

} // namespace

void initialize(EventSystem &events) {
  g_state = {};
  g_events = &events;
  g_initialized = true;
  KINFO("Input subsystem initialized.");
}

void shutdown() {
  g_initialized = false;
  g_events = nullptr;
}

void update(double /*delta_time*/) {
  if (!g_initialized) {
    return;
  }
  g_state.keyboard_previous = g_state.keyboard_current;
  g_state.mouse_previous = g_state.mouse_current;
}

void process_key(Key key, bool pressed) {
  auto &slot = g_state.keyboard_current.keys[idx(key)];
  if (slot != pressed) {
    slot = pressed;

    EventContext context{};
    context.data.u16[0] = static_cast<u16>(key);
    g_events->fire(pressed ? EventCode::EVENT_CODE_KEY_PRESSED
                           : EventCode::EVENT_CODE_KEY_RELEASED,
                   nullptr, context);
  }
}

void process_button(Button button, bool pressed) {
  auto &slot = g_state.mouse_current.buttons[idx(button)];
  if (slot != pressed) {
    slot = pressed;

    EventContext context{};
    context.data.u16[0] = static_cast<u16>(button);
    g_events->fire(pressed ? EventCode::EVENT_CODE_BUTTON_PRESSED
                           : EventCode::EVENT_CODE_BUTTON_RELEASED,
                   nullptr, context);
  }
}

void process_mouse_move(i16 x, i16 y) {
  if (g_state.mouse_current.x == x && g_state.mouse_current.y == y) {
    return;
  }

  g_state.mouse_current.x = x;
  g_state.mouse_current.y = y;

  EventContext context{};
  context.data.u16[0] = static_cast<u16>(x);
  context.data.u16[1] = static_cast<u16>(y);
  g_events->fire(EventCode::EVENT_CODE_MOUSE_MOVED, nullptr, context);
}

void process_mouse_wheel(i8 z_delta) {
  EventContext context{};
  context.data.u8[0] = static_cast<u8>(z_delta);
  g_events->fire(EventCode::EVENT_CODE_MOUSE_WHEEL, nullptr, context);
}

void process_resize(u16 width, u16 height) {
  EventContext context{};
  context.data.u16[0] = width;
  context.data.u16[1] = height;
  g_events->fire(EventCode::EVENT_CODE_RESIZED, nullptr, context);
}

// --- Keyboard queries -------------------------------------------------------

bool is_key_down(Key key) {
  return g_initialized && g_state.keyboard_current.keys[idx(key)];
}

bool is_key_up(Key key) {
  return !g_initialized || !g_state.keyboard_current.keys[idx(key)];
}

bool was_key_down(Key key) {
  return g_initialized && g_state.keyboard_previous.keys[idx(key)];
}

bool was_key_up(Key key) {
  return !g_initialized || !g_state.keyboard_previous.keys[idx(key)];
}

// --- Mouse queries ----------------------------------------------------------

bool is_button_down(Button button) {
  return g_initialized && g_state.mouse_current.buttons[idx(button)];
}

bool is_button_up(Button button) {
  return !g_initialized || !g_state.mouse_current.buttons[idx(button)];
}

bool was_button_down(Button button) {
  return g_initialized && g_state.mouse_previous.buttons[idx(button)];
}

bool was_button_up(Button button) {
  return !g_initialized || !g_state.mouse_previous.buttons[idx(button)];
}

MousePosition mouse_position() {
  if (!g_initialized) {
    return {0, 0};
  }
  return {g_state.mouse_current.x, g_state.mouse_current.y};
}

MousePosition previous_mouse_position() {
  if (!g_initialized) {
    return {0, 0};
  }
  return {g_state.mouse_previous.x, g_state.mouse_previous.y};
}

} // namespace input
