#pragma once
#include "../defines.h"
#include "../renderer/renderer_types.inl" // SculptBrushShape/SculptBrushOp

#include <glm/glm.hpp>
#include <optional>
#include <vector>

// M1a/M2a: the smallest possible CPU-side data model for a brush-sculpted
// volume -- just enough to record "a shape was stamped here (with what
// params/smoothness/op)" and how many GPU cells it ended up touching, so a
// future milestone (picking, undo, .sculpt save/load) has a real type to
// grow instead of reinventing one. M2a generalized this from M1a's
// sphere-only/hard-union-only SculptSphereStamp to carry any of the 15
// brush shapes + a smoothness + a union/subtraction op -- see /home/h-i/
// .claude/plans/joyful-skipping-giraffe.md's M2a section. Still explicitly
// NOT in scope here: edit history/undo (that is M2b, dispatched after this
// lands) and brush rotation (every shape stays in its canonical,
// axis-aligned local orientation).
//
// This is deliberately NOT a mirror of the GPU's own cell/brick/voxel
// layout (contrast with the plan's original, larger ambition for this
// type). VulkanRaymarchShader owns that addressing (kChunkTableDim/
// kChunkCoarseDim/kMaxResidentChunks/the brick free list are all private to
// vulkan_raymarch_shader.cpp, and reproducing that bookkeeping here as well
// would mean two independent sources of truth for the same allocation
// state, which is exactly the kind of drift the plan's own §1 warns about
// for the chunk table). Picking/undo, when they arrive, will need the CPU
// to resolve "what's at this point" against something -- at that point
// this struct is the natural place to grow a real voxel mirror; M1a/M2a
// have no picking or undo, so it stays this small on purpose.
struct SculptStamp {
  glm::vec3 center{0.0f};
  SculptBrushShape shape_type = SculptBrushShape::Sphere;
  // Meaning depends on shape_type -- see VulkanRaymarchShader::
  // stamp_shape()'s header comment for the full per-shape layout table.
  glm::vec4 params{0.0f};
  f32 smoothness = 0.0f;
  SculptBrushOp op = SculptBrushOp::Union;
  // Index into the per-primitive material arrays (ScenePrimitiveColours/
  // ScenePrimitiveMaterialScalars/ScenePrimitiveTransmission in Builtin.
  // DeferredShade.comp.glsl) this stamp's brick(s) were written with.
  // Always 0 so far -- see VulkanRaymarchShader::ensure_default_sculpt_
  // material(), the one material every stamp currently uses.
  i32 material_slot = 0;
  // How many level-0 coarse cells VulkanRaymarchShader::stamp_shape()
  // actually touched for this stamp (0 if the call failed outright, e.g.
  // a degenerate shape) -- purely informational for now (logging/smoke-test
  // verification); nothing reads it back yet.
  i32 cells_touched = 0;
  // M3 (Looseness): 0 (uniform/clean, the default) to 1 (maximally loose) --
  // authored per-stamp, same as every other field here. Now has a real
  // GPU-side effect: VulkanRendererBackend::apply_stamp_and_record()/
  // delete_edit()/move_edit() thread this straight through to
  // VulkanRaymarchShader::stamp_shape(), which writes it into a new per-
  // BRICK buffer (chunk_brick_looseness_buffer_, parallel to the existing
  // per-brick material buffer, same UNION-only-overwrites rule) alongside
  // the stamp's material -- Builtin.DeferredShade.comp.glsl reads it back
  // at the real-surface shading block to scale the hit brick's effective
  // Fleck strength/scale (see Builtin.FleckPattern.inc.glsl's entry
  // points). See the plan file's M3/M6 sections.
  f32 looseness = 0.0f;
};

