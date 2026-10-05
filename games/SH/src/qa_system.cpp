#include "qa_system.h"
#include "text_wrap.h"

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
// Mirrors kListX on the right -- how far a question/answer line is allowed
// to run before wrap_text() (text_wrap.h) breaks it onto a further line,
// so long .conversation-authored text can't run off the right edge of the
// screen.
constexpr f32 kRightMargin = 32.0f;
constexpr f32 kBottomMargin = 32.0f;
constexpr f32 kLineSpacing = 28.0f;
// Gap between the question list's last row and the hint line below it.
constexpr f32 kAnswerGap = 48.0f;
// Gap between the answer line and its own "[Enter] continue" hint below
// it -- tighter than kAnswerGap since these two lines belong together.
constexpr f32 kAnswerHintGap = 36.0f;
// Estimated width of the "> "/"  " cursor marker prefixed to every
// question line (see render() below) -- subtracted from the available
// wrap width so a wrapped question's continuation lines (indented to
// align under the marker, not past it) don't budget space for it twice.
constexpr f32 kMarkerWidth = 2.0f * kAverageCharWidth;

// Question colours: bright while unasked, permanently darkened once asked,
// and a warm highlight on whichever line the cursor is on.
constexpr glm::vec4 kUnasked{1.0f, 1.0f, 1.0f, 1.0f};
constexpr glm::vec4 kAsked{0.35f, 0.35f, 0.35f, 1.0f};
constexpr glm::vec4 kCursor{1.0f, 0.9f, 0.5f, 1.0f};
constexpr glm::vec4 kAnswer{0.85f, 0.85f, 0.85f, 1.0f};
constexpr glm::vec4 kHint{0.5f, 0.5f, 0.5f, 1.0f};
// The photographer's own lines -- the player is the one holding the
// camera, so his lines take the same warm tone as the cursor rather than
// a name label like everyone else's.
constexpr glm::vec4 kPhotographer{1.0f, 0.9f, 0.5f, 1.0f};
constexpr glm::vec4 kLink{0.45f, 0.75f, 1.0f, 1.0f};
// [overlay] text -- see script_line.h. Laid out in the upper part of the
// frame, each line of a run stepped further down and across so the run
// fills the open space instead of stacking into one block.
constexpr glm::vec4 kOverlay{0.45f, 0.6f, 1.0f, 1.0f};
constexpr f32 kOverlayTopFraction = 0.14f;
constexpr f32 kOverlayLineStep = 58.0f;
constexpr f32 kOverlayIndentFraction = 0.11f;
// [title] cards -- same centred-on-black look as SHGame's own intertitle
// screen (game_menus.cpp), just drawn over a scene that stays loaded.
constexpr f32 kTitleLineSpacing = 36.0f;
constexpr f32 kTitleHintGap = 40.0f;
constexpr f32 kTitleMaxWidthFraction = 0.7f;

bool key_pressed(input::Key key) {
  return input::is_key_down(key) && !input::was_key_down(key);
}

