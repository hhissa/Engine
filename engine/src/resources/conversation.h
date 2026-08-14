#pragma once
#include "../defines.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// A pure data/parsing module: turns a .conversation file into an in-memory
// question/answer tree, with no knowledge of any particular game's
// dialogue system -- mirrors sdf_scene.h's own split for .sdf scene files
// (parsing lives here; a game's own dialogue system, e.g. SH's
// QASystem::load_conversation() in games/SH/src/qa_system.cpp, is what
// actually turns this into a live, playable state machine). Also read by
// tools/conversation_editor, a Qt node-graph editor that authors these
// files -- shared here (rather than duplicated per consumer) so the file
// format has exactly one parser, the same reasoning sdf_scene.h documents
// for GeometrySystem/testbed's sdf_authoring.h/the SDF editor.
//
// File format (see assets/conversations/*.conversation for real examples):
// a sequence of top-level `question "<text>" { ... }` blocks, each
// containing zero or more `answer=<line>` entries (one per line, in file
// order), an optional `tag=<name>` entry, and, optionally, further nested
// `question` blocks -- each of those becomes a follow-up of its enclosing
// question, to any depth:
//
//   #conversation file
//   version=0.1
//
//   question "Who are you?" {
//       answer=...
//       answer=I don't remember.
//       answer=I've been here a long time.
//
//       question "Do you have a name?" {
//           tag=RevealName
//           answer=Not one I remember.
//       }
//
//       question "How long is a long time?" {
//           answer=Long enough to forget the question.
//       }
//   }
//
// Question text must be double-quoted (it's free-form prose, unlike an
// .sdf file's bare identifier names) -- everything between the first and
// last '"' character on the header line becomes the question, so a
// question can't itself contain a literal '"'. Blank lines and lines
// starting with '#' are ignored anywhere; a top-level "version=" line is
// accepted but not currently read (kept for forward compatibility, like
// every other file format in this engine).
//
// `tag=` is opaque to this parser -- it's a symbolic name a particular
// game's dialogue system can react to however it likes (e.g. SH's
// QASystem::register_scene_state()/Entry::tag dispatches it to a scene/
// model transition), the same way Ink's own `#tag` lines or Yarn Spinner's
// `<<command>>`s are meaningless to the story engine itself and only mean
// something to whatever host code is listening for that name. This module
// stays deliberately ignorant of what any given tag *does* -- see the file
// header comment above.
//
// A follow-up question can also be *shared*: authored once, then attached
// as a follow-up under more than one different question (e.g. two
// differently-worded prompts that both ought to lead to the very same next
// question), without retyping or copy-pasting its text/answer/tag/
// follow-ups at each attachment point. Give the one spot that actually
// carries the content an `id=<name>` line, and drop a bare
// `question_ref=<name>` line (no text, no `{ }` body -- just that one
// line) wherever else it should also be reachable from:
//
//   question "Do you have a name?" {
//       id=nameQ
//       tag=RevealName
//       answer=Not one I remember.
//   }
//
//   question "Who are you?" {
//       answer=I don't remember.
//       question_ref=nameQ
//   }
//
//   question "What should I call you?" {
//       answer=Call me whatever you like.
//       question_ref=nameQ
//   }
//
// Asking either "Who are you?" or "What should I call you?" leads to the
// very same "Do you have a name?" follow-up, authored exactly once in the
// file (tools/conversation_editor writes files this way automatically for
// a "merge point" -- see its GraphScene class comment -- rather than
// duplicating that node's whole subtree once per incoming connection).
// `id=` may appear on any question, top-level or nested; `question_ref=`
// may appear anywhere a nested `question { ... }` block could, and doesn't
// have to come after the id= it points to -- order in the file doesn't
// matter. This parser only records that raw id=/question_ref= structure
// (see ConversationQuestion::shared_id/is_reference below) without
// resolving it -- most consumers don't want to deal with reference stubs
// at all, so call resolve_shared_questions() below to get back a plain,
// fully-expanded tree instead (that's what QASystem::load_conversation()
// does; tools/conversation_editor works with the raw form directly, since
// it needs the id/ref structure to reconstruct the merge point rather than
// flatten it away).
//
// A question can also require -- or forbid -- one or more flags being set
// before it's even selectable, and can set flags of its own the instant
// it's asked (the same "reaction shot" timing `tag=` already dispatches
// on), so an earlier choice can gate, unlock, or hide a later question
// instead of every question always being reachable:
//
//   question "Ask about the scar." {
//       sets=AskedAboutScar
//       answer=It's from a long time ago.
//   }
//
//   question "Ask about it again." {
//       requires=AskedAboutScar
//       answer=I already told you.
//   }
//
//   question "Ask about something else." {
//       requires_not=AskedAboutScar
//       answer=Sure, what do you want to know?
//   }
//
// `requires=`/`requires_not=`/`sets=` may each appear more than once on the
// same question (like `answer=`) -- `requires=`/`requires_not=` are AND-only
// (every named flag must be set / every named flag must be unset for the
// question to be selectable at all; there's no OR or nested expression
// syntax), and a flag name is otherwise opaque to this parser, the same way
// `tag=`'s name is -- whatever sets/checks it is content-defined, not
// built in. A flag, once set, stays set for the rest of the conversation
// (there's no `unsets=`).
//
// A question can also be marked as an ending -- a bare `ending` line (no
// `=`, no value) instead of a `key=value` line, optionally paired with one
// or more `ending_text=` lines (same repeatable, one-per-line convention as
// `answer=`) giving that specific ending its own outro text:
//
//   question "Walk away." {
//       requires=AskedAboutScar
//       answer=You leave without another word.
//       ending
//       ending_text=You never come back.
//       ending_text=Some things are better left unsaid.
//   }
//
// Reaching an ending question (see ConversationQuestion::is_ending) means
// something distinct from an ordinary leaf running out of follow-ups --
// this parser only records the marker and ending_lines; a dialogue system
// (see SH's QASystem::update()) decides what actually happens once one is
// reached, e.g. displaying ending_lines on its own outro screen instead of
// a single generic message shared by every ending. `ending_text=` with no
// `ending` marker on the same question is accepted but meaningless (no
// consumer looks at it unless is_ending is also true).
//
// A question further down the chain can also *loop back* to an
// already-authored question instead of leading further down or dead-ending
// -- e.g. a "Let's go back to that" option that returns the player to an
// earlier point in the conversation rather than exhausting the branch. Give
// the target question an `id=<name>` (the very same mechanism `question_ref=`
// above uses to name a question), then add a `loop_to=<name>` line inside
// whichever later question should lead back to it:
//
//   question "Who are you?" {
//       id=whoQ
//       answer=I don't remember.
//
//       question "Do you have a name?" {
//           answer=Not one I remember.
//
//           question "Actually, who are you again?" {
//               answer=Same as before -- I still don't remember.
//               loop_to=whoQ
//           }
//       }
//   }
//
// Unlike `question_ref=`, `loop_to=` does not inline a copy of the target's
// content -- the question carrying it keeps its own text/answer/follow_ups
// exactly as authored, `loop_to` only names where a dialogue system should
// send the player once this question's answer finishes, in place of
// descending into follow_ups or popping back up. This is what lets it point
// backward (at an ancestor, or anywhere else already authored) without the
// infinite-expansion problem a `question_ref=` cycle would hit -- see
// resolve_shared_questions() below, which leaves `loop_to=` stubs alone
// rather than trying to expand them. A game's dialogue system (see SH's
// QASystem::update()) is what actually gives `loop_to=` its jump behavior;
// this parser only records the target id (ConversationQuestion::loop_target
// below).

