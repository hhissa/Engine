#include "test_manager.h"

#include "memory/linear_allocator_tests.h"

#include <core/hmemory.h>
#include <core/logger.h>

int main() {
  Memory::initialize();
  Logger::initialize_logging();

  register_linear_allocator_tests();

  KDEBUG("Starting tests...");

  TestManager::run_tests();

  Memory::shutdown();
  return 0;
}
