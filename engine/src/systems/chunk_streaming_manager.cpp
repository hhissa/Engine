#include "chunk_streaming_manager.h"

#include <algorithm>
#include <cmath>

ChunkStreamingManager::ChunkStreamingManager(u32 max_resident_chunks,
                                             i32 stream_radius_chunks,
                                             u32 frames_in_flight_delay,
                                             u32 slot_offset)
    : max_resident_chunks_(max_resident_chunks),
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