f32 centered_x(f32 screen_width, size_t char_count) {
  return (screen_width - static_cast<f32>(char_count) * kAverageCharWidth) /
         2.0f;
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
// the two are structurally identical (question/answer_lines/tag/
// follow_ups) except Entry also carries the in-game `asked` flag and
// on_selected callback conversation data has no business knowing about,
// which just default to false/empty here.
QASystem::Entry to_entry(const ConversationQuestion &question) {
  QASystem::Entry entry;
  entry.question = question.text;
  entry.answer_lines = question.answer_lines;
  entry.tag = question.tag;
  entry.shared_id = question.shared_id;
  entry.loop_target = question.loop_target;
  entry.requires_flags = question.requires_flags;
  entry.requires_not_flags = question.requires_not_flags;
  entry.sets_flags = question.sets_flags;
  entry.is_ending = question.is_ending;
  entry.is_auto = question.is_auto;
  entry.ending_lines = question.ending_lines;
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
  // parsed may contain question_ref= stand-ins for a question shared
  // across more than one attachment point (see resources/conversation.h's
  // file format comment) -- resolve those into a plain, fully-expanded
  // tree first, so to_entry() below never has to know sharing was ever
  // involved.
  Conversation resolved = resolve_shared_questions(*parsed);

  ConversationHandle handle = next_conversation_handle_++;
  for (const ConversationQuestion &question : resolved.questions) {
    entries_.push_back(to_entry(question));
    entry_handles_.push_back(handle);
  }
  // entries_ may have just reallocated (invalidating any pointer into a
  // follow_ups vector nested inside it), so loop_targets_ has to be rebuilt
  // fresh rather than merely extended -- see rebuild_loop_targets().
  rebuild_loop_targets();

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
  // worth the complexity next to just always resetting). list_stack_ and
  // layer_tag_stack_ are cleared alongside it for the same reason -- their
  // entries could otherwise dangle/refer to removed content once the
  // erase() loop below runs.
  current_list_ = &entries_;
  list_stack_.clear();
  layer_tag_stack_.clear();
  current_layer_tag_.reset();
  state_ = State::QuestionList;
  cursor_ = 0;
  answer_line_ = 0;
  // Flags are conversation-scoped -- a fresh begin_chapter_playing() always
  // unloads the previous conversation first (see game_menus.cpp), so this
  // is what keeps flags_ from leaking across chapters/fresh restarts;
  // Continue's apply_flags() (called after the following
  // load_conversation()) puts a saved chapter's flags back afterward.
  flags_.clear();
  // Scene-state tags are conversation-scoped too -- frees up whatever
  // names handle's chapter registered (e.g. "Room_01".."Room_05") so the
  // next chapter's begin_chapter_playing() can register the same names
  // again against its own scenes without register_scene_state()'s
  // duplicate-name warning firing. See clear_scene_states()'s own comment.
  clear_scene_states();

  for (size_t i = entries_.size(); i-- > 0;) {
    if (entry_handles_[i] == handle) {
      entries_.erase(entries_.begin() + static_cast<ptrdiff_t>(i));
      entry_handles_.erase(entry_handles_.begin() + static_cast<ptrdiff_t>(i));
    }
  }
  rebuild_loop_targets(); // handle's entries (and their ids) are gone now
}

namespace {
// Recursively searches list (and every entry's follow_ups) for an exact
// question-text match -- returns nullptr if none found. Shared by
// set_on_selected() below.
QASystem::Entry *find_entry_by_text(std::vector<QASystem::Entry> &list,
                                    std::string_view text) {
  for (QASystem::Entry &entry : list) {
    if (entry.question == text) {
      return &entry;
    }
    if (QASystem::Entry *found = find_entry_by_text(entry.follow_ups, text)) {
      return found;
    }
  }
  return nullptr;
}
} // namespace

void QASystem::set_on_selected(std::string_view question_text,
                               std::function<void()> callback) {
  Entry *entry = find_entry_by_text(entries_, question_text);
  if (!entry) {
    KWARN("QASystem::set_on_selected: no currently-loaded question has the "
         "exact text '{}'.",
         question_text);
    return;
  }
  entry->on_selected = std::move(callback);
}

void QASystem::set_on_returned_to_root(std::function<void()> callback) {
  on_returned_to_root_ = std::move(callback);
}

void QASystem::set_on_any_asked(std::function<void()> callback) {
  on_any_asked_ = std::move(callback);
}

namespace {
// Depth-first pre-order walk shared by asked_flags()/apply_asked_flags() --
// each entry's own flag before its follow_ups', in load/add order, exactly
// mirroring how to_entry()/index_loop_targets() already walk the same tree
// shape. Appends to (rather than returning) out_flags so the recursive
// case doesn't need to concatenate vectors.
void collect_asked_flags(const std::vector<QASystem::Entry> &list,
                         std::vector<bool> &out_flags) {
  for (const QASystem::Entry &entry : list) {
    out_flags.push_back(entry.asked);
    collect_asked_flags(entry.follow_ups, out_flags);
  }
}

// Mirrors collect_asked_flags()'s traversal to write flags back instead of
// reading them -- next_flag is an in/out cursor into flags shared across
// the whole recursive walk. Returns false (and stops early) the instant
// flags runs out, so a size mismatch can't read out of bounds.
bool apply_asked_flags_recursive(std::vector<QASystem::Entry> &list,
                                 const std::vector<bool> &flags,
                                 size_t &next_flag) {
  for (QASystem::Entry &entry : list) {
    if (next_flag >= flags.size()) {
      return false;
    }
    entry.asked = flags[next_flag++];
    if (!apply_asked_flags_recursive(entry.follow_ups, flags, next_flag)) {
      return false;
    }
  }
  return true;
}
} // namespace

std::vector<bool> QASystem::asked_flags() const {
  std::vector<bool> flags;
  collect_asked_flags(entries_, flags);
  return flags;
}

void QASystem::apply_asked_flags(const std::vector<bool> &flags) {
  size_t next_flag = 0;
  bool ok = apply_asked_flags_recursive(entries_, flags, next_flag);
  if (!ok || next_flag != flags.size()) {
    KWARN("QASystem::apply_asked_flags: flags.size() ({}) doesn't match the "
         "number of currently loaded entries -- ignoring (stale save vs. "
         "an edited conversation file?).",
         flags.size());
  }
}

std::vector<std::string> QASystem::flags() const {
  return std::vector<std::string>(flags_.begin(), flags_.end());
}

void QASystem::apply_flags(const std::vector<std::string> &flags) {
  flags_ = std::unordered_set<std::string>(flags.begin(), flags.end());
}

void QASystem::set_on_line_shown(
    std::function<void(const ScriptLine &)> callback) {
  on_line_shown_ = std::move(callback);
}

const ScriptLine *QASystem::current_line() const {
  return state_ == State::Answer ? &current_line_ : nullptr;
}

void QASystem::resume_at_first_unasked() {
  current_list_ = &entries_;
  list_stack_.clear();
  layer_tag_stack_.clear();
  current_layer_tag_.reset();
  state_ = State::QuestionList;
  answer_line_ = 0;
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (!entries_[i].asked && entry_visible(entries_[i])) {
      cursor_ = i;
      return;
    }
  }
}

