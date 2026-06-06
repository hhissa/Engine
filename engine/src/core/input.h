#pragma once

#include "../defines.h"
#include <cstdint>

// Forward-declare the global EventSystem so this header stays light.
class EventSystem;

namespace input {

enum class Button : std::uint8_t { Left, Right, Middle, Count };

enum class Key : std::uint16_t {
  None = 0,
  Backspace = 0x08,
  Tab = 0x09,
  Enter = 0x0D,
  Shift = 0x10,
  Control = 0x11,
  Pause = 0x13,
  Capital = 0x14,
  Escape = 0x1B,

  Convert = 0x1C,
  NonConvert = 0x1D,
  Accept = 0x1E,
  ModeChange = 0x1F,

  Space = 0x20,
  Prior = 0x21,
  Next = 0x22,
  End = 0x23,
  Home = 0x24,
  Left = 0x25,
  Up = 0x26,
  Right = 0x27,
  Down = 0x28,
  Select = 0x29,
  Print = 0x2A,
  Execute = 0x2B,
  Snapshot = 0x2C,
  Insert = 0x2D,
  Delete = 0x2E,
  Help = 0x2F,

  A = 0x41,
  B,
  C,
  D,
  E,
  F,
  G,
  H,
  I,
  J,
  K,
  L,
  M,
  N,
  O,
  P,
  Q,
  R,
  S,
  T,
  U,
  V,
  W,
  X,
  Y,
  Z,

  LWin = 0x5B,
  RWin = 0x5C,
  Apps = 0x5D,
  Sleep = 0x5F,

  Numpad0 = 0x60,
  Numpad1,
  Numpad2,
  Numpad3,
  Numpad4,
  Numpad5,
  Numpad6,
  Numpad7,
  Numpad8,
  Numpad9,

  Multiply = 0x6A,
  Add = 0x6B,
  Separator = 0x6C,
  Subtract = 0x6D,
  Decimal = 0x6E,
  Divide = 0x6F,

  F1 = 0x70,
  F2,
  F3,
  F4,
  F5,
  F6,
  F7,
  F8,
  F9,
  F10,
  F11,
  F12,
  F13,
  F14,
  F15,
  F16,
  F17,
  F18,
  F19,
  F20,
  F21,
  F22,
  F23,
  F24,

  NumLock = 0x90,
  Scroll = 0x91,
  NumpadEqual = 0x92,

  LShift = 0xA0,
  RShift = 0xA1,
  LControl = 0xA2,
  RControl = 0xA3,
  LMenu = 0xA4,
  RMenu = 0xA5,

  Semicolon = 0xBA,
  Plus = 0xBB,
  Comma = 0xBC,
  Minus = 0xBD,
  Period = 0xBE,
  Slash = 0xBF,
  Grave = 0xC0,
};

// Lifecycle. EventSystem is borrowed; caller owns it.
void initialize(EventSystem &events);
void shutdown();
void update(double delta_time);

// Keyboard
KAPI bool is_key_down(Key key);
KAPI bool is_key_up(Key key);
KAPI bool was_key_down(Key key);
KAPI bool was_key_up(Key key);

void process_key(Key key, bool pressed);

// Mouse
KAPI bool is_button_down(Button button);
KAPI bool is_button_up(Button button);
KAPI bool was_button_down(Button button);
KAPI bool was_button_up(Button button);

struct MousePosition {
  i32 x;
  i32 y;
};
KAPI MousePosition mouse_position();
KAPI MousePosition previous_mouse_position();

void process_button(Button button, bool pressed);
void process_mouse_move(i16 x, i16 y);
void process_mouse_wheel(i8 z_delta);

} // namespace input
