#include "sdf_authoring.h"

#include <core/logger.h>

#include <fstream>
#include <iomanip>
#include <limits>

namespace {
const char *to_string(SdfPrimitiveType type) {
  switch (type) {
  case SdfPrimitiveType::Box:
    return "box";
  case SdfPrimitiveType::Plane:
    return "plane";
  case SdfPrimitiveType::Torus:
    return "torus";
  case SdfPrimitiveType::CappedCylinder:
    return "capped_cylinder";
  case SdfPrimitiveType::CappedCone:
    return "capped_cone";
  case SdfPrimitiveType::RoundBox:
    return "round_box";
  case SdfPrimitiveType::BoxFrame:
    return "box_frame";
  case SdfPrimitiveType::Octahedron:
    return "octahedron";
  case SdfPrimitiveType::Pyramid:
    return "pyramid";
  case SdfPrimitiveType::HexPrism:
    return "hex_prism";
  case SdfPrimitiveType::RoundCone:
    return "round_cone";
  case SdfPrimitiveType::Capsule:
    return "capsule";
  case SdfPrimitiveType::Link:
    return "link";
  case SdfPrimitiveType::Ellipsoid:
    return "ellipsoid";
  case SdfPrimitiveType::Sphere:
  default:
    return "sphere";
  }
}

const char *to_string(SdfLayerOperation operation) {
  return operation == SdfLayerOperation::Subtraction ? "subtraction" : "union";
}

const char *to_string(SdfRepetitionMode mode) {
  switch (mode) {
  case SdfRepetitionMode::Infinite:
    return "infinite";
  case SdfRepetitionMode::Limited:
    return "limited";
  case SdfRepetitionMode::Rotational:
    return "rotational";
  case SdfRepetitionMode::Rectangular:
    return "rectangular";
  case SdfRepetitionMode::None:
  default:
    return "none";
  }
}

// The two-letter shorthand parse_bend_axis() (sdf_scene.cpp) reads back.
const char *to_string(SdfBendAxis axis) {
  switch (axis) {
  case SdfBendAxis::XToZ:
    return "xz";
  case SdfBendAxis::YToZ:
    return "yz";
  case SdfBendAxis::YToX:
    return "yx";
  case SdfBendAxis::ZToX:
    return "zx";
  case SdfBendAxis::ZToY:
    return "zy";
  case SdfBendAxis::XToY:
  default:
    return "xy";
  }
}

const char *to_string(SdfLightType type) {
  return type == SdfLightType::Point ? "point" : "directional";
}

void write_vec3(std::ostream &out, const glm::vec3 &v) {
  out << v.x << ' ' << v.y << ' ' << v.z;
}
} // namespace

std::optional<SdfScene> read_scene(std::string_view path) {
  return load_sdf_scene(path);
}

