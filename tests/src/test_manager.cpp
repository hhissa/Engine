#include "test_manager.h"

#include <core/logger.h>
#include <platform/platform.h>

#include <vector>

namespace TestManager {

namespace {

struct Entry {
  TestFn fn;
  std::string description;
};

std::vector<Entry> &tests() {
  static std::vector<Entry> instance;
  return instance;
}

} // namespace

void register_test(TestFn fn, std::string description) {
  tests().push_back({std::move(fn), std::move(description)});
}

void run_tests() {
  u32 passed = 0;
  u32 failed = 0;

  auto &all_tests = tests();
  f64 total_start = Platform::get_absolute_time();

  for (size_t i = 0; i < all_tests.size(); ++i) {
    f64 test_start = Platform::get_absolute_time();
    bool result = all_tests[i].fn();
    f64 test_elapsed = Platform::get_absolute_time() - test_start;

    if (result) {
      ++passed;
    } else {
      KERROR("[FAILED]: {}", all_tests[i].description);
      ++failed;
    }

    f64 total_elapsed = Platform::get_absolute_time() - total_start;
    KINFO("Executed {} of {} {} ({:.6f} sec / {:.6f} sec total)", i + 1,
         all_tests.size(), failed ? "*** FAILED ***" : "SUCCESS",
         test_elapsed, total_elapsed);
  }

  KINFO("Results: {} passed, {} failed.", passed, failed);
}

} // namespace TestManager
