#pragma once
#include "../defines.h"
#include <string>

enum class MemoryTag : u8 {
  Unknown,
  Array,
  DArray,
  Dict,
  RingQueue,
  BST,
  String,
  Application,
  Job,
  Texture,
  MaterialInstance,
  Renderer,
  Game,
  Transform,
  Entity,
  EntityNode,
  Scene,
  MaxTags
};

namespace Memory {
KAPI void initialize();
KAPI void shutdown();

KAPI void *allocate(u64 size, MemoryTag tag);
KAPI void free(void *block, u64 size, MemoryTag tag);

KAPI void *zero(void *block, u64 size);
KAPI void *copy(void *dest, const void *source, u64 size);
KAPI void *set(void *dest, i32 value, u64 size);

KAPI std::string usage_string();

template <typename T, typename... Args>
T *create(MemoryTag tag, Args &&...args) {
  void *mem = allocate(sizeof(T), tag);
  return new (mem) T(std::forward<Args>(args)...);
}

template <typename T> void destroy(T *obj, MemoryTag tag) {
  if (!obj)
    return;
  obj->~T();
  free(obj, sizeof(T), tag);
}
} // namespace Memory
