#pragma once
#include "../defines.h"

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class FileMode : u8 {
  Read = 0x1,
  Write = 0x2,
};

constexpr FileMode operator|(FileMode a, FileMode b) {
  return static_cast<FileMode>(static_cast<u8>(a) | static_cast<u8>(b));
}

constexpr bool has_flag(FileMode mode, FileMode flag) {
  return (static_cast<u8>(mode) & static_cast<u8>(flag)) != 0;
}

// RAII wrapper around a C file handle. Mirrors std::fstream's "constructing
// with a bad path leaves the handle invalid" behaviour rather than throwing,
// since a missing file (e.g. a typo'd shader path) is a routine, recoverable
// condition here, not an exceptional one.
class KAPI FileHandle {
public:
  FileHandle(std::string_view path, FileMode mode, bool binary);
  ~FileHandle();

  FileHandle(const FileHandle &) = delete;
  FileHandle &operator=(const FileHandle &) = delete;
  FileHandle(FileHandle &&other) noexcept;
  FileHandle &operator=(FileHandle &&other) noexcept;

  bool is_valid() const noexcept { return file_ != nullptr; }
  explicit operator bool() const noexcept { return is_valid(); }

  void close();

  // Reads up to a newline or EOF. Returns std::nullopt on failure/EOF.
  std::optional<std::string> read_line();

  // Writes text to the file, appending a newline. Returns true on success.
  bool write_line(std::string_view text);

  // Reads exactly data_size bytes. Returns true only if the full amount was
  // read; out_bytes_read reports how much actually came back either way.
  bool read(void *out_data, u64 data_size, u64 &out_bytes_read);

  // Reads the entire file as binary. Returns std::nullopt on failure.
  std::optional<std::vector<u8>> read_all_bytes();

  // Writes data_size bytes from data. Returns true on success.
  bool write(const void *data, u64 data_size, u64 &out_bytes_written);

private:
  FILE *file_ = nullptr;
};

namespace Filesystem {
// Checks if a file with the given path exists.
KAPI bool exists(std::string_view path);
} // namespace Filesystem
