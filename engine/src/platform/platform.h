#pragma once
#include "../defines.h"
#include <string>
#include <string_view>
#include <vector>

struct VulkanContext;
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

// The per-user directory for regenerable caches: $XDG_CACHE_HOME (else
// ~/.cache) on Linux, %LOCALAPPDATA% on Windows. Empty if none can be
// found -- callers should then run without an on-disk cache.
KAPI std::string user_cache_directory();
// setenv() on Linux, _putenv_s() on Windows. Lives in the engine so the
// write lands in the same C runtime the engine's getenv() calls read from.
KAPI void set_environment_variable(std::string_view name,
                                   std::string_view value, b8 overwrite);
// chdir()s to the directory holding the running executable. Every asset path
// is relative ("assets/..."), and so are the save/settings files, so without
// this a game launched from anywhere but its own folder (a desktop shortcut,
// a terminal elsewhere, a launcher) finds nothing. False if it couldn't.
KAPI b8 set_working_directory_to_executable();
} // namespace Platform

// An already-existing native window handle to render into, rather than one
// PlatformLayer creates and owns itself -- for embedding the renderer in a
// window owned by something else (e.g. a Qt QWindow). Opaque void*/u64
// rather than the real XCB types, so this header stays platform-agnostic
// (it's included transitively wherever PlatformLayer is, on every
// platform). On Linux: connection is an xcb_connection_t*, window is an
// xcb_window_t.
struct BorrowedWindowHandle {
  void *connection;
  u64 window;
};

// Windowing — needs state, so it's a class.
class KAPI PlatformLayer {
public:
  PlatformLayer(std::string_view application_name, i32 x, i32 y, i32 width,
                i32 height);
  // Wraps an already-existing window (see BorrowedWindowHandle) instead of
  // creating/owning one -- the destructor will not destroy it, and
  // platform_pump_messages() is a no-op (whatever owns the window's event
  // loop, e.g. Qt, is assumed to be pumping it already; polling the same
  // connection from two places races on the same socket).
  PlatformLayer(BorrowedWindowHandle handle, i32 width, i32 height);
  ~PlatformLayer();

  PlatformLayer(const PlatformLayer &) = delete;
  PlatformLayer &operator=(const PlatformLayer &) = delete;

  b8 platform_pump_messages();
  void get_required_extension_names(std::vector<const char *> &names) const;
  b8 create_vulkan_surface(VulkanContext &context);

private:
  std::string application_name;
  i32 x;
  i32 y;
  i32 width;
  i32 height;
  void *internal_state;
};
