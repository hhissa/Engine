#pragma once
#include <defines.h>
#include <glm/glm.hpp>
#include <string>
#include <string_view>
#include <vector>

// Identifies one load_conversation() call's worth of top-level questions
// (and, transitively, their nested follow-ups), so unload_conversation()
// can remove exactly those and nothing else -- mirrors renderer_frontend.h's
// SceneHandle/kInvalidSceneHandle pattern exactly. 0 is never a valid
// handle.
using ConversationHandle = u32;
constexpr ConversationHandle kInvalidConversationHandle = 0;

// The game's main text system: a question-and-answer dialogue, with
// optional follow-up questions per entry. Content is authored in
// .conversation files (see resources/conversation.h) and brought in via
// load_conversation() below -- QASystem itself has no hardcoded dialogue.
//
// Starting state is a vertical block of top-level questions. The player
// moves a cursor through them (Up/Down) and selects one (Enter); selecting
// a question permanently darkens it in the list and switches to showing
// that question's answer, one line at a time -- Enter cycles through the
// answer lines. The question list is hidden entirely while an answer is
// showing (only the answer line + "[Enter] continue" hint are drawn) --
// see render().
//
// Past the last answer line:
//   - If the just-answered entry has follow-up questions, the list swaps
//     to showing *that entry's* follow-ups instead of the list it was
//     just picked from -- so answering "Who are you?" can reveal a
//     handful of questions specific to that answer, not just dump the
//     player back at the top level.
//   - Otherwise (a "leaf" question, no follow-ups): if every question at
//     the current level has now been asked, the branch is fully explored,
//     so the view returns all the way to the top-level list (not just one
//     level up) -- otherwise it stays on the current list so the player
//     can pick a remaining sibling follow-up.
// This can nest to any depth (a follow-up can have its own follow-ups),
// though .conversation files are only expected to use one or two levels
// in practice.
//
// Questions stay selectable after being asked (they're just darkened), so
// the player can re-read an answer, and re-asking a leaf whose siblings
// are already all asked will (once its answer finishes) still pop back to
// the top level, same as the first time.
//
// Anchored to the bottom-left of the screen: the bottom-most line (the
// hint text, or the answer + hint while one is open) sits a fixed distance
// above the bottom edge regardless of screen height, and the question
// list builds *upward* from there -- so the bottom of the block stays in
// exactly the same place no matter how many questions are registered;
// only the top (where row 0 lands) moves further up as more are added.
// See render()'s screen_height parameter.
//
// update() reads input and must be called once per frame from the game's
// update(); render() queues its text via renderer_draw_text() and must be
// called once per frame from the game's render() (queued draws don't
// persist across frames -- see renderer_frontend.h).
class QASystem {
public:
  // One question: its answer (one element per Enter press while it's
  // open) and, optionally, follow-up sub-questions revealed once the
  // answer finishes playing -- see the class comment's "past the last
  // answer line" section for exactly when those appear. Public (not a
  // private implementation detail) since engine/src/resources/conversation.h's parser builds
  // a tree of these directly from a .conversation file.
  struct Entry {
    std::string question;
    std::vector<std::string> answer_lines;
    bool asked = false; // set on first selection -- darkens the question
                        // in the list permanently
    std::vector<Entry> follow_ups;
  };

  // Appends a new top-level question and returns a reference to it -- pass
  // that reference to add_follow_up() below to attach sub-questions before
  // adding another top-level entry. NOTE: entries_ is a std::vector, so any
  // previously taken Entry& (from this call or add_follow_up()'s, for an
  // entry living directly in entries_) becomes invalid the instant another
  // add_entry() call reallocates it -- finish attaching one entry's
  // follow-ups before adding the next top-level entry, rather than holding
  // references to several across interleaved add_entry() calls. Mainly
  // useful for a handful of hardcoded/test questions -- real game content
  // belongs in a .conversation file loaded via load_conversation() below.
  Entry &add_entry(std::string question, std::vector<std::string> answer_lines);

  // Appends a follow-up sub-question to parent.follow_ups and returns a
  // reference to it, mirroring add_entry() -- e.g. to attach a further
  // follow-up of its own. Same reallocation caveat as add_entry() applies
  // to parent.follow_ups (and, transitively, to parent itself if parent
  // lives in a vector some other add_entry()/add_follow_up() call could
  // still reallocate).
  Entry &add_follow_up(Entry &parent, std::string question,
                       std::vector<std::string> answer_lines);

  // Loads path (a .conversation file -- see resources/conversation.h for the format)
  // and appends its top-level questions (and their nested follow-ups) to
  // the question list, tagged with a freshly minted handle that
  // unload_conversation() can later use to remove exactly those questions
  // again -- mirrors renderer_load_scene()/renderer_remove_scene()'s
  // handle pattern, so multiple conversation files can be loaded
  // side-by-side (e.g. one per room/act) and torn down independently.
  // Returns kInvalidConversationHandle on failure (missing/malformed
  // file -- resources/conversation.h's parser already logged why).
  ConversationHandle load_conversation(std::string_view path);

  // Removes every top-level question (and its follow-ups) that handle's
  // load_conversation() call added, leaving every other loaded
  // conversation's questions untouched. No-op (logs a warning) if handle
  // isn't currently loaded. Always resets the view back to the top-level
  // list first (even if handle's questions aren't the ones currently being
  // browsed/answered) -- simpler and safer than working out whether the
  // currently-open list is about to be invalidated, at the minor cost of
  // always kicking the player back to the top level on an unload.
  void unload_conversation(ConversationHandle handle);

  void update();

  // screen_height is the current framebuffer height (screen pixels) --
  // needed to anchor the block to the bottom of the screen; see the class
  // comment.
  void render(u32 screen_height) const;

private:
  enum class State {
    QuestionList, // the vertical block; cursor moves, Enter selects
    Answer,       // showing current_list_[cursor_]'s answer; Enter advances
  };

  std::vector<Entry> entries_; // top-level questions, in load/add order
  // Parallel to entries_ -- which load_conversation() call (if any) added
  // that top-level entry; kInvalidConversationHandle for one added
  // directly via add_entry(). Only top-level entries are tracked (a
  // follow-up is removed along with whichever top-level entry owns it, as
  // part of the same std::vector<Entry> element), so this never needs to
  // reach into follow_ups.
  std::vector<ConversationHandle> entry_handles_;
  ConversationHandle next_conversation_handle_ = 1; // 0 is kInvalidConversationHandle

  // Whichever list (entries_, or some entry's follow_ups) is currently
  // being browsed/answered from -- see the class comment. Always points at
  // a still-live vector: entries_/follow_ups vectors are only ever
  // appended to or have whole top-level elements erased (see
  // unload_conversation(), which resets this to &entries_ first, before
  // any erase that could otherwise dangle it). Declared after entries_ so
  // its default member initializer (&entries_) runs after entries_ already
  // exists.
  std::vector<Entry> *current_list_ = &entries_;

  State state_ = State::QuestionList;
  size_t cursor_ = 0;      // which question the selection cursor is on, within *current_list_
  size_t answer_line_ = 0; // current line within the open answer
};
