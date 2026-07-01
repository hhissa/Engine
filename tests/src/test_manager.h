#pragma once
#include <functional>
#include <string>

using TestFn = std::function<bool()>;

namespace TestManager {

void register_test(TestFn fn, std::string description);
void run_tests();

} // namespace TestManager
