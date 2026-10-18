#pragma once
#include <defines.h> // f32/i32 -- moc-generated code (main_window.h signals/
                     // slots use these types) includes this header directly,
                     // without engine/src/renderer/* having pulled defines.h
                     // in first, so it must be included explicitly here
                     // rather than relying on transitive include order.
#include <renderer/renderer_types.inl> // SculptBrushShape/SculptBrushOp
#include <glm/glm.hpp>

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// M2a's own tool-side record of every brush stamp applied this session --
// NOT the engine's SculptVolume (engine/src/resources/sculpt_volume.h),
// which currently has no API to enumerate its stamps back out (see that
// header's own comment: it is deliberately minimal, append-only, and
// private to VulkanRaymarchShader's bookkeeping). This tool needs SOME
// record to serialize for save/load, so it keeps its own flat list in
// parallel with whatever the engine accumulates GPU-side, mirroring every
// renderer_sculpt_stamp_shape() call this process makes.
//
// This is intentionally not the plan's eventual `.sculpt` sparse-brick
// format (see /home/h-i/.claude/plans/joyful-skipping-giraffe.md's §1) --
// that belongs to a later milestone once SculptVolume grows a real CPU
// mirror of the brick data. For M2a the simplest correct format is still a
// trivial text file, one stamp per line -- just a wider line than M1b's:
//
//   FORMAT v3 (M3, Looseness): "x y z shape_type p0 p1 p2 p3 smoothness op
//   material_slot looseness" -- 12 columns.
//   FORMAT v2 (M2a): "x y z shape_type p0 p1 p2 p3 smoothness op
//   material_slot" -- 11 columns, upgraded on the fly to looseness=0.0
//   (the engine-side default -- see SculptStamp::looseness's own comment,
//   sculpt_volume.h -- Looseness has no shader effect yet regardless, so
//   this upgrade is exact, not an approximation).
//   FORMAT v1 (M1b, back-compat): "x y z radius material_slot" -- 5
//   columns. load() below detects which format a line is in BY COLUMN
//   COUNT (5, 11, or 12) and upgrades older lines on the fly -- a v1 line
//   becomes {shape=Sphere, params=(radius,0,0,0), smoothness=0,
//   op=Union, looseness=0}, its exact semantic equivalent -- so an
//   M1b/M2a-era .stamps file keeps loading correctly after this change,
//   not just a "your call" note that it doesn't. save() always writes the
//   current (v3, 12-column) format; only load() needs to know about v1/v2.
struct SculptStampRecord {
  glm::vec3 center{0.0f};
  SculptBrushShape shape_type = SculptBrushShape::Sphere;
  glm::vec4 params{0.0f};
  f32 smoothness = 0.0f;
  SculptBrushOp op = SculptBrushOp::Union;
  i32 material_slot = 0;
  // M3 (Looseness): see SculptStamp::looseness's own comment
  // (engine/src/resources/sculpt_volume.h) -- pure bookkeeping, no shader
  // effect yet, exists so an authored value survives save/load until the
  // Fleck evaluator (M6) can actually consume it.
  f32 looseness = 0.0f;
};

// Owns the flat stamp list and its plain-text save/load. Replaying every
// stamp back through renderer_sculpt_stamp_shape() on load is the caller's
// job (see ModellingWindow::load_document()) -- this struct is pure data,
// no renderer awareness, matching SculptVolume's own CPU/GPU split.
class SculptDocument {
public:
  void add_stamp(glm::vec3 center, SculptBrushShape shape_type,
                 glm::vec4 params, f32 smoothness, SculptBrushOp op,
                 i32 material_slot, f32 looseness = 0.0f) {
    stamps_.push_back(SculptStampRecord{center, shape_type, params,
                                        smoothness, op, material_slot,
                                        looseness});
  }

  void clear() { stamps_.clear(); }

  // M2b: removes and returns the most recently added record, or
  // std::nullopt if stamps_ is already empty -- the tool-side mirror of
  // SculptVolume::pop_last_stamp() (engine/src/resources/sculpt_volume.h),
  // kept in lockstep with it by ModellingWindow's Undo handler so a saved
  // .stamps file never includes a stamp the engine has actually undone.
  std::optional<SculptStampRecord> pop_last_stamp() {
    if (stamps_.empty()) {
      return std::nullopt;
    }
    SculptStampRecord last = stamps_.back();
    stamps_.pop_back();
    return last;
  }

  // M4b (Delete-of-an-arbitrary-edit): the tool-side mirror of
  // SculptVolume::remove_stamp_at() (engine/src/resources/sculpt_volume.h)
  // -- removes and returns the record at `index` (0 = oldest), or
  // std::nullopt if out of range. Kept in lockstep with the engine the same
  // way pop_last_stamp() always has: ModellingWindow only calls this AFTER
  // SceneViewport::delete_edit_at() has confirmed the engine's own
  // checkpoint-restore-and-replay actually dropped that edit GPU-side.
  std::optional<SculptStampRecord> remove_stamp_at(size_t index) {
    if (index >= stamps_.size()) {
      return std::nullopt;
    }
    SculptStampRecord removed = stamps_[index];
    stamps_.erase(stamps_.begin() + static_cast<std::ptrdiff_t>(index));
    return removed;
  }

