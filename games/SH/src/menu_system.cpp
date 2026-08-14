#include "menu_system.h"

#include <core/input.h>
#include <renderer/renderer_frontend.h>

namespace {

// Row layout (screen pixels, top-left origin -- matches renderer_draw_text
// and input::mouse_position()). No text-measurement API exists anywhere in
// this engine, so kRowWidth is a hand-picked, eyeballed hit-test width
// rather than a measured one -- nudge it if a button's clickable area ends
// up visibly narrower/wider than its label once you can see it rendered.
constexpr f32 kRowHeight = 36.0f;
constexpr f32 kRowWidth = 320.0f;

// Same two-tone scheme as QASystem's question list (qa_system.cpp's
// kUnasked/kCursor) -- declared fresh here rather than shared since
// QASystem's are private to its own anonymous namespace.
constexpr glm::vec4 kUnselected{1.0f, 1.0f, 1.0f, 1.0f};
constexpr glm::vec4 kSelected{1.0f, 0.9f, 0.5f, 1.0f};

bool key_pressed(input::Key key) {
  return input::is_key_down(key) && !input::was_key_down(key);
}

bool mouse_click_pressed() {
  return input::is_button_down(input::Button::Left) &&
        !input::was_button_down(input::Button::Left);
}

// Row i's top-left corner, counting from the bottom row upward -- shared by
// update()'s hit-testing and render()'s drawing so they can never disagree
// about where a row actually is. Mirrors QASystem::render()'s row_y
// formula: the last row (i == count - 1) lands exactly at bottom_left, and
// earlier rows stack upward above it.
glm::vec2 row_position(glm::vec2 bottom_left, size_t i, size_t count) {
  return bottom_left -
        glm::vec2(0.0f, static_cast<f32>(count - 1 - i) * kRowHeight);
}

bool point_in_row(glm::vec2 point, glm::vec2 row_top_left) {
  return point.x >= row_top_left.x && point.x <= row_top_left.x + kRowWidth &&
        point.y >= row_top_left.y && point.y <= row_top_left.y + kRowHeight;
}

} // namespace

void MenuSystem::set_items(std::vector<Item> items) {
  items_ = std::move(items);
  cursor_ = 0;
}

void MenuSystem::update(glm::vec2 bottom_left) {
  if (items_.empty()) {
    return;
  }

  if (key_pressed(input::Key::Up) && cursor_ > 0) {
    --cursor_;
  }
  if (key_pressed(input::Key::Down) && cursor_ + 1 < items_.size()) {
    ++cursor_;
  }

  input::MousePosition mouse = input::mouse_position();
  glm::vec2 mouse_pos(static_cast<f32>(mouse.x), static_cast<f32>(mouse.y));
  bool clicked = mouse_click_pressed();
  for (size_t i = 0; i < items_.size(); ++i) {
    if (point_in_row(mouse_pos, row_position(bottom_left, i, items_.size()))) {
      cursor_ = i; // hover follows the mouse, keeping keyboard/mouse in sync
      if (clicked && items_[i].on_select) {
        items_[i].on_select();
      }
      break; // rows don't overlap -- at most one can contain the pointer
    }
  }

  if (key_pressed(input::Key::Enter) && items_[cursor_].on_select) {
    items_[cursor_].on_select();
  }
}

void MenuSystem::render(glm::vec2 bottom_left) const {
  for (size_t i = 0; i < items_.size(); ++i) {
    bool on_cursor = i == cursor_;
    std::string line = std::string(on_cursor ? "> " : "  ") + items_[i].label;
    renderer_draw_text(line, row_position(bottom_left, i, items_.size()),
                       on_cursor ? kSelected : kUnselected);
  }
}
