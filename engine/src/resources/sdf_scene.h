#pragma once
#include "../defines.h"
#include "material_def.h"

#include <array>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
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
// versa). A top-level "ambient=0.15" line sets SdfScene::ambient, and a
// top-level "skybox=NAME" line sets SdfScene::skybox. A file
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
// A LAYER block accepts those same three keys (alongside operation=/
// smoothness=), meaning "repeat this whole layer": the fold runs once in
// world space and every primitive in the layer is evaluated at the folded
// point, so the primitives keep their arrangement relative to each other
// instead of each repeating around its own centre. See SdfLayerDef::
// repetition_mode.
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

// Which local axis a bend's rotation angle is driven by, and which axis it
// rotates that one toward -- the "direction" of the bend. XToY is the
// classic opCheapBend (Inigo Quilez, https://iquilezles.org/articles/
// distfunctions/ "Deforming"): the angle grows with local X and swings the
// shape toward +Y, i.e. a bar along X curves into an arc opening toward
// -Y. The other five are that same warp on a different pair of axes, so a
// bend can be aimed without having to rotate the primitive (and its
// repetition, and its parametric-attribute formulas, which all live in the
// same local space) just to reach the axis you wanted.
//
// The naming is <drive>To<target>: angle = bend * local.<drive>, rotating
// local.<drive>/local.<target> so a positive bend pushes toward +<target>.
// Value order is (drive, then the two remaining axes in cyclic order) --
// see bend_axes() in Builtin.SdfSceneCommon.inc.glsl, which decodes it
// arithmetically and so depends on exactly this ordering.
enum class SdfBendAxis : u32 {
  XToY = 0,
  XToZ = 1,
  YToZ = 2,
  YToX = 3,
  ZToX = 4,
  ZToY = 5,
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
  //   Pyramid: x=height, y=base half-extent (0 or omitted -- e.g. a scene
  //     saved before this param existed -- falls back to 0.5, the old
  //     hardcoded value). Position is the base center, apex sits above it
  //     -- not a centroid, matching the source formula's own convention.
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
  //   bend: radians of rotation per world-unit along the bend_axis' drive
  //     axis, rotating that axis toward the target one (rotates local.xy by
  //     bend*local.x for the XToY default -- see SdfBendAxis for the other
  //     five directions -- applied after twist). 0 = no bend, whatever
  //     bend_axis says.
  //   bend_axis: which pair of local axes the bend above warps, and which
  //     way round. Defaults to XToY, the behaviour bend had before this
  //     existed, so no file changes meaning by gaining a default.
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
  SdfBendAxis bend_axis = SdfBendAxis::XToY;
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
  // --- Material binding ---------------------------------------------
  //
  // A primitive names its material by ID (see MaterialId, material_def.h),
  // referring to one of SdfScene::materials. That indirection is what
  // makes a material an editable, renameable thing rather than a value
  // tuple encoded into a filename -- see material_def.h's header for the
  // full reasoning.
  //
  // kInvalidMaterialId means this primitive uses a LEGACY material
  // instead: material_name below names an assets/materials/<name>.kmt
  // file directly, the way every scene written before the material
  // library existed does. Both forms are read; only the id form is
  // written, so a scene converts the first time it is saved and never
  // half-converts. See sdf_scene_resolve_material().
  MaterialId material_id = kInvalidMaterialId;
  // Sparse per-primitive tweaks applied on top of the referenced
  // material. The alternative to overrides is forking -- duplicating a
  // material to change one value -- which is exactly how the old scheme
  // accumulated 949 files for 93 distinct materials.
  std::vector<MaterialOverride> material_overrides;
  // Legacy .kmt name; empty once material_id is set. During parsing this
  // also holds an as-yet-unresolved id string, because a "material=" line
  // may appear before the material block it names (ids are resolved in one
  // pass after the whole file is read -- see load_sdf_scene()).
  std::string material_name;
};

