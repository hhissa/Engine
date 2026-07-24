#pragma once
#include "material_system.h"

#include <array>
#include <glm/glm.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// The shape a registered piece of geometry evaluates to. kohi's geometry
// system stores vertex/index mesh data for a rasterizer to draw -- there's
// no equivalent here, since the render shader has no vertex pipeline at
// all. What's analogous for a raymarching engine is an analytic SDF
// primitive plus its parameters, which is what this engine stores instead.
// Mirrors SdfPrimitiveType (sdf_scene.h) value-for-value -- see its comment
// for what each type's params/extra_param mean and which ones are
// intentionally not included.
enum class PrimitiveType : u32 {
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

// How a layer's primitives combine with the scene built up so far (see
// SceneLayer below). Union and subtraction both take a smoothness: 0 gives
// a hard edge, > 0 blends across that radius instead (a smooth union
// rounds two shapes together; a smooth subtraction carves a rounded-edge
// notch instead of a sharp one).
enum class LayerOperation : u32 {
  Union = 0,
  Subtraction = 1,
};

// One layer's combine rule. Geometry::layer indexes into
// GeometrySystem::layers() to find the SceneLayer it belongs to -- a layer
// can hold as many primitives as you like, and this operation/smoothness
// applies to every one of them individually (each is folded into the
// running scene one at a time, in the voxelize shader), not once for the
// whole layer as a pre-merged blob. This is why a subtraction layer only
// ever affects primitives on its own layer: the layer boundary is the
// boolean operation boundary, and every primitive inside it gets that same
// boundary's operation applied.
struct SceneLayer {
  LayerOperation operation = LayerOperation::Union;
  f32 smoothness = 0.0f;
};

// Describes one primitive to register: its shape, world-space transform,
// and material. There's no on-disk config file format for these (unlike
// .kmt materials) since they're simple procedural parameter sets rather
// than asset data -- mirrors kohi's geometry_system_generate_plane_config()
// style of building a config in code, via the sphere()/box()/plane() helpers
// below.
struct GeometryConfig {
  std::string name;
  PrimitiveType type = PrimitiveType::Sphere;
  glm::vec3 position{0.0f};
  // Euler angles, radians (XYZ order). Meaningless for Plane -- always the
  // horizontal y=height plane (see plane() below), same as position.
  glm::vec3 rotation{0.0f};
  glm::vec3 params{1.0f}; // Per-type meaning -- see SdfPrimitiveDef::params
                         // (sdf_scene.h), which this mirrors exactly.
  f32 extra_param = 0.0f; // See SdfPrimitiveDef::extra_param.
  // Optional per-slot formula overriding the corresponding params.x/y/z/
  // extra_param constant -- see SdfPrimitiveDef::param_expressions. Source
  // text only; VulkanRaymarchShader::rebuild_static_scene() compiles it
  // (via compile_expression()) at GPU-upload time.
  std::array<std::string, 4> param_expressions;
  std::string material_name;

