#include "qa_system.h"

#include <core/input.h>
#include <core/logger.h>
#include <renderer/renderer_frontend.h>
#include <resources/conversation.h>

#include <algorithm>
#include <optional>

namespace {

// Layout (screen pixels, top-left origin -- matches renderer_draw_text).
// Built bottom-up (see the class comment): kBottomMargin is the fixed
// distance from the screen's bottom edge to the bottom-most line, and
// everything above it is stacked from there, so that anchor never moves
// as questions are added/removed.
constexpr f32 kListX = 32.0f;
constexpr f32 kBottomMargin = 32.0f;
constexpr f32 kLineSpacing = 28.0f;
// Gap between the question list's last row and the hint line below it.
constexpr f32 kAnswerGap = 48.0f;
// Gap between the answer line and its own "[Enter] continue" hint below
// it -- tighter than kAnswerGap since these two lines belong together.
constexpr f32 kAnswerHintGap = 36.0f;

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

QASystem::Entry &QASystem::add_entry(std::string question,
                                     std::vector<std::string> answer_lines) {
  entries_.push_back(Entry{std::move(question), std::move(answer_lines)});
  entry_handles_.push_back(kInvalidConversationHandle); // not from a conversation load
  return entries_.back();
}

QASystem::Entry &QASystem::add_follow_up(Entry &parent, std::string question,
                                         std::vector<std::string> answer_lines) {
  parent.follow_ups.push_back(Entry{std::move(question), std::move(answer_lines)});
  return parent.follow_ups.back();
}

namespace {
// Converts one parsed ConversationQuestion (engine/src/resources/
// conversation.h's pure data shape) into a QASystem::Entry, recursively --
// the two are structurally identical (question/answer_lines/follow_ups)
// except Entry also carries the in-game `asked` flag conversation data has
// no business knowing about, which just defaults to false here.
QASystem::Entry to_entry(const ConversationQuestion &question) {
  QASystem::Entry entry;
  entry.question = question.text;
  entry.answer_lines = question.answer_lines;
  entry.follow_ups.reserve(question.follow_ups.size());
  for (const ConversationQuestion &child : question.follow_ups) {
    entry.follow_ups.push_back(to_entry(child));
  }
  return entry;
}
} // namespace

ConversationHandle QASystem::load_conversation(std::string_view path) {
  std::optional<Conversation> parsed = load_conversation_file(path);
  if (!parsed) {
    return kInvalidConversationHandle; // load_conversation_file() already logged why
  }

  ConversationHandle handle = next_conversation_handle_++;
  for (const ConversationQuestion &question : parsed->questions) {
    entries_.push_back(to_entry(question));
    entry_handles_.push_back(handle);
  }

  KDEBUG("Loaded conversation '{}' ({} top-level question(s)) as handle {}.",
        path, parsed->questions.size(), handle);
  return handle;
}

void QASystem::unload_conversation(ConversationHandle handle) {
  if (handle == kInvalidConversationHandle) {
    KWARN("QASystem::unload_conversation called with kInvalidConversationHandle.");
    return;
  }
  bool found = false;
  for (ConversationHandle h : entry_handles_) {
    if (h == handle) {
      found = true;
      break;
    }
  }
  if (!found) {
    KWARN("QASystem::unload_conversation called with a handle that isn't "
         "currently loaded: {}.",
         handle);
    return;
  }

  // Reset to the top-level view *before* erasing anything below -- see
  // current_list_'s comment for why this has to happen first regardless of
  // whether handle's questions are the ones currently open (working out
  // whether current_list_ points into a subtree about to be erased isn't
  // worth the complexity next to just always resetting).
  current_list_ = &entries_;
  state_ = State::QuestionList;
  cursor_ = 0;
  answer_line_ = 0;

  for (size_t i = entries_.size(); i-- > 0;) {
    if (entry_handles_[i] == handle) {
      entries_.erase(entries_.begin() + static_cast<ptrdiff_t>(i));
      entry_handles_.erase(entry_handles_.begin() + static_cast<ptrdiff_t>(i));
    }
  }
}

void QASystem::update() {
  if (entries_.empty()) {
    return;
  }

  if (state_ == State::QuestionList) {
    std::vector<Entry> &list = *current_list_;
    if (key_pressed(input::Key::Up) && cursor_ > 0) {
      --cursor_;
    }
    if (key_pressed(input::Key::Down) && cursor_ + 1 < list.size()) {
      ++cursor_;
    }
    if (key_pressed(input::Key::Enter)) {
      list[cursor_].asked = true; // permanent -- never cleared
      answer_line_ = 0;
      state_ = State::Answer;
    }
  } else { // State::Answer
    if (key_pressed(input::Key::Enter)) {
      ++answer_line_;
      Entry &answered = (*current_list_)[cursor_];
      if (answer_line_ >= answered.answer_lines.size()) {
        // Past the last answer line -- decide what the question list shows
        // next (see the class comment for the exact policy).
        if (!answered.follow_ups.empty()) {
          current_list_ = &answered.follow_ups;
          cursor_ = 0;
        } else if (current_list_ != &entries_) {
          bool all_asked = std::all_of(
              current_list_->begin(), current_list_->end(),
              [](const Entry &e) { return e.asked; });
          if (all_asked) {
            current_list_ = &entries_;
            cursor_ = 0;
          }
          // else: unanswered siblings remain -- stay on current_list_ with
          // cursor_ unchanged (still a valid index into it).
        }
        // else: a leaf at the top level already -- nothing to do.
        state_ = State::QuestionList;
      }
    }
  }
}

void QASystem::render(u32 screen_height) const {
  // Built bottom-up (see the class comment): start from the fixed bottom
  // anchor and work upward, so the anchor itself never depends on how many
  // questions are in the currently-displayed list.
  f32 hint_y = static_cast<f32>(screen_height) - kBottomMargin;

  if (state_ == State::Answer) {
    // The question list is hidden entirely while an answer is showing --
    // only the answer line and its own hint are drawn.
    f32 answer_y = hint_y - kAnswerHintGap;
    const Entry &entry = (*current_list_)[cursor_];
    if (answer_line_ < entry.answer_lines.size()) {
      renderer_draw_text(entry.answer_lines[answer_line_],
                         glm::vec2(kListX, answer_y), kAnswer);
    }
    renderer_draw_text("[Enter] continue", glm::vec2(kListX, hint_y), kHint);
    return;
  }

  renderer_draw_text("[Up/Down] choose   [Enter] ask   [Tab] camera",
                     glm::vec2(kListX, hint_y), kHint);
  f32 list_bottom_y = hint_y - kAnswerGap;

  // Row i's y works backward from the list's fixed bottom, so the last row
  // always lands at list_bottom_y and earlier rows stack upward -- the top
  // of the block is the only part that moves as the list's size changes
  // (including when it swaps between the top-level list and a follow-up
  // one), exactly the "aligned regardless of count" property the bottom
  // anchor is for.
  const std::vector<Entry> &list = *current_list_;
  for (size_t i = 0; i < list.size(); ++i) {
    const Entry &entry = list[i];
    bool on_cursor = i == cursor_;

    glm::vec4 colour = entry.asked ? kAsked : kUnasked;
    if (on_cursor) {
      colour = kCursor;
    }

    std::string line = std::string(on_cursor ? "> " : "  ") + entry.question;
    f32 row_y =
        list_bottom_y - static_cast<f32>(list.size() - 1 - i) * kLineSpacing;
    renderer_draw_text(line, glm::vec2(kListX, row_y), colour);
  }
}
