#include "text_wrap.h"

#include <algorithm>
#include <cctype>

std::vector<std::string> wrap_text(std::string_view text, f32 max_width_px,
                                   f32 avg_char_width_px) {
  size_t max_chars = static_cast<size_t>(std::max(
      1.0f, max_width_px / std::max(avg_char_width_px, 1.0f)));

  std::vector<std::string> lines;
  std::string current;
  size_t pos = 0;
  while (pos < text.size()) {
    while (pos < text.size() &&
          std::isspace(static_cast<unsigned char>(text[pos]))) {
      ++pos;
    }
    size_t word_start = pos;
    while (pos < text.size() &&
          !std::isspace(static_cast<unsigned char>(text[pos]))) {
      ++pos;
    }
    if (word_start == pos) {
      break; // nothing but trailing whitespace left
    }
    std::string_view word = text.substr(word_start, pos - word_start);

    if (current.empty()) {
      current.assign(word);
    } else if (current.size() + 1 + word.size() <= max_chars) {
      current += ' ';
      current += word;
    } else {
      lines.push_back(std::move(current));
      current.assign(word);
    }
  }
  if (!current.empty() || lines.empty()) {
    lines.push_back(std::move(current));
  }
  return lines;
}
