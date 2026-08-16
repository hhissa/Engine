#pragma once
#include "chunk_types.h"

#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

// Camera-driven load/evict scheduler for the chunked field (see ChunkKey/
// ChunkRecord above, and VulkanRaymarchShader::voxelize_chunk()/
// evict_chunk() -- the GPU-side operations this class decides WHEN to call,
// without touching any GPU/Vulkan object itself. Pure CPU bookkeeping, on
// purpose: keeping this free of any Vulkan dependency is what makes the
// actual scheduling logic (which chunks should be resident, in what order,
// how slot reuse is delayed) unit-testable without standing up a real GPU
// context -- see tests/src/systems/chunk_streaming_manager_tests.cpp.
//
// Usage, once per frame (see VulkanRaymarchShader::update_streaming(), the
// real caller):
//   1. tick() -- ages any pending slot releases and advances the frame
//      generation counter. Call this even on a frame that does nothing else.
//   2. update(camera_pos, chunk_world_size) -- returns which chunks SHOULD
//      be resident right now that aren't (to_load, nearest-first) and which
//      ARE resident but shouldn't be anymore (to_evict).
//   3. For up to the caller's own per-frame budget of to_load: acquire_free_slot()
//      (skip this chunk this frame if it returns kInvalidChunkSlot -- no
//      free slots, try again next frame), dispatch the actual voxelize_chunk()
//      GPU call, then commit_load() to record it as resident.
//   4. For up to the caller's own per-frame budget of to_evict: dispatch the
//      actual evict_chunk() GPU call, then commit_evict() to begin that
//      slot's ring-delay before it's reusable.
//
// Deliberately does NOT decide "how many loads/evicts per frame" itself
// (see kMaxChunkBakesPerFrame, engine-side) -- update()'s lists can be
// longer than any one frame should actually act on; the caller enforces its
// own budget by only consuming a prefix of each list per frame (any
// unconsumed remainder is simply recomputed, and reprioritized by distance,
// on the next update() call -- no separate "carry-over queue" needed).
class ChunkStreamingManager {
public:
  // max_resident_chunks must match the GPU-side chunk table/indirection
  // buffer's actual slot capacity (kMaxResidentChunks engine-side) -- this
  // class enforces that cap via acquire_free_slot() returning
  // kInvalidChunkSlot once exhausted, but has no way to detect a mismatch
  // against the real GPU capacity itself.
  //
  // stream_radius_chunks is the Chebyshev (box, not sphere) radius around
  // the camera's own chunk that update() considers "wanted", in chunks --
  // a (2*radius+1)^3 candidate volume, which must stay comfortably under
  // max_resident_chunks or every frame will be evicting chunks it only just
  // loaded (thrashing at the boundary). Not validated here (a pure
  // scheduling policy, not a hard constraint this class can enforce) --
  // see VulkanRaymarchShader's constants for the actual chosen values and
  // that comment's sizing rationale.
  //
  // to_load's window uses this radius exactly, but to_evict deliberately
  // doesn't -- see kEvictHysteresisChunks below.
  //
  // frames_in_flight_delay is how many tick() calls must pass after an
  // eviction (or a load) before that slot's ring-delay is considered safe
  // -- see acquire_free_slot()/is_ready()'s comments. Should exceed the
  // real swapchain's max_frames_in_flight (see VulkanRaymarchShader's own
  // constant for why a fixed conservative value is used instead of wiring
  // through the actual runtime swapchain count).
  //
  // slot_offset shifts every slot this instance ever hands out by a fixed
  // amount -- for a caller (Phase 4's per-level VulkanRaymarchShader::
  // chunk_streaming_levels_) running one instance per clip level against a
  // single shared GPU indirection buffer partitioned into per-level
  // ranges: level L's instance passes L*max_resident_chunks here so its
  // slots [offset, offset+max_resident_chunks) never collide with any
  // other level's. Defaults to 0 (the single-level Phase 3b case, and
  // every existing test) -- purely additive, changes no behavior for a
  // caller that never passes it.
  ChunkStreamingManager(u32 max_resident_chunks, i32 stream_radius_chunks,
                        u32 frames_in_flight_delay, u32 slot_offset = 0);

  struct Plan {
    std::vector<ChunkKey> to_load;  // nearest-to-camera first
    std::vector<ChunkKey> to_evict; // unspecified order (see class comment)
  };

