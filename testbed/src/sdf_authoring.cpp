#include "sdf_authoring.h"

#include <core/logger.h>

#include <fstream>

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

bool save_scene(std::string_view path, const SdfScene &scene) {
  std::ofstream file{std::string(path)};
  if (!file.is_open()) {
    KERROR("Failed to open SDF scene file for writing: '{}'.", path);
    return false;
  }

  file << "#sdf scene file\n";
  file << "version=0.1\n";
  file << "ambient=" << scene.ambient << "\n";

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
      file << "        material=" << primitive.material_name << "\n";
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
    file << "    material=" << volumetric.material_name << "\n";
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
