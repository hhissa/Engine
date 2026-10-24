#pragma once

#include "../defines.h"

typedef enum renderer_backend_type {
  RENDERER_BACKEND_TYPE_VULKAN,
  RENDERER_BACKEND_TYPE_OPENGL,
  RENDERER_BACKEND_TYPE_DIRECTX
} renderer_backend_type;

struct render_packet {
  f32 delta_time;
};

// Identifies one renderer_load_scene() call -- every primitive that call
// registered is tracked under this handle, so renderer_remove_scene() can
// unload exactly that scene's primitives without touching any other
// concurrently loaded scene. 0 (kInvalidSceneHandle) is never a valid
// handle -- renderer_load_scene() returns it on failure (missing/malformed
// file).
using SceneHandle = u32;
constexpr SceneHandle kInvalidSceneHandle = 0;

// How the renderer uses the chunked field's baked surface-point cloud for
// primary visibility -- the Dreams-style point-splatting technique (see
// Builtin.ChunkPointSplat.comp.glsl's header comment). Lives here, in the
// shared types header, so the frontend API and the backend interface can
// both name it without either including the Vulkan-specific headers;
// VulkanRaymarchShader::SplatMode mirrors these values exactly (a
// static_assert in vulkan_backend.cpp pins them together).
//
// Only meaningful while the chunked field is enabled (see
// renderer_set_chunked_field_enabled()) -- the fixed-cube field bakes no
// points, so nothing splats there regardless.
enum class RendererSplatMode : i32 {
  // No splat prepass; every primary ray marches from the camera, exactly
  // as this renderer always did.
  Off = 0,
  // Splat, then start each primary ray just short of its pixel's nearest
  // splat. Same image as Off (the starts are conservative and uncovered
  // pixels start at 0), less empty space marched. The default.
  Prime = 1,
  // Shade the winning splat directly -- no primary march for a covered
  // pixel. Pixels no splat covers fall back to Prime, so the image stays
  // complete with no temporal accumulation needed.
  Visibility = 2,
};

// M2a: the 15 analytic brush shapes a sculpt stamp can evaluate -- see
// Builtin.BrushShapes.inc.glsl for each shape's underlying *_sdf() and
// Builtin.SculptStamp.comp.glsl's SHAPE_* constants, which these values
// must match exactly (nothing in C++ checks that half of the lockstep).
// Lives here, in the shared types header, rather than inside
// VulkanRaymarchShader, for the same reason RendererSplatMode does (so the
// frontend/backend interface can name it without pulling in Vulkan
// headers) -- VulkanRaymarchShader::BrushShape mirrors these values
// exactly, with a static_assert in vulkan_backend.cpp pinning them
// together, matching RendererSplatMode/SplatMode's own established
// pattern. Brush ROTATION is out of scope for M2a -- every shape stamps in
// its canonical, axis-aligned local orientation.
enum class SculptBrushShape : i32 {
  Sphere = 0,
  Box,
  Plane,
  Torus,
  CappedCylinder,
  CappedCone,
  RoundBox,
  BoxFrame,
  Octahedron,
  Pyramid,
  HexPrism,
  RoundCone,
  Capsule,
  Link,
  Ellipsoid,
};

// M2a: how a sculpt stamp blends its shape against whatever a touched
// voxel already holds -- see smooth_union()/smooth_subtraction() in
// Builtin.BrushShapes.inc.glsl. Must match Builtin.SculptStamp.comp.glsl's
// OP_* constants and VulkanRaymarchShader::BrushOp's mirror exactly (same
// lockstep note as SculptBrushShape above).
//
// Union and Subtraction act on the whole sculpt, in stamp order: a
// Subtraction carves everything stamped before it, whichever object that
// belongs to. The Object* ops are scoped to their own object instead (the
// controller's sculpt mode builds each sculpture from them): an object made
// of them is built up on its own and only then merged with the rest (a hard
// union), so its carves and crops never touch anything else.
//   ObjectUnion         adds the shape to the object
//   ObjectSubtraction   carves the shape out of the object
//   ObjectIntersection  keeps only the part of the object inside the shape
//                       (everything stamped into it before this)
//   ObjectPaint         no geometry: the object's surface inside the shape
//                       takes this stamp's material. Its looseness is
//                       its opacity instead: floor() a per-stroke seed,
//                       fract() 1 - opacity (0 = opaque); see
//                       paint_covers() in Builtin.SculptChunkRecompute
// An object's Object* stamps must be contiguous in the stamp list.
enum class SculptBrushOp : i32 {
  Union = 0,
  Subtraction = 1,
  ObjectUnion = 2,
  ObjectSubtraction = 3,
  ObjectIntersection = 4,
  ObjectPaint = 5,
};

// Whether a stamp with this op adds geometry.
inline bool sculpt_op_adds(SculptBrushOp op) {
  return op == SculptBrushOp::Union || op == SculptBrushOp::ObjectUnion;
}
