#pragma once
// Engine-facing sculpt helpers shared by BOTH of this tool's front ends --
// the Qt editor (SceneViewport) and the Qt-free controller mode
// (controller_mode.cpp). Nothing in here may include Qt: controller mode
// must run without ever touching it.
#include <defines.h>
#include <renderer/renderer_frontend.h>
#include <renderer/renderer_types.inl> // SculptBrushShape/SculptBrushOp
#include "sculpt_document.h"

#include <glm/glm.hpp>

#include <unordered_map>
#include <vector>

namespace sculpt_shared {

// M2a: maps the brush panel's ONE generic "size" number onto whichever
// subset of a shape's actual SDF params it needs -- see
// VulkanRaymarchShader::stamp_shape()'s header comment for what each
// shape's params.x/y/z/w mean. This is deliberately not a polished
// authoring experience (the plan's own M2a section says as much): proving
// shape/smoothness/subtraction actually work end to end only needs ONE
// slider, not 15 bespoke per-shape param panels. The mapping:
//   - Sphere: size is the radius directly.
//   - Box/RoundBox/Ellipsoid: size applies UNIFORMLY to all 3 axes (a
//     cube/uniform ellipsoid, not an elongated one) -- RoundBox's corner
//     radius is a fixed 0.2x fraction of size.
//   - BoxFrame: same uniform half_extents as Box, edge_thickness a fixed
//     0.2x fraction of size.
//   - Two-"radius"-like shapes (Torus/Capsule/CappedCylinder): the
//     secondary/minor radius is a fixed 0.3x fraction of size, exactly the
//     ratio the plan's own M2a section suggests ("minor_radius = size *
//     0.3") -- generalized to every shape in this family rather than
//     picked ad hoc per shape.
//   - CappedCone/RoundCone (two end radii + a height/half-height): the
//     wide end is 0.5x size, the narrow end 0.15x size, so the brush
//     visibly tapers instead of degenerating into a cylinder.
//   - Octahedron/Pyramid/HexPrism/Link: straightforward single- or
//     double-extent shapes where size (and, for Link, a 0.4x/0.15x split
//     for its two radii) is simply each extent-ish param directly.
// None of this needs to be tight or pretty -- it only has to put each of
// the 15 shapes somewhere visibly distinct and reasonably sized so a
// stamp of any of them is recognizable on screen.
inline glm::vec4 brush_params_from_size(SculptBrushShape shape_type, f32 size) {
  switch (shape_type) {
  case SculptBrushShape::Sphere:
    return glm::vec4(size, 0.0f, 0.0f, 0.0f);
  case SculptBrushShape::Box:
    return glm::vec4(size, size, size, 0.0f);
  case SculptBrushShape::Plane:
    return glm::vec4(size, 0.0f, 0.0f, 0.0f);
  case SculptBrushShape::Torus:
    return glm::vec4(size, size * 0.3f, 0.0f, 0.0f);
  case SculptBrushShape::CappedCylinder:
    return glm::vec4(size * 0.3f, size, 0.0f, 0.0f);
  case SculptBrushShape::CappedCone:
    return glm::vec4(size, size * 0.5f, size * 0.15f, 0.0f);
  case SculptBrushShape::RoundBox:
    return glm::vec4(size, size, size, size * 0.2f);
  case SculptBrushShape::BoxFrame:
    return glm::vec4(size, size, size, size * 0.2f);
  case SculptBrushShape::Octahedron:
    return glm::vec4(size, 0.0f, 0.0f, 0.0f);
  case SculptBrushShape::Pyramid:
    return glm::vec4(size, size, 0.0f, 0.0f);
  case SculptBrushShape::HexPrism:
    return glm::vec4(size, size, 0.0f, 0.0f);
  case SculptBrushShape::RoundCone:
    return glm::vec4(size * 0.5f, size * 0.15f, size, 0.0f);
  case SculptBrushShape::Capsule:
    return glm::vec4(size * 0.3f, size, 0.0f, 0.0f);
  case SculptBrushShape::Link:
    return glm::vec4(size, size * 0.4f, size * 0.15f, 0.0f);
  case SculptBrushShape::Ellipsoid:
    return glm::vec4(size, size, size, 0.0f);
  default:
    return glm::vec4(size, 0.0f, 0.0f, 0.0f);
  }
}

// M4a (Stretch): multiplies params.x/y/z by the brush panel's three
// Stretch sliders, ON TOP of whatever brush_params_from_size() already
// computed -- deliberately generic across all 15 shapes rather than
// switched per shape_type, same spirit as brush_params_from_size() itself
// not needing to be a polished authoring experience. params.w is NEVER
// touched (RoundBox's corner radius, BoxFrame's edge thickness) -- there
// is no fourth "Stretch W" slider, and those two are a roundness/thickness
// concept, not a per-axis extent, so leaving them fixed relative to the
// brush's un-stretched size is the more sensible default behaviour, not
// an oversight. For shapes whose params.x/y/z aren't literally "the
// brush's extent along world X/Y/Z" (e.g. Torus: x=major radius, y=minor
// radius; RoundCone: x=r1, y=r2, z=half_height) this still applies the
// multiply positionally (stretch_x onto whatever occupies params.x, and so
// on) rather than attempting a shape-aware "real" non-uniform transform --
// see brush_params_from_size()'s own per-shape table for exactly which
// components are meaningful vs. always-zero (hence a no-op) for each
// shape. This is the documented, accepted scope for M4a (no gizmo/local-
// axis-drag concept exists in this tool at all).
inline glm::vec4 apply_stretch(glm::vec4 params, f32 stretch_x, f32 stretch_y,
                        f32 stretch_z) {
  params.x *= stretch_x;
  params.y *= stretch_y;
  params.z *= stretch_z;
  return params;
}

// Turns on the chunked field the sculpt path needs and sets up the default
// sky + lights. Call once, right after the renderer is initialized.
inline void setup_sculpt_environment() {
  renderer_set_chunked_field_enabled(true);

  // Default lighting/environment -- without these the viewport was
  // visibly unlit (zero lights registered, no skybox: a leftover gap from
  // the M0 CSG deletion pass, which removed SdfScene::lights/
  // rebuild_static_scene() and never replaced that wiring for this tool).
  // Every M5 Finish (roughness/metallic/waxy) needs a real light to show
  // anything beyond flat ambient/skybox reflection -- see renderer_add_
  // directional_light()'s own doc comment in renderer_frontend.h.
  // sky_clearday is a real asset (bin/assets/textures/sky_clearday.png).
  //
  // M5 fix (Finishes visual-quality pass): a single directional "sun" left
  // every Finish showing at most one highlight, and for a rough/high-
  // roughness material that single highlight is wide and dim -- easy to
  // miss entirely on a Rough Metal (see /tmp material_harness screenshots
  // from this pass, MH_SCENARIO=metal under 1 vs 3 lights). Switched to a
  // simple 3-point product-shot rig instead of one sun: KEY keeps the
  // original sun's direction/colour/intensity exactly (so a Default-finish
  // stamp's dominant lighting direction doesn't change), FILL is a cooler,
  // dimmer light from the opposite side (lifts the shadow side instead of
  // leaving it near-black, standard 3-point practice), and RIM is from
  // behind/above (picks out silhouette edges and -- just as importantly
  // for a showcase tool -- gives every sphere a SECOND specular angle, so
  // a highlight landing outside the key light's cone on a curved surface
  // still has a second chance to be visible). This alone is NOT enough to
  // fix Rough Metal on its own (confirmed empirically: 3 lights still left
  // it near-black) -- see Builtin.DeferredShade.comp.glsl's own
  // "METAL_AMBIENT_SCALE" fix for the other half.
  renderer_enable_sky_box("sky_clearday");
  renderer_add_directional_light(glm::vec3(-0.4f, -0.8f, -0.3f),
                                 glm::vec3(1.0f, 0.96f, 0.9f), 2.5f);
  renderer_add_directional_light(glm::vec3(0.6f, -0.3f, 0.5f),
                                 glm::vec3(0.75f, 0.85f, 1.0f), 1.1f);
  renderer_add_directional_light(glm::vec3(0.1f, 0.6f, -0.9f),
                                 glm::vec3(1.0f, 1.0f, 1.0f), 0.9f);
}

// Stamps every record of `document` into the engine, in order, keeping its
// objects as objects. A file's object ids are mapped onto fresh engine ids
// (a load can land on top of an existing session), and the document's
// records are rewritten to the engine ids so later saves stay consistent.
// Leaves the engine's current object as it was.
// By default all of the document's stamps go to the engine in one batch,
// rebuilt once (one renderer_sculpt_stamp_shape() at a time costs a
// device-stalling dispatch per stamp: ~12ms each, 20s for 5000 stamps).
// one_by_one keeps the old per-stamp replay, which the Qt editor needs:
// it lets Ctrl+Z take loaded stamps back one at a time through the
// engine's incremental undo, which a batch doesn't record.
// Returns which engine object each file object id became.
inline std::unordered_map<i32, i32>
replay_document(SculptDocument &document, bool one_by_one = false) {
  if (one_by_one) {
    const i32 previous_object = renderer_sculpt_current_object();
    std::unordered_map<i32, i32> engine_id_for;
    for (SculptStampRecord &stamp : document.mutable_stamps()) {
      auto it = engine_id_for.find(stamp.object_id);
      if (it == engine_id_for.end()) {
        it = engine_id_for
                 .emplace(stamp.object_id, renderer_sculpt_begin_object())
                 .first;
      }
      stamp.object_id = it->second;
      renderer_sculpt_set_current_object(stamp.object_id);
      renderer_sculpt_stamp_shape(stamp.center, stamp.shape_type,
                                  stamp.params, stamp.smoothness, stamp.op,
                                  stamp.material_slot, stamp.looseness,
                                  stamp.rotation);
    }
    renderer_sculpt_set_current_object(previous_object);
    return engine_id_for;
  }
  const i32 previous_object = renderer_sculpt_current_object();
  std::unordered_map<i32, i32> engine_id_for;
  std::vector<i32> object_ids;
  std::vector<SculptStampInfo> stamps;
  object_ids.reserve(document.stamps().size());
  stamps.reserve(document.stamps().size());
  for (SculptStampRecord &stamp : document.mutable_stamps()) {
    auto it = engine_id_for.find(stamp.object_id);
    if (it == engine_id_for.end()) {
      it = engine_id_for.emplace(stamp.object_id, renderer_sculpt_begin_object())
               .first;
    }
    stamp.object_id = it->second;
    SculptStampInfo info;
    info.center = stamp.center;
    info.shape_type = stamp.shape_type;
    info.params = stamp.params;
    info.smoothness = stamp.smoothness;
    info.op = stamp.op;
    info.material_slot = stamp.material_slot;
    info.looseness = stamp.looseness;
    info.rotation = stamp.rotation;
    object_ids.push_back(stamp.object_id);
    stamps.push_back(info);
  }
  if (!stamps.empty()) {
    renderer_sculpt_add_stamps_to_objects(object_ids.data(), stamps.data(),
                                          static_cast<i32>(stamps.size()));
  }
  renderer_sculpt_set_current_object(previous_object);
  return engine_id_for;
}

} // namespace sculpt_shared
