#include "conversation_io.h"

#include <core/logger.h>

#include <fstream>

namespace {
void write_question(std::ostream &out, const ConversationQuestion &question,
                    int indent) {
  std::string pad(static_cast<size_t>(indent) * 4, ' ');
  if (question.is_reference) {
    // A bare stand-in for a question written in full elsewhere in this
    // file (see resources/conversation.h's `question_ref=` -- no text, no
    // `{ }` body of its own) -- just this one line where a nested
    // `question { ... }` block could otherwise be.
    out << pad << "question_ref=" << question.shared_id.value_or(std::string()) << "\n";
    return;
  }
  out << pad << "question \"" << question.text << "\" {\n";
  if (question.shared_id) {
    out << pad << "    id=" << *question.shared_id << "\n";
  }
  if (question.tag) {
    out << pad << "    tag=" << *question.tag << "\n";
  }
  if (question.loop_target) {
    // See resources/conversation.h's `loop_to=` -- jumps a dialogue system
    // back to the id='d question named here once this one's own answer
    // finishes, instead of writing out follow_ups (a looping question
    // isn't expected to have any -- see graph_scene.cpp's to_question()).
    out << pad << "    loop_to=" << *question.loop_target << "\n";
  }
  for (const std::string &answer : question.answer_lines) {
    out << pad << "    answer=" << answer << "\n";
  }
  for (const ConversationQuestion &child : question.follow_ups) {
    out << "\n";
    write_question(out, child, indent + 1);
  }
  out << pad << "}\n";
}
} // namespace

bool save_conversation(std::string_view path, const Conversation &conversation) {
  std::ofstream file{std::string(path)};
  if (!file.is_open()) {
    KERROR("Failed to open conversation file for writing: '{}'.", path);
    return false;
  }

  file << "#conversation file\n";
  file << "version=0.1\n";

  for (const ConversationQuestion &question : conversation.questions) {
    file << "\n";
    write_question(file, question, 0);
  }

  return true;
}
