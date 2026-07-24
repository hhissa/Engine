#pragma once
#include "../defines.h"

#include <optional>
#include <string>
#include <string_view>
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
// order) and, optionally, further nested `question` blocks -- each of
// those becomes a follow-up of its enclosing question, to any depth:
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

struct ConversationQuestion {
  std::string text;
  std::vector<std::string> answer_lines;
  // Nested sub-questions, revealed (by whichever system is playing this
  // conversation) once this question's own answer finishes -- see the
  // file format comment above for how these nest in the source file.
  std::vector<ConversationQuestion> follow_ups;
};

struct Conversation {
  std::vector<ConversationQuestion> questions; // top-level, in file order
};

// Parses path (see the format above). Returns std::nullopt on failure
// (missing file); a malformed individual line is skipped with a logged
// warning rather than failing the whole load, so one typo doesn't lose the
// rest of the conversation.
std::optional<Conversation> load_conversation_file(std::string_view path);
