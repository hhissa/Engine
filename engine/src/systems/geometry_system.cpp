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

// reconcile_scene() uses this to decide whether an already-registered
// primitive needs mark_dirty() (a chunk-level re-bake) -- deliberately
// excludes material_name: a material swap needs MaterialSystem book-
// keeping (handled separately, see reconcile_scene() itself) and a
// scene_dirty_ re-upload (the caller's job), but never changes which
// voxels/bricks a primitive occupies, so folding it in here would force
// pointless chunk re-bakes for a pure colour/texture edit.
bool primitive_shape_matches(const Geometry &g, const SdfPrimitiveDef &def) {
  return g.type == to_primitive_type(def.type) && g.position == def.position &&
      g.rotation == def.rotation && g.params == def.params &&
      g.extra_param == def.extra_param && g.twist == def.twist &&
      g.bend == def.bend && g.displace_amplitude == def.displace_amplitude &&
      g.displace_frequency == def.displace_frequency &&
      g.param_expressions == def.param_expressions &&
      g.repetition_mode == to_repetition_mode(def.repetition_mode) &&
      g.repetition_cell == def.repetition_cell &&
      g.repetition_count == def.repetition_count;
}

bool light_matches(const Light &l, const SdfLightDef &def) {
  glm::vec3 vector =
      def.type == SdfLightType::Point ? def.position : def.direction;
  return l.type == to_light_type(def.type) && l.vector == vector &&
      l.colour == def.colour && l.intensity == def.intensity;
}