  // Decides what should be resident given the camera's current position (in
  // the same render-space chunk coordinates are computed in -- see
  // ChunkKey's own comment) and the level-0 chunk size in world units (see
  // chunk_world_size(), chunk_types.h -- passed in rather than assumed, so
  // this class stays level-agnostic ready for a future multi-level caller).
  // Chunks already Queued/Baking are excluded from to_load (already in
  // progress); chunks already Evicting are excluded from to_evict (already
  // in progress) -- calling update() repeatedly without ever consuming its
  // output is always safe and idempotent.
  Plan update(glm::vec3 camera_pos, f32 chunk_world_size);

  // Records key as newly resident at gpu_slot (state -> Baking) -- call
  // right after actually recording that chunk's voxelize_chunk() dispatch.
  void commit_load(ChunkKey key, u32 gpu_slot);

  // Begins key's ring-delay (state -> Evicting) -- call right after
  // actually recording that chunk's evict_chunk() dispatch. The chunk (and
  // its gpu_slot) are fully forgotten, and the slot returned to the free
  // pool, only once frames_in_flight_delay tick()s have passed (see
  // tick()'s own comment) -- until then it still counts against
  // max_resident_chunks (acquire_free_slot() won't hand it out), so a
  // caller must not treat commit_evict() as freeing capacity immediately.
  void commit_evict(ChunkKey key);

  // Advances the internal frame-generation counter and finalizes any
  // load/evict whose ring-delay has now elapsed: a Baking chunk becomes
  // Ready (safe to sample -- its voxelize_chunk() dispatch is guaranteed to
  // have actually completed on the GPU by now, not just been recorded);
  // an Evicting chunk is fully erased and its slot returned to the free
  // pool. Call exactly once per frame, unconditionally (even if update()
  // wasn't called, or its Plan was empty) -- the ring-delay is measured in
  // tick() calls, not in loads/evicts.
  void tick();

  // Pops a free slot (immediately available, i.e. never handed to a chunk
  // still mid-evict-ring-delay), or kInvalidChunkSlot if none are free
  // right now (either every slot is genuinely resident, or ring-delayed --
  // the caller can't tell which from this alone, and doesn't need to: both
  // cases mean "try again a later frame").
  u32 acquire_free_slot();

  size_t resident_count() const noexcept { return resident_.size(); }
  ChunkState state_of(ChunkKey key) const noexcept;
  const std::unordered_map<ChunkKey, ChunkRecord, ChunkKeyHash> &
  resident_chunks() const noexcept {
    return resident_;
  }

private:
  // Regression test for a real anti-thrashing bug: to_load's window and
  // to_evict's used to share the exact same stream_radius_chunks_ radius,
  // so a chunk sitting right at the boundary would evict, then immediately
  // reload, every time the camera's own chunk index dithered back and
  // forth across that edge (e.g. small orbit-camera jitter, or a straight
  // line of travel that happens to track a chunk boundary) -- extra GPU
  // work on every such frame, and a visible pop each time the chunk
  // briefly vanished mid-cycle. Widening ONLY the evict test by this
  // margin means a chunk that's already resident stays resident slightly
  // past to_load's own window before qualifying for eviction, so ordinary
  // jitter right at the edge no longer costs anything. Does not change
  // to_load's own footprint (still (2*stream_radius_chunks_+1)^3
  // candidates in steady state) -- this only affects how long an
  // already-loaded chunk lingers, so max_resident_chunks doesn't need
  // resizing for this alone (the sampling shader's own window, Builtin.
  // ChunkedFieldCommon.inc.glsl's sample_clipmap_field(), intentionally
  // still uses the tighter stream_radius_chunks_ -- see its own comment --
  // so this hysteresis chunk is loaded but briefly unsampled, not visually
  // used, purely a GPU-churn optimization).
  static constexpr i32 kEvictHysteresisChunks = 1;

  std::unordered_map<ChunkKey, ChunkRecord, ChunkKeyHash> resident_;
  std::vector<u32> free_slots_;
  u32 max_resident_chunks_;
  i32 stream_radius_chunks_;
  u32 frames_in_flight_delay_;
  // Bumped once per tick() -- ChunkRecord::bake_generation is stamped with
  // this value on every commit_load()/commit_evict() (repurposed here as a
  // generic "generation this state transition was issued at" marker, not
  // literally "which bake" once a chunk is Evicting -- see its own comment
  // engine-side for the field's primary meaning).
  u64 generation_ = 0;
};
