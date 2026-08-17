#pragma once
#include "../defines.h"

#include <functional>
#include <glm/glm.hpp>

// Identifies one chunk in the (not yet built -- see chunk_streaming_manager.h,
// a later phase) clipmap voxelization scheme: level L's chunks tile world
// space into cubes of side chunk_world_size(L), indexed by an absolute
// integer coordinate rather than a render-space float. Deliberately
// origin-independent (i64, not tied to any particular floating-origin
// recenter) so a recenter never needs to touch a resident chunk's key --
// only its small render-space placement metadata (see ChunkRecord::
// world_min below) shifts; the key by which it's looked up stays exactly
// what it was. i64 (not i32) headroom is cheap and avoids ever having to
// revisit this if a chunk size/level combination puts a legitimate chunk
// coordinate past i32 range.
struct ChunkKey {
  i32 level = 0;
  i64 cx = 0;
  i64 cy = 0;
  i64 cz = 0;

  friend bool operator==(const ChunkKey &a, const ChunkKey &b) noexcept {
    return a.level == b.level && a.cx == b.cx && a.cy == b.cy && a.cz == b.cz;
  }
};

// World-space side length of one level-`level` chunk -- level 0 is the
// finest, each subsequent level doubles (matches the clipmap convention a
// later phase's LOD scheme uses: constant chunk *count* per level, so
// doubling chunk size is what makes coverage grow exponentially with level
// while chunk count per level stays fixed).
constexpr f32 chunk_world_size(f32 level0_size, i32 level) noexcept {
  f32 size = level0_size;
  for (i32 i = 0; i < level; ++i) {
    size *= 2.0f;
  }
  return size;
}

// Combines level.cx.cy.cz into one hash -- std::unordered_map<ChunkKey, ...>
// needs this explicitly since ChunkKey has no default std::hash
// specialization (it's a plain aggregate, not one of the types the standard
// library knows how to hash). Simple multiply-and-XOR chain, adequate for a
// key space that's never adversarially chosen (game/engine-internal use
// only, not exposed to untrusted input).
struct ChunkKeyHash {
  size_t operator()(const ChunkKey &key) const noexcept {
    size_t h = std::hash<i32>{}(key.level);
    h = h * 1099511628211ull ^ std::hash<i64>{}(key.cx);
    h = h * 1099511628211ull ^ std::hash<i64>{}(key.cy);
    h = h * 1099511628211ull ^ std::hash<i64>{}(key.cz);
    return h;
  }
};

// A resident (or in-flight) chunk's bookkeeping state -- not yet populated/
// consumed by anything (a later phase's ChunkStreamingManager owns the
// actual load/evict lifecycle this describes); exists now purely as the
// shared vocabulary GeometrySystem::chunks_touched_by() and that later
// phase both need to agree on.
enum class ChunkState {
  Empty,    // No record for this key at all (the implicit default -- not
            // normally stored, just what "not in the resident map" means).
  Queued,   // Wants to be resident; not yet baked.
  Baking,   // A (re)voxelize dispatch for this chunk is in flight.
  Ready,    // Baked and safe to sample.
  Evicting, // No longer wanted; its brick(s) not yet safe to reuse (see a
            // later phase's free-list ring-delay).
};

constexpr u32 kInvalidChunkSlot = 0xFFFFFFFFu;

struct ChunkRecord {
  ChunkState state = ChunkState::Empty;
  // Index into whichever fixed-size GPU-side per-level table addresses this
  // chunk's baked content (see a later phase) -- kInvalidChunkSlot until
  // actually assigned one.
  u32 gpu_slot = kInvalidChunkSlot;
  // Bumped every time this chunk is (re)baked -- lets a caller tell a stale
  // in-flight bake result apart from the current one if this chunk gets
  // evicted and reloaded before an earlier bake finishes.
  u64 bake_generation = 0;
  // This chunk's min corner in *render space* (see VulkanRendererBackend::
  // world_origin_offset_) -- unlike ChunkKey (origin-independent), this
  // shifts on every floating-origin recenter, exactly like every other
  // render-space position. Kept here (not re-derived from ChunkKey alone)
  // since a resident chunk's key never changes but this must.
  glm::vec3 world_min{0.0f};
};
