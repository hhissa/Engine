#pragma once
#include "../defines.h"

// A simple bump allocator: allocate() hands out sequential chunks of a
// fixed-size block, and free_all() rewinds the offset to zero without
// touching individual allocations. Useful for scoped/arena-style lifetimes
// (e.g. a frame's worth of scratch allocations) where per-object free() would
// be wasted bookkeeping.
class KAPI LinearAllocator {
public:
  // If memory is nullptr, the allocator allocates (and owns) its own
  // MemoryTag::LinearAllocator-tagged block of total_size bytes. If memory
  // is provided, the allocator writes into it but does not own/free it.
  explicit LinearAllocator(u64 total_size, void *memory = nullptr);
  ~LinearAllocator();

  LinearAllocator(const LinearAllocator &) = delete;
  LinearAllocator &operator=(const LinearAllocator &) = delete;
  LinearAllocator(LinearAllocator &&other) noexcept;
  LinearAllocator &operator=(LinearAllocator &&other) noexcept;

  // Returns a pointer to a size-byte block, or nullptr if the allocator is
  // out of space.
  void *allocate(u64 size);

  // Rewinds the allocator back to empty and zeroes its memory. Existing
  // pointers previously returned by allocate() are invalidated.
  void free_all();

  u64 total_size() const noexcept { return total_size_; }
  u64 allocated() const noexcept { return allocated_; }
  void *memory() const noexcept { return memory_; }

private:
  u64 total_size_ = 0;
  u64 allocated_ = 0;
  void *memory_ = nullptr;
  bool owns_memory_ = false;
};
