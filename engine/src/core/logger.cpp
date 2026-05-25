#include "logger.h"
#include "asserts.h"
#include <cstdio>
#include <iostream>
#include <string_view>
namespace Logger {
b8 initialize_logging() {
  // TODO: create log file
  return true;
};
void shutdown_logging() {
  // TODO: clean up logging queued entries
};

void log_output(log_level level, std::string_view message) {
  constexpr std::array<std::string_view, 6> level_strings = {
      "[FATAL]: ", "[ERROR]: ", "[WARN]:  ",
      "[INFO]:  ", "[DEBUG]: ", "[TRACE]: "};

  // b8 is_error = level <= log_level::ERROR;
  //  TODO: platfrom specific printing
  std::cout << level_strings[static_cast<size_t>(level)] << message << '\n';
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
