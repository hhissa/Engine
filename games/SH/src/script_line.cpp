#include "script_line.h"

#include <core/logger.h>

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
