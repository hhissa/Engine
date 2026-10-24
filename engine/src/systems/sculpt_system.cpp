#include "sculpt_system.h"
#include "../core/logger.h"

void SculptSystem::apply_stamp(glm::vec3 center, SculptBrushShape shape_type,
                               glm::vec4 params, f32 smoothness,
                               SculptBrushOp op, i32 material_slot,
                               i32 cells_touched, f32 looseness,
                               glm::quat rotation, i32 object_id) {
  // -1: applied later by a recompute pass (VulkanRendererBackend::
  // add_stamps()), which doesn't count per stamp.
  if (cells_touched == 0) {
    KWARN("SculptSystem::apply_stamp() recorded a stamp at ({:.3f}, {:.3f}, "
         "{:.3f}) that touched no cells (GPU side rejected or truncated "
         "it) -- recording it anyway for bookkeeping symmetry with "
         "VulkanRaymarchShader's own return value.",
         center.x, center.y, center.z);
  }
  volume_.add_stamp(center, shape_type, params, smoothness, op,
                    material_slot, cells_touched, looseness, rotation,
                    object_id);
}