void QASystem::ask_current() {
  std::vector<Entry> &list = *current_list_;
  list[cursor_].asked = true; // permanent -- never cleared
  // Flags this question sets are applied the same instant, before
  // on_selected/the tag reaction fire below -- so either can already see
  // the consequence of this question having been asked (e.g. a registered
  // scene state that itself checks game-side state influenced by a flag
  // wouldn't need to, but on_selected reaching back into game code might
  // reasonably expect flags_ to be current).
  for (const std::string &flag : list[cursor_].sets_flags) {
    flags_.insert(flag);
  }
  if (on_any_asked_) {
    on_any_asked_();
  }
  if (list[cursor_].on_selected) {
    list[cursor_].on_selected();
  }
  // The question is being asked right now -- fire its own tag's scene
  // state immediately (the "reaction shot"; see the class comment), rather
  // than waiting for its answer to finish.
  fire_scene_state(list[cursor_]);
  answer_line_ = 0;
  state_ = State::Answer;
  // A fresh answer starts with nobody speaking yet and no overlay run --
  // speakers carry over between lines of one answer, never between
  // answers.
  current_line_ = ScriptLine{};
  overlay_run_.clear();
  show_current_line();
}

void QASystem::show_current_line() {
  const Entry &entry = (*current_list_)[cursor_];
  if (answer_line_ >= entry.answer_lines.size()) {
    return;
  }
  std::optional<std::string> previous_speaker = current_line_.speaker;
  current_line_ = parse_script_line(entry.answer_lines[answer_line_]);
  if (!current_line_.speaker) {
    current_line_.speaker = std::move(previous_speaker);
  }
  if (current_line_.overlay) {
    overlay_run_.push_back(current_line_.text);
  } else {
    overlay_run_.clear();
  }
  if (on_line_shown_) {
    on_line_shown_(current_line_);
  }
}

void QASystem::set_on_ending_reached(std::function<void(const Entry &)> callback) {
  on_ending_reached_ = std::move(callback);
}

bool QASystem::entry_visible(const Entry &entry) const {
  for (const std::string &flag : entry.requires_flags) {
    if (!flags_.contains(flag)) {
      return false;
    }
  }
  for (const std::string &flag : entry.requires_not_flags) {
    if (flags_.contains(flag)) {
      return false;
    }
  }
  return true;
}