bool volumetric_matches(const Volumetric &v, const SdfVolumetricDef &def) {
  return v.type == to_primitive_type(def.type) && v.position == def.position &&
      v.rotation == def.rotation && v.params == def.params &&
      v.extra_param == def.extra_param && v.density == def.density &&
      v.material_name == def.material_name;
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

f32 geometry_bounding_radius(const Geometry &geometry) noexcept {
  if (geometry.type == PrimitiveType::Plane ||
      geometry.repetition_mode == RepetitionMode::Infinite) {
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

  // Displacement (see evaluate_primitive_at()'s own comment in Builtin.
  // SdfSceneCommon.inc.glsl) perturbs the returned distance directly, by
  // up to +-displace_amplitude -- the true surface can sit that much
  // farther out than the undisplaced shape. Twist/bend need no equivalent
  // allowance: both warp the sample point by rotating two of its
  // components (the rotation angle is parameter-dependent, but any
  // rotation matrix preserves vector length), so neither can move a
  // point's distance from the shape's own origin at all.
  return local_bound + repeat_reach + std::abs(geometry.displace_amplitude);
}

std::vector<ChunkKey> chunks_touched_by(const Geometry &geometry,
                                        f32 chunk_size) {
  std::vector<ChunkKey> touched;
  if (chunk_size <= 0.0f) {
    return touched;
  }
  f32 radius = geometry_bounding_radius(geometry);
  if (radius >= kUnboundedBoundingRadius) {
    return touched;
  }

  glm::vec3 min_corner = geometry.position - glm::vec3(radius);
  glm::vec3 max_corner = geometry.position + glm::vec3(radius);
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
    geometry.displace_amplitude = config.displace_amplitude;
    geometry.displace_frequency = config.displace_frequency;
    geometry.param_expressions = config.param_expressions;
    geometry.repetition_mode = config.repetition_mode;
    geometry.repetition_cell = config.repetition_cell;
    geometry.repetition_count = config.repetition_count;
    geometry.material_name = config.material_name;
    geometry.material = &material_system_->acquire(config.material_name, true);
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
    volumetric.material_name = config.material_name;
    volumetric.material = &material_system_->acquire(config.material_name, true);
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

LoadedSceneNames GeometrySystem::load_scene(const SdfScene &scene,
                                            bool auto_release,
                                            std::string_view name_prefix) {
  LoadedSceneNames result;
  ambient_ = scene.ambient;

  for (const SdfLayerDef &layer_def : scene.layers) {
    SceneLayer layer;
    layer.operation = to_layer_operation(layer_def.operation);
    layer.smoothness = layer_def.smoothness;

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
      if (layer_ref_counts_[i] == 0) {
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
    layers_[layer_index] = layer;

    for (const SdfPrimitiveDef &primitive_def : layer_def.primitives) {
      GeometryConfig config;
      config.name =
          std::string(name_prefix) + layer_def.name + "/" + primitive_def.name;
      config.type = to_primitive_type(primitive_def.type);
      config.position = primitive_def.position;
      config.rotation = primitive_def.rotation;
      config.params = primitive_def.params;
      config.extra_param = primitive_def.extra_param;
      config.twist = primitive_def.twist;
      config.bend = primitive_def.bend;
      config.displace_amplitude = primitive_def.displace_amplitude;
      config.displace_frequency = primitive_def.displace_frequency;
      config.param_expressions = primitive_def.param_expressions;
      config.repetition_mode = to_repetition_mode(primitive_def.repetition_mode);
      config.repetition_cell = primitive_def.repetition_cell;
      config.repetition_count = primitive_def.repetition_count;
      config.material_name = primitive_def.material_name;

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
    config.material_name = volumetric_def.material_name;

    acquire_volumetric(config, auto_release);
    result.volumetric_names.push_back(config.name);
  }

  return result;
}

bool GeometrySystem::reconcile_scene(const SdfScene &scene,
                                     LoadedSceneNames &loaded,
                                     bool auto_release,
                                     std::string_view name_prefix) {
  bool changed = false;
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
  for (const SdfLayerDef &layer_def : scene.layers) {
    auto existing = loaded.layer_index_by_name.find(layer_def.name);
    u32 layer_index;
    if (existing != loaded.layer_index_by_name.end() &&
        existing->second < layers_.size()) {
      layer_index = existing->second;
    } else {
      // Same reuse-a-freed-slot-first policy as load_scene() -- see its
      // own comment for why (unbounded layers_ growth otherwise).
      layer_index = 0;
      bool reused_slot = false;
      for (u32 i = 1; i < layer_ref_counts_.size(); ++i) {
        if (layer_ref_counts_[i] == 0) {
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

    LayerOperation new_operation = to_layer_operation(layer_def.operation);
    if (layers_[layer_index].operation != new_operation ||
        layers_[layer_index].smoothness != layer_def.smoothness) {
      layers_[layer_index].operation = new_operation;
      layers_[layer_index].smoothness = layer_def.smoothness;
      layers_needing_full_mark_dirty.insert(layer_index);
      changed = true;
    }
    new_layer_index_by_name[layer_def.name] = layer_index;
  }

  // --- Primitives: diff by full derived name (identical derivation to
  // load_scene()'s own "prefix + layer_name + / + primitive_name"). ---
  std::vector<std::string> new_primitive_names;
  std::unordered_set<std::string> new_primitive_name_set;
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
      new_primitive_name_set.insert(full_name);
      new_primitive_names.push_back(full_name);

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
        config.position = primitive_def.position;
        config.rotation = primitive_def.rotation;
        config.params = primitive_def.params;
        config.extra_param = primitive_def.extra_param;
        config.twist = primitive_def.twist;
        config.bend = primitive_def.bend;
        config.displace_amplitude = primitive_def.displace_amplitude;
        config.displace_frequency = primitive_def.displace_frequency;
        config.param_expressions = primitive_def.param_expressions;
        config.repetition_mode = to_repetition_mode(primitive_def.repetition_mode);
        config.repetition_cell = primitive_def.repetition_cell;
        config.repetition_count = primitive_def.repetition_count;
        config.material_name = primitive_def.material_name;

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
      if (existing->material_name != primitive_def.material_name) {
        material_system_->release(existing->material_name);
        existing->material =
            &material_system_->acquire(primitive_def.material_name, true);
        existing->material_name = primitive_def.material_name;
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
      if (layer_op_changed || layer_reassigned ||
          !primitive_shape_matches(*existing, primitive_def)) {
        existing->type = to_primitive_type(primitive_def.type);
        existing->position = primitive_def.position;
        existing->rotation = primitive_def.rotation;
        existing->params = primitive_def.params;
        existing->extra_param = primitive_def.extra_param;
        existing->twist = primitive_def.twist;
        existing->bend = primitive_def.bend;
        existing->displace_amplitude = primitive_def.displace_amplitude;
        existing->displace_frequency = primitive_def.displace_frequency;
        existing->param_expressions = primitive_def.param_expressions;
        existing->repetition_mode = to_repetition_mode(primitive_def.repetition_mode);
        existing->repetition_cell = primitive_def.repetition_cell;
        existing->repetition_count = primitive_def.repetition_count;
        mark_dirty(full_name);
        changed = true;
      }
    }
  }

  for (const std::string &old_name : loaded.primitive_names) {
    if (!new_primitive_name_set.count(old_name)) {
      release(old_name);
      changed = true;
    }
  }

  // --- Lights: diff by name, same acquire-new/release-gone/update-in-
  // place pattern as primitives above, minus the layer/mark_dirty concerns
  // -- lights aren't baked into the chunked/voxel field at all (see
  // Light's own comment), so nothing here affects update_streaming(). ---
  std::unordered_set<std::string> new_light_names;
  for (const SdfLightDef &light_def : scene.lights) {
    std::string full_name = std::string(name_prefix) + light_def.name;
    new_light_names.insert(full_name);

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
  for (const std::string &old_name : loaded.light_names) {
    if (!new_light_names.count(old_name)) {
      release_light(old_name);
      changed = true;
    }
  }

  // --- Volumetrics: same pattern as lights, plus the material handling
  // primitives use above (volumetrics DO have a material -- see
  // VolumetricConfig's comment). ---
  std::unordered_set<std::string> new_volumetric_names;
  for (const SdfVolumetricDef &volumetric_def : scene.volumetrics) {
    std::string full_name = std::string(name_prefix) + volumetric_def.name;
    new_volumetric_names.insert(full_name);

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
      config.material_name = volumetric_def.material_name;
      acquire_volumetric(config, auto_release);
      changed = true;
      continue;
    }
    if (existing->material_name != volumetric_def.material_name) {
      material_system_->release(existing->material_name);
      existing->material =
          &material_system_->acquire(volumetric_def.material_name, true);
      existing->material_name = volumetric_def.material_name;
      changed = true;
    }
    if (!volumetric_matches(*existing, volumetric_def)) {
      existing->type = to_primitive_type(volumetric_def.type);
      existing->position = volumetric_def.position;
      existing->rotation = volumetric_def.rotation;
      existing->params = volumetric_def.params;
      existing->extra_param = volumetric_def.extra_param;
      existing->density = volumetric_def.density;
      changed = true;
    }
  }
  for (const std::string &old_name : loaded.volumetric_names) {
    if (!new_volumetric_names.count(old_name)) {
      release_volumetric(old_name);
      changed = true;
    }
  }

  loaded.primitive_names = std::move(new_primitive_names);
  loaded.light_names.assign(new_light_names.begin(), new_light_names.end());
  loaded.volumetric_names.assign(new_volumetric_names.begin(),
                                 new_volumetric_names.end());
  loaded.layer_index_by_name = std::move(new_layer_index_by_name);

  return changed;
}
