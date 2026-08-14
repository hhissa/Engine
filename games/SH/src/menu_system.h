#pragma once
#include <defines.h>
#include <glm/glm.hpp>

#include <functional>
#include <string>
#include <vector>

// A vertical list of text-only buttons, navigable by both keyboard
// (Up/Down/Enter) and mouse (hover + click) -- used for the title screen,
// chapter select, and the "Back" row on the settings screen. Deliberately
// text-only rather than drawing a highlight quad behind the selected row:
// renderer_draw_solid_quad() is always painted last of all queued 2D draws
// for the frame regardless of call order (see renderer_backend.h's
// end_frame()), so a quad queued as a "button background" would always
// paint on top of the label text, not behind it. Selection is shown the
// same way QASystem's question list already shows its cursor instead: a
// warm colour plus a "> " prefix on the selected row.
//
// Anchored bottom-up, same as QASystem's question list: bottom_left is
// where the *last* row sits, and earlier rows stack upward above it -- so
// the bottom of the block stays put as items_ grows/shrinks (e.g. "Continue"
// appearing once a save exists) instead of the whole block sliding down,
// and a caller anchoring bottom_left near the bottom of the screen never
// has a row clipped off the bottom edge on a shorter window.
//
// update() and render() both take the same bottom_left so their per-row
// rects can never disagree -- update() hit-tests the mouse against exactly
// the rects render() is about to draw at. Call both once per frame.
class MenuSystem {
public:
  struct Item {
    std::string label;
    std::function<void()> on_select; // fired by Enter on the cursor row, or
                                     // a click landing on this row
  };

  // Replaces the item list and resets the cursor to the top row -- call
  // whenever a screen's item list changes (e.g. rebuilding the title menu
  // so "Continue" only appears once a save file exists).
  void set_items(std::vector<Item> items);

  // How many rows are currently showing -- lets a caller position other
  // text (e.g. a heading) relative to the top of the block, which moves as
  // items_ grows/shrinks (see the class comment).
  size_t item_count() const { return items_.size(); }

  // Reads Up/Down/Enter (edge-triggered, same idiom as qa_system.cpp's
  // key_pressed()) plus mouse hover/click. Mouse hover moves the keyboard
  // cursor to whichever row the pointer is over, so the two input modes
  // never show two different rows highlighted at once.
  void update(glm::vec2 bottom_left);

  void render(glm::vec2 bottom_left) const;

private:
  std::vector<Item> items_;
  size_t cursor_ = 0;
};
