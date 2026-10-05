#include "geometry_system.h"
#include "../core/logger.h"
#include "../resources/sdf_scene.h"

#include <algorithm>
#include <cmath>

namespace {
PrimitiveType to_primitive_type(SdfPrimitiveType type) {
  switch (type) {
  case SdfPrimitiveType::Box:
    return PrimitiveType::Box;
  case SdfPrimitiveType::Plane:
    return PrimitiveType::Plane;
  case SdfPrimitiveType::Torus:
    return PrimitiveType::Torus;
  case SdfPrimitiveType::CappedCylinder:
    return PrimitiveType::CappedCylinder;
  case SdfPrimitiveType::CappedCone:
    return PrimitiveType::CappedCone;
  case SdfPrimitiveType::RoundBox:
    return PrimitiveType::RoundBox;
  case SdfPrimitiveType::BoxFrame:
    return PrimitiveType::BoxFrame;
  case SdfPrimitiveType::Octahedron:
    return PrimitiveType::Octahedron;
  case SdfPrimitiveType::Pyramid:
    return PrimitiveType::Pyramid;
  case SdfPrimitiveType::HexPrism:
    return PrimitiveType::HexPrism;
  case SdfPrimitiveType::RoundCone:
    return PrimitiveType::RoundCone;
  case SdfPrimitiveType::Capsule:
    return PrimitiveType::Capsule;
  case SdfPrimitiveType::Link:
    return PrimitiveType::Link;
  case SdfPrimitiveType::Ellipsoid:
    return PrimitiveType::Ellipsoid;
  case SdfPrimitiveType::Sphere:
  default:
    return PrimitiveType::Sphere;
  }
}

LayerOperation to_layer_operation(SdfLayerOperation operation) {
  return operation == SdfLayerOperation::Subtraction
             ? LayerOperation::Subtraction
             : LayerOperation::Union;
}

LightType to_light_type(SdfLightType type) {
  return type == SdfLightType::Point ? LightType::Point : LightType::Directional;
}

RepetitionMode to_repetition_mode(SdfRepetitionMode mode) {
  switch (mode) {
  case SdfRepetitionMode::Infinite:
    return RepetitionMode::Infinite;
  case SdfRepetitionMode::Limited:
    return RepetitionMode::Limited;
  case SdfRepetitionMode::Rotational:
    return RepetitionMode::Rotational;
  case SdfRepetitionMode::Rectangular:
    return RepetitionMode::Rectangular;
  case SdfRepetitionMode::None:
  default:
    return RepetitionMode::None;
  }
}

BendAxis to_bend_axis(SdfBendAxis axis) {
  switch (axis) {
  case SdfBendAxis::XToZ:
    return BendAxis::XToZ;
  case SdfBendAxis::YToZ:
    return BendAxis::YToZ;
  case SdfBendAxis::YToX:
    return BendAxis::YToX;
  case SdfBendAxis::ZToX:
    return BendAxis::ZToX;
  case SdfBendAxis::ZToY:
    return BendAxis::ZToY;
  case SdfBendAxis::XToY:
  default:
    return BendAxis::XToY;
  }
}

// reconcile_scene() uses this to decide whether an already-registered
// primitive needs mark_dirty() (a chunk-level re-bake) -- deliberately
// excludes material_name: a material swap needs MaterialSystem book-
// keeping (handled separately, see reconcile_scene() itself) and a
// scene_dirty_ re-upload (the caller's job), but never changes which
// voxels/bricks a primitive occupies, so folding it in here would force
// pointless chunk re-bakes for a pure colour/texture edit.
//
// `world` is def's transform with its layer's own folded in (see
// sdf_layer_world_transform()) -- Geometry stores world space, def stores
// layer-local, so comparing def's raw position/rotation here would report
// every primitive in a transformed layer as changed on every reconcile.
bool primitive_shape_matches(const Geometry &g, const SdfPrimitiveDef &def,
                             const SdfTransform &world) {
  return g.type == to_primitive_type(def.type) && g.position == world.position &&
      g.rotation == world.rotation && g.params == def.params &&
      g.extra_param == def.extra_param && g.twist == def.twist &&
      g.bend == def.bend && g.bend_axis == to_bend_axis(def.bend_axis) &&
      g.displace_amplitude == def.displace_amplitude &&
      g.displace_frequency == def.displace_frequency &&
      g.param_expressions == def.param_expressions &&
      g.repetition_mode == to_repetition_mode(def.repetition_mode) &&
      g.repetition_cell == def.repetition_cell &&
      g.repetition_count == def.repetition_count;
}

// Acquires the runtime material a config asks for, by content when the
// caller resolved a MaterialDef and by legacy .kmt name otherwise, and
// reports the key it was cached under.
//
// That key is what gets stored on Geometry/Volumetric::material_name and
// what release() must later be called with -- NOT the author-facing name,
// which for a library material is a renameable label that MaterialSystem
// never sees.
template <typename Config>
Material &acquire_config_material(MaterialSystem &materials,
                                 const Config &config, std::string &out_key) {
  Material &material = config.material_def
                          ? materials.acquire_def(*config.material_def, true)
                          : materials.acquire(config.material_name, true);
  out_key = material.name;
  return material;
}

bool light_matches(const Light &l, const SdfLightDef &def) {
  glm::vec3 vector =
      def.type == SdfLightType::Point ? def.position : def.direction;
  return l.type == to_light_type(def.type) && l.vector == vector &&
      l.colour == def.colour && l.intensity == def.intensity;
}

// `material_key` is the resolved content key for def's material (see
// acquire_config_material()), computed by the caller because resolving a
// binding needs the scene, which this helper does not have.
//
// Comparing resolved keys rather than authored names is what makes a
// material EDIT cheap: renaming a material, or pointing a primitive at a
// different library material that happens to hold the same values,
// produces the same key and so is correctly not a swap.
bool volumetric_matches(const Volumetric &v, const SdfVolumetricDef &def,
                       const std::string &material_key) {
  return v.type == to_primitive_type(def.type) && v.position == def.position &&
      v.rotation == def.rotation && v.params == def.params &&
      v.extra_param == def.extra_param && v.density == def.density &&
      v.material_name == material_key;
}
} // namespace

