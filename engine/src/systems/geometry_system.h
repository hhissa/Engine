#pragma once
#include "material_system.h"

#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// The shape a registered piece of geometry evaluates to. Originally shared
// between the (now-removed, M0 CSG deletion pass) CSG primitive-authoring
// system and Volumetric below -- Volumetric is the one surviving consumer,
// so this stays as its shape vocabulary rather than being folded into
// Volumetric directly, in case a future consumer besides Volumetric needs
// the same enum.
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

// Mirrors SdfLightType (the now-removed CSG scene format) value-for-value.
enum class LightType : u32 {
  Directional = 0,
  Point = 1,
};

// Describes one light to register -- no material to resolve, just a
// shape-free shading contribution. There's no sphere()/box()-style set of
// named static builders since a light only ever has these two shapes --
// callers just fill this in directly (see acquire_light()).
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

// Describes one volumetric "light shaft" shape to register -- same shape/
// transform vocabulary as the (now-removed) CSG primitive system, and a
// material resolved the same way through MaterialSystem, but this one is
// never folded into any boolean scene graph: VulkanRaymarchShader uploads
// it into its own tail range that the opaque voxel bake never iterates.
// The render pass instead marches through it as a transparent, textured
// volume -- see density below.
struct VolumetricConfig {
  std::string name;
  PrimitiveType type = PrimitiveType::Box;
  glm::vec3 position{0.0f};
  glm::vec3 rotation{0.0f};
  glm::vec3 params{1.0f};
  f32 extra_param = 0.0f;
  // How strongly this shape accumulates its material's tinted/textured glow
  // per world unit the primary ray travels through it. Higher reads as a
  // denser/brighter shaft.
  f32 density = 1.0f;
  // The resolved material for this primitive, when the caller has one.
  //
  // When set, this wins and material_name below is ignored: the runtime
  // material is acquired by CONTENT (MaterialSystem::acquire_def()), so
  // Geometry::material_name ends up holding that content key rather than
  // anything an author typed. Leave it unset to acquire a legacy
  // assets/materials/<material_name>.kmt file instead.
  std::optional<MaterialDef> material_def;
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

// Lights and volumetrics only -- the CSG primitive-authoring surface
// (GeometryConfig/Geometry/SceneLayer/LoadedSceneNames/load_scene()/
// reconcile_scene()/chunks_touched_by()/the dirty-tracking apparatus that
// existed purely to serve ChunkStreamingManager) was removed in the M0 CSG
// deletion pass (see /home/h-i/.claude/plans/joyful-skipping-giraffe.md) --
// Light/Volumetric are CSG-independent concepts a viewport still needs, so
// the class survives, trimmed to just them. The name GeometrySystem stays
// (deliberately not renamed) since a later milestone's SculptSystem is the
// actual replacement for the authoring role this used to also play.
class GeometrySystem {
public:
  explicit GeometrySystem(MaterialSystem &material_system);
  ~GeometrySystem() = default;

  GeometrySystem(const GeometrySystem &) = delete;
  GeometrySystem &operator=(const GeometrySystem &) = delete;

  // Registers config.name (or bumps its reference count if already
  // registered). Returns a stable reference: std::unordered_map never
  // relocates existing entries on insertion, so the reference stays valid
  // for as long as the entry does.
  Light &acquire_light(const LightConfig &config, bool auto_release);
  void release_light(std::string_view name);
  std::vector<Light> light_snapshot() const;
  // Same stability guarantee as find_light() below -- needed by a caller
  // that wants to move an already-registered light in place.
  Light *find_light(std::string_view name);

  // Mirrors acquire_light()/release_light()/light_snapshot() above, for
  // volumetric "light shaft" primitives instead -- a separate map, so a
  // volumetric can share a name with a light with no collision.
  Volumetric &acquire_volumetric(const VolumetricConfig &config,
                                 bool auto_release);
  void release_volumetric(std::string_view name);
  std::vector<Volumetric> volumetric_snapshot() const;
  Volumetric *find_volumetric(std::string_view name);

  // Scene-wide ambient factor. No longer has a live data source now that
  // the CSG scene file format (which used to set it via load_scene()) is
  // gone -- stays at its constructed default unless a future milestone
  // gives it a new home (see SculptVolume/SceneEnvironment in the sculptor
  // plan). Kept rather than deleted: nothing else about ambient lighting
  // changed, and the renderer's own ambient push-constant path is still
  // independently callable.
  f32 ambient() const noexcept { return ambient_; }

  // Shifts every registered light/volumetric's render-space position by
  // delta, in place -- the floating-origin recenter primitive (see
  // VulkanRendererBackend::maybe_recenter_origin()). Same per-type
  // special-casing as before: only a Point light's vector is a position (a
  // Directional light's is a pure direction, untouched); a volumetric with
  // PrimitiveType::Plane has no position (only params.x, its height, moves,
  // and only for delta.y).
  void shift_all(glm::vec3 delta) noexcept {
    for (auto &[name, entry] : volumetrics_) {
      if (entry.volumetric.type == PrimitiveType::Plane) {
        entry.volumetric.params.x += delta.y;
      } else {
        entry.volumetric.position += delta;
      }
    }
    for (auto &[name, entry] : lights_) {
      if (entry.light.type == LightType::Point) {
        entry.light.vector += delta;
      }
    }
  }

private:
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
  std::unordered_map<std::string, LightEntry> lights_;
  std::unordered_map<std::string, VolumetricEntry> volumetrics_;
  f32 ambient_ = 0.15f; // See ambient()'s comment -- matches the CSG scene
                       // format's own former default.
};