struct ConversationQuestion {
  std::string text;
  std::vector<std::string> answer_lines;
  // Opaque symbolic name a game's dialogue system can react to -- see the
  // file format comment above. std::nullopt if this question has no
  // `tag=` line.
  std::optional<std::string> tag;
  // Nested sub-questions, revealed (by whichever system is playing this
  // conversation) once this question's own answer finishes -- see the
  // file format comment above for how these nest in the source file.
  std::vector<ConversationQuestion> follow_ups;

  // Non-nullopt if this question was written with an `id=` line (a shared
  // definition, is_reference == false) or is a `question_ref=` stand-in
  // for one (is_reference == true) -- see the file format comment above.
  // Two ConversationQuestions with the same shared_id refer to the same
  // authored content; at most one of them (the definition) actually
  // carries it directly.
  std::optional<std::string> shared_id;
  // True for a `question_ref=<id>` stand-in: text/answer_lines/tag/
  // follow_ups are all left default here rather than copied in -- see
  // resolve_shared_questions() for a consumer that wants those filled in.
  // Always false when shared_id is nullopt.
  bool is_reference = false;

  // Non-nullopt if this question was written with a `loop_to=<id>` line --
  // the id (see shared_id above) of whichever question a dialogue system
  // should jump back to once this question's own answer finishes, instead
  // of descending into follow_ups (normally empty on a looping question --
  // see the file format comment above) or popping back up. Left alone
  // (never expanded/copied) by resolve_shared_questions(), unlike
  // question_ref= stand-ins -- see that function's own comment.
  std::optional<std::string> loop_target;