GeometryConfig GeometryConfig::sphere(std::string name, glm::vec3 position,
                                      glm::vec3 rotation, f32 radius,
                                      std::string material_name) {
  GeometryConfig config;
  config.name = std::move(name);
  config.type = PrimitiveType::Sphere;
  config.position = position;
  config.rotation = rotation;
  config.params = glm::vec3(radius, 0.0f, 0.0f);
  config.material_name = std::move(material_name);
  return config;
}

GeometryConfig GeometryConfig::box(std::string name, glm::vec3 position,
                                   glm::vec3 rotation, glm::vec3 half_extents,
                                   std::string material_name) {
  GeometryConfig config;
  config.name = std::move(name);
  config.type = PrimitiveType::Box;
  config.position = position;
  config.rotation = rotation;
  config.params = half_extents;
  config.material_name = std::move(material_name);
  return config;
}

GeometryConfig GeometryConfig::plane(std::string name, f32 height,
                                     std::string material_name) {
  GeometryConfig config;
  config.name = std::move(name);
  config.type = PrimitiveType::Plane;
  config.position = glm::vec3(0.0f);
  config.params = glm::vec3(height, 0.0f, 0.0f);
  config.material_name = std::move(material_name);
  return config;
}

f32 geometry_instance_radius(const Geometry &geometry) noexcept {
  if (geometry.type == PrimitiveType::Plane ||
      geometry.repetition_mode == RepetitionMode::Infinite ||
      geometry.layer_repetition_mode == RepetitionMode::Infinite) {
    return kUnboundedBoundingRadius;
  }
  for (const std::string &expr : geometry.param_expressions) {
    if (!expr.empty()) {
      return kUnboundedBoundingRadius;
    }
  }

  const glm::vec3 &p = geometry.params;
  f32 local_bound;
  switch (geometry.type) {
  case PrimitiveType::Sphere:
    local_bound = p.x;
    break;
  case PrimitiveType::Box:
    local_bound = glm::length(p);
    break;
  case PrimitiveType::Torus:
    local_bound = p.x + p.y; // major_radius + minor_radius
    break;
  case PrimitiveType::CappedCylinder:
    local_bound = glm::length(glm::vec2(p.x, p.y)); // radius, half_height
    break;
  case PrimitiveType::CappedCone:
    // half_height=p.x, r1=p.y, r2=p.z
    local_bound = glm::length(glm::vec2(std::max(p.y, p.z), p.x));
    break;
  case PrimitiveType::RoundBox:
    local_bound = glm::length(p) + geometry.extra_param; // + corner_radius
    break;
  case PrimitiveType::BoxFrame:
    // edge_thickness (extra_param) insets, never extends outward.
    local_bound = glm::length(p);
    break;
  case PrimitiveType::Octahedron:
    local_bound = p.x; // vertices sit exactly at distance s along each axis
    break;
  case PrimitiveType::Pyramid: {
    // Base is a square of half-extent p.y at local y=0 (0 or unset ->
    // 0.5, the old hardcoded value, for scenes saved before this param
    // existed -- see pyramid_sdf()'s own fallback in Builtin.
    // SdfSceneCommon.inc.glsl), apex at y=h=p.x. 1.4142136 = sqrt(2), the
    // half-diagonal-to-half-extent ratio for a square (its corners'
    // distance from the position/base-center origin) -- every point of a
    // convex pyramid lies within the convex hull of its 4 base corners +
    // apex, so the farther of these two bounds covers the whole shape.
    f32 base_half_extent = p.y > 0.0f ? p.y : 0.5f;
    local_bound = std::max(base_half_extent * 1.4142136f, p.x);
    break;
  }
  case PrimitiveType::HexPrism:
    // inradius=p.x, half_height=p.y; 1.1547005 = 2/sqrt(3), the
    // inradius->circumradius factor for a regular hexagon.
    local_bound = glm::length(glm::vec2(p.x * 1.1547005f, p.y));
    break;
  case PrimitiveType::RoundCone:
    // r1=p.x, r2=p.y, half_height=p.z
    local_bound = glm::length(glm::vec2(std::max(p.x, p.y), p.z));
    break;
  case PrimitiveType::Capsule:
    local_bound = p.x + p.y; // radius + half_height
    break;
  case PrimitiveType::Link:
    // half_length=p.x, r1=p.y, r2=p.z -- generous (sum, not the tighter
    // achievable combination) is fine given this function's own bias.
    local_bound = p.x + p.y + p.z;
    break;
  case PrimitiveType::Ellipsoid:
    local_bound = std::max({p.x, p.y, p.z});
    break;
  default:
    // Plane is handled (returned) above and never reaches here; a future
    // PrimitiveType added without a case here falls back to this same
    // generous Ellipsoid-style bound rather than failing to compile --
    // less protective than an exhaustive switch, but a missing case here
    // only costs cull effectiveness (worst case, that type is never
    // culled), not correctness.
    local_bound = std::max({p.x, p.y, p.z});
    break;
  }

  // Displacement (see evaluate_primitive_at()'s own comment in Builtin.
  // SdfSceneCommon.inc.glsl) perturbs the returned distance directly, by
  // up to +-displace_amplitude -- the true surface can sit that much
  // farther out than the undisplaced shape. Twist/bend need no equivalent
  // allowance: both warp the sample point by rotating two of its
  // components (the rotation angle is parameter-dependent, but any
  // rotation matrix preserves vector length), so neither can move a
  // point's distance from the shape's own origin at all.
  //
  // Repetition is deliberately NOT included here -- that is exactly the
  // difference between this function and geometry_bounding_radius() below.
  return local_bound + std::abs(geometry.displace_amplitude);
}