std::vector<size_t> QASystem::visible_indices(const std::vector<Entry> &list) const {
  std::vector<size_t> visible;
  for (size_t i = 0; i < list.size(); ++i) {
    if (entry_visible(list[i])) {
      visible.push_back(i);
    }
  }
  return visible;
}

void QASystem::set_base_scene_state(std::function<void()> callback) {
  base_scene_state_ = std::move(callback);
}

void QASystem::register_scene_state(std::string_view name,
                                    std::function<void()> callback) {
  std::string key(name);
  if (scene_states_.contains(key)) {
    KWARN("QASystem::register_scene_state: '{}' was already registered -- "
         "overwriting.",
         key);
  }
  scene_states_[key] = std::move(callback);
}

void QASystem::clear_scene_states() { scene_states_.clear(); }

void QASystem::fire_scene_state(const Entry &entry) {
  if (!entry.tag) {
    return;
  }
  auto it = scene_states_.find(*entry.tag);
  if (it != scene_states_.end()) {
    it->second();
  } else {
    KWARN("QASystem: question '{}' has tag '{}' with no registered "
         "scene state.",
         entry.question, *entry.tag);
  }
}

void QASystem::apply_layer_scene_state() {
  if (!current_layer_tag_) {
    if (base_scene_state_) {
      base_scene_state_();
    }
    return;
  }
  auto it = scene_states_.find(*current_layer_tag_);
  if (it != scene_states_.end()) {
    it->second();
  } else {
    KWARN("QASystem: layer tag '{}' has no registered scene state.",
         *current_layer_tag_);
  }
}

void QASystem::index_loop_targets(std::vector<Entry> &list,
                                  std::vector<std::vector<Entry> *> ancestors,
                                  std::vector<std::optional<std::string>> ancestor_tags,
                                  std::optional<std::string> layer_tag) {
  for (size_t i = 0; i < list.size(); ++i) {
    Entry &entry = list[i];
    if (entry.shared_id) {
      auto [it, inserted] = loop_targets_.emplace(
          *entry.shared_id, LoopTarget{&list, i, ancestors, ancestor_tags, layer_tag});
      if (!inserted) {
        KWARN("QASystem: duplicate id '{}' across loaded conversations -- "
             "keeping the first found as the loop_to= target.",
             *entry.shared_id);
      }
    }
    if (!entry.follow_ups.empty()) {
      std::vector<std::vector<Entry> *> child_ancestors = ancestors;
      child_ancestors.push_back(&list);
      std::vector<std::optional<std::string>> child_tags = ancestor_tags;
      child_tags.push_back(layer_tag);
      std::optional<std::string> child_layer_tag = entry.tag ? entry.tag : layer_tag;
      index_loop_targets(entry.follow_ups, std::move(child_ancestors),
                         std::move(child_tags), std::move(child_layer_tag));
    }
  }
}

void QASystem::rebuild_loop_targets() {
  loop_targets_.clear();
  index_loop_targets(entries_, {}, {}, std::nullopt);
}

