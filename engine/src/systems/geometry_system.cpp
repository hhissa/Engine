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