f32 geometry_bounding_radius(const Geometry &geometry) noexcept {
  f32 instance_bound = geometry_instance_radius(geometry);
  if (instance_bound >= kUnboundedBoundingRadius) {
    return kUnboundedBoundingRadius;
  }

  // Domain repetition (see repeat_*() in Builtin.SdfSceneCommon.inc.glsl)
  // spreads copies of the same local shape across a finite span -- widen
  // the bound to cover the farthest copy's own reach, not just the
  // original instance's. Infinite is handled (unbounded) above; None and
  // Rotational need no widening at all -- rotating the sample point around
  // the primitive's own local Y axis preserves its distance from the
  // origin exactly (a rotation matrix, however parameter-dependent its
  // angle, can't change a vector's length), so a rotationally-repeated
  // instance never reaches farther than the unrepeated one.
  f32 repeat_reach = 0.0f;
  if (geometry.repetition_mode == RepetitionMode::Limited) {
    glm::vec3 half_span =
        glm::max(geometry.repetition_count - 1.0f, 0.0f) * 0.5f;
    repeat_reach = glm::length(half_span * geometry.repetition_cell);
  } else if (geometry.repetition_mode == RepetitionMode::Rectangular) {
    // X/Z only -- see repeat_rectangular()'s own comment (Y is always left
    // untouched by this mode).
    glm::vec2 count_xz(geometry.repetition_count.x, geometry.repetition_count.z);
    glm::vec2 cell_xz(geometry.repetition_cell.x, geometry.repetition_cell.z);
    glm::vec2 half_span = glm::max(count_xz - 1.0f, 0.0f) * 0.5f;
    repeat_reach = glm::length(half_span * cell_xz);
  }

  // The layer's own repetition (see Geometry::layer_repetition_mode)
  // spreads copies of this primitive the same way. The linear modes are
  // arithmetically identical to the per-primitive case above -- the fold
  // runs in world space, but layer_fold_candidate() measures the instance
  // id from this primitive's own position, so its copies still land at
  // +/-half_span*cell around where it stands. Rotational is the one that
  // differs, because that one really does fold about the world axis.
  f32 layer_reach = 0.0f;
  if (geometry.layer_repetition_mode == RepetitionMode::Limited) {
    glm::vec3 half_span =
        glm::max(geometry.layer_repetition_count - 1.0f, 0.0f) * 0.5f;
    layer_reach = glm::length(half_span * geometry.layer_repetition_cell);
  } else if (geometry.layer_repetition_mode == RepetitionMode::Rectangular) {
    glm::vec2 count_xz(geometry.layer_repetition_count.x,
                       geometry.layer_repetition_count.z);
    glm::vec2 cell_xz(geometry.layer_repetition_cell.x,
                      geometry.layer_repetition_cell.z);
    glm::vec2 half_span = glm::max(count_xz - 1.0f, 0.0f) * 0.5f;
    layer_reach = glm::length(half_span * cell_xz);
  } else if (geometry.layer_repetition_mode == RepetitionMode::Rotational) {
    // Unlike the per-primitive case, this rotation is NOT about the
    // primitive's own origin: the layer folds around the world Y axis, so
    // a primitive standing d units out from that axis has copies all round
    // a circle of radius d, the farthest of them 2d away from where this
    // one stands. (A primitive sitting on the axis has d = 0 and doesn't
    // move, which this gives for free.)
    f32 axis_distance =
        glm::length(glm::vec2(geometry.position.x, geometry.position.z));
    layer_reach = 2.0f * axis_distance;
  }

  return instance_bound + repeat_reach + layer_reach;
}

std::vector<ChunkKey> chunks_touched_by(const Geometry &geometry,
                                        f32 chunk_size, f32 extra_margin) {
  return chunks_touched_by(geometry.position,
                           geometry_bounding_radius(geometry), chunk_size,
                           extra_margin);
}

std::vector<ChunkKey> chunks_touched_by(glm::vec3 position, f32 radius,
                                        f32 chunk_size, f32 extra_margin) {
  std::vector<ChunkKey> touched;
  if (chunk_size <= 0.0f) {
    return touched;
  }
  if (radius >= kUnboundedBoundingRadius) {
    return touched;
  }
  // A primitive's influence on the baked field is wider than the primitive
  // -- see extra_margin's comment in the header.
  radius += std::max(extra_margin, 0.0f);

  glm::vec3 min_corner = position - glm::vec3(radius);
  glm::vec3 max_corner = position + glm::vec3(radius);
  glm::ivec3 min_chunk(glm::floor(min_corner / chunk_size));
  glm::ivec3 max_chunk(glm::floor(max_corner / chunk_size));

  touched.reserve(static_cast<size_t>(max_chunk.x - min_chunk.x + 1) *
                  (max_chunk.y - min_chunk.y + 1) *
                  (max_chunk.z - min_chunk.z + 1));
  for (i32 cz = min_chunk.z; cz <= max_chunk.z; ++cz) {
    for (i32 cy = min_chunk.y; cy <= max_chunk.y; ++cy) {
      for (i32 cx = min_chunk.x; cx <= max_chunk.x; ++cx) {
        touched.push_back(ChunkKey{0, cx, cy, cz});
      }
    }
  }
  return touched;
}

GeometrySystem::GeometrySystem(MaterialSystem &material_system)
    : material_system_(&material_system) {}

Geometry &GeometrySystem::acquire(const GeometryConfig &config,
                                  bool auto_release) {
  Entry &entry = geometries_.try_emplace(config.name).first->second;

  if (entry.reference_count == 0) {
    entry.auto_release = auto_release;

    Geometry geometry;
    geometry.name = config.name;
    geometry.type = config.type;
    geometry.position = config.position;
    geometry.rotation = config.rotation;
    geometry.params = config.params;
    geometry.extra_param = config.extra_param;
    geometry.twist = config.twist;
    geometry.bend = config.bend;
    geometry.bend_axis = config.bend_axis;
    geometry.displace_amplitude = config.displace_amplitude;
    geometry.displace_frequency = config.displace_frequency;
    geometry.param_expressions = config.param_expressions;
    geometry.repetition_mode = config.repetition_mode;
    geometry.repetition_cell = config.repetition_cell;
    geometry.repetition_count = config.repetition_count;
    geometry.layer_repetition_mode = config.layer_repetition_mode;
    geometry.layer_repetition_cell = config.layer_repetition_cell;
    geometry.layer_repetition_count = config.layer_repetition_count;
    geometry.material = &acquire_config_material(*material_system_, config,
                                                geometry.material_name);
    entry.geometry = std::move(geometry);
    mark_dirty(config.name);
    // A fresh registration -- see newly_added_since_last_snapshot()'s own
    // comment for why this is the ONE case update_streaming() can safely
    // force-evict a surgical set of chunks for instead of every resident
    // one.
    newly_added_since_last_snapshot_.emplace(config.name);

    KTRACE("Geometry '{}' registered.", config.name);
  }
  ++entry.reference_count;

  return entry.geometry;
}