  static GeometryConfig sphere(std::string name, glm::vec3 position,
                               glm::vec3 rotation, f32 radius,
                               std::string material_name);
  static GeometryConfig box(std::string name, glm::vec3 position,
                           glm::vec3 rotation, glm::vec3 half_extents,
                           std::string material_name);
  // Always the horizontal plane y = height (normal fixed to +Y) -- enough
  // for a ground plane without needing a general plane-orientation config.
  static GeometryConfig plane(std::string name, f32 height,
                             std::string material_name);
};

// One registered piece of raymarch geometry: baked-scene parameters (shape,
// transform) plus its resolved material. VulkanRaymarchShader reads every
// currently-registered Geometry once per bake (see
// GeometrySystem::snapshot()) to union all of them into the sparse voxel
// field -- except for whichever one it's chosen to animate every frame
// instead, which it keeps live-mutating via the reference returned by
// acquire() and excludes from the baked snapshot entirely.
struct Geometry {
  std::string name;
  PrimitiveType type = PrimitiveType::Sphere;
  glm::vec3 position{0.0f};
  glm::vec3 rotation{0.0f}; // Euler angles, radians -- see GeometryConfig.
  glm::vec3 params{1.0f};
  f32 extra_param = 0.0f; // See GeometryConfig::extra_param.
  std::array<std::string, 4> param_expressions; // See GeometryConfig::param_expressions.
  // Accumulated uniform scale applied to the param_expressions above (only
  // -- plain params/extra_param are scaled directly). A formula computes a
  // *length* from a local-space point, so uniformly scaling the shape by s
  // must turn f(p) into s*f(p/s): evaluate at the unscaled point, scale
  // the resulting length. The strings can't express that without
  // rewriting them, so scale_scene() accumulates s here instead and the
  // GPU applies the transformation at evaluation time (see
  // resolve_params() in Builtin.SdfSceneCommon.inc.glsl). 1 = unscaled;
  // ignored for slots with no formula.
  f32 param_expr_scale = 1.0f;
  // Accumulated uniform scale applied to this primitive's *effective*
  // texture_scale (world units per texture tile -- see Material::
  // texture_scale) at upload time: effective = material->texture_scale *
  // texture_scale_factor. A separate per-Geometry factor rather than
  // scaling material->texture_scale directly, because Material is a
  // shared, reference-counted resource (MaterialSystem::acquire() caches
  // by name) -- multiple primitives, even across different loaded scenes,
  // can point at the exact same Material instance, so mutating it in
  // scale_scene() would incorrectly rescale every OTHER primitive using
  // that material too, not just the one scene actually being scaled. Like
  // param_expr_scale above, this is a length-ish quantity (how big the
  // texture pattern looks on the surface) that must shrink/grow in
  // lockstep with the primitive itself -- left alone, scaling the model
  // down leaves the texture tiling at its old (relatively now much
  // larger) frequency, same failure mode smoothness had before it got the
  // same treatment. 1 = unscaled.
  f32 texture_scale_factor = 1.0f;
  std::string material_name; // Key MaterialSystem was acquired with --
                            // needed again at release time.
  Material *material = nullptr; // Non-owning -- owned by MaterialSystem.
  u32 layer = 0; // Index into GeometrySystem::layers() -- which SceneLayer
                // (operation + smoothness) this primitive combines under.
                // Defaults to 0, the always-present default layer (plain
                // union, no smoothing) that acquire() alone -- without
                // going through load_scene() -- implicitly uses.
};

// Mirrors SdfLightType (sdf_scene.h) value-for-value.
enum class LightType : u32 {
  Directional = 0,
  Point = 1,
};

// Describes one light to register -- mirrors GeometryConfig's role for
// primitives, but much simpler: no material to resolve, just a shape-free
// shading contribution. There's no sphere()/box()-style set of named
// static builders since a light only ever has these two shapes -- callers
// just fill this in directly (see acquire_light()).
struct LightConfig {
  std::string name;
  LightType type = LightType::Directional;
  // Direction (Directional, doesn't need to be pre-normalized) or
  // world-space position (Point) -- see LightType's comment.
  glm::vec3 vector{0.6f, 0.7f, -0.6f};
  glm::vec3 colour{1.0f};
  f32 intensity = 1.0f;
};

// One registered light. VulkanRaymarchShader reads every currently-
// registered light once per bake (see GeometrySystem::light_snapshot()) and
// uploads them for the render pass's per-pixel lighting loop to sum -- see
// Builtin.RaymarchShader.comp.glsl.
struct Light {
  std::string name;
  LightType type = LightType::Directional;
  glm::vec3 vector{0.6f, 0.7f, -0.6f}; // See LightConfig::vector.
  glm::vec3 colour{1.0f};
  f32 intensity = 1.0f;
};

// Describes one volumetric "light shaft" shape to register -- mirrors
// GeometryConfig's role for opaque primitives (same shape/transform vocabulary,
// and a material resolved the same way through MaterialSystem), but this one
// is never folded into a SceneLayer or baked into the opaque voxel field: it
// has no `layer`, and VulkanRaymarchShader uploads it into a separate tail
// range of primitive_buffer_ that scene_map()/the voxelize pass never
// iterate (see rebuild_static_scene()). The render pass instead marches
// through it as a transparent, textured volume -- see density below and
// accumulate_volumetrics() in Builtin.RaymarchShader.comp.glsl.
struct VolumetricConfig {
  std::string name;
  PrimitiveType type = PrimitiveType::Box;
  glm::vec3 position{0.0f};
  glm::vec3 rotation{0.0f};
  glm::vec3 params{1.0f};
  f32 extra_param = 0.0f;
  // How strongly this shape accumulates its material's tinted/textured glow
  // per world unit the primary ray travels through it -- see
  // accumulate_volumetrics() in Builtin.RaymarchShader.comp.glsl. Higher
  // reads as a denser/brighter shaft.
  f32 density = 1.0f;
  std::string material_name;
};

// One registered volumetric primitive. VulkanRaymarchShader reads every
// currently-registered one (see GeometrySystem::volumetric_snapshot()) each
// time it rebakes, uploading it alongside the opaque primitives but outside
// any layer's range -- see VolumetricConfig's comment.
struct Volumetric {
  std::string name;
  PrimitiveType type = PrimitiveType::Box;
  glm::vec3 position{0.0f};
  glm::vec3 rotation{0.0f};
  glm::vec3 params{1.0f};
  f32 extra_param = 0.0f;
  f32 density = 1.0f;
  std::string material_name;
  Material *material = nullptr; // Non-owning -- owned by MaterialSystem.
};

struct SdfScene;

// Returned by GeometrySystem::load_scene(): the names of everything that
// call registered (primitives, lights, and volumetrics are named+refcounted,
// but in separate maps -- see release()/release_light()/
// release_volumetric()), so the caller (VulkanRendererBackend::load_scene(),
// see its loaded_scenes_) knows what to release later via
// remove_scene()/clear_scenes().
struct LoadedSceneNames {
  std::vector<std::string> primitive_names;
  std::vector<std::string> light_names;
  std::vector<std::string> volumetric_names;
};

class GeometrySystem {
public:
  explicit GeometrySystem(MaterialSystem &material_system);
  ~GeometrySystem() = default;

