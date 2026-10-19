#pragma once
#include "../defines.h"
#include "../resources/sculpt_volume.h"

#include <glm/glm.hpp>

// M1a/M2a: the thin CPU-side owner of a sculpture's SculptVolume. Deliberately
// NOT the owner of the GPU-side chunk-table-slot/brick-free-list
// bookkeeping the plan's §1/§2 originally described living here -- that
// bookkeeping needs VulkanRaymarchShader's own private addressing
// constants (kChunkTableDim/kChunkCoarseDim/kMaxResidentChunks/the brick
// free list, all file-local to vulkan_raymarch_shader.cpp) to never
// disagree with the actual GPU buffers, so it stays there (see
// VulkanRaymarchShader::acquire_sculpt_chunk_slot()/stamp_shape()) rather
// than being duplicated here against the risk of the two drifting apart.
// SculptSystem's own job is narrower: record what was authored (for a
// later milestone's picking/undo/.sculpt save-load to build on) and be the
// one call site a renderer backend goes through to do it.
//
// engine/src/systems/material_system.h already establishes that a
// "system" in this engine may depend on Vulkan types directly when it
// needs to (it holds VulkanTexture/VulkanShader) -- SculptSystem does not
// need to here (VulkanRendererBackend::stamp_shape() calls this AND
// VulkanRaymarchShader::stamp_shape() itself, side by side), so it stays
// backend-agnostic on purpose, not because the pattern forbids otherwise.
class SculptSystem {
public:
  // Records a brush stamp into this sculpture's CPU volume. Pure
  // bookkeeping -- does NOT touch the GPU itself; the caller (see
  // VulkanRendererBackend::stamp_shape()) is responsible for separately
  // driving the actual GPU-side stamp (VulkanRaymarchShader::
  // stamp_shape()) and passing back how many cells it touched via
  // cells_touched, so the two records can never silently diverge about
  // whether a stamp "happened".
  void apply_stamp(glm::vec3 center, SculptBrushShape shape_type,
                   glm::vec4 params, f32 smoothness, SculptBrushOp op,
                   i32 material_slot, i32 cells_touched,
                   f32 looseness = 0.0f,
                   glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                   i32 object_id = 0);

  // M2b: removes and returns the most recently applied stamp's bookkeeping
  // record -- the CPU-side half of an Undo (see VulkanRendererBackend::
  // undo_stamp(), which calls this only after VulkanRaymarchShader::
  // undo_stamp() has confirmed the GPU-side revert actually happened, so
  // the two records can never disagree about whether an Undo "happened").
  // See SculptVolume::pop_last_stamp()'s own comment for why this breaks
  // the type's otherwise append-only history.
  std::optional<SculptStamp> pop_last_stamp() {
    return volume_.pop_last_stamp();
  }

  // M4b (Delete-of-an-arbitrary-edit): the CPU-side half of
  // VulkanRendererBackend::delete_edit() -- see SculptVolume::
  // remove_stamp_at()'s own comment. Only ever called AFTER the GPU-side
  // checkpoint-restore-and-replay has already confirmed the edit at `index`
  // was actually dropped, same "GPU first, then retract the record"
  // discipline pop_last_stamp()'s own call site (undo_stamp()) already
  // follows.
  std::optional<SculptStamp> remove_stamp_at(size_t index) {
    return volume_.remove_stamp_at(index);
  }

  // M4c (Move-of-an-arbitrary-edit): the CPU-side half of
  // VulkanRendererBackend::move_edit() -- see SculptVolume::
  // update_stamp_center()'s own comment. Only ever called AFTER the GPU-side
  // checkpoint-restore-and-replay (re-stamping `index` at new_center) has
  // already confirmed the move actually happened, same "GPU first, then
  // update the record" discipline remove_stamp_at()/pop_last_stamp()'s own
  // call sites already follow.
  bool update_stamp_center(size_t index, glm::vec3 new_center,
                           i32 cells_touched) {
    return volume_.update_stamp_center(index, new_center, cells_touched);
  }

  bool update_stamp_transform(size_t index, glm::vec3 new_center,
                              glm::quat new_rotation, glm::vec4 new_params,
                              f32 new_smoothness, i32 cells_touched) {
    return volume_.update_stamp_transform(index, new_center, new_rotation,
                                          new_params, new_smoothness,
                                          cells_touched);
  }

  bool set_stamp_material(size_t index, i32 material_slot) {
    return volume_.set_stamp_material(index, material_slot);
  }

  const SculptVolume &volume() const noexcept { return volume_; }

private:
  SculptVolume volume_;
};