void GeometrySystem::release(std::string_view name) {
  std::string key(name);
  auto it = geometries_.find(key);
  if (it == geometries_.end() || it->second.reference_count == 0) {
    KWARN("GeometrySystem::release called for a geometry with no "
         "outstanding references: '{}'.",
         name);
    return;
  }

  Entry &entry = it->second;
  --entry.reference_count;
  if (entry.reference_count == 0 && entry.auto_release) {
    material_system_->release(entry.geometry.material_name);
    // Free this geometry's load_scene()-assigned layer slot (if any) so a
    // future load_scene() can reuse it instead of growing layers_ forever.
    u32 layer = entry.geometry.layer;
    if (layer != 0 && layer < layer_ref_counts_.size() &&
        layer_ref_counts_[layer] > 0) {
      --layer_ref_counts_[layer];
    }
    mark_dirty(name);
    // See any_released_since_last_snapshot()'s own comment -- forces
    // update_streaming()'s next sweep back to the brute-force full-scene
    // path, even if every OTHER name dirtied this cycle is a fresh add.
    any_released_since_last_snapshot_ = true;
    geometries_.erase(it);
  }
}

Geometry *GeometrySystem::find(std::string_view name) {
  auto it = geometries_.find(std::string(name));
  if (it == geometries_.end()) {
    return nullptr;
  }
  return &it->second.geometry;
}

std::vector<Geometry> GeometrySystem::snapshot() const {
  std::vector<Geometry> result;
  result.reserve(geometries_.size());
  for (const auto &[name, entry] : geometries_) {
    result.push_back(entry.geometry);
  }
  // SORTED BY NAME, and that is load-bearing rather than tidiness.
  //
  // geometries_ is an unordered_map, so iteration order depends on the
  // container's insertion/erase HISTORY, not on the scene. Everything
  // downstream inherits that: rebuild_static_scene() assigns primitive
  // indices in this order, and fills primitive_bounds_ in it, which is the
  // order each chunk's candidate list ends up in.
  //
  // chunk_content_hash() mixes candidates in order and includes each
  // primitive's index, so an unstable order means the SAME scene hashes to
  // different chunk keys depending on what was loaded before it. Live
  // symptom: load scene A (cache fills), load B, load A again -- every
  // chunk missed, re-baked and re-wrote, reporting "0 already on disk",
  // while loading A in a fresh process hit the cache perfectly, because
  // there the insertion sequence happened to be identical.
  //
  // Sorting by name makes the order a function of the scene's content
  // alone, so a chunk's key depends on what is in it and nothing else.
  std::sort(result.begin(), result.end(),
           [](const Geometry &a, const Geometry &b) { return a.name < b.name; });
  return result;
}

Light &GeometrySystem::acquire_light(const LightConfig &config,
                                     bool auto_release) {
  LightEntry &entry = lights_.try_emplace(config.name).first->second;

  if (entry.reference_count == 0) {
    entry.auto_release = auto_release;

    Light light;
    light.name = config.name;
    light.type = config.type;
    light.vector = config.vector;
    light.colour = config.colour;
    light.intensity = config.intensity;
    entry.light = std::move(light);

    KTRACE("Light '{}' registered.", config.name);
  }
  ++entry.reference_count;

  return entry.light;
}

void GeometrySystem::release_light(std::string_view name) {
  std::string key(name);
  auto it = lights_.find(key);
  if (it == lights_.end() || it->second.reference_count == 0) {
    KWARN("GeometrySystem::release_light called for a light with no "
         "outstanding references: '{}'.",
         name);
    return;
  }

  LightEntry &entry = it->second;
  --entry.reference_count;
  if (entry.reference_count == 0 && entry.auto_release) {
    lights_.erase(it);
  }
}

std::vector<Light> GeometrySystem::light_snapshot() const {
  std::vector<Light> result;
  result.reserve(lights_.size());
  for (const auto &[name, entry] : lights_) {
    result.push_back(entry.light);
  }
  return result;
}

Light *GeometrySystem::find_light(std::string_view name) {
  auto it = lights_.find(std::string(name));
  if (it == lights_.end()) {
    return nullptr;
  }
  return &it->second.light;
}

Volumetric &GeometrySystem::acquire_volumetric(const VolumetricConfig &config,
                                               bool auto_release) {
  VolumetricEntry &entry = volumetrics_.try_emplace(config.name).first->second;

  if (entry.reference_count == 0) {
    entry.auto_release = auto_release;

    Volumetric volumetric;
    volumetric.name = config.name;
    volumetric.type = config.type;
    volumetric.position = config.position;
    volumetric.rotation = config.rotation;
    volumetric.params = config.params;
    volumetric.extra_param = config.extra_param;
    volumetric.density = config.density;
    volumetric.material = &acquire_config_material(*material_system_, config,
                                                  volumetric.material_name);
    entry.volumetric = std::move(volumetric);

    KTRACE("Volumetric '{}' registered.", config.name);
  }
  ++entry.reference_count;

  return entry.volumetric;
}

void GeometrySystem::release_volumetric(std::string_view name) {
  std::string key(name);
  auto it = volumetrics_.find(key);
  if (it == volumetrics_.end() || it->second.reference_count == 0) {
    KWARN("GeometrySystem::release_volumetric called for a volumetric with "
         "no outstanding references: '{}'.",
         name);
    return;
  }

  VolumetricEntry &entry = it->second;
  --entry.reference_count;
  if (entry.reference_count == 0 && entry.auto_release) {
    material_system_->release(entry.volumetric.material_name);
    volumetrics_.erase(it);
  }
}

std::vector<Volumetric> GeometrySystem::volumetric_snapshot() const {
  std::vector<Volumetric> result;
  result.reserve(volumetrics_.size());
  for (const auto &[name, entry] : volumetrics_) {
    result.push_back(entry.volumetric);
  }
  return result;
}

