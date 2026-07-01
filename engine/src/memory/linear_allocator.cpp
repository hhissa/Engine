#include "linear_allocator.h"
#include "../core/hmemory.h"
#include "../core/logger.h"

LinearAllocator::LinearAllocator(u64 total_size, void *memory)
    : total_size_(total_size), owns_memory_(memory == nullptr) {
  memory_ = memory ? memory
                   : Memory::allocate(total_size_, MemoryTag::LinearAllocator);
}

LinearAllocator::~LinearAllocator() {
  if (owns_memory_ && memory_) {
    Memory::free(memory_, total_size_, MemoryTag::LinearAllocator);
  }
  memory_ = nullptr;
  total_size_ = 0;
  allocated_ = 0;
}

LinearAllocator::LinearAllocator(LinearAllocator &&other) noexcept
    : total_size_(other.total_size_), allocated_(other.allocated_),
      memory_(other.memory_), owns_memory_(other.owns_memory_) {
  other.memory_ = nullptr;
  other.total_size_ = 0;
  other.allocated_ = 0;
  other.owns_memory_ = false;
}

LinearAllocator &LinearAllocator::operator=(LinearAllocator &&other) noexcept {
  if (this != &other) {
    if (owns_memory_ && memory_) {
      Memory::free(memory_, total_size_, MemoryTag::LinearAllocator);
    }
    total_size_ = other.total_size_;
    allocated_ = other.allocated_;
    memory_ = other.memory_;
    owns_memory_ = other.owns_memory_;

    other.memory_ = nullptr;
    other.total_size_ = 0;
    other.allocated_ = 0;
    other.owns_memory_ = false;
  }
  return *this;
}

void *LinearAllocator::allocate(u64 size) {
  if (!memory_) {
    KERROR("LinearAllocator::allocate - allocator not initialized.");
    return nullptr;
  }

  if (allocated_ + size > total_size_) {
    u64 remaining = total_size_ - allocated_;
    KERROR("LinearAllocator::allocate - tried to allocate {}B, only {}B "
          "remaining.",
          size, remaining);
    return nullptr;
  }

  void *block = static_cast<u8 *>(memory_) + allocated_;
  allocated_ += size;
  return block;
}

void LinearAllocator::free_all() {
  if (memory_) {
    allocated_ = 0;
    Memory::zero(memory_, total_size_);
  }
}
