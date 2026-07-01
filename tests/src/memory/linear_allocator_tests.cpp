#include "linear_allocator_tests.h"
#include "../expect.h"
#include "../test_manager.h"

#include <defines.h>
#include <memory/linear_allocator.h>

namespace {

// LinearAllocator is RAII (constructor allocates, destructor frees), so
// unlike upstream's create()/destroy() pair this only checks the
// post-construction invariants; there's no explicit destroy() call to
// re-check fields against afterward.
bool linear_allocator_should_create() {
  LinearAllocator alloc(sizeof(u64));

  EXPECT_NE(nullptr, alloc.memory());
  EXPECT_EQ(sizeof(u64), alloc.total_size());
  EXPECT_EQ(0u, alloc.allocated());

  return true;
}

bool linear_allocator_single_allocation_all_space() {
  LinearAllocator alloc(sizeof(u64));

  void *block = alloc.allocate(sizeof(u64));

  EXPECT_NE(nullptr, block);
  EXPECT_EQ(sizeof(u64), alloc.allocated());

  return true;
}

bool linear_allocator_multi_allocation_all_space() {
  constexpr u64 max_allocs = 1024;
  LinearAllocator alloc(sizeof(u64) * max_allocs);

  void *block = nullptr;
  for (u64 i = 0; i < max_allocs; ++i) {
    block = alloc.allocate(sizeof(u64));
    EXPECT_NE(nullptr, block);
    EXPECT_EQ(sizeof(u64) * (i + 1), alloc.allocated());
  }

  return true;
}

bool linear_allocator_multi_allocation_over_allocate() {
  constexpr u64 max_allocs = 3;
  LinearAllocator alloc(sizeof(u64) * max_allocs);

  void *block = nullptr;
  for (u64 i = 0; i < max_allocs; ++i) {
    block = alloc.allocate(sizeof(u64));
    EXPECT_NE(nullptr, block);
    EXPECT_EQ(sizeof(u64) * (i + 1), alloc.allocated());
  }

  KDEBUG("Note: The following error is intentionally caused by this test.");

  // Ask for one more allocation. Should fail and return nullptr.
  block = alloc.allocate(sizeof(u64));
  EXPECT_EQ(nullptr, block);
  EXPECT_EQ(sizeof(u64) * max_allocs, alloc.allocated());

  return true;
}

bool linear_allocator_multi_allocation_all_space_then_free() {
  constexpr u64 max_allocs = 1024;
  LinearAllocator alloc(sizeof(u64) * max_allocs);

  void *block = nullptr;
  for (u64 i = 0; i < max_allocs; ++i) {
    block = alloc.allocate(sizeof(u64));
    EXPECT_NE(nullptr, block);
    EXPECT_EQ(sizeof(u64) * (i + 1), alloc.allocated());
  }

  // Validate that the offset is reset.
  alloc.free_all();
  EXPECT_EQ(0u, alloc.allocated());

  return true;
}

} // namespace

void register_linear_allocator_tests() {
  TestManager::register_test(linear_allocator_should_create,
                             "Linear allocator should create with expected "
                             "initial state");
  TestManager::register_test(linear_allocator_single_allocation_all_space,
                             "Linear allocator single alloc for all space");
  TestManager::register_test(linear_allocator_multi_allocation_all_space,
                             "Linear allocator multi alloc for all space");
  TestManager::register_test(linear_allocator_multi_allocation_over_allocate,
                             "Linear allocator try over allocate");
  TestManager::register_test(
      linear_allocator_multi_allocation_all_space_then_free,
      "Linear allocator allocated should be 0 after free_all");
}
