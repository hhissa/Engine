#include "geometry_system.h"
#include "../core/logger.h"

namespace {
// Acquires the runtime material a config asks for, by content when the
// caller resolved a MaterialDef and by legacy .kmt name otherwise, and
// reports the key it was cached under.
//
// That key is what gets stored on Volumetric::material_name and what
// release() must later be called with -- NOT the author-facing name, which
// for a library material is a renameable label that MaterialSystem never
// sees.
template <typename Config>
Material &acquire_config_material(MaterialSystem &materials,
                                 const Config &config, std::string &out_key) {
  Material &material = config.material_def
                          ? materials.acquire_def(*config.material_def, true)
                          : materials.acquire(config.material_name, true);
  out_key = material.name;
  return material;
}
} // namespace

GeometrySystem::GeometrySystem(MaterialSystem &material_system)
    : material_system_(&material_system) {}

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