bool save_scene(std::string_view path, const SdfScene &input) {
  std::ofstream file{std::string(path)};
  if (!file.is_open()) {
    KERROR("Failed to open SDF scene file for writing: '{}'.", path);
    return false;
  }

  // Written scenes are ALWAYS fully converted to the material library
  // form. Converting a copy here (rather than requiring every caller to
  // have done it, or writing whichever form each binding happens to be
  // in) is what makes the conversion total: a file this function produces
  // never mixes id bindings with legacy .kmt names, so it cannot later be
  // read back into a half-converted state.
  //
  // The caller's own scene is left alone -- a code-driven scene built with
  // add_box(..., "test_material") keeps working exactly as it did.
  SdfScene scene = input;
  sdf_scene_import_legacy_materials(scene);

  // ostream's default float precision is 6 significant digits -- lossy
  // for an f32 (needs up to max_digits10 == 9 to round-trip exactly).
  // Any caller that re-parses what it just wrote to compare against the
  // in-memory value (see GeometrySystem::reconcile_scene()'s primitive_
  // shape_matches(), engine-side -- exactly what sdf_editor's live-preview
  // sync does every edit) would otherwise see a tiny write/reparse drift
  // on EVERY float field of EVERY primitive, not just the one actually
  // edited, making every primitive look "changed" every single sync and
  // defeating that surgical-update path entirely -- the whole scene's
  // chunks would look dirty even when only one primitive moved. Set once,
  // stream-wide, before anything is written -- std::ofstream's precision
  // persists across every subsequent `<<` on this stream.
  file << std::setprecision(std::numeric_limits<f32>::max_digits10);

  file << "#sdf scene file\n";
  // 0.2 introduced the scene-embedded material library below. The reader
  // accepts both -- a 0.1 file names its materials by .kmt filename and is
  // converted on load (see sdf_scene_import_legacy_materials()) -- but
  // everything written from here on is 0.2.
  file << "version=0.2\n";
  file << "ambient=" << scene.ambient << "\n";
  // Only written when the scene actually names one -- an empty skybox means
  // "unspecified" rather than "off" (see SdfScene::skybox), so emitting an
  // empty line would be writing a value the reader would have to invent a
  // meaning for, and it keeps every file authored before this existed
  // byte-for-byte unchanged.
  if (!scene.skybox.empty()) {
    file << "skybox=" << scene.skybox << "\n";
  }

  // The material library, written FIRST so the file reads top-down: every
  // "material=<id>" line below refers to something already defined. (The
  // reader does not require this -- it resolves bindings in a pass after
  // the whole file is read -- but a file a human may open should not make
  // them scroll to find out what a primitive is made of.)
  //
  // The block header carries the display name, exactly as a layer or light
  // block does, which is why material_def_write() does not emit one.
  for (const MaterialDef &material : scene.materials) {
    file << "\nmaterial " << material.display_name << " {\n";
    material_def_write(material, file, "    ");
    file << "}\n";
  }

  for (const SdfLightDef &light : scene.lights) {
    file << "\nlight " << light.name << " {\n";
    file << "    type=" << to_string(light.type) << "\n";
    if (light.type == SdfLightType::Point) {
      file << "    position=";
      write_vec3(file, light.position);
      file << "\n";
    } else {
      file << "    direction=";
      write_vec3(file, light.direction);
      file << "\n";
    }
    file << "    colour=";
    write_vec3(file, light.colour);
    file << "\n    intensity=" << light.intensity << "\n";
    file << "}\n";
  }

  for (const SdfLayerDef &layer : scene.layers) {
    file << "\nlayer " << layer.name << " {\n";
    file << "    operation=" << to_string(layer.operation) << "\n";
    file << "    smoothness=" << layer.smoothness << "\n";
    // The layer's own transform -- only emitted when it is not the
    // identity, the same "keeps files written before this existed
    // byte-for-byte unchanged" convention the repetition below uses. Both
    // are written whenever either is set, so the pair always reads
    // together.
    if (layer.position != glm::vec3(0.0f) || layer.rotation != glm::vec3(0.0f)) {
      file << "    position=";
      write_vec3(file, layer.position);
      file << "\n    rotation=";
      write_vec3(file, layer.rotation);
      file << "\n";
    }
    // The layer's own repetition -- only emitted when it actually repeats,
    // same "keeps files written before this existed byte-for-byte
    // unchanged" convention the per-primitive repetition below uses.
    if (layer.repetition_mode != SdfRepetitionMode::None) {
      file << "    repetition=" << to_string(layer.repetition_mode) << "\n";
      file << "    repetition_cell=";
      write_vec3(file, layer.repetition_cell);
      file << "\n    repetition_count=";
      write_vec3(file, layer.repetition_count);
      file << "\n";
    }

    for (const SdfPrimitiveDef &primitive : layer.primitives) {
      file << "\n    primitive " << primitive.name << " {\n";
      file << "        type=" << to_string(primitive.type) << "\n";
      switch (primitive.type) {
      case SdfPrimitiveType::Sphere:
        file << "        position=";
        write_vec3(file, primitive.position);
        file << "\n        rotation=";
        write_vec3(file, primitive.rotation);
        file << "\n        radius=" << primitive.params.x << "\n";
        break;
      case SdfPrimitiveType::Box:
        file << "        position=";
        write_vec3(file, primitive.position);
        file << "\n        rotation=";
        write_vec3(file, primitive.rotation);
        file << "\n        half_extents=";
        write_vec3(file, primitive.params);
        file << "\n";
        break;
      case SdfPrimitiveType::Plane:
        // Matches the loader's convention (see assets/scenes/*.sdf): a
        // plane's world position is fixed (GeometryConfig::plane() always
        // uses vec3(0)), only its height (params.x) is meaningful, so
        // there's no position= line to emit.
        file << "        height=" << primitive.params.x << "\n";
        break;
      default:
        // Every other type (see SdfPrimitiveType's comment) uses the
        // generic "params=x y z w" line instead of named keys.
        file << "        position=";
        write_vec3(file, primitive.position);
        file << "\n        rotation=";
        write_vec3(file, primitive.rotation);
        file << "\n        params=" << primitive.params.x << ' '
            << primitive.params.y << ' ' << primitive.params.z << ' '
            << primitive.extra_param << "\n";
        break;
      }
      for (size_t slot = 0; slot < primitive.param_expressions.size(); ++slot) {
        const std::string &formula = primitive.param_expressions[slot];
        if (!formula.empty()) {
          file << "        param_expr=" << slot << ' ' << formula << "\n";
        }
      }
      // Only emitted when actually repeated -- keeps every file authored
      // before this existed byte-for-byte unchanged, same convention as
      // param_expr= above.
      if (primitive.repetition_mode != SdfRepetitionMode::None) {
        file << "        repetition=" << to_string(primitive.repetition_mode) << "\n";
        file << "        repetition_cell=";
        write_vec3(file, primitive.repetition_cell);
        file << "\n        repetition_count=";
        write_vec3(file, primitive.repetition_count);
        file << "\n";
      }
      // Only emitted when actually deformed -- same "keeps old files
      // unchanged" reasoning as repetition above. displace_frequency
      // defaults to 20 (not 0, see SdfPrimitiveDef's own comment), so its
      // own check is against that default, not 0.
      if (primitive.twist != 0.0f || primitive.bend != 0.0f ||
          primitive.bend_axis != SdfBendAxis::XToY ||
          primitive.displace_amplitude != 0.0f ||
          primitive.displace_frequency != 20.0f) {
        file << "        twist=" << primitive.twist << "\n";
        file << "        bend=" << primitive.bend << "\n";
        file << "        bend_axis=" << to_string(primitive.bend_axis) << "\n";
        file << "        displace_amplitude=" << primitive.displace_amplitude << "\n";
        file << "        displace_frequency=" << primitive.displace_frequency << "\n";
      }
      // Only ever the id form: a scene converts on its first save and
      // never half-converts, so a legacy .kmt name cannot survive a
      // round trip through the editor.
      file << "        material="
          << (primitive.material_id != kInvalidMaterialId
                  ? material_id_to_string(primitive.material_id)
                  : primitive.material_name)
          << "\n";
      for (const MaterialOverride &override_entry :
           primitive.material_overrides) {
        file << "        material_override=" << override_entry.key << ' '
            << override_entry.value << "\n";
      }
      file << "    }\n";
    }

    file << "}\n";
  }

  for (const SdfVolumetricDef &volumetric : scene.volumetrics) {
    file << "\nvolumetric " << volumetric.name << " {\n";
    file << "    type=" << to_string(volumetric.type) << "\n";
    file << "    position=";
    write_vec3(file, volumetric.position);
    file << "\n    rotation=";
    write_vec3(file, volumetric.rotation);
    file << "\n    params=" << volumetric.params.x << ' ' << volumetric.params.y
        << ' ' << volumetric.params.z << ' ' << volumetric.extra_param << "\n";
    file << "    density=" << volumetric.density << "\n";
    file << "    material="
        << (volumetric.material_id != kInvalidMaterialId
                ? material_id_to_string(volumetric.material_id)
                : volumetric.material_name)
        << "\n";
    for (const MaterialOverride &override_entry :
         volumetric.material_overrides) {
      file << "    material_override=" << override_entry.key << ' '
          << override_entry.value << "\n";
    }
    file << "}\n";
  }

  return true;
}

