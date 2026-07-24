#include "conversation_io.h"

#include <core/logger.h>

#include <fstream>

namespace {
void write_question(std::ostream &out, const ConversationQuestion &question,
                    int indent) {
  std::string pad(static_cast<size_t>(indent) * 4, ' ');
  out << pad << "question \"" << question.text << "\" {\n";
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
