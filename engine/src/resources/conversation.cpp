#include "conversation.h"
#include "../core/logger.h"

#include <fstream>
#include <unordered_set>

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
    if (eq != std::string::npos && trimmed.compare(0, eq, "tag") == 0) {
      question.tag = trim(trimmed.substr(eq + 1));
      continue;
    }
    // A shared question's own definition -- see the file format comment's
    // "shared" section. Doesn't stop this question from also being a
    // question_ref= target itself elsewhere in the file.
    if (eq != std::string::npos && trimmed.compare(0, eq, "id") == 0) {
      question.shared_id = trim(trimmed.substr(eq + 1));
      continue;
    }
    // A bare stand-in for a question defined (via id=) elsewhere -- no
    // text, no `{ }` body of its own, just this one line where a nested
    // `question { ... }` block could otherwise be. Left as an unresolved
    // stub here (empty text/answer_lines/follow_ups) -- see
    // resolve_shared_questions() for turning this into the real content.
    if (eq != std::string::npos && trimmed.compare(0, eq, "question_ref") == 0) {
      ConversationQuestion &ref = question.follow_ups.emplace_back();
      ref.shared_id = trim(trimmed.substr(eq + 1));
      ref.is_reference = true;
      continue;
    }
    // Sends a dialogue system back to an already-authored question (named
    // by its id=) once this question's own answer finishes -- see the file
    // format comment's "loop back" section. Unlike question_ref=, this
    // stays on `question` itself rather than creating a follow-up stub.
    if (eq != std::string::npos && trimmed.compare(0, eq, "loop_to") == 0) {
      question.loop_target = trim(trimmed.substr(eq + 1));
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

namespace {
void collect_shared_definitions_recursive(
    const ConversationQuestion &question,
    std::unordered_map<std::string, const ConversationQuestion *> &out) {
  if (question.shared_id && !question.is_reference) {
    auto [it, inserted] = out.emplace(*question.shared_id, &question);
    if (!inserted) {
      KWARN("Duplicate id='{}' -- keeping the first definition found.",
           *question.shared_id);
    }
  }
  for (const ConversationQuestion &child : question.follow_ups) {
    collect_shared_definitions_recursive(child, out);
  }
}
} // namespace

std::unordered_map<std::string, const ConversationQuestion *>
collect_shared_definitions(const Conversation &conversation) {
  std::unordered_map<std::string, const ConversationQuestion *> definitions;
  for (const ConversationQuestion &question : conversation.questions) {
    collect_shared_definitions_recursive(question, definitions);
  }
  return definitions;
}

namespace {
// in_progress tracks shared_ids currently being expanded along the current
// path, so a reference cycle (a shared definition that, directly or
// transitively, contains a question_ref= back to itself) is caught instead
// of recursing forever.
//
// preserve_shared_id is false exactly when this call is expanding a
// question_ref= stand-in's target -- the resulting copy deliberately does
// NOT keep that target's shared_id, so an id keeps exactly one canonical
// location in the resolved tree (the definition's own natural position,
// walked via the plain follow_ups recursion below, where preserve_shared_id
// is true) no matter how many question_ref= stubs also expand to copies of
// it. loop_to= (see ConversationQuestion::loop_target) relies on that
// uniqueness to have exactly one place to jump to.
ConversationQuestion resolve_question(
    const ConversationQuestion &question,
    const std::unordered_map<std::string, const ConversationQuestion *> &definitions,
    std::unordered_set<std::string> &in_progress, bool preserve_shared_id) {
  if (question.is_reference) {
    const std::string &id = question.shared_id.value();
    auto it = definitions.find(id);
    if (it == definitions.end()) {
      KWARN("question_ref='{}' does not match any id= definition.", id);
      return ConversationQuestion{};
    }
    if (!in_progress.insert(id).second) {
      KWARN("Circular question_ref chain involving id '{}'.", id);
      return ConversationQuestion{};
    }
    ConversationQuestion resolved =
        resolve_question(*it->second, definitions, in_progress,
                         /*preserve_shared_id=*/false);
    in_progress.erase(id);
    return resolved;
  }

  ConversationQuestion resolved;
  resolved.text = question.text;
  resolved.answer_lines = question.answer_lines;
  resolved.tag = question.tag;
  resolved.loop_target = question.loop_target;
  if (preserve_shared_id) {
    resolved.shared_id = question.shared_id;
  }
  resolved.follow_ups.reserve(question.follow_ups.size());
  for (const ConversationQuestion &child : question.follow_ups) {
    resolved.follow_ups.push_back(
        resolve_question(child, definitions, in_progress, /*preserve_shared_id=*/true));
  }
  return resolved;
}
} // namespace

Conversation resolve_shared_questions(const Conversation &conversation) {
  std::unordered_map<std::string, const ConversationQuestion *> definitions =
      collect_shared_definitions(conversation);

  Conversation resolved;
  std::unordered_set<std::string> in_progress;
  resolved.questions.reserve(conversation.questions.size());
  for (const ConversationQuestion &question : conversation.questions) {
    resolved.questions.push_back(
        resolve_question(question, definitions, in_progress, /*preserve_shared_id=*/true));
  }
  return resolved;
}
