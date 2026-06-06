#pragma once
#include "../defines.h"
class Clock {
public:
  f64 start_time;
  f64 elapsed;

  void update();
  void start();
  void stop();
};