void QASystem::update() {
  if (entries_.empty()) {
    return;
  }

  if (state_ == State::QuestionList) {
    std::vector<Entry> &list = *current_list_;
    std::vector<size_t> visible = visible_indices(list);
    if (!visible.empty()) {
      // Where cursor_ currently sits within the visible sequence -- used to
      // step to its visible neighbor below rather than the raw-index
      // neighbor, which could be a hidden (flag-gated) entry. Falls back to
      // 0 if cursor_ itself isn't visible right now (shouldn't normally
      // happen -- see the Enter guard below -- but a flag change elsewhere
      // could in principle make the previously-visible cursor entry hidden
      // between frames).
      auto it = std::find(visible.begin(), visible.end(), cursor_);
      size_t visible_pos = it != visible.end()
                              ? static_cast<size_t>(it - visible.begin())
                              : 0;
      if (key_pressed(input::Key::Up) && visible_pos > 0) {
        cursor_ = visible[visible_pos - 1];
      }
      if (key_pressed(input::Key::Down) && visible_pos + 1 < visible.size()) {
        cursor_ = visible[visible_pos + 1];
      }
    }
    if (cursor_ < list.size() && entry_visible(list[cursor_])) {
      // An `auto` question asks itself the moment the cursor lands on it
      // (see resources/conversation.h) -- whether that's a loop_to= jump,
      // a descent into follow-ups, or the very first frame of a fresh
      // chapter. Only while still unasked, so one the player somehow
      // navigates back onto doesn't replay on its own.
      const Entry &entry = list[cursor_];
      if ((entry.is_auto && !entry.asked) || key_pressed(input::Key::Enter)) {
        ask_current();
      }
    }
  } else { // State::Answer
    if (key_pressed(input::Key::Enter)) {
      ++answer_line_;
      Entry &answered = (*current_list_)[cursor_];
      show_current_line();
      if (answer_line_ >= answered.answer_lines.size()) {
        // Past the last answer line -- the question is now fully read.
        // Decide what the question list shows next (see the class comment
        // for the exact policy), updating current_layer_tag_ to match
        // wherever navigation lands, then apply whichever scene state that
        // leaves current -- see apply_layer_scene_state().
        if (answered.is_ending) {
          // Highest priority -- an ending question isn't expected to also
          // have follow_ups/loop_to= of its own (same convention loop_to=
          // itself already follows), so reaching one stops navigation
          // right where it is instead of continuing the follow_ups/
          // pop-up/loop_to dance below. No apply_layer_scene_state() call
          // either -- there's no "list about to show" to pick a layer tag
          // for; it's up to on_ending_reached_ to decide what happens
          // next.
          if (on_ending_reached_) {
            on_ending_reached_(answered);
          }
          state_ = State::QuestionList; // must reset, or a further Enter
                                        // re-increments answer_line_ and
                                        // re-fires this
          return;
        }
        if (answered.loop_target) {
          // Jump straight to the loop_to= target's own location, exactly
          // as if the player had navigated there normally -- see
          // rebuild_loop_targets()/index_loop_targets(). Takes priority
          // over follow_ups/popping below (a looping question isn't
          // expected to also have follow_ups of its own -- see
          // resources/conversation.h's file format comment).
          auto it = loop_targets_.find(*answered.loop_target);
          if (it != loop_targets_.end()) {
            const LoopTarget &target = it->second;
            current_list_ = target.list;
            list_stack_.assign(target.ancestors.begin(), target.ancestors.end());
            layer_tag_stack_.assign(target.ancestor_tags.begin(),
                                    target.ancestor_tags.end());
            current_layer_tag_ = target.layer_tag;
            cursor_ = target.index;
          } else {
            KWARN("QASystem: question '{}' has loop_to='{}' with no "
                 "matching id= anywhere currently loaded.",
                 answered.question, *answered.loop_target);
          }
        } else if (!answered.follow_ups.empty()) {
          list_stack_.push_back(current_list_);
          layer_tag_stack_.push_back(current_layer_tag_);
          if (answered.tag) {
            // This question's own tag becomes the resting state for its
            // follow-up list (and, transitively, for anything answered
            // within it) -- see the class comment. An untagged question
            // leaves current_layer_tag_ exactly as it was, so its
            // follow-ups inherit whatever state already governed it.
            current_layer_tag_ = answered.tag;
          }
          current_list_ = &answered.follow_ups;
          std::vector<size_t> visible = visible_indices(*current_list_);
          cursor_ = visible.empty() ? 0 : visible.front();
        } else if (!list_stack_.empty()) {
          std::vector<size_t> visible = visible_indices(*current_list_);
          bool all_asked = std::all_of(visible.begin(), visible.end(), [this](size_t i) {
            return (*current_list_)[i].asked;
          });
          // Keep popping up one level at a time -- not just once -- for as
          // long as the level just landed on is *also* fully asked (this
          // happens whenever the question whose follow-ups we just
          // finished was the last unasked sibling at that shallower level
          // too), until reaching a level with an unasked question left, or
          // unwinding all the way back to entries_ (see the class
          // comment). Restore the tag that governed each shallower list in
          // turn as we go.
          while (all_asked) {
            current_list_ = list_stack_.back();
            list_stack_.pop_back();
            current_layer_tag_ = layer_tag_stack_.back();
            layer_tag_stack_.pop_back();
            std::vector<size_t> popped_visible = visible_indices(*current_list_);
            cursor_ = popped_visible.empty() ? 0 : popped_visible.front();
            if (current_list_ == &entries_) {
              // Nowhere further up to pop to -- list_stack_ is empty here
              // per its invariant, so the loop ends regardless.
              if (on_returned_to_root_) {
                on_returned_to_root_();
              }
              break;
            }
            all_asked = std::all_of(popped_visible.begin(), popped_visible.end(),
                                    [this](size_t i) { return (*current_list_)[i].asked; });
          }
          // else (loop never entered/exited early): unanswered siblings
          // remain at wherever we ended up -- stay there with cursor_ and
          // current_layer_tag_ unchanged (still valid/current).
        }
        // else: a leaf at the top level already (list_stack_ empty) --
        // nothing to do.
        apply_layer_scene_state();
        state_ = State::QuestionList;
        // Landed on an `auto` question -- ask it now rather than next
        // frame, so the list never flashes up for the one frame in between.
        std::vector<Entry> &landed = *current_list_;
        if (cursor_ < landed.size() && landed[cursor_].is_auto &&
            !landed[cursor_].asked && entry_visible(landed[cursor_])) {
          ask_current();
        }
      }
    }
  }
}