struct SdfLayerDef {
  std::string name;
  SdfLayerOperation operation = SdfLayerOperation::Union;
  f32 smoothness = 0.0f;
  // The layer's own rigid transform ("position="/"rotation=" inside a layer
  // block), applied to every primitive in it as a group: each one's
  // authored position/rotation is read as LAYER-LOCAL, and what actually
  // renders is this transform composed onto it (rotate about the layer's
  // origin, then translate). Moving a layer moves its whole arrangement
  // without touching a single primitive; rotating one turns the
  // arrangement about the layer's origin rather than spinning each shape
  // in place, which is what a per-primitive rotation already does.
  //
  // rotation is Euler angles in radians (XYZ order), exactly like
  // SdfPrimitiveDef::rotation. Both default to identity, so a layer that
  // has never been moved behaves exactly as it did before this existed and
  // its block is written byte-for-byte unchanged.
  //
  // Composed onto each primitive at the point the scene becomes runtime
  // geometry (GeometrySystem::load_scene()/reconcile_scene(), and
  // raycast_scene() for the editor's picker -- see
  // sdf_layer_world_transform() below) rather than carried separately all
  // the way to the shader. That is what keeps "Geometry::position/rotation
  // are world space" true for every downstream consumer -- the voxel bake,
  // the per-primitive bounding radius, chunk culling, the splat passes --
  // none of which has a layer index to look a transform up with.
  //
  // Two consequences of composing rather than carrying, both deliberate:
  //   - A Plane is left alone entirely. It has no meaningful position or
  //     orientation of its own (see GeometryConfig::plane()) and never
  //     rotates, so a layer transform has nothing to compose onto -- same
  //     rule its own rotation already follows.
  //   - repetition below still folds along WORLD axes, not the rotated
  //     layer's. A repeated, rotated layer therefore steps its copies
  //     along world axes; the copies themselves are correctly rotated.
  glm::vec3 position{0.0f};
  glm::vec3 rotation{0.0f};
  // Domain repetition applied to the layer AS A WHOLE ("repetition="/
  // "repetition_cell="/"repetition_count=" inside a layer block) -- the
  // same five modes, cell and count a primitive's own repetition uses (see
  // SdfRepetitionMode and SdfPrimitiveDef::repetition_mode for what each
  // mode does with them), folded in WORLD space before each primitive's
  // own rotation instead of in its rotated local space.
  //
  // That difference is the whole point of it existing alongside the
  // per-primitive one. Repeating each primitive individually repeats each
  // around its own centre and along its own rotated axes: a table built
  // from a top and four legs, set to repeat rotationally, becomes five
  // separately-spinning parts, not five tables. Repeating the layer folds
  // the sample point once, before any of its primitives are evaluated, and
  // every primitive in it steps by the same world vector -- so the
  // arrangement between them survives and the layer is the unit that
  // repeats. Layers are also where the boolean operation lives, so a
  // subtraction layer repeats its cuts as a set, cutting the same pattern
  // into every copy.
  //
  // Costs what it sounds like: every primitive in a repeated layer is
  // evaluated once per candidate copy of the fold, and an Infinite layer
  // makes all of them unbounded (never cullable, folded at every voxel of
  // every bake -- see [[unbounded-primitives]] and
  // geometry_bounding_radius()). None (the default) leaves the layer
  // exactly as it was before this existed.
  SdfRepetitionMode repetition_mode = SdfRepetitionMode::None;
  glm::vec3 repetition_cell{1.0f};
  glm::vec3 repetition_count{1.0f};
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
  // See SdfPrimitiveDef's material binding block above -- a volumetric
  // refers to its material exactly the same way.
  MaterialId material_id = kInvalidMaterialId;
  std::vector<MaterialOverride> material_overrides;
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
  // The scene's material library: every material any primitive or
  // volumetric in it refers to, by MaterialId. Order is authored order and
  // is not otherwise significant -- lookup is always by id.
  //
  // Scene-embedded rather than a global folder of files, deliberately. A
  // shared mutable global library is what made Material a
  // reference-counted object that several scenes could alias, which in
  // turn is why Geometry carries texture_scale_factor/texture_offset_scale
  // (scale_scene() cannot mutate a shared material, so it has to modulate
  // it per primitive instead). A self-contained scene also round-trips
  // safely through the editor's live-preview file.
  std::vector<MaterialDef> materials;
  // Scene-wide ambient factor (added once, not per-light) -- 0 means fully
  // unlit surfaces facing away from every light are pure black; matches the
  // old hardcoded default this replaces.
  f32 ambient = 0.15f;
  // The equirectangular (lat/long, NOT 6-face cubemap) image drawn behind
  // this scene's geometry and reflected off its glossy surfaces -- an
  // assets/textures/<name>.png basename, exactly what
  // renderer_enable_sky_box() takes (see renderer_frontend.h) and what a
  // primitive's own texture names.
  //
  // EMPTY MEANS UNSPECIFIED, NOT "NO SKYBOX". Loading a scene with no
  // skybox= line leaves whatever skybox is currently set alone rather than
  // turning it off -- games set theirs from code (see
  // games/SH/src/game_scene_state.cpp) and then load scene files that
  // predate this field, and those must not switch the sky off from under
  // them. A caller that genuinely wants no skybox says so directly, with
  // renderer_disable_sky_box(); that is exactly what the editor does when
  // its skybox is cleared, and why clearing writes no line rather than
  // writing an empty one.
  std::string skybox;
};

