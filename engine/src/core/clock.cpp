#include "clock.h"

#include "../platform/platform.h"

void Clock::update() {
  if (start_time != 0) {
    elapsed = Platform::get_absolute_time();
  }
}

void Clock::start() {
  start_time = Platform::get_absolute_time();
  elapsed = 0;
}

void Clock::stop() { start_time = 0; }
