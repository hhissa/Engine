#include "filesystem.h"
#include "../core/logger.h"

#include <sys/stat.h>

namespace Filesystem {

bool exists(std::string_view path) {
  struct stat buffer;
  return stat(std::string(path).c_str(), &buffer) == 0;
}

} // namespace Filesystem

namespace {

const char *mode_string(FileMode mode, bool binary) {
  bool read = has_flag(mode, FileMode::Read);
  bool write = has_flag(mode, FileMode::Write);
  if (read && write) {
    return binary ? "w+b" : "w+";
  }
  if (read) {
    return binary ? "rb" : "r";
  }
  if (write) {
    return binary ? "wb" : "w";
  }
  return nullptr;
}

} // namespace

FileHandle::FileHandle(std::string_view path, FileMode mode, bool binary) {
  const char *mode_str = mode_string(mode, binary);
  if (!mode_str) {
    KERROR("Invalid mode passed while trying to open file: '{}'", path);
    return;
  }

  file_ = std::fopen(std::string(path).c_str(), mode_str);
  if (!file_) {
    KERROR("Error opening file: '{}'", path);
  }
}

FileHandle::~FileHandle() { close(); }

FileHandle::FileHandle(FileHandle &&other) noexcept : file_(other.file_) {
  other.file_ = nullptr;
}

FileHandle &FileHandle::operator=(FileHandle &&other) noexcept {
  if (this != &other) {
    close();
    file_ = other.file_;
    other.file_ = nullptr;
  }
  return *this;
}

void FileHandle::close() {
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

std::optional<std::string> FileHandle::read_line() {
  if (!file_) {
    return std::nullopt;
  }

  // Long enough for any reasonable text line (shader source, config, etc).
  char buffer[32000];
  if (!std::fgets(buffer, sizeof(buffer), file_)) {
    return std::nullopt;
  }
  return std::string(buffer);
}

bool FileHandle::write_line(std::string_view text) {
  if (!file_) {
    return false;
  }

  std::string line(text);
  line.push_back('\n');

  bool ok = std::fwrite(line.data(), 1, line.size(), file_) == line.size();

  // Flush immediately so a crash right after this call doesn't lose data.
  std::fflush(file_);
  return ok;
}

bool FileHandle::read(void *out_data, u64 data_size, u64 &out_bytes_read) {
  if (!file_ || !out_data) {
    return false;
  }

  out_bytes_read = std::fread(out_data, 1, data_size, file_);
  return out_bytes_read == data_size;
}

std::optional<std::vector<u8>> FileHandle::read_all_bytes() {
  if (!file_) {
    return std::nullopt;
  }

  std::fseek(file_, 0, SEEK_END);
  long size = std::ftell(file_);
  std::rewind(file_);
  if (size < 0) {
    return std::nullopt;
  }

  std::vector<u8> bytes(static_cast<size_t>(size));
  u64 bytes_read = std::fread(bytes.data(), 1, bytes.size(), file_);
  if (bytes_read != bytes.size()) {
    return std::nullopt;
  }
  return bytes;
}

bool FileHandle::write(const void *data, u64 data_size,
                       u64 &out_bytes_written) {
  if (!file_) {
    return false;
  }

  out_bytes_written = std::fwrite(data, 1, data_size, file_);
  std::fflush(file_);
  return out_bytes_written == data_size;
}
