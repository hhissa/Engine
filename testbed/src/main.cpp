#include <core/asserts.h>
#include <core/logger.h>
#include <platform/platform.h>

int main(void) {
  auto platform =
      new PlatformLayer("Hassan's Platform Layer", 100, 100, 1280, 720);

  while (platform->platform_pump_messages()) {
    // game loop body
  }
  KINFO("Shutting down.");
  delete platform;
  return 0;
}