Volumetric *GeometrySystem::find_volumetric(std::string_view name) {
  auto it = volumetrics_.find(std::string(name));
  if (it == volumetrics_.end()) {
    return nullptr;
  }
  return &it->second.volumetric;
}

// Drops TRAILING layer slots nothing references any more.
//
// layers_ reuses freed slots but never shrank, so its size was a high-water
// mark over the whole session rather than a property of the current scene.
// That leaks straight into the chunk cache key: VulkanRaymarchShader hashes
// the layer COUNT and every layer's content into chunk_content_hash(), so
// after loading a 21-layer scene, re-loading a 4-layer one hashed against 21
// layers -- 17 of them stale leftovers -- and every chunk key differed from
// the same scene loaded into a fresh process. Live symptom: load A, load B,
// load A again, and pre-warm reported "0 already on disk" and re-baked the
// whole scene, while loading A first thing in a new process hit the cache
// perfectly.
//
// Only TRAILING slots, and never index 0 (the always-present default): a
// zero-refcount slot in the middle must keep its index, because every
// still-registered geometry stores its layer as an index into this vector.
void GeometrySystem::trim_unused_layers() {
  while (layers_.size() > 1 && layer_ref_counts_.back() == 0) {
    layers_.pop_back();
    layer_ref_counts_.pop_back();
  }
}

LoadedSceneNames GeometrySystem::load_scene(const SdfScene &scene,
                                            bool auto_release,
                                            std::string_view name_prefix) {
  LoadedSceneNames result;
  ambient_ = scene.ambient;

  // Slots handed out during THIS load -- see reconcile_scene()'s identical
  // set for the failure this prevents. Here the window is narrower, because
  // each layer's primitives are attached in the nested loop below before the
  // next layer is considered, so a slot is normally spoken for by the time
  // the next scan runs. It is not closed, though: a layer with NO primitives
  // leaves its slot at ref count 0, and the next layer to need one takes it,
  // overwriting the first layer's operation and smoothness and merging two
  // authored layers into a single slot.
  std::unordered_set<u32> claimed_this_load;
  u32 layer_order = 0;
  for (const SdfLayerDef &layer_def : scene.layers) {
    SceneLayer layer;
    layer.operation = to_layer_operation(layer_def.operation);
    layer.smoothness = layer_def.smoothness;
    layer.repetition_mode = to_repetition_mode(layer_def.repetition_mode);
    layer.repetition_cell = layer_def.repetition_cell;
    layer.repetition_count = layer_def.repetition_count;
    // 1-based: 0 belongs to the default layer, which always folds first.
    layer.order = ++layer_order;

    // Reuse a layer slot whose ref count has dropped to zero rather than
    // always appending -- otherwise a caller that repeatedly clears and
    // reloads the same scene (e.g. the SDF editor's live preview, re-baked
    // on every gizmo drag release) grows layers_ without bound until real
    // primitives' layer indices exceed VulkanRaymarchShader's kMaxLayers
    // cap and silently stop being baked/rendered. Index 0 is the
    // always-present default layer and is never handed out here.
    u32 layer_index = 0;
    bool reused_slot = false;
    for (u32 i = 1; i < layer_ref_counts_.size(); ++i) {
      if (layer_ref_counts_[i] == 0 && !claimed_this_load.count(i)) {
        layer_index = i;
        reused_slot = true;
        break;
      }
    }
    if (!reused_slot) {
      layer_index = static_cast<u32>(layers_.size());
      layers_.push_back(SceneLayer{});
      layer_ref_counts_.push_back(0);
    }
    claimed_this_load.insert(layer_index);
    layers_[layer_index] = layer;

    for (const SdfPrimitiveDef &primitive_def : layer_def.primitives) {
      GeometryConfig config;
      config.name =
          std::string(name_prefix) + layer_def.name + "/" + primitive_def.name;
      config.type = to_primitive_type(primitive_def.type);
      // World space, not the authored (layer-local) values: the layer's own
      // transform is composed in here, once, rather than carried alongside
      // the primitive -- see SdfLayerDef::position for why every consumer
      // downstream of this point is better off never knowing about it.
      const SdfTransform world =
          sdf_layer_world_transform(layer_def, primitive_def);
      config.position = world.position;
      config.rotation = world.rotation;
      config.params = primitive_def.params;
      config.extra_param = primitive_def.extra_param;
      config.twist = primitive_def.twist;
      config.bend = primitive_def.bend;
      config.bend_axis = to_bend_axis(primitive_def.bend_axis);
      config.displace_amplitude = primitive_def.displace_amplitude;
      config.displace_frequency = primitive_def.displace_frequency;
      config.param_expressions = primitive_def.param_expressions;
      config.repetition_mode = to_repetition_mode(primitive_def.repetition_mode);
      config.repetition_cell = primitive_def.repetition_cell;
      config.repetition_count = primitive_def.repetition_count;
      // Every primitive in a repeated layer carries the layer's fold -- see
      // Geometry::layer_repetition_mode for why it rides on the primitive.
      config.layer_repetition_mode = layer.repetition_mode;
      config.layer_repetition_cell = layer.repetition_cell;
      config.layer_repetition_count = layer.repetition_count;
      config.material_def = sdf_scene_resolve_material(scene, primitive_def);

      Geometry &geometry = acquire(config, auto_release);
      u32 old_layer = geometry.layer;
      geometry.layer = layer_index;
      if (old_layer != layer_index) {
        if (old_layer != 0 && old_layer < layer_ref_counts_.size() &&
            layer_ref_counts_[old_layer] > 0) {
          --layer_ref_counts_[old_layer];
        }
        ++layer_ref_counts_[layer_index];
      }
      result.primitive_names.push_back(config.name);
    }
  }

  for (const SdfLightDef &light_def : scene.lights) {
    LightConfig config;
    config.name = std::string(name_prefix) + light_def.name;
    config.type = to_light_type(light_def.type);
    config.vector = light_def.type == SdfLightType::Point ? light_def.position
                                                          : light_def.direction;
    config.colour = light_def.colour;
    config.intensity = light_def.intensity;

    acquire_light(config, auto_release);
    result.light_names.push_back(config.name);
  }

  for (const SdfVolumetricDef &volumetric_def : scene.volumetrics) {
    VolumetricConfig config;
    config.name = std::string(name_prefix) + volumetric_def.name;
    config.type = to_primitive_type(volumetric_def.type);
    config.position = volumetric_def.position;
    config.rotation = volumetric_def.rotation;
    config.params = volumetric_def.params;
    config.extra_param = volumetric_def.extra_param;
    config.density = volumetric_def.density;
    config.material_def = sdf_scene_resolve_material(scene, volumetric_def);

    acquire_volumetric(config, auto_release);
    result.volumetric_names.push_back(config.name);
  }

  trim_unused_layers();
  return result;
}

