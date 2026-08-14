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
// Top-level "volumetric NAME { ... }" blocks (also siblings of "layer"
// blocks) describe SdfVolumetricDef entries: a shape (any SdfPrimitiveType,
// same type=/position=/rotation=/params= keys a primitive block uses),
// plus density= and material=. Unlike a primitive, a volumetric is never
// baked into the opaque voxel field -- rays pass straight through it -- so
// it renders as a transparent, textured glow (its material's diffuse
// colour/texture) instead of a solid surface, the shape and texture giving
// it a "god ray"/light-shaft look. See GeometrySystem::acquire_volumetric()
// and accumulate_volumetrics() in Builtin.RaymarchShader.comp.glsl for how
// it's actually evaluated.
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
// Any primitive may also carry "repetition=<mode>" (none/infinite/limited/
// rotational/rectangular; omitted means none), "repetition_cell=x y z", and
// "repetition_count=x y z" -- see SdfRepetitionMode/SdfPrimitiveDef::
// repetition_mode's comments for what each mode does with cell/count.
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

// Domain repetition (Inigo Quilez, https://iquilezles.org/articles/
// sdfrepetition/): evaluates a primitive's shape at one or more repeated
// copies of the local-space sample point instead of just the point itself,
// applied after position/rotation (see SdfPrimitiveDef::repetition_mode's
// comment for exactly what each mode does, and primitive_sdf()/repeat_*() in
// Builtin.SdfSceneCommon.inc.glsl for the actual per-mode point-folding
// math).
enum class SdfRepetitionMode : u32 {
  None = 0,
  Infinite = 1,
  Limited = 2,
  Rotational = 3,
  Rectangular = 4,
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
  // Domain deformation (Inigo Quilez, https://iquilezles.org/articles/
  // distfunctions/ "Deforming" section) -- warps this primitive's own
  // local-space sample point before its shape function runs (twist/bend)
  // or perturbs the resulting distance (displacement), applied in that
  // order: twist, then bend, then the shape, then + displacement. All
  // default to their identity/no-op value, so an existing file with none
  // of these lines renders exactly as before.
  //   twist: radians of rotation per world-unit of local Y, around local Y
  //     (rotates local.xz by twist*local.y). 0 = no twist.
  //   bend: radians of rotation per world-unit of local X, around local Z
  //     (rotates local.xy by bend*local.x, applied after twist). 0 = no bend.
  //   displace_amplitude: added straight onto the shape's distance as
  //     displace_amplitude * sin(f*x)*sin(f*y)*sin(f*z) (f =
  //     displace_frequency below), evaluated at the *pre*-twist/bend local
  //     point -- an approximate, non-exact perturbation (Quilez's own
  //     "Warning!" on that article: this breaks the distance field's
  //     Lipschitz-1 guarantee), same caveat as Twist/Bend above. 0 = no
  //     displacement, regardless of displace_frequency.
  //   displace_frequency: the sin() rate above. Defaults to 20 (this
  //     engine's stand-in for "a reasonable ripple density", matching
  //     Quilez's own example) purely so a freshly nonzero
  //     displace_amplitude alone already looks like something -- has no
  //     effect while displace_amplitude is 0.
  f32 twist = 0.0f;
  f32 bend = 0.0f;
  f32 displace_amplitude = 0.0f;
  f32 displace_frequency = 20.0f;
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
  // Domain repetition -- see SdfRepetitionMode's comment for the technique,
  // and the meaning of repetition_cell/repetition_count below for each mode.
  // Applied in this primitive's own local space, after position/rotation --
  // a repeated primitive can still be positioned/oriented as a whole exactly
  // like an unrepeated one.
  //   None (default): unrepeated -- repetition_cell/repetition_count are
  //     ignored entirely.
  //   Infinite: repeats forever every repetition_cell.axis units, per axis
  //     independently -- an axis with repetition_cell.axis <= 0 is left
  //     unrepeated. repetition_count is ignored.
  //   Limited: same as Infinite, but capped to repetition_count.axis copies
  //     per axis (a 3D box grid) -- an axis with repetition_count.axis <= 1
  //     keeps exactly one, centered instance.
  //   Rotational: repetition_count.x (rounded, >= 2) evenly-spaced copies
  //     around this primitive's own local Y axis -- compose with `rotation`
  //     to repeat around any axis/orientation instead. repetition_cell and
  //     repetition_count.y/z are ignored.
  //   Rectangular: a 2D grid confined to the local XZ plane (Y untouched) --
  //     repetition_cell.xz spacing, repetition_count.xz copies; the .y
  //     component of both is ignored. The common "tile the ground" case; use
  //     Limited instead for a full 3D grid.
  SdfRepetitionMode repetition_mode = SdfRepetitionMode::None;
  glm::vec3 repetition_cell{1.0f};
  glm::vec3 repetition_count{1.0f};
  std::string material_name;
};

struct SdfLayerDef {
  std::string name;
  SdfLayerOperation operation = SdfLayerOperation::Union;
  f32 smoothness = 0.0f;
  std::vector<SdfPrimitiveDef> primitives;
};

// A transparent, textured "volumetric light" shape -- e.g. a cone or
// capped-cylinder standing in for a visible light shaft/god ray. Shares
// SdfPrimitiveType/position/rotation/params/extra_param with SdfPrimitiveDef
// (the same shape catalogue applies), but is never combined into a layer:
// it has no operation/smoothness, and GeometrySystem never bakes it into
// the opaque voxel field -- see the file header comment above.
struct SdfVolumetricDef {
  std::string name;
  SdfPrimitiveType type = SdfPrimitiveType::Box;
  glm::vec3 position{0.0f};
  glm::vec3 rotation{0.0f};
  glm::vec3 params{1.0f};
  f32 extra_param = 0.0f;
  // How strongly this shape accumulates its material's tinted/textured
  // glow per world unit the primary ray travels through it -- see
  // accumulate_volumetrics() in Builtin.RaymarchShader.comp.glsl. Higher
  // reads as a denser/brighter shaft; 0 would be fully invisible.
  f32 density = 1.0f;
  std::string material_name;
};

struct SdfScene {
  std::vector<SdfLayerDef> layers; // In file order -- this is also
                                  // evaluation order (see above).
  std::vector<SdfLightDef> lights; // Order doesn't matter -- lighting sums
                                   // every light's contribution equally.
  std::vector<SdfVolumetricDef> volumetrics; // Order doesn't matter -- each
                                             // renders independently (see
                                             // SdfVolumetricDef above).
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
