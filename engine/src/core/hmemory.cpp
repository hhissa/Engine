#include "hmemory.h"
#include "../platform/platform.h"
#include "logger.h"
#include <array>
#include <cstring>
#include <format>
#include <string>
#include <string_view>

namespace {
constexpr std::array<std::string_view, static_cast<size_t>(MemoryTag::MaxTags)>
    TAG_NAMES = {"UNKNOWN    ", "ARRAY      ", "LINEAR_ALLC", "DARRAY     ",
                 "DICT       ", "RING_QUEUE ", "BST        ", "STRING     ",
                 "APPLICATION",
                 "JOB        ", "TEXTURE    ", "MAT_INST   ", "RENDERER   ",
                 "GAME       ", "TRANSFORM  ", "ENTITY     ", "ENTITY_NODE",
                 "SCENE      ", "SHADER     "};

struct MemoryStats {
  u64 total_allocated = 0;
  std::array<u64, static_cast<size_t>(MemoryTag::MaxTags)> tagged_allocations{};
};

MemoryStats stats;
} // namespace

namespace Memory {

void initialize() { stats = {}; }

void shutdown() {
  // future: report leaks here
}

void *allocate(u64 size, MemoryTag tag) {
  if (tag == MemoryTag::Unknown) {
    KWARN("Memory::allocate called with MemoryTag::Unknown. Re-class this "
          "allocation.");
  }
  stats.total_allocated += size;
  stats.tagged_allocations[static_cast<size_t>(tag)] += size;

  // TODO: memory alignment
  void *block = Platform::allocate(size, false);
  zero(block, size);
  return block;
}

void free(void *block, u64 size, MemoryTag tag) {
  if (tag == MemoryTag::Unknown) {
    KWARN("Memory::free called with MemoryTag::Unknown. Re-class this "
          "allocation.");
  }
  stats.total_allocated -= size;
  stats.tagged_allocations[static_cast<size_t>(tag)] -= size;

  // TODO: memory alignment
  Platform::free(block, false);
}

void *zero(void *block, u64 size) { return Platform::zero_memory(block, size); }

void *copy(void *dest, const void *source, u64 size) {
  return Platform::copy_memory(dest, source, size);
}

void *set(void *dest, i32 value, u64 size) {
  return Platform::set_memory(dest, value, size);
}

std::string usage_string() {
  constexpr u64 GiB = 1024ULL * 1024 * 1024;
  constexpr u64 MiB = 1024ULL * 1024;
  constexpr u64 KiB = 1024ULL;

  std::string out = "System memory use (tagged):\n";

  for (size_t i = 0; i < TAG_NAMES.size(); ++i) {
    u64 bytes = stats.tagged_allocations[i];

    f32 amount;
    std::string_view unit;
    if (bytes >= GiB) {
      amount = bytes / static_cast<f32>(GiB);
      unit = "GiB";
    } else if (bytes >= MiB) {
      amount = bytes / static_cast<f32>(MiB);
      unit = "MiB";
    } else if (bytes >= KiB) {
      amount = bytes / static_cast<f32>(KiB);
      unit = "KiB";
    } else {
      amount = static_cast<f32>(bytes);
      unit = "B";
    }

    out += std::format("  {}: {:.2f}{}\n", TAG_NAMES[i], amount, unit);
  }

  return out;
}

} // namespace Memory
