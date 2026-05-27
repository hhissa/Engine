#pragma once
#include "../defines.h"
#include <string>
#include <string_view>

// OS-level utility functions — don't need a PlatformLayer instance.
namespace Platform {
KAPI void *allocate(u64 size, b8 aligned);
KAPI void free(void *block, b8 aligned);
KAPI void *zero_memory(void *block, u64 size);
KAPI void *copy_memory(void *dest, const void *source, u64 size);
KAPI void *set_memory(void *dest, i32 value, u64 size);

KAPI void console_write(std::string_view message, u8 colour);
KAPI void console_write_error(std::string_view message, u8 colour);

KAPI f64 get_absolute_time();
KAPI void sleep(u64 ms);
} // namespace Platform

// Windowing — needs state, so it's a class.
class KAPI PlatformLayer {
public:
  PlatformLayer(std::string_view application_name, i32 x, i32 y, i32 width,
                i32 height);
  ~PlatformLayer();

  PlatformLayer(const PlatformLayer &) = delete;
  PlatformLayer &operator=(const PlatformLayer &) = delete;

  b8 platform_pump_messages();

private:
  std::string application_name;
  i32 x;
  i32 y;
  i32 width;
  i32 height;
  void *internal_state;
};
