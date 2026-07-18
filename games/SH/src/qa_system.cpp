#include "qa_system.h"

#include <core/input.h>
#include <renderer/renderer_frontend.h>

namespace {

// Layout (screen pixels, top-left origin -- matches renderer_draw_text).
constexpr f32 kListX = 32.0f;
constexpr f32 kListY = 96.0f;
constexpr f32 kLineSpacing = 28.0f;
// The answer text sits below the question block, however long it is.
constexpr f32 kAnswerGap = 48.0f;

// Question colours: bright while unasked, permanently darkened once asked,
// and a warm highlight on whichever line the cursor is on.
constexpr glm::vec4 kUnasked{1.0f, 1.0f, 1.0f, 1.0f};
constexpr glm::vec4 kAsked{0.35f, 0.35f, 0.35f, 1.0f};
constexpr glm::vec4 kCursor{1.0f, 0.9f, 0.5f, 1.0f};
constexpr glm::vec4 kAnswer{0.85f, 0.85f, 0.85f, 1.0f};
constexpr glm::vec4 kHint{0.5f, 0.5f, 0.5f, 1.0f};

bool key_pressed(input::Key key) {
  return input::is_key_down(key) && !input::was_key_down(key);
}

} // namespace

void QASystem::add_entry(std::string question,
                         std::vector<std::string> answer_lines) {
  entries_.push_back(Entry{std::move(question), std::move(answer_lines)});
}

void QASystem::update() {
  if (entries_.empty()) {
    return;
  }

  if (state_ == State::QuestionList) {
    if (key_pressed(input::Key::Up) && cursor_ > 0) {
      --cursor_;
    }
    if (key_pressed(input::Key::Down) && cursor_ + 1 < entries_.size()) {
      ++cursor_;
    }
    if (key_pressed(input::Key::Enter)) {
      entries_[cursor_].asked = true; // permanent -- never cleared
      answer_line_ = 0;
      state_ = State::Answer;
    }
  } else { // State::Answer
    if (key_pressed(input::Key::Enter)) {
      ++answer_line_;
      if (answer_line_ >= entries_[cursor_].answer_lines.size()) {
        state_ = State::QuestionList;
      }
    }
  }
}

void QASystem::render() const {
  // The question block is always visible, whichever state we're in --
  // asked questions stay in the list, just darkened.
  for (size_t i = 0; i < entries_.size(); ++i) {
    const Entry &entry = entries_[i];
    bool on_cursor = state_ == State::QuestionList && i == cursor_;

    glm::vec4 colour = entry.asked ? kAsked : kUnasked;
    if (on_cursor) {
      colour = kCursor;
    }

    std::string line =
        std::string(on_cursor ? "> " : "  ") + entry.question;
    renderer_draw_text(line, glm::vec2(kListX, kListY + i * kLineSpacing),
                       colour);
  }

  f32 below_list = kListY + entries_.size() * kLineSpacing + kAnswerGap;

  if (state_ == State::Answer) {
    const Entry &entry = entries_[cursor_];
    if (answer_line_ < entry.answer_lines.size()) {
      renderer_draw_text(entry.answer_lines[answer_line_],
                         glm::vec2(kListX, below_list), kAnswer);
    }
    renderer_draw_text("[Enter] continue",
                       glm::vec2(kListX, below_list + kLineSpacing + 8.0f),
                       kHint);
  } else {
    renderer_draw_text("[Up/Down] choose   [Enter] ask   [Tab] camera",
                       glm::vec2(kListX, below_list), kHint);
  }
}