  // Flag names this question requires to be set (requires_flags) or unset
  // (requires_not_flags) before it's selectable at all -- see the file
  // format comment above. Empty means unconditionally selectable (the
  // common case). AND-only: every named flag must match for the question
  // to be visible.
  std::vector<std::string> requires_flags;
  std::vector<std::string> requires_not_flags;
  // Flag names set (permanently, for the rest of the conversation) the
  // instant this question is asked -- see the file format comment above.
  std::vector<std::string> sets_flags;
  // True if this question was written with a bare `ending` line -- see the
  // file format comment above.
  bool is_ending = false;
  // This ending's own outro text, one element per `ending_text=` line, in
  // file order -- see the file format comment above. Empty unless the
  // question has at least one `ending_text=` line, regardless of is_ending
  // (a consumer should only look at this when is_ending is also true).
  std::vector<std::string> ending_lines;
};

struct Conversation {
  std::vector<ConversationQuestion> questions; // top-level, in file order
};

// Parses path (see the format above). Returns std::nullopt on failure
// (missing file); a malformed individual line is skipped with a logged
// warning rather than failing the whole load, so one typo doesn't lose the
// rest of the conversation.
std::optional<Conversation> load_conversation_file(std::string_view path);

// Walks conversation (top-level questions and, recursively, every nested
// follow-up) and returns a map from each `id=`'d question's shared_id to a
// pointer at it -- only entries with is_reference == false are included,
// since a question_ref= stub carries no content of its own to point at.
// The returned pointers alias conversation, so they're only valid as long
// as conversation itself is still alive. A shared_id claimed by more than
// one definition logs a warning and keeps whichever was found first (file/
// tree order); shared by resolve_shared_questions() below and by
// tools/conversation_editor's GraphScene::load_from_conversation(), which
// both need to look up a question_ref='s actual content.
std::unordered_map<std::string, const ConversationQuestion *>
collect_shared_definitions(const Conversation &conversation);

// Returns a copy of conversation with every `question_ref=` stand-in (see
// ConversationQuestion::is_reference) replaced by a deep copy of the
// question its shared_id points to (itself resolved, recursively, so a
// shared definition that contains further refs still comes out fully
// expanded) -- the plain tree shape most consumers actually want, e.g.
// SH's QASystem, which turns this into a live Entry tree with no idea
// shared questions were ever involved (see qa_system.cpp's to_entry()).
// A question_ref= with no matching id= anywhere in conversation, or a
// reference cycle (a shared definition that, directly or by way of other
// shared ids, ends up containing a question_ref= back to itself), is
// logged as a warning and resolved to an empty leaf question rather than
// recursing forever.
//
// A `loop_to=` question (ConversationQuestion::loop_target) is passed
// through unexpanded -- it isn't a stand-in for content living elsewhere
// (unlike is_reference), it's an ordinary question that additionally names
// where to jump next, so there's nothing to inline and no cycle risk here
// even when the target is one of its own ancestors. Only the *canonical*
// id= definition (wherever it was actually authored, not a question_ref=
// copy of it) keeps its shared_id in the returned tree, so a loop_to= can
// still find exactly one unambiguous target after resolution -- see
// QASystem::load_conversation() for the consumer that relies on this.
Conversation resolve_shared_questions(const Conversation &conversation);
