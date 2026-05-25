#pragma once

#include "../defines.h"
#include <string>
#include <string_view>

class KAPI PlatformLayer {
public:
  // implicit copying of the string view
  std::string application_name;
  i32 x;
  i32 y;
  i32 width;
  i32 height;
  PlatformLayer(const std::string_view application_name, i32 x, i32 y,
                i32 width, i32 height);
  ~PlatformLayer();

  b8 platform_pump_messages();

  void *platform_allocate(u64 size, b8 aligned);
  void platform_free(void *block, b8 aligned);
  void *platform_zero_memory(void *block, u64 size);
  void *platform_copy_memory(void *dest, const void *source, u64 size);
  void *platform_set_memory(void *dest, i32 value, u64 size);

  void platform_console_write(const std::string_view message, u8 colour);
  void platform_console_write_error(const std::string_view message, u8 colour);

  f64 platform_get_absolute_time();

  void platform_sleep(u64 ms);

private:
  void *internal_state;
};