  GeometrySystem(const GeometrySystem &) = delete;
  GeometrySystem &operator=(const GeometrySystem &) = delete;

  // Registers config.name (or bumps its reference count if already
  // registered) and resolves its material through MaterialSystem. Returns a
  // stable reference: callers that need to animate a piece of geometry
  // frame-to-frame (see VulkanRaymarchShader) can hold onto it and mutate
  // position/params directly -- std::unordered_map never relocates
  // existing entries on insertion, so the reference stays valid for as
  // long as the entry does.
  Geometry &acquire(const GeometryConfig &config, bool auto_release);

  // Mirrors TextureSystem::release/MaterialSystem::release. Also releases
  // the geometry's material reference.
  void release(std::string_view name);

  // The registered geometry called `name`, or nullptr if there isn't one.
  // Same stability guarantee as acquire()'s returned reference; mutations
  // only become visible once the raymarch scene is re-baked (see
  // VulkanRaymarchShader::rebake()).
  Geometry *find(std::string_view name);

  // A snapshot copy of every currently registered geometry, in unspecified
  // (map iteration) order -- read by VulkanRaymarchShader each time it
  // bakes the sparse voxel field.
  std::vector<Geometry> snapshot() const;

  // Mirrors acquire()/release()/snapshot() above, for lights instead of
  // geometry -- a separate map, so a light and a primitive can share a name
  // with no collision.
  Light &acquire_light(const LightConfig &config, bool auto_release);
  void release_light(std::string_view name);
  std::vector<Light> light_snapshot() const;

  // Mirrors acquire()/release()/snapshot() above, for volumetric "light
  // shaft" primitives instead -- a separate map, so a volumetric can share a
  // name with a primitive/light with no collision. Deliberately excluded
  // from snapshot(): volumetrics never belong to a layer and must never be
  // baked into the opaque voxel field (see VolumetricConfig's comment).
  Volumetric &acquire_volumetric(const VolumetricConfig &config,
                                 bool auto_release);
  void release_volumetric(std::string_view name);
  std::vector<Volumetric> volumetric_snapshot() const;
  // Same stability guarantee as find() above, for a volumetric -- needed by
  // VulkanRendererBackend::translate_scene()/rotate_scene()/scale_scene() to
  // move a loaded scene's volumetrics in lockstep with its opaque geometry.
  Volumetric *find_volumetric(std::string_view name);

