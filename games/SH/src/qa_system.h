#pragma once
#include <defines.h>
#include <glm/glm.hpp>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
// Two scene-state changes happen per question, at two different moments
// (see register_scene_state() below for what a tag actually triggers):
//   - The instant a question is *asked* (Enter pressed on it in the list,
//     before its answer starts playing): that question's own `tag=`
//     scene state fires, if it has one. This is the "reaction shot" --
//     it fires immediately, same as Entry::on_selected.
//   - The instant its answer finishes (past the last answer line, Enter
//     pressed once more): the *current layer's* scene state fires instead
//     -- see current_layer_tag_. Each layer's scene state is established
//     by whichever question was answered to reveal it: answering a
//     question with follow-ups carries that question's own tag forward as
//     the resting state for the follow-up list now being shown, and that
//     same state persists for every further question answered within that
//     list (including nested follow-ups of follow-ups, and so on to any
//     depth) until a deeper question with its own tag establishes a new
//     one for its own follow-ups, or the view pops back up to a
//     shallower list, which restores whatever state was established there.
//     A question with no tag of its own leaves the current layer's state
//     exactly as it already was rather than clearing it -- e.g. an intro
//     question with no tag doesn't blank out whatever state its own
//     follow-up list should keep using. If no layer has established any
//     state yet (nothing tagged has been answered along the current
//     chain), the designated base scene state fires instead -- see
//     set_base_scene_state().
//
// Past the last answer line, the question list itself is then updated:
//   - If the just-answered entry has follow-up questions, the list swaps
//     to showing *that entry's* follow-ups instead of the list it was
//     just picked from -- so answering "Who are you?" can reveal a
//     handful of questions specific to that answer, not just dump the
//     player back at the top level.
//   - Otherwise (a "leaf" question, no follow-ups): if every question at
//     the current level has now been asked, the branch is fully explored,
//     so the view pops back up one level in the chain it descended
//     through to get here (the list containing whichever question's
//     follow-ups current_list_ now is). If that shallower level turns out
//     to be fully asked too -- which happens whenever the question that
//     led down into the just-finished branch was the last unasked sibling
//     at that level as well -- the view keeps popping up one level at a
//     time, and so on to any depth, until it either lands on a level with
//     an unasked question left, or unwinds all the way to the top-level
//     list. Otherwise (an unasked question remains at the level just
//     landed on) it stays there so the player can pick a remaining
//     sibling follow-up.
//   - Unless the just-answered entry has a loop_to= target (see
//     resources/conversation.h) instead: rather than either of the above,
//     the view jumps straight to wherever the target question was
//     authored (its own containing list, cursor landing on it), as if the
//     player had navigated there normally -- see loop_targets_. Lets a
//     question further down the chain send the player back to an earlier
//     point in the conversation (including, unlike a plain follow-up
//     chain, back up to one of its own ancestors) instead of dead-ending
//     or continuing to dive deeper.
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
// Long question/answer text wraps onto further lines rather than running
// off the right edge (see text_wrap.h's wrap_text()), which the same
// upward-building layout absorbs for free -- a wrapped entry just costs
// more rows. See render()'s screen_width/screen_height parameters.
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
    // Fired from update(), once, the instant the player selects this
    // question (Enter pressed while the cursor is on it) -- before its
    // answer starts playing, at the same point `asked` is set. Empty by
    // default (most questions have no side effect). Set via
    // set_on_selected() below rather than assigned directly -- game code
    // has no other way to reach a specific Entry living inside entries_/
    // some other entry's follow_ups.
    std::function<void()> on_selected;
    // Opaque symbolic name from the loaded .conversation file's `tag=`
    // line (see resources/conversation.h -- copied verbatim by to_entry()
    // in qa_system.cpp), or nullopt if this question has none. Dispatched
    // by name against scene_states_ in update() the instant this question
    // is selected (same moment as on_selected above and `asked` is set) --
    // the "reaction shot" for asking this specific question. See the
    // class comment for how this fits together with the base scene state
    // that fires once the answer is done being read, and
    // register_scene_state() below for what a tag actually triggers.
    std::optional<std::string> tag;

    // Copied from ConversationQuestion::shared_id (see resources/
    // conversation.h) -- non-nullopt if this entry is the canonical
    // location some loop_target below points at. Only used to build
    // loop_targets_ right after loading; never read afterward.
    std::optional<std::string> shared_id;
    // Copied from ConversationQuestion::loop_target -- non-nullopt if
    // selecting this entry should jump the view back to whichever entry
    // was authored with a matching shared_id, instead of the normal
    // descend-into-follow_ups/pop-back-up handling -- see update()'s
    // "past the last answer line" handling and the class comment's note
    // on looping.
    std::optional<std::string> loop_target;

    // Flag names this question requires set/unset to be selectable at all
    // -- copied from ConversationQuestion::requires_flags/
    // requires_not_flags (see resources/conversation.h's file format
    // comment). Checked by entry_visible() below; a question failing
    // either check is skipped entirely by navigation and render(), as if
    // it weren't in the list.
    std::vector<std::string> requires_flags;
    std::vector<std::string> requires_not_flags;
    // Flag names set (in flags_ below), permanently, the instant this
    // question is asked -- copied from ConversationQuestion::sets_flags.
    std::vector<std::string> sets_flags;
    // Copied from ConversationQuestion::is_ending -- see update()'s "past
    // the last answer line" handling and set_on_ending_reached() below.
    bool is_ending = false;
    // Copied from ConversationQuestion::ending_lines -- this ending's own
    // outro text (see resources/conversation.h's `ending_text=` lines),
    // meaningful only when is_ending is true. set_on_ending_reached()'s
    // callback is handed the whole Entry, so it can read this directly to
    // show a per-ending outro instead of one generic message shared by
    // every ending.
    std::vector<std::string> ending_lines;
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

  // Attaches callback as the given question's on-select hook (see
  // Entry::on_selected) -- e.g. to swap out a scene/model the moment the
  // player picks a particular dialogue option:
  //
  //   qa_.set_on_selected("Do you remember what happened?", [this] {
  //     renderer_remove_scene(scene_);
  //     scene_ = renderer_load_scene("assets/scenes/man_transformed.sdf")
  //                  .scale(0.35)
  //                  .translate(glm::vec3(0.0, -2.8, 0.0));
  //   });
  //
  // A general escape hatch for a one-off side effect keyed by a question's
  // exact text -- for anything that names a reusable, symbolic scene state
  // in the .conversation file itself (so content can declare the
  // association instead of game code searching for literal text), use
  // register_scene_state() below instead; that's what a `tag=` line
  // dispatches to.
  //
  // Searches every currently-loaded top-level question and its follow-ups
  // (recursively, in load/add order) for an exact match against
  // question_text; logs a warning and does nothing if none matches (a
  // typo, or called before the question's conversation is loaded -- call
  // this after load_conversation() returns, not before). Matches by the
  // question's own text rather than a numeric path into entries_/
  // follow_ups, so a hookup in game code reads naturally against the
  // .conversation file's actual content and doesn't silently point at the
  // wrong question if the file is later reordered. The callback itself
  // lives inside that Entry (not tracked by a separate pointer/index this
  // class has to keep valid), so once attached it survives any later
  // entries_ reallocation from a further add_entry()/load_conversation()
  // call.
  void set_on_selected(std::string_view question_text, std::function<void()> callback);

  // Attaches callback as the "returned to the top-level list" hook -- fired
  // from update() exactly when a one-level-up pop (see the class comment's
  // "past the last answer line" section) lands on entries_ itself, i.e.
  // the *last* pop of a chain, whether that's a single pop (one level of
  // nesting) or the final one of several (a level popped back into that
  // then turned out to be fully asked too, on some later exhaustion --
  // each individual pop only ever moves up one level, so reaching entries_
  // from several levels deep takes one exhaustion per level, not one
  // callback firing per level). NOT fired for a leaf question that was
  // already at the top level (there's nowhere to "pop back" from), and not
  // fired repeatedly while sitting at the top level -- only on the
  // transition itself. One hook for the whole conversation (unlike
  // set_on_selected(), which targets a single question) -- e.g. to reset
  // the scene back to its base state once the player has exhausted a
  // branch:
  //
  //   qa_.set_on_returned_to_root([this] { apply_scene_state(SceneState::TheoRoom1); });
  void set_on_returned_to_root(std::function<void()> callback);

  // Attaches callback as the fallback scene state -- fired from update()
  // once an answer finishes (see the class comment's current_layer_tag_
  // section) whenever no layer along the current chain has established a
  // tag-based state of its own yet, e.g. right after an untagged
  // introductory question is answered, before any of its (possibly
  // tagged) follow-ups have been picked:
  //
  //   qa_.set_base_scene_state([this] { apply_scene_state(SceneState::TheoRoom1); });
  void set_base_scene_state(std::function<void()> callback);

  // Registers callback under name -- fired from update() the instant a
  // question whose `tag=` line (see resources/conversation.h's file
  // format, and Entry::tag above) equals name is selected -- see the class
  // comment for exactly when. Mirrors how Ink's host game reacts to a `#tag`
  // line, or how Yarn Spinner dispatches a `<<command>>` to a handler
  // registered via AddCommandHandler(name, ...): the dialogue content
  // declares a symbolic name, game code says what that name means, and
  // QASystem itself never interprets it. E.g.:
  //
  //   qa_.register_scene_state("Room_01",
  //                            [this] { apply_scene_state(SceneState::TheoRoom1); });
  //
  // Unlike set_on_selected(), this has nothing to do with any particular
  // Entry -- name -> callback is stored independently of entries_/
  // follow_ups, so call order relative to load_conversation() doesn't
  // matter. Tag names only need to be unique among whatever's currently
  // registered, though -- see clear_scene_states() below for the intended
  // way to free a name back up (e.g. so every chapter's .conversation file
  // can reuse the same small set of tag names, like "Room_01".."Room_05",
  // without colliding with one another). Logs a warning (but still
  // overwrites) if name is already registered *and wasn't cleared first* --
  // that's almost certainly a content mistake (two questions/files meaning
  // to use two different names, colliding by accident) rather than an
  // intentional handoff.
  void register_scene_state(std::string_view name, std::function<void()> callback);

  // Forgets every name->callback registered via register_scene_state() so
  // far, freeing all of them back up to be registered again under a new
  // meaning -- e.g. game code re-registering "Room_01".."Room_05" against a
  // different chapter's own scenes each time a new .conversation file is
  // loaded, without register_scene_state()'s duplicate-name warning firing
  // on what's actually an intentional handoff rather than a content
  // mistake. Called from unload_conversation() below for exactly that
  // reason -- mirrors flags_ being cleared there too. Does not touch
  // base_scene_state_/on_returned_to_root_/on_ending_reached_ (see
  // set_base_scene_state()/set_on_returned_to_root()/
  // set_on_ending_reached()) -- those aren't name-keyed, so they're
  // typically registered once and left alone across every chapter.
  void clear_scene_states();

  // Attaches callback as the "any question was just asked" hook -- fired
  // from update() every time Enter is pressed on a question in the list
  // (right after that Entry's `asked` flag is set), regardless of which
  // question it was or whether it has a tag. Unlike set_on_selected() (one
  // specific question) or register_scene_state() (one specific tag), this
  // fires for every single question across every currently loaded
  // conversation -- meant for a generic "the player just made progress"
  // signal (e.g. game code autosaving via asked_flags() below) that
  // shouldn't need a callback wired to each individual question.
  void set_on_any_asked(std::function<void()> callback);

  // Returns entries_'s (and, recursively, every follow_ups vector nested
  // inside it) `asked` flags, flattened via a depth-first pre-order walk:
  // each entry's own flag, then its follow_ups' flags (recursively),
  // before moving to the next sibling. Meant to be persisted verbatim
  // (e.g. to a save file) and handed back to apply_asked_flags() below --
  // the traversal order is otherwise not meaningful on its own.
  std::vector<bool> asked_flags() const;

  // Applies a previously-captured asked_flags() result back onto the
  // currently loaded entries_, using the exact same depth-first pre-order
  // walk -- so this only makes sense right after loading the same
  // conversation(s) asked_flags() was captured from, in the same order.
  // Logs a warning and does nothing if flags.size() doesn't match the
  // number of entries currently loaded (e.g. a save from before a
  // .conversation file was edited) rather than guess at a partial
  // application. Does not touch navigation state (current_list_, cursor_,
  // etc.) -- the view stays wherever it already was; callers that want to
  // land back at the top level after applying a save should do so
  // themselves (e.g. by calling this right after load_conversation(),
  // before the player has navigated anywhere).
  void apply_asked_flags(const std::vector<bool> &flags);

  // Returns every currently-set flag name (see Entry::requires_flags/
  // sets_flags above), in unspecified order -- meant to be persisted
  // verbatim and handed back to apply_flags() below, mirroring
  // asked_flags()/apply_asked_flags()'s own pattern exactly, just without
  // needing a traversal since flags_ is already a flat set.
  std::vector<std::string> flags() const;

  // Applies a previously-captured flags() result -- replaces flags_
  // outright (not merged with whatever's already set), so this only makes
  // sense right after loading, before the player has asked anything new.
  void apply_flags(const std::vector<std::string> &flags);

  // Attaches callback as the "an ending was just reached" hook -- fired
  // from update() the instant an Entry::is_ending question's answer
  // finishes (see the class comment's "past the last answer line" section
  // and resources/conversation.h's `ending` marker), in place of the
  // normal follow_ups/pop-up/loop_to handling: reaching an ending stops
  // dialogue navigation right where it is rather than continuing to
  // browse, leaving it up to callback to decide what happens next (e.g.
  // showing an outro and leaving the chapter). The finished Entry itself
  // is passed through so callback can inspect its tag/text if it wants a
  // distinct reaction per ending -- reusing the existing tag dispatch
  // (register_scene_state() below) rather than a second naming scheme
  // just for endings.
  void set_on_ending_reached(std::function<void(const Entry &)> callback);

  void update();

  // screen_width/screen_height are the current framebuffer size (screen
  // pixels) -- screen_height anchors the block to the bottom of the screen
  // (see the class comment), screen_width bounds how wide a question/
  // answer line is allowed to get before wrap_text() (text_wrap.h) breaks
  // it onto a further line, so long .conversation-authored text doesn't
  // run off the right edge.
  void render(u32 screen_width, u32 screen_height) const;

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

  // The chain of ancestor lists current_list_ descended through to get
  // where it is -- list_stack_.back() is current_list_'s immediate parent
  // (the list containing whichever question's follow_ups current_list_
  // now is), and so on back to (but not including) &entries_, which is
  // implicitly where the chain bottoms out. Pushed in update() whenever an
  // answered question's follow-ups are shown (descending one level),
  // popped whenever a fully-asked level pops back up one level (see the
  // class comment's "past the last answer line" section) -- so this is
  // always empty exactly when current_list_ == &entries_, and cleared
  // alongside current_list_'s reset in unload_conversation() for the same
  // reason (its entries could otherwise dangle once erase() runs).
  std::vector<std::vector<Entry> *> list_stack_;

  // Parallel to list_stack_ -- layer_tag_stack_[i] is the value
  // current_layer_tag_ held right before descending into list_stack_[i],
  // so popping back up one level restores the tag that governed that
  // shallower list too, rather than leaving whatever tag governed the
  // deeper branch just left behind in place. Pushed/popped in lockstep
  // with list_stack_ for that reason.
  std::vector<std::optional<std::string>> layer_tag_stack_;

  // The tag whose scene state governs the list current_list_ currently
  // is -- see the class comment. nullopt until some question with a tag
  // of its own has been answered along the current chain (in which case
  // set_base_scene_state()'s fallback applies instead). Updated in
  // update() only when descending into a tagged question's follow_ups
  // (an untagged question's follow_ups leave it as whatever it already
  // was -- inherited, not cleared); restored from layer_tag_stack_ when
  // popping back up; reset to nullopt alongside current_list_'s reset in
  // unload_conversation().
  std::optional<std::string> current_layer_tag_;

  // See set_on_returned_to_root() above.
  std::function<void()> on_returned_to_root_;

  // See set_base_scene_state() above.
  std::function<void()> base_scene_state_;

  // Every flag name currently set -- see flags()/apply_flags() and
  // Entry::requires_flags/requires_not_flags/sets_flags above. Presence in
  // this set means "set"; there's no explicit false/unset state to track
  // (a flag either has been set or hasn't), and no `unsets=` to remove one
  // once set.
  std::unordered_set<std::string> flags_;

  // True if every one of entry.requires_flags is in flags_ and none of
  // entry.requires_not_flags is -- an entry failing this is skipped
  // entirely by visible_indices() below, navigation, and render(), as if
  // it weren't in the list at all (not just grayed out/unselectable).
  bool entry_visible(const Entry &entry) const;

  // Indices into list of every entry currently passing entry_visible(),
  // in list order -- the single source of truth update()'s Up/Down/
  // cursor-reset handling and render()'s row layout both go through, so
  // the two can never disagree about which rows exist right now. list is
  // small (a conversation's question count), so this is recomputed fresh
  // wherever needed rather than cached.
  std::vector<size_t> visible_indices(const std::vector<Entry> &list) const;

  // See set_on_ending_reached() above.
  std::function<void(const Entry &)> on_ending_reached_;

  // See set_on_any_asked() above.
  std::function<void()> on_any_asked_;

  // See register_scene_state() above.
  std::unordered_map<std::string, std::function<void()>> scene_states_;

  // Looks up entry.tag in scene_states_ and fires it, if present; logs a
  // warning (naming entry.question) if entry.tag is set but no scene state
  // was ever registered under that name. No-op if entry.tag is nullopt.
  // Called from update() the instant a question is selected -- see the
  // class comment.
  void fire_scene_state(const Entry &entry);

  // Fires the scene state named by current_layer_tag_ if it's set (logging
  // a warning if that name was never registered), or base_scene_state_
  // otherwise. Called from update() once an answer finishes and
  // current_layer_tag_ has already been updated/restored for wherever
  // navigation landed -- see the class comment.
  void apply_layer_scene_state();

  // Everything update() needs to jump straight to a loop_to= target's own
  // location, as if the player had navigated there normally -- i.e. a
  // precomputed list_stack_/layer_tag_stack_/current_layer_tag_ snapshot
  // for that target, built once by rebuild_loop_targets() rather than
  // re-walked from entries_ on every jump.
  struct LoopTarget {
    std::vector<Entry> *list;        // the list containing the target entry
    size_t index;                    // target entry's index within *list
    std::vector<std::vector<Entry> *> ancestors;          // -> list_stack_
    std::vector<std::optional<std::string>> ancestor_tags; // -> layer_tag_stack_
    std::optional<std::string> layer_tag; // -> current_layer_tag_, governs *list
  };

  // Rebuilds loop_targets_ from scratch by walking entries_ -- called at
  // the end of load_conversation()/unload_conversation(), since either can
  // change which id='d entries exist (and, for a load, entries_'s buffer
  // may have just reallocated, moving every follow_ups vector nested
  // inside it -- rebuilding fresh rather than patching incrementally means
  // this never has to reason about which previously-recorded pointers are
  // still valid). Logs a warning (keeping the first found) if the same
  // shared_id shows up on more than one entry across every currently
  // loaded conversation -- loop_to= needs exactly one unambiguous target.
  void rebuild_loop_targets();

  // Recursive helper for rebuild_loop_targets() -- walks list (and,
  // recursively, every entry's follow_ups) recording an ancestors/
  // ancestor_tags/layer_tag path in lockstep with how update() itself
  // would build list_stack_/layer_tag_stack_/current_layer_tag_ descending
  // to reach it, so a jump into loop_targets_[id] later reproduces exactly
  // where a normal descent would have left navigation state.
  void index_loop_targets(std::vector<Entry> &list,
                          std::vector<std::vector<Entry> *> ancestors,
                          std::vector<std::optional<std::string>> ancestor_tags,
                          std::optional<std::string> layer_tag);

  std::unordered_map<std::string, LoopTarget> loop_targets_;

  State state_ = State::QuestionList;
  size_t cursor_ = 0;      // which question the selection cursor is on, within *current_list_
  size_t answer_line_ = 0; // current line within the open answer
};
