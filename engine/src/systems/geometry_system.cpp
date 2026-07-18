#include "geometry_system.h"
#include "../core/logger.h"
#include "../resources/sdf_scene.h"

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
    geometry.param_expressions = config.param_expressions;
    geometry.material_name = config.material_name;
    geometry.material = &material_system_->acquire(config.material_name, true);
    entry.geometry = std::move(geometry);

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
      config.param_expressions = primitive_def.param_expressions;
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

  return result;
}