SdfLayerDef &add_layer(SdfScene &scene, std::string name,
                      SdfLayerOperation operation, f32 smoothness) {
  SdfLayerDef layer;
  layer.name = std::move(name);
  layer.operation = operation;
  layer.smoothness = smoothness;
  scene.layers.push_back(std::move(layer));
  return scene.layers.back();
}

SdfPrimitiveDef &add_sphere(SdfLayerDef &layer, std::string name,
                           glm::vec3 position, glm::vec3 rotation,
                           f32 radius, std::string material_name) {
  SdfPrimitiveDef primitive;
  primitive.name = std::move(name);
  primitive.type = SdfPrimitiveType::Sphere;
  primitive.position = position;
  primitive.rotation = rotation;
  primitive.params = glm::vec3(radius, 0.0f, 0.0f);
  primitive.material_name = std::move(material_name);
  layer.primitives.push_back(std::move(primitive));
  return layer.primitives.back();
}

SdfPrimitiveDef &add_box(SdfLayerDef &layer, std::string name,
                        glm::vec3 position, glm::vec3 rotation,
                        glm::vec3 half_extents, std::string material_name) {
  SdfPrimitiveDef primitive;
  primitive.name = std::move(name);
  primitive.type = SdfPrimitiveType::Box;
  primitive.position = position;
  primitive.rotation = rotation;
  primitive.params = half_extents;
  primitive.material_name = std::move(material_name);
  layer.primitives.push_back(std::move(primitive));
  return layer.primitives.back();
}