bool GeometrySystem::reconcile_scene(const SdfScene &scene,
                                     LoadedSceneNames &loaded,
                                     bool auto_release,
                                     std::string_view name_prefix,
                                     const std::function<void()> &before_first_destructive) {
  bool changed = false;
  // Fire-once wrapper around before_first_destructive -- see the header
  // comment. Every site below that can destroy a GPU resource calls this
  // first; only the first call actually invokes the callback.
  bool destructive_gate_fired = false;
  auto destructive_gate = [&]() {
    if (!destructive_gate_fired) {
      destructive_gate_fired = true;
      if (before_first_destructive) {
        before_first_destructive();
      }
    }
  };
  ambient_ = scene.ambient;

  // --- Layers: match by name against loaded.layer_index_by_name, reusing
  // an already-registered layer_index in place (just updating operation/
  // smoothness if those changed) instead of always allocating a fresh slot
  // the way load_scene() does -- a layer reallocated fresh every reconcile
  // would make every one of its primitives look "moved to a new layer"
  // below, defeating the whole point of this function. Tracks which
  // layer_indexes actually changed operation/smoothness -- every primitive
  // under one of those needs mark_dirty() below even if its OWN fields
  // didn't change, since a layer's combine rule affects every primitive it
  // folds in, not just the one that triggered the edit.
  std::unordered_map<std::string, u32> new_layer_index_by_name;
  std::unordered_set<u32> layers_needing_full_mark_dirty;
  // Every slot handed out during THIS pass, whether reused, freshly pushed
  // or matched by name.
  //
  // Without this, the allocator below can hand the same slot to every layer
  // in the scene. Layers are resolved here, in one loop, but a slot's
  // ref count only rises later, when the primitive loop attaches a primitive
  // to it -- so a slot claimed here still reads as free for the rest of this
  // loop. The first layer that misses the by-name lookup pushes a new slot;
  // the next layer's "reuse a freed slot first" scan finds that same slot at
  // ref count 0 and takes it too, and so on for every remaining layer. They
  // all collapse onto one index, that index's operation and smoothness end
  // up whichever layer was resolved last, and the primitive loop then files
  // every primitive in the scene under it.
  //
  // The result is a scene that renders as one flat union: every subtraction
  // layer is left holding nothing, so its cuts vanish, and every smooth
  // layer is left holding nothing, so its blends harden -- scene-wide, from
  // the first reconcile onward, while the original load_scene() looked
  // perfect because its layers were allocated before any of them had
  // primitives to reuse away.
  std::unordered_set<u32> claimed_this_pass;
  u32 layer_order = 0;
  for (const SdfLayerDef &layer_def : scene.layers) {
    // 1-based, matching load_scene() -- see SceneLayer::order.
    const u32 this_order = ++layer_order;
    auto existing = loaded.layer_index_by_name.find(layer_def.name);
    u32 layer_index;
    if (existing != loaded.layer_index_by_name.end() &&
        existing->second < layers_.size() &&
        !claimed_this_pass.count(existing->second)) {
      layer_index = existing->second;
    } else {
      // Same reuse-a-freed-slot-first policy as load_scene() -- see its
      // own comment for why (unbounded layers_ growth otherwise) -- but
      // skipping anything already spoken for in this pass.
      layer_index = 0;
      bool reused_slot = false;
      for (u32 i = 1; i < layer_ref_counts_.size(); ++i) {
        if (layer_ref_counts_[i] == 0 && !claimed_this_pass.count(i)) {
          layer_index = i;
          reused_slot = true;
          break;
        }
      }
      if (!reused_slot) {
        layer_index = static_cast<u32>(layers_.size());
        layers_.push_back(SceneLayer{});
        layer_ref_counts_.push_back(0);
      }
    }
    claimed_this_pass.insert(layer_index);

    LayerOperation new_operation = to_layer_operation(layer_def.operation);
    RepetitionMode new_repetition = to_repetition_mode(layer_def.repetition_mode);
    // Authored position counts as a change like the operation does: the fold
    // is ordered, so moving a subtraction layer ahead of a union it used to
    // follow changes the result everywhere that layer reaches, without any
    // primitive or any operation having changed. Reordering is rare, and the
    // slot a layer is matched to here is not stable across scene loads, so
    // this deliberately re-dirties on a reorder rather than trying to prove
    // one didn't happen.
    if (layers_[layer_index].operation != new_operation ||
        layers_[layer_index].smoothness != layer_def.smoothness ||
        layers_[layer_index].order != this_order ||
        // The layer's own repetition changes the shape of every primitive
        // under it without any of them changing, exactly like its
        // operation/smoothness do -- see SdfLayerDef::repetition_mode.
        layers_[layer_index].repetition_mode != new_repetition ||
        layers_[layer_index].repetition_cell != layer_def.repetition_cell ||
        layers_[layer_index].repetition_count != layer_def.repetition_count) {
      layers_[layer_index].operation = new_operation;
      layers_[layer_index].smoothness = layer_def.smoothness;
      layers_[layer_index].order = this_order;
      layers_[layer_index].repetition_mode = new_repetition;
      layers_[layer_index].repetition_cell = layer_def.repetition_cell;
      layers_[layer_index].repetition_count = layer_def.repetition_count;
      layers_needing_full_mark_dirty.insert(layer_index);
      changed = true;
    }
    new_layer_index_by_name[layer_def.name] = layer_index;
  }

  // --- What the new scene contains, derived before anything is acquired.
  //
  // Every name in the incoming scene, by the identical derivation
  // load_scene() uses ("prefix + layer_name + / + primitive_name"), so the
  // release sweep below can run BEFORE the acquire passes rather than after
  // them. Ordering matters, and not just for tidiness: acquiring first meant
  // that for the duration of a reconcile, both the outgoing and the incoming
  // scene's resources were resident at once. For an edit that is nothing --
  // an edit removes almost nothing and adds almost nothing. For a whole-file
  // swap (the sdf_editor opens every scene after the first through this
  // function, never through load_scene()) it is the entire previous scene
  // plus the entire next one, and the resources concerned are 2048x2048
  // textures on a GPU whose device-local memory the field's brick pools have
  // already largely spoken for. That peak is what ran a 4GB card out of
  // device memory partway through opening a second .sdf.
  //
  // Deliberately after the layer pass above, not before it: releasing frees
  // layer slots, and the slot allocator up there reuses free slots, so
  // sweeping first would let the incoming scene's layers land on different
  // indices than they do today for no gain.
  std::vector<std::string> new_primitive_names;
  std::unordered_set<std::string> new_primitive_name_set;
  for (const SdfLayerDef &layer_def : scene.layers) {
    for (const SdfPrimitiveDef &primitive_def : layer_def.primitives) {
      std::string full_name =
          std::string(name_prefix) + layer_def.name + "/" + primitive_def.name;
      new_primitive_name_set.insert(full_name);
      new_primitive_names.push_back(std::move(full_name));
    }
  }
  std::unordered_set<std::string> new_light_names;
  for (const SdfLightDef &light_def : scene.lights) {
    new_light_names.insert(std::string(name_prefix) + light_def.name);
  }
  std::unordered_set<std::string> new_volumetric_names;
  for (const SdfVolumetricDef &volumetric_def : scene.volumetrics) {
    new_volumetric_names.insert(std::string(name_prefix) +
                                volumetric_def.name);
  }

  for (const std::string &old_name : loaded.primitive_names) {
    if (!new_primitive_name_set.count(old_name)) {
      destructive_gate();
      release(old_name);
      changed = true;
    }
  }
  for (const std::string &old_name : loaded.light_names) {
    if (!new_light_names.count(old_name)) {
      destructive_gate();
      release_light(old_name);
      changed = true;
    }
  }
  for (const std::string &old_name : loaded.volumetric_names) {
    if (!new_volumetric_names.count(old_name)) {
      destructive_gate();
      release_volumetric(old_name);
      changed = true;
    }
  }

  // --- Primitives: add what is new, update what stayed. ---
  for (const SdfLayerDef &layer_def : scene.layers) {
    u32 layer_index = new_layer_index_by_name.at(layer_def.name);
    // Per LAYER, not per primitive -- deliberately declared outside the
    // primitive loop below and never mutated by it (unlike the per-
    // primitive layer_reassigned flag inside that loop): every primitive
    // in this layer needs mark_dirty() if the layer itself changed,
    // regardless of what any OTHER primitive in the same layer did.
    bool layer_op_changed = layers_needing_full_mark_dirty.count(layer_index) > 0;

    for (const SdfPrimitiveDef &primitive_def : layer_def.primitives) {
      std::string full_name =
          std::string(name_prefix) + layer_def.name + "/" + primitive_def.name;

      // The layer's transform composed onto this primitive, once, before
      // the add/update split -- both branches write world space into
      // Geometry, and the update branch compares against it too (see
      // primitive_shape_matches()).
      const SdfTransform world =
          sdf_layer_world_transform(layer_def, primitive_def);

      // Resolved once, before the add/update split, because both branches
      // need it -- and because the KEY is what the update branch compares
      // against, not the authored name (see below).
      const MaterialDef resolved_material =
          sdf_scene_resolve_material(scene, primitive_def);
      const std::string resolved_material_key =
          material_def_content_key(resolved_material);

      Geometry *existing = find(full_name);
      if (!existing) {
        // Brand new -- acquire() (not a plain field copy) so it goes
        // through the normal fresh-registration path, including
        // GeometrySystem::newly_added_since_last_snapshot()'s tracking
        // (see its own comment for why that's what lets update_streaming()
        // force-evict only THIS primitive's own chunks instead of every
        // resident one).
        GeometryConfig config;
        config.name = full_name;
        config.type = to_primitive_type(primitive_def.type);
        config.position = world.position;
        config.rotation = world.rotation;
        config.params = primitive_def.params;
        config.extra_param = primitive_def.extra_param;
        config.twist = primitive_def.twist;
        config.bend = primitive_def.bend;
        config.bend_axis = to_bend_axis(primitive_def.bend_axis);
        config.displace_amplitude = primitive_def.displace_amplitude;
        config.displace_frequency = primitive_def.displace_frequency;
        config.param_expressions = primitive_def.param_expressions;
        config.repetition_mode = to_repetition_mode(primitive_def.repetition_mode);
        config.repetition_cell = primitive_def.repetition_cell;
        config.repetition_count = primitive_def.repetition_count;
        config.layer_repetition_mode = layers_[layer_index].repetition_mode;
        config.layer_repetition_cell = layers_[layer_index].repetition_cell;
        config.layer_repetition_count = layers_[layer_index].repetition_count;
        config.material_def = resolved_material;

        Geometry &geometry = acquire(config, auto_release);
        geometry.layer = layer_index;
        ++layer_ref_counts_[layer_index];
        changed = true;
        continue;
      }

      // Already registered -- update in place rather than release()+
      // acquire() again: acquire() on an already-resident name is a pure
      // ref-count bump, it never touches fields (see its own comment), and
      // release()-then-acquire() would needlessly force the brute-force
      // full-sweep fallback in update_streaming() (see any_released_
      // since_last_snapshot()'s own comment for exactly why).
      // Compared by RESOLVED CONTENT KEY, not by the authored binding.
      //
      // This is what makes editing a material cheap. Under the old scheme
      // every property lived in the material's own filename, so nudging a
      // spinbox produced a different name here, which read as a material
      // swap -- and a swap is destructive (it can drop the last reference
      // to a VkImage/VkSampler), so it fired destructive_gate() and
      // drained the GPU queue. The single most common authoring action sat
      // on the most expensive path in the system.
      //
      // Keyed by content instead, the only thing that counts as a swap is
      // an actual change in rendered VALUES. Renaming a material, or
      // repointing a primitive at a different library material holding the
      // same values, correctly costs nothing at all.
      if (existing->material_name != resolved_material_key) {
        destructive_gate();
        material_system_->release(existing->material_name);
        existing->material =
            &material_system_->acquire_def(resolved_material, true);
        existing->material_name = existing->material->name;
        changed = true;
      }
      bool layer_reassigned = existing->layer != layer_index;
      if (layer_reassigned) {
        if (existing->layer != 0 && existing->layer < layer_ref_counts_.size() &&
            layer_ref_counts_[existing->layer] > 0) {
          --layer_ref_counts_[existing->layer];
        }
        ++layer_ref_counts_[layer_index];
        existing->layer = layer_index;
        // A layer reassignment can change how THIS primitive combines
        // into the SDF (a different operation/smoothness), even if its
        // own shape fields didn't move -- same reasoning as
        // layer_op_changed above, just scoped to this one primitive
        // rather than every primitive in the layer.
      }
      bool own_shape_changed =
          !primitive_shape_matches(*existing, primitive_def, world);
      if (layer_op_changed || layer_reassigned || own_shape_changed) {
        if (own_shape_changed && !layer_op_changed && !layer_reassigned) {
          // Pre-edit snapshot -- see dirty_previous_state()'s own comment
          // for exactly why ONLY this narrow case (nothing about this
          // primitive's layer changed, just its own shape/transform) is
          // safe for update_streaming() to treat as a bounded, surgical
          // edit instead of falling back to the brute-force full sweep.
          dirty_previous_state_.emplace(full_name, *existing);
        }
        existing->type = to_primitive_type(primitive_def.type);
        existing->position = world.position;
        existing->rotation = world.rotation;
        existing->params = primitive_def.params;
        existing->extra_param = primitive_def.extra_param;
        existing->twist = primitive_def.twist;
        existing->bend = primitive_def.bend;
        existing->bend_axis = to_bend_axis(primitive_def.bend_axis);
        existing->displace_amplitude = primitive_def.displace_amplitude;
        existing->displace_frequency = primitive_def.displace_frequency;
        existing->param_expressions = primitive_def.param_expressions;
        existing->repetition_mode = to_repetition_mode(primitive_def.repetition_mode);
        existing->repetition_cell = primitive_def.repetition_cell;
        existing->repetition_count = primitive_def.repetition_count;
        // Re-taken from the layer rather than left alone: this block runs
        // whenever the layer changed (layer_op_changed covers its
        // repetition too) or this primitive moved to a different layer, and
        // both mean the copy it is carrying is now the wrong fold.
        existing->layer_repetition_mode = layers_[layer_index].repetition_mode;
        existing->layer_repetition_cell = layers_[layer_index].repetition_cell;
        existing->layer_repetition_count = layers_[layer_index].repetition_count;
        mark_dirty(full_name);
        changed = true;
      }
    }
  }

  // --- Lights: same add-new/update-in-place pattern as primitives above
  // (their removals were swept with everything else's, before the acquires),
  // minus the layer/mark_dirty concerns -- lights aren't baked into the
  // chunked/voxel field at all (see Light's own comment), so nothing here
  // affects update_streaming(). ---
  for (const SdfLightDef &light_def : scene.lights) {
    std::string full_name = std::string(name_prefix) + light_def.name;

    Light *existing = find_light(full_name);
    if (!existing) {
      LightConfig config;
      config.name = full_name;
      config.type = to_light_type(light_def.type);
      config.vector = light_def.type == SdfLightType::Point ? light_def.position
                                                             : light_def.direction;
      config.colour = light_def.colour;
      config.intensity = light_def.intensity;
      acquire_light(config, auto_release);
      changed = true;
      continue;
    }
    if (!light_matches(*existing, light_def)) {
      existing->type = to_light_type(light_def.type);
      existing->vector = light_def.type == SdfLightType::Point ? light_def.position
                                                                : light_def.direction;
      existing->colour = light_def.colour;
      existing->intensity = light_def.intensity;
      changed = true;
    }
  }

  // --- Volumetrics: same pattern as lights, plus the material handling
  // primitives use above (volumetrics DO have a material -- see
  // VolumetricConfig's comment). ---
  for (const SdfVolumetricDef &volumetric_def : scene.volumetrics) {
    std::string full_name = std::string(name_prefix) + volumetric_def.name;

    // Same resolve-once-then-compare-by-content-key shape the primitive
    // loop above uses, for the same reasons.
    const MaterialDef resolved_material =
        sdf_scene_resolve_material(scene, volumetric_def);
    const std::string resolved_material_key =
        material_def_content_key(resolved_material);

    Volumetric *existing = find_volumetric(full_name);
    if (!existing) {
      VolumetricConfig config;
      config.name = full_name;
      config.type = to_primitive_type(volumetric_def.type);
      config.position = volumetric_def.position;
      config.rotation = volumetric_def.rotation;
      config.params = volumetric_def.params;
      config.extra_param = volumetric_def.extra_param;
      config.density = volumetric_def.density;
      config.material_def = resolved_material;
      acquire_volumetric(config, auto_release);
      changed = true;
      continue;
    }
    if (existing->material_name != resolved_material_key) {
      destructive_gate();
      material_system_->release(existing->material_name);
      existing->material =
          &material_system_->acquire_def(resolved_material, true);
      existing->material_name = existing->material->name;
      changed = true;
    }
    if (!volumetric_matches(*existing, volumetric_def,
                            resolved_material_key)) {
      existing->type = to_primitive_type(volumetric_def.type);
      existing->position = volumetric_def.position;
      existing->rotation = volumetric_def.rotation;
      existing->params = volumetric_def.params;
      existing->extra_param = volumetric_def.extra_param;
      existing->density = volumetric_def.density;
      changed = true;
    }
  }

  loaded.primitive_names = std::move(new_primitive_names);
  loaded.light_names.assign(new_light_names.begin(), new_light_names.end());
  loaded.volumetric_names.assign(new_volumetric_names.begin(),
                                 new_volumetric_names.end());
  loaded.layer_index_by_name = std::move(new_layer_index_by_name);

  trim_unused_layers();
  return changed;
}
