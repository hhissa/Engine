#include "logger.h"
#include "asserts.h"
#include "../platform/filesystem.h"
#include <array>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string_view>

namespace Logger {

namespace {
// Deferred (rather than a plain FileHandle member) since FileHandle has no
// default constructor — the log file path is only known once
// initialize_logging() runs, not at static-init time.
std::optional<FileHandle> log_file;

void append_to_log_file(std::string_view message) {
  if (log_file && log_file->is_valid()) {
    // message already ends in '\n', so write the bytes directly.
    u64 written = 0;
    if (!log_file->write(message.data(), message.size(), written)) {
      std::cerr << "ERROR writing to console.log.\n";
    }
  }
}
} // namespace

b8 initialize_logging() {
  // Create new/wipe existing log file, then open it.
  log_file.emplace("console.log", FileMode::Write, false);
  if (!log_file->is_valid()) {
    std::cerr << "ERROR: Unable to open console.log for writing.\n";
    return false;
  }
  return true;
};
void shutdown_logging() { log_file.reset(); };

void log_output(log_level level, std::string_view message) {
  constexpr std::array<std::string_view, 6> level_strings = {
      "[FATAL]: ", "[ERROR]: ", "[WARN]:  ",
      "[INFO]:  ", "[DEBUG]: ", "[TRACE]: "};

  // b8 is_error = level <= log_level::ERROR;
  //  TODO: platfrom specific printing
  std::string out_message = std::format(
      "{}{}\n", level_strings[static_cast<size_t>(level)], message);

  std::cout << out_message;

  // Queue a copy to be written to the log file.
  append_to_log_file(out_message);
};
} // namespace Logger

namespace Asserts {
void report_assertion_failure(const char *expression, const char *message,
                              const char *file, i32 line) {
  Logger::log_output(
      log_level::ERROR,
      "Assertion Failure: {}, message: '{}', in file: {}, line: {}", expression,
      message, file, line);
}
} // namespace Asserts
