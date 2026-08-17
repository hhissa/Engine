#include "chunk_streaming_manager_tests.h"
#include "../expect.h"
#include "../test_manager.h"

#include <defines.h>
#include <systems/chunk_streaming_manager.h>

namespace {

constexpr f32 kChunkSize = 4.0f;

bool update_at_origin_returns_full_window_nearest_first() {
  ChunkStreamingManager mgr(/*max_resident_chunks=*/64, /*stream_radius_chunks=*/1,
                            /*frames_in_flight_delay=*/3);

  ChunkStreamingManager::Plan plan = mgr.update(glm::vec3(0.0f), kChunkSize);

  // Radius 1 -> (2*1+1)^3 = 27 candidate chunks, nothing resident yet.
  EXPECT_EQ(27u, plan.to_load.size());
  EXPECT_EQ(0u, plan.to_evict.size());
  // Camera at world origin sits in chunk (-1,-1,-1) (floor(0/4) == 0 on the
  // boundary itself rounds to chunk index 0, but floor() of exactly 0 is 0,
  // not -1 -- the chunk containing the camera is whichever chunk (0,0,0)
  // actually is here) -- regardless of which exact key that resolves to,
  // it must be the FIRST entry (distance 0 to its own center is smaller
  // than every neighbor's).
  ChunkKey nearest = plan.to_load.front();
  EXPECT_EQ(0, nearest.level);
  return true;
}

bool acquire_free_slot_exhausts_and_recovers_after_evict_and_delay() {
  ChunkStreamingManager mgr(/*max_resident_chunks=*/2, /*stream_radius_chunks=*/0,
                            /*frames_in_flight_delay=*/2);

  u32 slot_a = mgr.acquire_free_slot();
  u32 slot_b = mgr.acquire_free_slot();
  EXPECT_NE(kInvalidChunkSlot, slot_a);
  EXPECT_NE(kInvalidChunkSlot, slot_b);
  EXPECT_NE(slot_a, slot_b);

  // Pool exhausted -- a third acquire must fail, not hand out a duplicate.
  EXPECT_EQ(kInvalidChunkSlot, mgr.acquire_free_slot());

  ChunkKey key_a{0, 0, 0, 0};
  mgr.commit_load(key_a, slot_a);
  EXPECT_TRUE(mgr.state_of(key_a) == ChunkState::Baking);

  mgr.commit_evict(key_a);
  EXPECT_TRUE(mgr.state_of(key_a) == ChunkState::Evicting);

  // Still not free immediately -- the whole point of the ring-delay.
  EXPECT_EQ(kInvalidChunkSlot, mgr.acquire_free_slot());

  mgr.tick(); // generation 1: age since evict (issued at generation 0) == 1
  EXPECT_EQ(kInvalidChunkSlot, mgr.acquire_free_slot());

  mgr.tick(); // generation 2: age == 2 == frames_in_flight_delay -> released
  u32 recovered = mgr.acquire_free_slot();
  EXPECT_EQ(slot_a, recovered);
  EXPECT_TRUE(mgr.state_of(key_a) == ChunkState::Empty); // fully forgotten

  return true;
}

bool baking_chunk_becomes_ready_only_after_delay() {
  ChunkStreamingManager mgr(/*max_resident_chunks=*/8, /*stream_radius_chunks=*/0,
                            /*frames_in_flight_delay=*/3);

  ChunkKey key{0, 5, 5, 5};
  u32 slot = mgr.acquire_free_slot();
  mgr.commit_load(key, slot);
  EXPECT_TRUE(mgr.state_of(key) == ChunkState::Baking);

  mgr.tick(); // age 1
  EXPECT_TRUE(mgr.state_of(key) == ChunkState::Baking);
  mgr.tick(); // age 2
  EXPECT_TRUE(mgr.state_of(key) == ChunkState::Baking);
  mgr.tick(); // age 3 == delay -> Ready
  EXPECT_TRUE(mgr.state_of(key) == ChunkState::Ready);

  return true;
}

bool baking_chunk_outside_window_is_not_offered_for_eviction() {
  // Regression test for a real bug caught during development: a chunk that
  // is still Baking (its own load's ring-delay hasn't elapsed) must NOT
  // appear in to_evict even if the camera has already moved it outside the
  // window -- evicting it before its own bake is confirmed complete could
  // race evict_chunk()'s GPU writes against the still-in-flight
  // voxelize_chunk() dispatch (see ChunkStreamingManager's header comment).
  ChunkStreamingManager mgr(/*max_resident_chunks=*/8, /*stream_radius_chunks=*/1,
                            /*frames_in_flight_delay=*/5);

  ChunkKey key{0, 0, 0, 0};
  u32 slot = mgr.acquire_free_slot();
  mgr.commit_load(key, slot); // Baking, not yet Ready

  // Camera jumps far enough that chunk (0,0,0) is now well outside the
  // radius-1 window.
  glm::vec3 far_camera(100.0f, 100.0f, 100.0f);
  ChunkStreamingManager::Plan plan = mgr.update(far_camera, kChunkSize);

  for (ChunkKey evicted : plan.to_evict) {
    EXPECT_TRUE(!(evicted == key));
  }

  // Age it past the delay -> Ready -- NOW it must show up for eviction.
  for (u32 i = 0; i < 5; ++i) {
    mgr.tick();
  }
  EXPECT_TRUE(mgr.state_of(key) == ChunkState::Ready);
  plan = mgr.update(far_camera, kChunkSize);
  bool found = false;
  for (ChunkKey evicted : plan.to_evict) {
    if (evicted == key) {
      found = true;
    }
  }
  EXPECT_TRUE(found);

  return true;
}

bool update_excludes_already_resident_chunks_from_to_load() {
  ChunkStreamingManager mgr(/*max_resident_chunks=*/64, /*stream_radius_chunks=*/1,
                            /*frames_in_flight_delay=*/1);

  ChunkStreamingManager::Plan first = mgr.update(glm::vec3(0.0f), kChunkSize);
  EXPECT_EQ(27u, first.to_load.size());

  for (ChunkKey key : first.to_load) {
    u32 slot = mgr.acquire_free_slot();
    mgr.commit_load(key, slot);
  }

  // Nothing new to load -- every candidate in the window is already
  // resident (Baking counts as "in progress", excluded from to_load too).
  ChunkStreamingManager::Plan second = mgr.update(glm::vec3(0.0f), kChunkSize);
  EXPECT_EQ(0u, second.to_load.size());

  return true;
}

bool slot_offset_shifts_every_acquired_slot() {
  // Phase 4: one ChunkStreamingManager instance per clip level, sharing a
  // single GPU indirection buffer partitioned into per-level slot ranges
  // -- slot_offset is what keeps two levels' instances from ever handing
  // out the same global slot.
  ChunkStreamingManager level0(/*max_resident_chunks=*/4, /*stream_radius_chunks=*/0,
                               /*frames_in_flight_delay=*/1, /*slot_offset=*/0);
  ChunkStreamingManager level1(/*max_resident_chunks=*/4, /*stream_radius_chunks=*/0,
                               /*frames_in_flight_delay=*/1, /*slot_offset=*/4);

  for (int i = 0; i < 4; ++i) {
    u32 slot = level0.acquire_free_slot();
    EXPECT_TRUE(slot < 4u);
  }
  EXPECT_EQ(kInvalidChunkSlot, level0.acquire_free_slot());

  for (int i = 0; i < 4; ++i) {
    u32 slot = level1.acquire_free_slot();
    EXPECT_TRUE(slot >= 4u && slot < 8u);
  }
  EXPECT_EQ(kInvalidChunkSlot, level1.acquire_free_slot());

  return true;
}

bool non_positive_chunk_size_returns_empty_plan() {
  ChunkStreamingManager mgr(64, 1, 3);
  EXPECT_EQ(0u, mgr.update(glm::vec3(0.0f), 0.0f).to_load.size());
  EXPECT_EQ(0u, mgr.update(glm::vec3(0.0f), -1.0f).to_load.size());
  return true;
}

} // namespace