// A position and an orientation, in whichever space the function producing
// it names. rotation is Euler angles in radians (XYZ order), matching
// SdfPrimitiveDef::rotation -- so a result drops straight into one.
struct SdfTransform {
  glm::vec3 position{0.0f};
  glm::vec3 rotation{0.0f};
};

// A layer's own rotation as a quaternion (see SdfLayerDef::rotation).
// Identity for a layer that has never been rotated.
glm::quat sdf_layer_rotation(const SdfLayerDef &layer);

// Where one of a layer's primitives actually sits and points in WORLD
// space: its authored, layer-local transform with the layer's own folded
// in. This is THE definition of what a layer transform means -- every
// consumer that turns an authored scene into something renderable or
// pickable goes through it, so the bake and the editor's picker cannot
// disagree about where a primitive is.
//
// A Plane is returned unchanged: it has no meaningful position or
// orientation to compose onto (see SdfLayerDef::position).
SdfTransform sdf_layer_world_transform(const SdfLayerDef &layer,
                                       const SdfPrimitiveDef &primitive);

// The exact inverse of the above: the authored, layer-local transform a
// primitive of `type` would need in order to sit at world_position facing
// world_rotation. The editor's gizmo drags in world space and writes
// authored values back, so it needs this direction.
SdfTransform sdf_layer_local_transform(const SdfLayerDef &layer,
                                       SdfPrimitiveType type,
                                       glm::vec3 world_position,
                                       glm::vec3 world_rotation);

// The material with this id, or nullptr if the scene has no such entry.
const MaterialDef *sdf_scene_find_material(const SdfScene &scene,
                                          MaterialId id);
MaterialDef *sdf_scene_find_material(SdfScene &scene, MaterialId id);

// Resolves what a primitive/volumetric actually renders as: its library
// material with its own overrides folded in, or -- for a scene still using
// the legacy form -- the .kmt file its material_name points at.
//
// This is the one function that knows both forms exist, so everything
// downstream (GeometrySystem, the editor's property panel, the eventual
// GPU packer) sees a single resolved MaterialDef and never branches on
// whether the scene has been converted yet.
//
// `binding_id`/`binding_name`/`overrides` are the three fields the two
// binding blocks declare; both SdfPrimitiveDef and SdfVolumetricDef are
// passed through the templated overload below rather than duplicating this.
MaterialDef sdf_scene_resolve_material(
    const SdfScene &scene, MaterialId binding_id,
    const std::string &binding_name,
    const std::vector<MaterialOverride> &overrides);

template <typename Bound>
MaterialDef sdf_scene_resolve_material(const SdfScene &scene,
                                      const Bound &bound) {
  return sdf_scene_resolve_material(scene, bound.material_id,
                                    bound.material_name,
                                    bound.material_overrides);
}

// Converts every legacy .kmt reference in the scene into a library
// material, deduplicating by content so the 35 distinct value tuples a
// scene actually uses become 35 definitions rather than one per primitive.
// Imported materials get a readable initial display name (see
// material_def_suggest_name()) and a fresh id.
//
// Idempotent: a scene with no legacy references is left untouched, and
// returns 0.
//
// Called automatically by load_sdf_scene(), so nothing downstream ever
// sees a half-converted scene; exposed here because the editor also needs
// to report how many materials an opened scene imported.
u32 sdf_scene_import_legacy_materials(SdfScene &scene);

// Parses path (see the format description above). Returns std::nullopt on
// failure (missing file); malformed individual lines are skipped with a
// logged warning rather than failing the whole load, so a modelling tool
// emitting a slightly-off file doesn't lose the rest of the scene.
std::optional<SdfScene> load_sdf_scene(std::string_view path);
