#include "script_line.h"

#include <core/logger.h>

#include <cstdlib>
#include <string>

namespace {

std::string_view trim(std::string_view text) {
  size_t first = text.find_first_not_of(" \t");
  if (first == std::string_view::npos) {
    return {};
  }
  size_t last = text.find_last_not_of(" \t");
  return text.substr(first, last - first + 1);
}

} // namespace

ScriptLine parse_script_line(std::string_view raw) {
  ScriptLine line;
  std::string_view rest = trim(raw);

  while (!rest.empty() && rest.front() == '[') {
    size_t close = rest.find(']');
    if (close == std::string_view::npos) {
      break; // unclosed -- the rest is text, see the header comment
    }
    std::string_view directive = trim(rest.substr(1, close - 1));
    rest = trim(rest.substr(close + 1));

    size_t space = directive.find(' ');
    std::string_view word = directive.substr(0, space);
    std::string_view argument =
        space == std::string_view::npos ? std::string_view{}
                                        : trim(directive.substr(space + 1));

    if (word == "title") {
      line.title = true;
    } else if (word == "overlay") {
      line.overlay = true;
    } else if (word == "cutaway") {
      line.cutaway = std::string(argument);
    } else if (word == "photo") {
      line.photo = std::string(argument);
    } else if (word == "link") {
      line.link = std::string(argument);
    } else if (word == "size") {
      const std::string number(argument);
      char *end = nullptr;
      const f32 size = std::strtof(number.c_str(), &end);
      if (end == number.c_str() || size <= 0.0f) {
        KWARN("Script line '{}': '[{}]' needs a positive number -- ignored.",
              raw, directive);
      } else {
        line.size = size;
      }
    } else if (word == "flash" && argument.empty()) {
      line.flash = true;
    } else if (word == "stack" && argument.empty()) {
      line.stack = true;
    } else if (word == "room" && argument.empty()) {
      line.room = true;
    } else if (word == "fill" && argument.empty()) {
      line.fill = true;
    } else if (word == "center" && argument.empty()) {
      line.center = true;
    } else if (word == "quiet" && argument.empty()) {
      line.quiet = true;
    } else if (word == "invert" && argument.empty()) {
      line.invert = true;
    } else if (word == "at") {
      const std::string numbers(argument);
      char *end_x = nullptr;
      const f32 x = std::strtof(numbers.c_str(), &end_x);
      char *end_y = nullptr;
      const f32 y = std::strtof(end_x, &end_y);
      if (end_x == numbers.c_str() || end_y == end_x) {
        KWARN("Script line '{}': '[{}]' needs two numbers -- ignored.", raw,
              directive);
      } else {
        line.has_at = true;
        line.at_x = x;
        line.at_y = y;
      }
    } else if (argument.empty()) {
      line.speaker = std::string(word);
    } else {
      KWARN("Script line '{}': unknown directive '[{}]' -- ignored.", raw,
            directive);
    }
  }

  line.text = std::string(rest);
  return line;
}
