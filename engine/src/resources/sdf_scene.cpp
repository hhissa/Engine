#include "sdf_scene.h"
#include "../core/logger.h"

#include <fstream>
#include <sstream>

namespace {
std::string trim(const std::string &s) {
  constexpr const char *kWhitespace = " \t\r\n";
  auto start = s.find_first_not_of(kWhitespace);
  if (start == std::string::npos) {
    return "";
  }
  auto end = s.find_last_not_of(kWhitespace);
  return s.substr(start, end - start + 1);
}

enum class Context { TopLevel, Layer, Primitive, Light, Volumetric };

// Shared by Primitive and Volumetric contexts below -- both blocks use the
// exact same "type=" string set (see SdfPrimitiveType's comment).
bool parse_primitive_type(const std::string &value, SdfPrimitiveType &out) {
  if (value == "sphere") {
    out = SdfPrimitiveType::Sphere;
  } else if (value == "box") {
    out = SdfPrimitiveType::Box;
  } else if (value == "plane") {
    out = SdfPrimitiveType::Plane;
  } else if (value == "torus") {
    out = SdfPrimitiveType::Torus;
  } else if (value == "capped_cylinder") {
    out = SdfPrimitiveType::CappedCylinder;
  } else if (value == "capped_cone") {
    out = SdfPrimitiveType::CappedCone;
  } else if (value == "round_box") {
    out = SdfPrimitiveType::RoundBox;
  } else if (value == "box_frame") {
    out = SdfPrimitiveType::BoxFrame;
  } else if (value == "octahedron") {
    out = SdfPrimitiveType::Octahedron;
  } else if (value == "pyramid") {
    out = SdfPrimitiveType::Pyramid;
  } else if (value == "hex_prism") {
    out = SdfPrimitiveType::HexPrism;
  } else if (value == "round_cone") {
    out = SdfPrimitiveType::RoundCone;
  } else if (value == "capsule") {
    out = SdfPrimitiveType::Capsule;
  } else if (value == "link") {
    out = SdfPrimitiveType::Link;
  } else if (value == "ellipsoid") {
    out = SdfPrimitiveType::Ellipsoid;
  } else {
    return false;
  }
  return true;
}

bool parse_vec3(const std::string &value, glm::vec3 &out) {
  std::istringstream iss(value);
  glm::vec3 v;
  iss >> v.x >> v.y >> v.z;
  if (iss.fail()) {
    return false;
  }
  out = v;
  return true;
}

// Backs the generic "params=x y z w" line used by every primitive type
// that doesn't have its own named key(s) -- see SdfPrimitiveType's comment.
bool parse_vec4(const std::string &value, glm::vec4 &out) {
  std::istringstream iss(value);
  glm::vec4 v;
  iss >> v.x >> v.y >> v.z >> v.w;
  if (iss.fail()) {
    return false;
  }
  out = v;
  return true;
}
} // namespace

