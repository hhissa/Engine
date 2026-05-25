#pragma once
#include "../defines.h"
#include <format>
#include <string_view>

#define LOG_WARN_ENABLED 1
#define LOG_INFO_ENABLED 1
#define LOG_DEBUG_ENABLED 1
#define LOG_TRACE_ENABLED 1
#if KRELEASE == 1
#define LOG_DEBUG_ENABLED 0
#define LOG_TRACE_ENABLED 0
#endif

enum class log_level {
  FATAL = 0,
  ERROR = 1,
  WARN = 2,
  INFO = 3,
  DEBUG = 4,
  TRACE = 5
};

namespace Logger {
b8 initialize_logging();
void shutdown_logging();
KAPI void log_output(log_level level, std::string_view message);
template <typename... Args>
void log_output(log_level level, std::string_view fmt, Args &&...args) {
  log_output(level, std::vformat(fmt, std::make_format_args(args...)));
}
} // namespace Logger

// Logs a fatal-level message.
#define KFATAL(message, ...)                                                   \
  ::Logger::log_output(log_level::FATAL, message __VA_OPT__(, ) __VA_ARGS__)

#ifndef KERROR
// Logs an error-level message.
#define KERROR(message, ...)                                                   \
  ::Logger::log_output(log_level::ERROR, message __VA_OPT__(, ) __VA_ARGS__)
#endif

#if LOG_WARN_ENABLED == 1
#define KWARN(message, ...)                                                    \
  ::Logger::log_output(log_level::WARN, message __VA_OPT__(, ) __VA_ARGS__)
#else
#define KWARN(message, ...) ((void)0)
#endif

#if LOG_INFO_ENABLED == 1
#define KINFO(message, ...)                                                    \
  ::Logger::log_output(log_level::INFO, message __VA_OPT__(, ) __VA_ARGS__)
#else
#define KINFO(message, ...) ((void)0)
#endif

#if LOG_DEBUG_ENABLED == 1
#define KDEBUG(message, ...)                                                   \
  ::Logger::log_output(log_level::DEBUG, message __VA_OPT__(, ) __VA_ARGS__)
#else
#define KDEBUG(message, ...) ((void)0)
#endif

#if LOG_TRACE_ENABLED == 1
#define KTRACE(message, ...)                                                   \
  ::Logger::log_output(log_level::TRACE, message __VA_OPT__(, ) __VA_ARGS__)
#else
#define KTRACE(message, ...) ((void)0)
#endif
