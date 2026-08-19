#include "chunk_streaming_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

ChunkStreamingManager::ChunkStreamingManager(u32 max_resident_chunks,
                                             i32 stream_radius_chunks,
                                             u32 frames_in_flight_delay,
                                             u32 slot_offset,
                                             u32 lazy_evict_min_free)
    : max_resident_chunks_(max_resident_chunks),
      lazy_evict_min_free_(lazy_evict_min_free),
      stream_radius_chunks_(stream_radius_chunks),
      frames_in_flight_delay_(frames_in_flight_delay) {
  free_slots_.reserve(max_resident_chunks_);
  // Reverse order so acquire_free_slot() (a back-pop) hands out
  // slot_offset first -- purely cosmetic (any order is equally correct),
  // but makes early-slot assignment deterministic and easy to reason
  // about in tests.
  for (u32 i = max_resident_chunks_; i > 0; --i) {
    free_slots_.push_back(slot_offset + i - 1);
  }
}

namespace {
glm::vec3 chunk_center(ChunkKey key, f32 chunk_world_size) {
  return glm::vec3(static_cast<f32>(key.cx) + 0.5f,
                   static_cast<f32>(key.cy) + 0.5f,
                   static_cast<f32>(key.cz) + 0.5f) *
      chunk_world_size;
}
} // namespace

ChunkStreamingManager::Plan ChunkStreamingManager::update(glm::vec3 camera_pos,
                                                          f32 chunk_world_size) {
  Plan plan;
  if (chunk_world_size <= 0.0f) {
    return plan;
  }

  i64 camera_cx = static_cast<i64>(std::floor(camera_pos.x / chunk_world_size));
  i64 camera_cy = static_cast<i64>(std::floor(camera_pos.y / chunk_world_size));
  i64 camera_cz = static_cast<i64>(std::floor(camera_pos.z / chunk_world_size));

  // to_evict: every RESIDENT (Ready) chunk outside the current Chebyshev
  // window, widened by kEvictHysteresisChunks (see its own comment) -- NOT
  // the same radius to_load uses below. Deliberately excludes Baking, not
  // just Evicting: a chunk still mid-load hasn't cleared its own ring-delay
  // yet, so its voxelize_chunk() dispatch isn't guaranteed complete on the
  // GPU -- evicting it now could race evict_chunk()'s writes against that
  // still-in-flight bake (two separate frames' command buffers aren't
  // ordered relative to each other without an explicit fence/barrier
  // between them, only same-queue submission *start* order is implicit --
  // see this class's header comment on why tick()'s delay exists at all).
  // A Baking chunk that's moved outside the window simply becomes eligible
  // on a later update() call, once its own tick()-driven transition to
  // Ready confirms it's safe -- a small extra delay in a rare case, not a
  // correctness issue.
  if (lazy_evict_min_free_ == 0) {
    // Eager (original) behavior: evict on window-exit regardless of slot
    // pressure.
    i32 evict_radius = stream_radius_chunks_ + kEvictHysteresisChunks;
    for (const auto &[key, record] : resident_) {
      if (record.state != ChunkState::Ready) {
        continue;
      }
      bool within_evict_window = std::abs(key.cx - camera_cx) <= evict_radius &&
          std::abs(key.cy - camera_cy) <= evict_radius &&
          std::abs(key.cz - camera_cz) <= evict_radius;
      if (!within_evict_window) {
        plan.to_evict.push_back(key);
      }
    }
  } else if (free_slots_.size() + releasing_slots_.size() <
             lazy_evict_min_free_) {
    // Lazy eviction (see the constructor comment), and slots have actually
    // become scarce: trim the cache. Anything outside the LOAD window
    // itself is fair game -- deliberately NOT widened by
    // kEvictHysteresisChunks here: under pressure the hysteresis ring is
    // just more cache, and only listing chunks beyond it could leave
    // nothing evictable at all (the ring alone can hold more chunks than
    // there are slots). The anti-thrash job hysteresis did is done by the
    // laziness itself now -- no pressure, no eviction, so boundary jitter
    // costs nothing. Farthest-first, so the least-likely-to-be-wanted
    // chunks go before ones the camera might swing back to; releasing_
    // slots_ counts toward the free side since those slots are already on
    // their way back without any new eviction.
    for (const auto &[key, record] : resident_) {
      if (record.state != ChunkState::Ready) {
        continue;
      }
      i64 chebyshev = std::max({std::abs(key.cx - camera_cx),
                                std::abs(key.cy - camera_cy),
                                std::abs(key.cz - camera_cz)});
      if (chebyshev > stream_radius_chunks_) {
        plan.to_evict.push_back(key);
      }
    }
    std::sort(plan.to_evict.begin(), plan.to_evict.end(),
             [&](ChunkKey a, ChunkKey b) {
               i64 da = std::max({std::abs(a.cx - camera_cx),
                                  std::abs(a.cy - camera_cy),
                                  std::abs(a.cz - camera_cz)});
               i64 db = std::max({std::abs(b.cx - camera_cx),
                                  std::abs(b.cy - camera_cy),
                                  std::abs(b.cz - camera_cz)});
               return da > db;
             });
  }

  // to_load: every chunk in the window not already resident/in-progress,
  // nearest-to-camera first.
  for (i64 dz = -stream_radius_chunks_; dz <= stream_radius_chunks_; ++dz) {
    for (i64 dy = -stream_radius_chunks_; dy <= stream_radius_chunks_; ++dy) {
      for (i64 dx = -stream_radius_chunks_; dx <= stream_radius_chunks_; ++dx) {
        ChunkKey candidate{0, camera_cx + dx, camera_cy + dy, camera_cz + dz};
        if (resident_.find(candidate) == resident_.end()) {
          plan.to_load.push_back(candidate);
        }
      }
    }
  }
  std::sort(plan.to_load.begin(), plan.to_load.end(),
           [&](ChunkKey a, ChunkKey b) {
             f32 da = glm::length(chunk_center(a, chunk_world_size) - camera_pos);
             f32 db = glm::length(chunk_center(b, chunk_world_size) - camera_pos);
             return da < db;
           });

  return plan;
}

