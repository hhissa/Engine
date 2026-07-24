#include "conversation.h"
#include "../core/logger.h"

#include <fstream>

namespace {

std::string trim(const std::string &s) {
  constexpr const char *kWhitespace = " \t\r\n";
  auto start = s.find_first_not_of(kWhitespace);
  if (start == std::string::npos) {
    return "";
  }
  auto end = s.find_last_not_of(kWhitespace);
  return s.substr(start, end - start + 1);
}

// Extracts the text between the first and last '"' on a header line like
// `question "Who are you?" {`. Returns false (line unusable) if it doesn't
// contain two distinct quote characters.
bool parse_quoted(const std::string &line, std::string &out) {
  auto first = line.find('"');
  if (first == std::string::npos) {
    return false;
  }
  auto last = line.rfind('"');
  if (last == std::string::npos || last <= first) {
    return false;
  }
  out = line.substr(first + 1, last - first - 1);
  return true;
}

// True if trimmed is a block header ("question "..." {") -- mirrors
// sdf_scene.cpp's own "keyword name {" block-header convention, except the
// "name" here is double-quoted free-form prose (a question) rather than a
// bare identifier.
bool is_question_header(const std::string &trimmed) {
  if (trimmed.empty() || trimmed.back() != '{') {
    return false;
  }
  std::string header = trim(trimmed.substr(0, trimmed.size() - 1));
  auto space = header.find(' ');
  std::string keyword = space == std::string::npos ? header : header.substr(0, space);
  return keyword == "question";
}

// Parses one question's body -- "answer=" lines and nested "question ..."
// blocks -- starting at lines[pos], up to and including this block's
// closing "}" (or eof, logged as a warning for an unclosed block). pos is
// advanced past everything consumed, for the caller (or a recursive nested
// call) to resume from.
void parse_question_body(const std::vector<std::string> &lines, size_t &pos,
                         ConversationQuestion &question, std::string_view path) {
  while (pos < lines.size()) {
    size_t line_number = pos + 1;
    std::string trimmed = trim(lines[pos]);
    ++pos;

    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }
    if (trimmed == "}") {
      return; // closes this question's own block
    }
    if (is_question_header(trimmed)) {
      std::string question_text;
      if (!parse_quoted(trimmed, question_text)) {
        KWARN("'{}': malformed question header at line {}: '{}'.", path,
             line_number, trimmed);
        continue;
      }
      ConversationQuestion &child = question.follow_ups.emplace_back();
      child.text = std::move(question_text);
      parse_question_body(lines, pos, child, path);
      continue;
    }

    auto eq = trimmed.find('=');
    if (eq != std::string::npos && trimmed.compare(0, eq, "answer") == 0) {
      question.answer_lines.push_back(trim(trimmed.substr(eq + 1)));
      continue;
    }

    KWARN("'{}': unexpected line at {}: '{}'.", path, line_number, trimmed);
  }

  KWARN("'{}': file ended with an unclosed question block ('{}').", path,
       question.text);
}

} // namespace

std::optional<Conversation> load_conversation_file(std::string_view path) {
  std::ifstream file{std::string(path)};
  if (!file.is_open()) {
    KERROR("Failed to open conversation file: '{}'.", path);
    return std::nullopt;
  }

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(file, line)) {
    lines.push_back(line);
  }

  Conversation conversation;
  size_t pos = 0;
  while (pos < lines.size()) {
    size_t line_number = pos + 1;
    std::string trimmed = trim(lines[pos]);
    ++pos;

    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }
    if (trimmed.compare(0, 8, "version=") == 0) {
      continue; // accepted, not currently read -- see the file header comment
    }
    if (is_question_header(trimmed)) {
      std::string question_text;
      if (!parse_quoted(trimmed, question_text)) {
        KWARN("'{}': malformed question header at line {}: '{}'.", path,
             line_number, trimmed);
        continue;
      }
      ConversationQuestion question;
      question.text = std::move(question_text);
      parse_question_body(lines, pos, question, path);
      conversation.questions.push_back(std::move(question));
      continue;
    }

    KWARN("'{}': unexpected top-level line at {}: '{}'.", path, line_number,
         trimmed);
  }

  return conversation;
}