SdfPrimitiveDef &add_plane(SdfLayerDef &layer, std::string name, f32 height,
                         std::string material_name) {
  SdfPrimitiveDef primitive;
  primitive.name = std::move(name);
  primitive.type = SdfPrimitiveType::Plane;
  primitive.position = glm::vec3(0.0f);
  primitive.params = glm::vec3(height, 0.0f, 0.0f);
  primitive.material_name = std::move(material_name);
  layer.primitives.push_back(std::move(primitive));
  return layer.primitives.back();
}

SdfPrimitiveDef &add_primitive(SdfLayerDef &layer, std::string name,
                              SdfPrimitiveType type, glm::vec3 position,
                              glm::vec3 rotation, glm::vec3 params,
                              f32 extra_param, std::string material_name) {
  SdfPrimitiveDef primitive;
  primitive.name = std::move(name);
  primitive.type = type;
  primitive.position = position;
  primitive.rotation = rotation;
  primitive.params = params;
  primitive.extra_param = extra_param;
  primitive.material_name = std::move(material_name);
  layer.primitives.push_back(std::move(primitive));
  return layer.primitives.back();
}

SdfLightDef &add_directional_light(SdfScene &scene, std::string name,
                                   glm::vec3 direction, glm::vec3 colour,
                                   f32 intensity) {
  SdfLightDef light;
  light.name = std::move(name);
  light.type = SdfLightType::Directional;
  light.direction = direction;
  light.colour = colour;
  light.intensity = intensity;
  scene.lights.push_back(std::move(light));
  return scene.lights.back();
}

SdfLightDef &add_point_light(SdfScene &scene, std::string name,
                            glm::vec3 position, glm::vec3 colour,
                            f32 intensity) {
  SdfLightDef light;
  light.name = std::move(name);
  light.type = SdfLightType::Point;
  light.position = position;
  light.colour = colour;
  light.intensity = intensity;
  scene.lights.push_back(std::move(light));
  return scene.lights.back();
}

SdfVolumetricDef &add_volumetric(SdfScene &scene, std::string name,
                                 SdfPrimitiveType type, glm::vec3 position,
                                 glm::vec3 rotation, glm::vec3 params,
                                 f32 extra_param, f32 density,
                                 std::string material_name) {
  SdfVolumetricDef volumetric;
  volumetric.name = std::move(name);
  volumetric.type = type;
  volumetric.position = position;
  volumetric.rotation = rotation;
  volumetric.params = params;
  volumetric.extra_param = extra_param;
  volumetric.density = density;
  volumetric.material_name = std::move(material_name);
  scene.volumetrics.push_back(std::move(volumetric));
  return scene.volumetrics.back();
}