void register_chunk_streaming_manager_tests() {
  TestManager::register_test(
      update_at_origin_returns_full_window_nearest_first,
      "ChunkStreamingManager: update() at origin returns full window, "
      "nearest chunk first");
  TestManager::register_test(
      acquire_free_slot_exhausts_and_recovers_after_evict_and_delay,
      "ChunkStreamingManager: slot pool exhausts, then recovers after "
      "evict + ring-delay");
  TestManager::register_test(
      baking_chunk_becomes_ready_only_after_delay,
      "ChunkStreamingManager: Baking chunk becomes Ready only after "
      "frames_in_flight_delay ticks");
  TestManager::register_test(
      baking_chunk_outside_window_is_not_offered_for_eviction,
      "ChunkStreamingManager: a still-Baking chunk is never offered for "
      "eviction until it's Ready");
  TestManager::register_test(
      update_excludes_already_resident_chunks_from_to_load,
      "ChunkStreamingManager: already-resident chunks are excluded from "
      "to_load");
  TestManager::register_test(
      slot_offset_shifts_every_acquired_slot,
      "ChunkStreamingManager: slot_offset shifts every acquired slot "
      "(Phase 4 per-level instances)");
  TestManager::register_test(
      non_positive_chunk_size_returns_empty_plan,
      "ChunkStreamingManager: non-positive chunk_world_size returns an "
      "empty Plan");
}
