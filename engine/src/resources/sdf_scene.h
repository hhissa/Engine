#pragma once
#include "../defines.h"

#include <array>
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A pure data/parsing module: turns a .sdf scene file into an in-memory
// description, with no knowledge of GeometrySystem, materials, or the
// renderer. GeometrySystem::load_scene() is what actually turns this into
// registered geometry/lights -- see that for how
// SdfPrimitiveType/SdfLayerOperation/SdfLightType map onto its own
// PrimitiveType/LayerOperation/LightType.
//
// File format (see assets/scenes/*.sdf for real examples): a sequence of
// brace-delimited layer blocks, each containing one or more primitive
// blocks:
//
//   #sdf scene file
//   version=0.1
//
//   layer ground {
//       operation=union
//       smoothness=0.0
//
//       primitive floor {
//           type=plane
//           height=-1.4
//           material=test_material
//       }
//   }
//
//   light sun {
//       type=directional
//       direction=0.6 0.7 -0.6
//       colour=1.0 1.0 1.0
//       intensity=0.85
//   }
//
// Top-level "light NAME { ... }" blocks (siblings of "layer" blocks, not
// nested inside one) describe SdfLightDef entries -- type is "directional"
// or "point", with direction=/position=/colour=/intensity= read into the
// matching fields (position= is ignored for a directional light and vice
// versa). A top-level "ambient=0.15" line sets SdfScene::ambient. A file
// with no light blocks at all still renders lit -- see
// VulkanRaymarchShader::rebuild_static_scene()'s fallback default light.
//
// A rotatable primitive (anything but Plane) may also carry a
// "rotation=x y z" line (Euler angles in radians, XYZ order); omitted means
// the identity rotation.
//
// Any primitive (of any type) may also carry zero or more
// "param_expr=<slot> <formula>" lines -- <slot> is 0/1/2/3 (params.x/y/z/
// extra_param), and everything after the following space is the formula
// text verbatim (see engine/src/resources/expression.h for the syntax,
// e.g. "param_expr=1 0.1 + 0.1*p.y"). A slot with no param_expr line just
// uses its plain constant, same as before parametric attributes existed.
//

// Sphere/Box/Plane keep their own named keys (radius=/half_extents=/
// height=) below for backward compatibility with older files; every other
// primitive type is configured with a single generic
// "params=x y z w" line instead (4 floats -- see SdfPrimitiveDef::params/
// extra_param below for what each type reads out of it, and
// Builtin.RaymarchVoxelize.comp.glsl's primitive_sdf() for the exact
// distance function each one evaluates).
//
// Layers are evaluated in file order. Every primitive inside a layer is
// folded into the scene built up so far using *that layer's* operation
// (union or subtraction) and smoothness (0 = a hard edge; > 0 = a
// smooth/rounded blend radius) -- a layer can hold as many primitives as
// you like, and the operation applies to every one of them individually,
// not once for the whole layer. A subtraction layer with 3 primitives
// carves 3 independent (optionally rounded) notches, each into whatever
// the scene looked like at that point; a union layer with several
// primitives smooth-blends all of them together, not just against earlier
// layers. This is why subtraction only ever affects shapes on the same
// layer as the cutting primitives: the layer boundary is the boolean
// operation boundary.
// Every distance function is evaluated in the primitive's own local space
// (world position subtracted, then rotated by the inverse of `rotation` --
// see primitive_sdf() in Builtin.RaymarchVoxelize.comp.glsl), except Plane,
// which never rotates and is always the horizontal y=height plane.
// Adapted from Inigo Quilez's SDF primitive catalogue
// (https://iquilezles.org/articles/distfunctions/); primitives needing
// arbitrary point pairs instead of a single position+rotation (Triangle,
// Quad, Vesica, and the point-to-point Capsule/Cylinder/Cone/RoundCone
// variants), or unbounded ones (infinite Cylinder/Cone, Solid Angle), or
// niche booleans (Death Star, Cut (Hollow) Sphere, Rhombus, Capped Torus)
// aren't included -- they don't fit this engine's one-primitive-per-
// position+rotation+params model the way every type below does.
enum class SdfPrimitiveType : u32 {
  Sphere = 0,
  Box = 1,
  Plane = 2,
  Torus = 3,
  CappedCylinder = 4,
  CappedCone = 5,
  RoundBox = 6,
  BoxFrame = 7,
  Octahedron = 8,
  Pyramid = 9,
  HexPrism = 10,
  RoundCone = 11,
  Capsule = 12,
  Link = 13,
  Ellipsoid = 14,
};

enum class SdfLayerOperation : u32 {
  Union = 0,
  Subtraction = 1,
};