  // M4c (Move-of-an-arbitrary-edit): the tool-side mirror of
  // SculptVolume::update_stamp_center() (engine/src/resources/
  // sculpt_volume.h) -- updates the record at `index`'s center in place,
  // leaving its shape/params/smoothness/op/material_slot/looseness and its
  // position in stamps_ untouched. Kept in lockstep with the engine the
  // same way remove_stamp_at() always has: ModellingWindow only calls this
  // AFTER SceneViewport's move-edit call has confirmed the engine's own
  // checkpoint-restore-and-replay actually moved that edit GPU-side.
  // Returns false if index is out of range.
  bool update_stamp_center(size_t index, glm::vec3 new_center) {
    if (index >= stamps_.size()) {
      return false;
    }
    stamps_[index].center = new_center;
    return true;
  }

  const std::vector<SculptStampRecord> &stamps() const noexcept {
    return stamps_;
  }

  bool empty() const noexcept { return stamps_.empty(); }

  // Plain text, one stamp per line, v3 (M3/Looseness) format -- see this
  // header's own comment. Returns false if the file couldn't be opened for
  // writing.
  bool save(const std::string &path) const {
    std::ofstream out(path);
    if (!out) {
      return false;
    }
    out << "# modelling_software_v2 sculpt stamps (v3/M3)\n";
    out << "# x y z shape_type p0 p1 p2 p3 smoothness op material_slot "
          "looseness\n";
    for (const SculptStampRecord &stamp : stamps_) {
      out << stamp.center.x << ' ' << stamp.center.y << ' ' << stamp.center.z
          << ' ' << static_cast<i32>(stamp.shape_type) << ' ' << stamp.params.x
          << ' ' << stamp.params.y << ' ' << stamp.params.z << ' '
          << stamp.params.w << ' ' << stamp.smoothness << ' '
          << static_cast<i32>(stamp.op) << ' ' << stamp.material_slot << ' '
          << stamp.looseness << '\n';
    }
    return static_cast<bool>(out);
  }

  // Replaces stamps_ with whatever parses out of path (lines starting with
  // '#' or blank lines are skipped). Each non-comment line is parsed by
  // COLUMN COUNT: 12 columns is the current (v3/M3) format; 11 columns is
  // the old v2/M2a format (no `looseness`), upgraded on the fly to
  // looseness=0.0; exactly 5 columns is the old M1b format ("x y z radius
  // material_slot"), upgraded on the fly to {shape=Sphere,
  // params=(radius,0,0,0), smoothness=0, op=Union, looseness=0} -- the
  // exact stamp that old line used to produce back when
  // renderer_sculpt_stamp_sphere() was the only call this tool made. A line
  // matching none of these column counts is skipped (same "tolerate and
  // skip" policy the original parser already had for anything
  // std::istringstream couldn't fully extract). Returns false if the file
  // couldn't be opened for reading -- stamps_ is left untouched in that
  // case so a failed load doesn't silently wipe out an in-progress session.
  bool load(const std::string &path) {
    std::ifstream in(path);
    if (!in) {
      return false;
    }
    std::vector<SculptStampRecord> loaded;
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') {
        continue;
      }
      std::istringstream count_iss(line);
      std::vector<std::string> tokens;
      std::string tok;
      while (count_iss >> tok) {
        tokens.push_back(tok);
      }

      std::istringstream iss(line);
      if (tokens.size() == 12) {
        SculptStampRecord stamp;
        i32 shape_type_raw = 0;
        i32 op_raw = 0;
        if (iss >> stamp.center.x >> stamp.center.y >> stamp.center.z >>
            shape_type_raw >> stamp.params.x >> stamp.params.y >>
            stamp.params.z >> stamp.params.w >> stamp.smoothness >> op_raw >>
            stamp.material_slot >> stamp.looseness) {
          stamp.shape_type = static_cast<SculptBrushShape>(shape_type_raw);
          stamp.op = static_cast<SculptBrushOp>(op_raw);
          loaded.push_back(stamp);
        }
      } else if (tokens.size() == 11) {
        SculptStampRecord stamp;
        i32 shape_type_raw = 0;
        i32 op_raw = 0;
        if (iss >> stamp.center.x >> stamp.center.y >> stamp.center.z >>
            shape_type_raw >> stamp.params.x >> stamp.params.y >>
            stamp.params.z >> stamp.params.w >> stamp.smoothness >> op_raw >>
            stamp.material_slot) {
          stamp.shape_type = static_cast<SculptBrushShape>(shape_type_raw);
          stamp.op = static_cast<SculptBrushOp>(op_raw);
          stamp.looseness = 0.0f;
          loaded.push_back(stamp);
        }
      } else if (tokens.size() == 5) {
        // v1 (M1b) back-compat: "x y z radius material_slot".
        SculptStampRecord stamp;
        f32 radius = 0.0f;
        if (iss >> stamp.center.x >> stamp.center.y >> stamp.center.z >>
            radius >> stamp.material_slot) {
          stamp.shape_type = SculptBrushShape::Sphere;
          stamp.params = glm::vec4(radius, 0.0f, 0.0f, 0.0f);
          stamp.smoothness = 0.0f;
          stamp.op = SculptBrushOp::Union;
          stamp.looseness = 0.0f;
          loaded.push_back(stamp);
        }
      }
      // Any other column count: skip the line, same tolerate-and-skip
      // policy malformed lines already got.
    }
    stamps_ = std::move(loaded);
    return true;
  }

private:
  std::vector<SculptStampRecord> stamps_;
};