std::optional<SdfScene> load_sdf_scene(std::string_view path) {
  std::ifstream file{std::string(path)};
  if (!file.is_open()) {
    KERROR("Failed to open SDF scene file: '{}'.", path);
    return std::nullopt;
  }

  SdfScene scene;
  Context context = Context::TopLevel;
  SdfLayerDef current_layer;
  SdfPrimitiveDef current_primitive;
  SdfLightDef current_light;
  SdfVolumetricDef current_volumetric;

  std::string line;
  int line_number = 0;
  while (std::getline(file, line)) {
    ++line_number;
    std::string trimmed = trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }

    if (trimmed == "}") {
      if (context == Context::Primitive) {
        current_layer.primitives.push_back(std::move(current_primitive));
        current_primitive = SdfPrimitiveDef{};
        context = Context::Layer;
      } else if (context == Context::Layer) {
        scene.layers.push_back(std::move(current_layer));
        current_layer = SdfLayerDef{};
        context = Context::TopLevel;
      } else if (context == Context::Light) {
        scene.lights.push_back(std::move(current_light));
        current_light = SdfLightDef{};
        context = Context::TopLevel;
      } else if (context == Context::Volumetric) {
        scene.volumetrics.push_back(std::move(current_volumetric));
        current_volumetric = SdfVolumetricDef{};
        context = Context::TopLevel;
      } else {
        KWARN("'{}': unexpected '}}' at line {}.", path, line_number);
      }
      continue;
    }

    // "layer <name> {" or "primitive <name> {" or "light <name> {"
    if (!trimmed.empty() && trimmed.back() == '{') {
      std::string header = trim(trimmed.substr(0, trimmed.size() - 1));
      auto space = header.find(' ');
      std::string keyword =
          space == std::string::npos ? header : header.substr(0, space);
      std::string name =
          space == std::string::npos ? "" : trim(header.substr(space + 1));

      if (keyword == "layer" && context == Context::TopLevel) {
        current_layer = SdfLayerDef{};
        current_layer.name = name;
        context = Context::Layer;
      } else if (keyword == "primitive" && context == Context::Layer) {
        current_primitive = SdfPrimitiveDef{};
        current_primitive.name = name;
        context = Context::Primitive;
      } else if (keyword == "light" && context == Context::TopLevel) {
        current_light = SdfLightDef{};
        current_light.name = name;
        context = Context::Light;
      } else if (keyword == "volumetric" && context == Context::TopLevel) {
        current_volumetric = SdfVolumetricDef{};
        current_volumetric.name = name;
        context = Context::Volumetric;
      } else {
        KWARN("'{}': unexpected block '{}' at line {}.", path, header,
             line_number);
      }
      continue;
    }

    // key=value
    auto eq = trimmed.find('=');
    if (eq == std::string::npos) {
      KWARN("'{}': malformed line {}: '{}'.", path, line_number, trimmed);
      continue;
    }
    std::string key = trim(trimmed.substr(0, eq));
    std::string value = trim(trimmed.substr(eq + 1));

    if (context == Context::Layer) {
      if (key == "operation") {
        if (value == "union") {
          current_layer.operation = SdfLayerOperation::Union;
        } else if (value == "subtraction") {
          current_layer.operation = SdfLayerOperation::Subtraction;
        } else {
          KWARN("'{}': unknown layer operation '{}' at line {}; defaulting "
               "to union.",
               path, value, line_number);
        }
      } else if (key == "smoothness") {
        current_layer.smoothness = std::stof(value);
      } else {
        KWARN("'{}': unknown layer property '{}' at line {}.", path, key,
             line_number);
      }
    } else if (context == Context::Primitive) {
      if (key == "type") {
        if (!parse_primitive_type(value, current_primitive.type)) {
          KWARN("'{}': unknown primitive type '{}' at line {}.", path, value,
               line_number);
        }
      } else if (key == "params") {
        glm::vec4 v;
        if (parse_vec4(value, v)) {
          current_primitive.params = glm::vec3(v);
          current_primitive.extra_param = v.w;
        } else {
          KWARN("'{}': malformed params '{}' at line {}.", path, value,
               line_number);
        }
      } else if (key == "position") {
        glm::vec3 v;
        if (parse_vec3(value, v)) {
          current_primitive.position = v;
        } else {
          KWARN("'{}': malformed position '{}' at line {}.", path, value,
               line_number);
        }
      } else if (key == "rotation") {
        glm::vec3 v;
        if (parse_vec3(value, v)) {
          current_primitive.rotation = v;
        } else {
          KWARN("'{}': malformed rotation '{}' at line {}.", path, value,
               line_number);
        }
      } else if (key == "radius" || key == "height") {
        current_primitive.params.x = std::stof(value);
      } else if (key == "half_extents") {
        glm::vec3 v;
        if (parse_vec3(value, v)) {
          current_primitive.params = v;
        } else {
          KWARN("'{}': malformed half_extents '{}' at line {}.", path, value,
               line_number);
        }
      } else if (key == "param_expr") {
        auto space = value.find(' ');
        std::string slot_str = space == std::string::npos ? value : value.substr(0, space);
        std::string formula =
            space == std::string::npos ? "" : trim(value.substr(space + 1));
        int slot = -1;
        try {
          slot = std::stoi(slot_str);
        } catch (...) {
          slot = -1;
        }
        if (slot < 0 || slot > 3 || formula.empty()) {
          KWARN("'{}': malformed param_expr '{}' at line {}; expected "
               "'<0-3> <formula>'.",
               path, value, line_number);
        } else {
          current_primitive.param_expressions[static_cast<size_t>(slot)] = formula;
        }
      } else if (key == "material") {
        current_primitive.material_name = value;
      } else {
        KWARN("'{}': unknown primitive property '{}' at line {}.", path, key,
             line_number);
      }
    } else if (context == Context::Light) {
      if (key == "type") {
        if (value == "directional") {
          current_light.type = SdfLightType::Directional;
        } else if (value == "point") {
          current_light.type = SdfLightType::Point;
        } else {
          KWARN("'{}': unknown light type '{}' at line {}; defaulting to "
               "directional.",
               path, value, line_number);
        }
      } else if (key == "direction") {
        glm::vec3 v;
        if (parse_vec3(value, v)) {
          current_light.direction = v;
        } else {
          KWARN("'{}': malformed direction '{}' at line {}.", path, value,
               line_number);
        }
      } else if (key == "position") {
        glm::vec3 v;
        if (parse_vec3(value, v)) {
          current_light.position = v;
        } else {
          KWARN("'{}': malformed position '{}' at line {}.", path, value,
               line_number);
        }
      } else if (key == "colour" || key == "color") {
        glm::vec3 v;
        if (parse_vec3(value, v)) {
          current_light.colour = v;
        } else {
          KWARN("'{}': malformed colour '{}' at line {}.", path, value,
               line_number);
        }
      } else if (key == "intensity") {
        current_light.intensity = std::stof(value);
      } else {
        KWARN("'{}': unknown light property '{}' at line {}.", path, key,
             line_number);
      }
    } else if (context == Context::Volumetric) {
      if (key == "type") {
        if (!parse_primitive_type(value, current_volumetric.type)) {
          KWARN("'{}': unknown volumetric type '{}' at line {}.", path, value,
               line_number);
        }
      } else if (key == "position") {
        glm::vec3 v;
        if (parse_vec3(value, v)) {
          current_volumetric.position = v;
        } else {
          KWARN("'{}': malformed position '{}' at line {}.", path, value,
               line_number);
        }
      } else if (key == "rotation") {
        glm::vec3 v;
        if (parse_vec3(value, v)) {
          current_volumetric.rotation = v;
        } else {
          KWARN("'{}': malformed rotation '{}' at line {}.", path, value,
               line_number);
        }
      } else if (key == "params") {
        glm::vec4 v;
        if (parse_vec4(value, v)) {
          current_volumetric.params = glm::vec3(v);
          current_volumetric.extra_param = v.w;
        } else {
          KWARN("'{}': malformed params '{}' at line {}.", path, value,
               line_number);
        }
      } else if (key == "density") {
        current_volumetric.density = std::stof(value);
      } else if (key == "material") {
        current_volumetric.material_name = value;
      } else {
        KWARN("'{}': unknown volumetric property '{}' at line {}.", path, key,
             line_number);
      }
    } else if (key == "ambient") {
      scene.ambient = std::stof(value);
    } else if (key != "version") {
      // "version" is intentionally ignored at the top level, like the
      // material file format -- nothing yet depends on it.
      KWARN("'{}': unexpected top-level property '{}' at line {}.", path,
           key, line_number);
    }
  }

  if (context != Context::TopLevel) {
    KWARN("'{}': file ended with unclosed block(s).", path);
  }

  return scene;
}