// A Directional light has no position -- it shines uniformly from
// `direction` (doesn't need to be pre-normalized) with no falloff, like the
// sun. A Point light shines from `position` in every direction with
// inverse-square falloff, like a bulb. See GpuLight/the lighting loop in
// Builtin.RaymarchShader.comp.glsl for exactly how each is evaluated.
enum class SdfLightType : u32 {
  Directional = 0,
  Point = 1,
};

struct SdfLightDef {
  std::string name;
  SdfLightType type = SdfLightType::Directional;
  glm::vec3 direction{0.6f, 0.7f, -0.6f}; // Directional only.
  glm::vec3 position{0.0f};              // Point only.
  glm::vec3 colour{1.0f};
  // Directional: multiplies the diffuse term directly. Point: multiplies
  // the inverse-square-falloff term (i.e. roughly "brightness at 1 world
  // unit away").
  f32 intensity = 1.0f;
};

struct SdfPrimitiveDef {
  std::string name;
  SdfPrimitiveType type = SdfPrimitiveType::Sphere;
  glm::vec3 position{0.0f};
  // Euler angles, radians (XYZ order). Meaningless for Plane -- a plane is
  // always the horizontal y=height plane (see GeometryConfig::plane()/
  // add_plane()), same as position.
  glm::vec3 rotation{0.0f};
  // Meaning is entirely per-type -- see SdfPrimitiveType's own comment and
  // primitive_sdf() in Builtin.RaymarchVoxelize.comp.glsl for exactly what
  // each type reads out of params.xyz/extra_param:
  //   Sphere: x=radius.
  //   Box: xyz=half-extents.
  //   Plane: x=world-space Y height.
  //   Torus: x=major radius, y=minor radius.
  //   CappedCylinder: x=radius, y=half-height.
  //   CappedCone: x=half-height, y=base radius, z=tip radius.
  //   RoundBox: xyz=half-extents, extra_param=corner radius.
  //   BoxFrame: xyz=half-extents, extra_param=edge thickness.
  //   Octahedron: x=size.
  //   Pyramid: x=height (position is the base center, apex sits above it --
  //     not a centroid, matching the source formula's own convention).
  //   HexPrism: x=inradius, y=half-height.
  //   RoundCone: x=base radius, y=tip radius, z=half-height.
  //   Capsule: x=radius, y=half-height.
  //   Link: x=half-length, y=inner radius, z=thickness.
  //   Ellipsoid: xyz=radii (bound, not exact -- see ellipsoid_sdf() in
  //     Builtin.RaymarchVoxelize.comp.glsl).
  glm::vec3 params{1.0f};
  // 4th scalar parameter, only meaningful for RoundBox/BoxFrame above (see
  // params' comment) -- broken out as its own field rather than a vec4
  // params so every other type's existing `params.x/y/z` reads didn't need
  // touching.
  f32 extra_param = 0.0f;
  // Optional "parametric attribute" per params slot (index 0/1/2 ->
  // params.x/y/z, index 3 -> extra_param): a formula in p.x/p.y/p.z
  // (evaluated at the primitive's own local-space sample point -- see
  // params' comment) that overrides the plain constant for that slot when
  // non-empty. See engine/src/resources/expression.h for the supported
  // syntax; a slot left empty (the default) just uses its constant, same
  // as before this existed. Compiled to bytecode once, at GPU-upload time
  // (VulkanRaymarchShader::rebuild_static_scene()) / once per raymarch
  // pick (ray_intersect.h) -- not stored here, since this struct mirrors
  // the on-disk/authored form, not a derived one.
  std::array<std::string, 4> param_expressions;
  std::string material_name;
};

struct SdfLayerDef {
  std::string name;
  SdfLayerOperation operation = SdfLayerOperation::Union;
  f32 smoothness = 0.0f;
  std::vector<SdfPrimitiveDef> primitives;
};

struct SdfScene {
  std::vector<SdfLayerDef> layers; // In file order -- this is also
                                  // evaluation order (see above).
  std::vector<SdfLightDef> lights; // Order doesn't matter -- lighting sums
                                   // every light's contribution equally.
  // Scene-wide ambient factor (added once, not per-light) -- 0 means fully
  // unlit surfaces facing away from every light are pure black; matches the
  // old hardcoded default this replaces.
  f32 ambient = 0.15f;
};

// Parses path (see the format description above). Returns std::nullopt on
// failure (missing file); malformed individual lines are skipped with a
// logged warning rather than failing the whole load, so a modelling tool
// emitting a slightly-off file doesn't lose the rest of the scene.
std::optional<SdfScene> load_sdf_scene(std::string_view path);