  // Registers every primitive in every layer of `scene`, every light, and
  // every volumetric (see sdf_scene.h -- this is how an externally-authored
  // SDF file, e.g. from a modelling tool, actually gets turned into
  // rendered geometry/lights/volumetrics), primitives each under a freshly
  // appended SceneLayer so layer indices from multiple loaded scenes never
  // collide with each other or with the default layer 0. Also overwrites
  // ambient() with scene.ambient -- like the baked voxel field itself,
  // there's only one merged scene regardless of how many SceneHandles are
  // concurrently loaded, so the most-recently-loaded scene's ambient wins.
  // Returns the generated name of each registered primitive/light/volumetric
  // -- callers own releasing each of these later via release()/
  // release_light()/release_volumetric() respectively (see
  // LoadedSceneNames).
  //
  // name_prefix is prepended to every generated primitive/light name.
  // Callers loading multiple scenes MUST pass a per-load unique prefix
  // (VulkanRendererBackend uses "sceneN/" from the scene handle):
  // acquire() keys purely off the name, so two scenes both containing
  // "layer0/layer0_primitive" (the SDF editor names every file's layers
  // the same way) would otherwise collide -- the second load bumps the
  // first scene's entry, silently discards its own shape's parameters,
  // and re-layers the first scene's geometry into the second's layers.
  LoadedSceneNames load_scene(const SdfScene &scene, bool auto_release,
                              std::string_view name_prefix = {});

  // Every layer currently known, indexed by Geometry::layer. Index 0 is
  // always present (the default layer everything acquire()'d without
  // load_scene() implicitly belongs to).
  const std::vector<SceneLayer> &layers() const noexcept { return layers_; }

  // Multiplies layer_index's smoothness by factor -- called once per
  // distinct layer a scaled scene touches (see
  // VulkanRendererBackend::scale_scene()). Smoothness is a blend-radius
  // *length*, exactly like a primitive's position/params/param_expr_scale,
  // so it must scale in lockstep with the shapes it blends -- left alone,
  // scaling a scene down shrinks its primitives while the blend radius
  // stays full-size, so the (relatively now much larger) smoothness
  // engulfs whatever it's blending into an oversized, undifferentiated
  // blob. No-op if layer_index is out of range.
  void scale_layer_smoothness(u32 layer_index, f32 factor) noexcept {
    if (layer_index < layers_.size()) {
      layers_[layer_index].smoothness *= factor;
    }
  }

  // Scene-wide ambient factor from the most recently load_scene()'d
  // SdfScene -- see SdfScene::ambient.
  f32 ambient() const noexcept { return ambient_; }

private:
  struct Entry {
    Geometry geometry;
    u32 reference_count = 0;
    bool auto_release = false;
  };
  struct LightEntry {
    Light light;
    u32 reference_count = 0;
    bool auto_release = false;
  };
  struct VolumetricEntry {
    Volumetric volumetric;
    u32 reference_count = 0;
    bool auto_release = false;
  };

  MaterialSystem *material_system_;
  std::unordered_map<std::string, Entry> geometries_;
  std::unordered_map<std::string, LightEntry> lights_;
  std::unordered_map<std::string, VolumetricEntry> volumetrics_;
  std::vector<SceneLayer> layers_{SceneLayer{}};
  // Parallel to layers_: how many currently-registered geometries reference
  // each layer index. Index 0 (the default layer) is never touched by
  // this -- load_scene() never assigns it -- so it never looks "free" to
  // reuse. load_scene() below hands out a slot whose count has dropped to
  // zero instead of always growing layers_, so a caller that repeatedly
  // clears and reloads the same scene (e.g. the SDF editor tool re-baking
  // on every gizmo drag) doesn't leak layer indices past
  // VulkanRaymarchShader's kMaxLayers cap.
  std::vector<u32> layer_ref_counts_{0};
  f32 ambient_ = 0.15f; // See ambient() -- matches SdfScene::ambient's own
                       // default so an engine that never loads any scene
                       // (or loads one that never sets ambient=) still
                       // renders with the same look as before this existed.
};