// Owns the (mostly append-only -- see pop_last_stamp()/remove_stamp_at()
// below) list of brush strokes applied to one sculpture. SculptSystem is the
// only intended mutator; this struct itself has no GPU awareness at all, by
// design -- see its own header comment for why that split exists.
//
// M2b: gained pop_last_stamp(), breaking the previously-strict "append-
// only" invariant -- Undo needs a way to retract the bookkeeping record for
// whatever stamp VulkanRaymarchShader::undo_stamp() just reverted GPU-side,
// or this list (which tools/modelling_software_v2's SculptDocument mirrors
// for save/load, and which Redo also reads) would silently disagree with
// what the GPU field actually shows. Only ever removed from the TAIL (the
// most recent stamp), matching a linear undo stack's own LIFO semantics.
//
// M4b (Delete-of-an-arbitrary-edit): gained remove_stamp_at(), the arbitrary-
// index counterpart pop_last_stamp()'s own comment used to say didn't exist
// yet. What unlocked it: VulkanRaymarchShader gained a genuine checkpoint/
// replay engine (capture_sculpt_checkpoint()/restore_sculpt_checkpoint_
// before()/invalidate_sculpt_checkpoints_from(), vulkan_raymarch_shader.h)
// that can reconstruct the GPU field's state as of any past edit index by
// restoring the nearest periodic full-volume checkpoint at or before that
// index and replaying every stamp after it via stamp_shape() -- i.e. this
// list stopped being merely a passive record ALONGSIDE the GPU's own
// forward-only bake, and became the actual authoritative source of truth
// the GPU field gets *regenerated from*. Once that exists, removing an
// arbitrary entry here and telling the orchestration layer
// (VulkanRendererBackend::delete_edit()) to restore-then-replay around the
// gap is no harder than removing the tail was -- the hard part was never
// this vector operation, it was having something to replay THROUGH. See the
// plan file's M4b section (formerly "M2b" in this comment, before Delete was
// split out into its own milestone) for the full design.
//
// M4c (Move-of-an-arbitrary-edit): gained update_stamp_center(), the
// in-place counterpart to remove_stamp_at() -- same checkpoint/replay engine
// unlock, but here the edit at `index` isn't retracted at all, just given a
// new center (and refreshed cells_touched) while staying at its original
// list position. See VulkanRendererBackend::move_edit()'s own comment for
// the replay mechanics (identical to delete_edit()'s restore-then-replay,
// except the target index is re-stamped at the new center instead of
// skipped).
class SculptVolume {
public:
  const SculptStamp &add_stamp(glm::vec3 center, SculptBrushShape shape_type,
                               glm::vec4 params, f32 smoothness,
                               SculptBrushOp op, i32 material_slot,
                               i32 cells_touched, f32 looseness = 0.0f) {
    stamps_.push_back(SculptStamp{center, shape_type, params, smoothness, op,
                                  material_slot, cells_touched, looseness});
    return stamps_.back();
  }

  // Removes and returns the most recently added stamp, or std::nullopt if
  // stamps_ is already empty. See this class's own comment for why this
  // exists despite the "append-only" history the rest of the type implies.
  std::optional<SculptStamp> pop_last_stamp() {
    if (stamps_.empty()) {
      return std::nullopt;
    }
    SculptStamp last = stamps_.back();
    stamps_.pop_back();
    return last;
  }

  // M4b (Delete-of-an-arbitrary-edit): removes and returns the stamp at
  // `index` (0 = oldest, matching stamps()'s own order), or std::nullopt if
  // index is out of range. Unlike pop_last_stamp(), this can retract any
  // entry, not just the tail -- see this class's own header comment for why
  // that only became safe to add once VulkanRaymarchShader grew a real
  // checkpoint/replay engine to reconstruct the GPU field around the gap.
  // Pure bookkeeping, same as every other method here -- the caller
  // (VulkanRendererBackend::delete_edit()) is responsible for having
  // ALREADY driven the GPU-side restore-and-replay before calling this, so
  // this list and the GPU field never disagree about which edit still
  // exists.
  std::optional<SculptStamp> remove_stamp_at(size_t index) {
    if (index >= stamps_.size()) {
      return std::nullopt;
    }
    SculptStamp removed = stamps_[index];
    stamps_.erase(stamps_.begin() + static_cast<std::ptrdiff_t>(index));
    return removed;
  }

  // M4c (Move-of-an-arbitrary-edit): updates the stamp at `index` IN PLACE
  // -- new center, new cells_touched (the actual cell count the re-stamp at
  // the new position just reported -- see VulkanRendererBackend::
  // move_edit()'s own comment for why this is passed back rather than left
  // stale), everything else (shape_type/params/smoothness/op/
  // material_slot/looseness) untouched. Unlike remove_stamp_at(), the
  // list's ORDER and SIZE never change -- a moved edit stays at the exact
  // same position in the sequence, which is what lets VulkanRendererBackend
  // ::move_edit() reuse delete_edit()'s own restore-checkpoint-then-replay
  // machinery almost verbatim (replay `index` itself with the new center
  // instead of skipping it, rather than removing an entry and shifting
  // everything after it down). Returns false if index is out of range.
  bool update_stamp_center(size_t index, glm::vec3 new_center,
                           i32 cells_touched) {
    if (index >= stamps_.size()) {
      return false;
    }
    stamps_[index].center = new_center;
    stamps_[index].cells_touched = cells_touched;
    return true;
  }

  const std::vector<SculptStamp> &stamps() const noexcept { return stamps_; }

  bool empty() const noexcept { return stamps_.empty(); }

private:
  std::vector<SculptStamp> stamps_;
};