void QASystem::render(u32 screen_width, u32 screen_height) const {
  // Built bottom-up (see the class comment): start from the fixed bottom
  // anchor and work upward, so the anchor itself never depends on how many
  // questions are in the currently-displayed list. max_text_width bounds
  // how wide any wrapped line below is allowed to get -- see kListX/
  // kRightMargin.
  f32 hint_y = static_cast<f32>(screen_height) - kBottomMargin;
  f32 max_text_width = static_cast<f32>(screen_width) - kListX - kRightMargin;

  if (state_ == State::Answer) {
    const ScriptLine &line = current_line_;
    const f32 width = static_cast<f32>(screen_width);
    const f32 height = static_cast<f32>(screen_height);

    if (line.title) {
      // A [title] card: the text alone, centred on black, over a scene
      // that stays loaded underneath -- a backdrop quad rather than
      // clearing the scene the way SHGame::show_intertitle() does, so the
      // room the card interrupts doesn't have to be re-baked afterwards.
      renderer_draw_backdrop_quad(glm::vec2(0.0f), glm::vec2(width, height),
                                  glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
      std::vector<std::string> wrapped =
          wrap_text(line.text, width * kTitleMaxWidthFraction);
      f32 block_height =
          static_cast<f32>(wrapped.size() - 1) * kTitleLineSpacing;
      f32 top_y = height / 2.0f - block_height / 2.0f;
      for (size_t i = 0; i < wrapped.size(); ++i) {
        renderer_draw_text(
            wrapped[i],
            glm::vec2(centered_x(width, wrapped[i].size()),
                      top_y + static_cast<f32>(i) * kTitleLineSpacing),
            glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
      }
      constexpr std::string_view kTitleHint = "[Enter] continue";
      renderer_draw_text(kTitleHint,
                         glm::vec2(centered_x(width, kTitleHint.size()),
                                   top_y + block_height + kTitleHintGap),
                         kHint);
      return;
    }

    if (line.overlay) {
      // An [overlay] run (see overlay_run_): every line so far stays up,
      // each stepped down and across from the one before, wrapping back
      // to the left after three -- out in the open frame rather than in
      // the dialogue corner.
      for (size_t i = 0; i < overlay_run_.size(); ++i) {
        f32 x = kListX + width * kOverlayIndentFraction * static_cast<f32>(i % 3);
        f32 y = height * kOverlayTopFraction +
                static_cast<f32>(i) * kOverlayLineStep;
        std::vector<std::string> wrapped =
            wrap_text(overlay_run_[i], width - x - kRightMargin);
        for (size_t w = 0; w < wrapped.size(); ++w) {
          renderer_draw_text(wrapped[w],
                             glm::vec2(x, y + static_cast<f32>(w) * kLineSpacing),
                             kOverlay);
        }
      }
      renderer_draw_text("[Enter] continue", glm::vec2(kListX, hint_y), kHint);
      return;
    }

    // Ordinary dialogue, bottom-left. Everyone but the photographer is
    // named; his lines are the player's own, so they're told apart by
    // colour instead (see kPhotographer). A long line wraps onto further
    // lines above answer_bottom_y, growing upward -- same "bottom edge
    // never moves" reasoning as the question list's own row stacking
    // below, just per-wrapped-line instead of per-entry.
    f32 answer_bottom_y = hint_y - kAnswerHintGap;
    const bool photographer = line.speaker == kPhotographerSpeaker;
    std::string shown = line.text;
    if (line.speaker && !photographer) {
      shown = *line.speaker + ": " + shown;
    }
    glm::vec4 colour = photographer ? kPhotographer : kAnswer;
    if (line.link) {
      colour = kLink;
    }
    std::vector<std::string> wrapped = wrap_text(shown, max_text_width);
    for (size_t i = 0; i < wrapped.size(); ++i) {
      f32 line_y = answer_bottom_y -
          static_cast<f32>(wrapped.size() - 1 - i) * kLineSpacing;
      renderer_draw_text(wrapped[i], glm::vec2(kListX, line_y), colour);
    }
    if (line.link) {
      // Underlined like a link -- under the last wrapped line, which is
      // where the linked text itself ends up. The text baseline sits a
      // few pixels above line_y's anchor.
      f32 underline_y = answer_bottom_y + 6.0f;
      f32 underline_width =
          static_cast<f32>(wrapped.back().size()) * kAverageCharWidth;
      renderer_draw_line(glm::vec2(kListX, underline_y),
                         glm::vec2(kListX + underline_width, underline_y),
                         kLink);
      renderer_draw_text("[Enter] continue   [L] open link",
                         glm::vec2(kListX, hint_y), kHint);
      return;
    }
    renderer_draw_text("[Enter] continue", glm::vec2(kListX, hint_y), kHint);
    return;
  }

  // The cursor is parked on an `auto` question update() is about to ask --
  // nothing to offer the player, so draw no list at all for this frame.
  {
    const std::vector<Entry> &list = *current_list_;
    if (cursor_ < list.size() && list[cursor_].is_auto && !list[cursor_].asked) {
      return;
    }
  }

  renderer_draw_text("[Up/Down] choose   [Enter] ask   [Tab] camera",
                     glm::vec2(kListX, hint_y), kHint);
  f32 list_bottom_y = hint_y - kAnswerGap;

  // Wrap every visible entry's question text up front, into (prefixed
  // text, colour) rows -- a wrapped entry now consumes more than one row,
  // so the bottom-up stacking below works in units of individual *rows*
  // rather than *entries*. Only the first wrapped line of an entry gets
  // the "> "/"  " cursor marker; continuation lines get "  " so they stay
  // indented to align under the first line's text rather than under the
  // marker.
  const std::vector<Entry> &list = *current_list_;
  std::vector<size_t> visible = visible_indices(list);
  struct Row {
    std::string text;
    glm::vec4 colour;
  };
  std::vector<Row> rows;
  for (size_t n = 0; n < visible.size(); ++n) {
    size_t i = visible[n];
    const Entry &entry = list[i];
    if (entry.is_auto) {
      continue; // never a choice -- see Entry::is_auto
    }
    bool on_cursor = i == cursor_;

    glm::vec4 colour = entry.asked ? kAsked : kUnasked;
    if (on_cursor) {
      colour = kCursor;
    }

    std::vector<std::string> wrapped =
        wrap_text(entry.question, max_text_width - kMarkerWidth);
    for (size_t w = 0; w < wrapped.size(); ++w) {
      std::string marker = (w == 0) ? (on_cursor ? "> " : "  ") : "  ";
      rows.push_back({marker + wrapped[w], colour});
    }
  }

  // Row position k (k's own position within the flattened row sequence,
  // not its source entry's index) works backward from the list's fixed
  // bottom, so the last row always lands at list_bottom_y and earlier
  // ones stack upward -- same "aligned regardless of count" property the
  // bottom anchor is for, now measured in wrapped rows so a flag-gated
  // (hidden) entry, or an entry that happened to wrap onto fewer/more
  // lines, never leaves a gap or an overlap.
  for (size_t k = 0; k < rows.size(); ++k) {
    f32 row_y =
        list_bottom_y - static_cast<f32>(rows.size() - 1 - k) * kLineSpacing;
    renderer_draw_text(rows[k].text, glm::vec2(kListX, row_y), rows[k].colour);
  }
}