void ChunkStreamingManager::commit_load(ChunkKey key, u32 gpu_slot) {
  ChunkRecord record;
  record.state = ChunkState::Baking;
  record.gpu_slot = gpu_slot;
  record.bake_generation = generation_;
  resident_[key] = record;
}

void ChunkStreamingManager::commit_evict(ChunkKey key) {
  auto it = resident_.find(key);
  if (it == resident_.end()) {
    return;
  }
  it->second.state = ChunkState::Evicting;
  it->second.bake_generation = generation_;
}

void ChunkStreamingManager::tick() {
  ++generation_;

  // Slots released outright (release_slot()) age exactly like an evicting
  // chunk's own slot does, for the same reason -- see acquire_free_slot().
  for (auto it = releasing_slots_.begin(); it != releasing_slots_.end();) {
    if (generation_ - it->second >= frames_in_flight_delay_) {
      free_slots_.push_back(it->first);
      it = releasing_slots_.erase(it);
    } else {
      ++it;
    }
  }

  for (auto it = resident_.begin(); it != resident_.end();) {
    ChunkRecord &record = it->second;
    u64 age = generation_ - record.bake_generation;
    if (record.state == ChunkState::Baking && age >= frames_in_flight_delay_) {
      record.state = ChunkState::Ready;
      ++it;
    } else if (record.state == ChunkState::Evicting &&
              age >= frames_in_flight_delay_) {
      free_slots_.push_back(record.gpu_slot);
      it = resident_.erase(it);
    } else {
      ++it;
    }
  }
}

void ChunkStreamingManager::release_slot(u32 gpu_slot) {
  if (gpu_slot == kInvalidChunkSlot) {
    return;
  }
  // Already free, or already awaiting release -- see the header comment on
  // why a redundant call has to be harmless rather than corrupting.
  if (std::find(free_slots_.begin(), free_slots_.end(), gpu_slot) !=
      free_slots_.end()) {
    return;
  }
  for (const auto &[slot, generation] : releasing_slots_) {
    if (slot == gpu_slot) {
      return;
    }
  }
  releasing_slots_.emplace_back(gpu_slot, generation_);
}

u32 ChunkStreamingManager::acquire_free_slot() {
  if (free_slots_.empty()) {
    return kInvalidChunkSlot;
  }
  u32 slot = free_slots_.back();
  free_slots_.pop_back();
  return slot;
}

ChunkState ChunkStreamingManager::state_of(ChunkKey key) const noexcept {
  auto it = resident_.find(key);
  return it == resident_.end() ? ChunkState::Empty : it->second.state;
}
