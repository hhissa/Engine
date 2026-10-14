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
  i32 wheel_delta_accum{0}; // see mouse_wheel_delta()
};

bool g_initialized = false;
InputState g_state{};
EventSystem *g_events = nullptr; // non-owning; provided to initialize()

// Every process_*() fires through here. The platform layer can deliver input
// before initialize() or after shutdown(): Windows sends WM_SIZE (and mouse
// messages, if the cursor is over the window) synchronously from inside
// CreateWindowEx/ShowWindow -- i.e. while Application is still constructing
// its PlatformLayer, before input::initialize() has run. Dereferencing the
// null g_events there crashed every Windows launch instantly (X11 only
// delivers events through the pump, so Linux never hit it).
void fire(EventCode code, EventContext context) {
  if (g_events) {
    g_events->fire(code, nullptr, context);
  }
}

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
  // Cleared here, at the end of this frame's iteration (Application's loop
  // calls this after game->update()/render() -- see mouse_wheel_delta()'s
  // comment) -- ready to accumulate fresh events for the next frame.
  g_state.wheel_delta_accum = 0;
}

void process_key(Key key, bool pressed) {
  auto &slot = g_state.keyboard_current.keys[idx(key)];
  if (slot != pressed) {
    slot = pressed;

    EventContext context{};
    context.data.u16[0] = static_cast<u16>(key);
    fire(pressed ? EventCode::EVENT_CODE_KEY_PRESSED
                 : EventCode::EVENT_CODE_KEY_RELEASED,
         context);
  }
}

void process_button(Button button, bool pressed) {
  auto &slot = g_state.mouse_current.buttons[idx(button)];
  if (slot != pressed) {
    slot = pressed;

    EventContext context{};
    context.data.u16[0] = static_cast<u16>(button);
    fire(pressed ? EventCode::EVENT_CODE_BUTTON_PRESSED
                 : EventCode::EVENT_CODE_BUTTON_RELEASED,
         context);
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
  fire(EventCode::EVENT_CODE_MOUSE_MOVED, context);
}

void process_mouse_wheel(i8 z_delta) {
  g_state.wheel_delta_accum += z_delta;

  EventContext context{};
  context.data.u8[0] = static_cast<u8>(z_delta);
  fire(EventCode::EVENT_CODE_MOUSE_WHEEL, context);
}

void process_resize(u16 width, u16 height) {
  EventContext context{};
  context.data.u16[0] = width;
  context.data.u16[1] = height;
  fire(EventCode::EVENT_CODE_RESIZED, context);
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

i32 mouse_wheel_delta() {
  if (!g_initialized) {
    return 0;
  }
  return g_state.wheel_delta_accum;
}

} // namespace input
