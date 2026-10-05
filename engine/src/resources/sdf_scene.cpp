#include "sdf_scene.h"
#include "../core/logger.h"

#include <fstream>
#include <sstream>
#include <unordered_map>

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

enum class Context { TopLevel, Layer, Primitive, Light, Volumetric, Material };

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

bool parse_repetition_mode(const std::string &value, SdfRepetitionMode &out) {
  if (value == "none") {
    out = SdfRepetitionMode::None;
  } else if (value == "infinite") {
    out = SdfRepetitionMode::Infinite;
  } else if (value == "limited") {
    out = SdfRepetitionMode::Limited;
  } else if (value == "rotational") {
    out = SdfRepetitionMode::Rotational;
  } else if (value == "rectangular") {
    out = SdfRepetitionMode::Rectangular;
  } else {
    return false;
  }
  return true;
}

// "<drive><target>" (e.g. "xy", "zx") -- the same two-letter shorthand
// SdfBendAxis' own names use, minus the "To". Deliberately not accepting a
// bare axis letter: a bend needs both a drive axis and a target, and
// guessing the second one would silently pick a direction the author never
// asked for.
bool parse_bend_axis(const std::string &value, SdfBendAxis &out) {
  if (value == "xy") {
    out = SdfBendAxis::XToY;
  } else if (value == "xz") {
    out = SdfBendAxis::XToZ;
  } else if (value == "yz") {
    out = SdfBendAxis::YToZ;
  } else if (value == "yx") {
    out = SdfBendAxis::YToX;
  } else if (value == "zx") {
    out = SdfBendAxis::ZToX;
  } else if (value == "zy") {
    out = SdfBendAxis::ZToY;
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

// "material_override=<property> <value>" -- splits at the FIRST space, so
// the value keeps its own spaces ("base_colour 0.6 0.5 0.4 1") without
// needing quoting, exactly like param_expr= already does for formulas.
bool parse_material_override(const std::string &value, MaterialOverride &out) {
  auto space = value.find(' ');
  if (space == std::string::npos || space == 0) {
    return false;
  }
  out.key = value.substr(0, space);
  out.value = trim(value.substr(space + 1));
  return !out.value.empty();
}

// Decides, for every binding in the scene, whether its "material=" value
// was a MaterialId or a legacy .kmt name.
//
// The rule is deliberately conservative: a value is an id ONLY if it parses
// as hex AND names a material actually present in this scene's library.
// Anything else stays a legacy name. That ordering matters -- a legacy
// name like "qt_colour_dddbbeff_ts060_white" is not valid hex so it could
// never be mistaken for an id, but a short all-hex texture name could be,
// and requiring the id to resolve rules that out entirely.
void resolve_material_bindings(SdfScene &scene) {
  auto resolve = [&](MaterialId &id, std::string &name) {
    if (id != kInvalidMaterialId || name.empty()) {
      return;
    }
    MaterialId parsed = kInvalidMaterialId;
    if (material_id_from_string(name, parsed) &&
        sdf_scene_find_material(scene, parsed) != nullptr) {
      id = parsed;
      name.clear();
    }
  };

  for (SdfLayerDef &layer : scene.layers) {
    for (SdfPrimitiveDef &primitive : layer.primitives) {
      resolve(primitive.material_id, primitive.material_name);
    }
  }
  for (SdfVolumetricDef &volumetric : scene.volumetrics) {
    resolve(volumetric.material_id, volumetric.material_name);
  }
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
  MaterialDef current_material;

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
      } else if (context == Context::Material) {
        // A material block with no id= line is still usable -- it just
        // gets a fresh one, so a hand-written scene does not have to
        // invent 16 hex digits to try something out.
        if (current_material.id == kInvalidMaterialId) {
          current_material.id = material_id_generate();
        }
        scene.materials.push_back(std::move(current_material));
        current_material = MaterialDef{};
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
      } else if (keyword == "material" && context == Context::TopLevel) {
        // The block header carries the display name, exactly as it does
        // for a layer or a light -- which is why MaterialDef's writer
        // deliberately does not emit a display_name= line (see
        // material_def_write()).
        current_material = MaterialDef{};
        current_material.display_name = name;
        context = Context::Material;
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
      } else if (key == "position") {
        // Same two keys, same parser and same units (radians for rotation)
        // a primitive block uses -- see SdfLayerDef::position for what
        // applying them to a whole layer means.
        glm::vec3 v;
        if (parse_vec3(value, v)) {
          current_layer.position = v;
        } else {
          KWARN("'{}': malformed layer position '{}' at line {}.", path, value,
               line_number);
        }
      } else if (key == "rotation") {
        glm::vec3 v;
        if (parse_vec3(value, v)) {
          current_layer.rotation = v;
        } else {
          KWARN("'{}': malformed layer rotation '{}' at line {}.", path, value,
               line_number);
        }
      } else if (key == "repetition") {
        // Same three keys a primitive block accepts, with the same
        // parsers -- see SdfLayerDef::repetition_mode for what applying
        // them to a whole layer means.
        if (!parse_repetition_mode(value, current_layer.repetition_mode)) {
          KWARN("'{}': unknown layer repetition mode '{}' at line {}.", path,
               value, line_number);
        }
      } else if (key == "repetition_cell") {
        glm::vec3 v;
        if (parse_vec3(value, v)) {
          current_layer.repetition_cell = v;
        } else {
          KWARN("'{}': malformed layer repetition_cell '{}' at line {}.", path,
               value, line_number);
        }
      } else if (key == "repetition_count") {
        glm::vec3 v;
        if (parse_vec3(value, v)) {
          current_layer.repetition_count = v;
        } else {
          KWARN("'{}': malformed layer repetition_count '{}' at line {}.", path,
               value, line_number);
        }
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
      } else if (key == "repetition") {
        if (!parse_repetition_mode(value, current_primitive.repetition_mode)) {
          KWARN("'{}': unknown repetition mode '{}' at line {}.", path, value,
               line_number);
        }
      } else if (key == "repetition_cell") {
        glm::vec3 v;
        if (parse_vec3(value, v)) {
          current_primitive.repetition_cell = v;
        } else {
          KWARN("'{}': malformed repetition_cell '{}' at line {}.", path,
               value, line_number);
        }
      } else if (key == "repetition_count") {
        glm::vec3 v;
        if (parse_vec3(value, v)) {
          current_primitive.repetition_count = v;
        } else {
          KWARN("'{}': malformed repetition_count '{}' at line {}.", path,
               value, line_number);
        }
      } else if (key == "twist") {
        current_primitive.twist = std::stof(value);
      } else if (key == "bend") {
        current_primitive.bend = std::stof(value);
      } else if (key == "bend_axis") {
        if (!parse_bend_axis(value, current_primitive.bend_axis)) {
          KWARN("'{}': unknown bend axis '{}' at line {}; expected one of "
               "xy/xz/yz/yx/zx/zy.",
               path, value, line_number);
        }
      } else if (key == "displace_amplitude") {
        current_primitive.displace_amplitude = std::stof(value);
      } else if (key == "displace_frequency") {
        current_primitive.displace_frequency = std::stof(value);
      } else if (key == "material") {
        // Either a MaterialId or a legacy .kmt name -- which one is not
        // decidable here, because the "material" block this may name can
        // appear later in the file. Stored verbatim and resolved in one
        // pass after the whole file is read (see resolve_material_bindings()
        // below).
        current_primitive.material_name = value;
      } else if (key == "material_override") {
        MaterialOverride override_entry;
        if (parse_material_override(value, override_entry)) {
          current_primitive.material_overrides.push_back(
              std::move(override_entry));
        } else {
          KWARN("'{}': malformed material_override '{}' at line {}; expected "
               "'<property> <value>'.",
               path, value, line_number);
        }
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
        current_volumetric.material_name = value; // see the primitive case
      } else if (key == "material_override") {
        MaterialOverride override_entry;
        if (parse_material_override(value, override_entry)) {
          current_volumetric.material_overrides.push_back(
              std::move(override_entry));
        } else {
          KWARN("'{}': malformed material_override '{}' at line {}; expected "
               "'<property> <value>'.",
               path, value, line_number);
        }
      } else {
        KWARN("'{}': unknown volumetric property '{}' at line {}.", path, key,
             line_number);
      }
    } else if (context == Context::Material) {
      if (key == "id") {
        if (!material_id_from_string(value, current_material.id)) {
          KWARN("'{}': malformed material id '{}' at line {}; a fresh one "
               "will be generated.",
               path, value, line_number);
        }
      } else if (!material_def_set_property(current_material, key, value)) {
        // Not a property this build knows about. Preserved verbatim rather
        // than dropped, so a scene written by a newer editor survives a
        // round trip through an older one -- see MaterialDef::unknown_keys.
        current_material.unknown_keys.emplace_back(key, value);
      }
    } else if (key == "ambient") {
      scene.ambient = std::stof(value);
    } else if (key == "skybox") {
      scene.skybox = value;
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

  // Now that every material block has been seen, decide for each binding
  // whether its "material=" value was an id or a legacy .kmt name. Doing
  // this here rather than at the point of parse is what makes the file
  // order-independent: a primitive may reference a material block that
  // appears after it.
  resolve_material_bindings(scene);
  // Then fold any legacy references into the library, so nothing
  // downstream ever has to know both forms exist.
  const u32 imported = sdf_scene_import_legacy_materials(scene);
  if (imported > 0) {
    KINFO("'{}': imported {} legacy .kmt material(s) into the scene's "
         "material library.",
         path, imported);
  }

  return scene;
}

glm::quat sdf_layer_rotation(const SdfLayerDef &layer) {
  return glm::quat(layer.rotation);
}

namespace {
// Whether this layer has a transform at all -- which is to say, whether
// composing it onto a primitive can change anything.
//
// Both compositions below round a rotation through
// quat -> compose -> eulerAngles, and that round trip is not bit-exact
// even against the identity: it returns angles encoding the SAME rotation,
// not the same triple. Harmless for what renders, ruinous for what
// compares. GeometrySystem::reconcile_scene() diffs the composed transform
// against the one it already holds to decide whether a primitive needs a
// chunk re-bake (see primitive_shape_matches()), so an identity layer that
// nudged the last bit of every Euler angle would report every rotated
// primitive in the scene as moved, on every single edit, and re-bake all
// of them.
//
// So the untouched case is answered exactly, by not doing the arithmetic
// at all -- which is also every layer in every scene authored before layer
// transforms existed.
bool layer_is_untransformed(const SdfLayerDef &layer) {
  return layer.position == glm::vec3(0.0f) && layer.rotation == glm::vec3(0.0f);
}
} // namespace

SdfTransform sdf_layer_world_transform(const SdfLayerDef &layer,
                                       const SdfPrimitiveDef &primitive) {
  if (layer_is_untransformed(layer) ||
      primitive.type == SdfPrimitiveType::Plane) {
    // Nothing to compose, or nothing to compose onto -- see
    // SdfLayerDef::position.
    return SdfTransform{primitive.position, primitive.rotation};
  }
  const glm::quat layer_rotation = sdf_layer_rotation(layer);
  // Rotate about the layer's origin first, then translate -- the ordinary
  // rigid composition, and the one that makes the layer's own `position`
  // read as "where the layer's origin is" rather than "how far its
  // contents were nudged after being spun".
  //
  // eulerAngles() of the composed quaternion, rather than adding the two
  // Euler triples: Euler angles do not add (XYZ rotations don't commute),
  // and the angles this returns need not match either input -- only the
  // rotation they encode has to, which is exactly what downstream reads
  // out of them. Same round-trip SceneViewport's rotate drag already does
  // when it composes a delta onto a primitive's own orientation.
  return SdfTransform{
      layer.position + layer_rotation * primitive.position,
      glm::eulerAngles(layer_rotation * glm::quat(primitive.rotation))};
}

SdfTransform sdf_layer_local_transform(const SdfLayerDef &layer,
                                       SdfPrimitiveType type,
                                       glm::vec3 world_position,
                                       glm::vec3 world_rotation) {
  if (layer_is_untransformed(layer) || type == SdfPrimitiveType::Plane) {
    return SdfTransform{world_position, world_rotation};
  }
  // The layer's rotation is unit-length by construction, so its conjugate
  // is its inverse.
  const glm::quat inverse_rotation =
      glm::conjugate(sdf_layer_rotation(layer));
  return SdfTransform{
      inverse_rotation * (world_position - layer.position),
      glm::eulerAngles(inverse_rotation * glm::quat(world_rotation))};
}

const MaterialDef *sdf_scene_find_material(const SdfScene &scene,
                                          MaterialId id) {
  if (id == kInvalidMaterialId) {
    return nullptr;
  }
  for (const MaterialDef &material : scene.materials) {
    if (material.id == id) {
      return &material;
    }
  }
  return nullptr;
}

MaterialDef *sdf_scene_find_material(SdfScene &scene, MaterialId id) {
  return const_cast<MaterialDef *>(
      sdf_scene_find_material(static_cast<const SdfScene &>(scene), id));
}

MaterialDef sdf_scene_resolve_material(
    const SdfScene &scene, MaterialId binding_id,
    const std::string &binding_name,
    const std::vector<MaterialOverride> &overrides) {
  if (const MaterialDef *found = sdf_scene_find_material(scene, binding_id)) {
    return material_def_apply_overrides(*found, overrides);
  }

  // A legacy binding, or a dangling id. Both fall back to reading the
  // .kmt file the name points at, which is also what makes a scene that
  // has not been converted yet render exactly as it always did.
  MaterialDef def;
  if (!binding_name.empty() && material_def_load_kmt(binding_name, def)) {
    def.display_name = material_def_suggest_name(def);
    return material_def_apply_overrides(def, overrides);
  }

  if (binding_id != kInvalidMaterialId) {
    KWARN("Material id '{}' is not in this scene's library and has no "
         "legacy name to fall back on; using default material values.",
         material_id_to_string(binding_id));
  }
  return material_def_apply_overrides(MaterialDef{}, overrides);
}

u32 sdf_scene_import_legacy_materials(SdfScene &scene) {
  // Deduplicated by CONTENT, which is what collapses a scene's legacy
  // references down to the handful of distinct value tuples it actually
  // uses -- 59 primitives referencing 13 tuples import 13 materials, not
  // 59. (Content addressing is correct here for the same reason it is
  // correct in the renderer's packing step and wrong as an authoring
  // identity: it is deciding sameness, not naming a thing.)
  std::unordered_map<std::string, MaterialId> by_content;
  for (const MaterialDef &existing : scene.materials) {
    by_content.emplace(material_def_content_key(existing), existing.id);
  }

  u32 imported = 0;
  auto convert = [&](MaterialId &id, std::string &name) {
    if (id != kInvalidMaterialId || name.empty()) {
      return;
    }
    MaterialDef def;
    if (!material_def_load_kmt(name, def)) {
      KWARN("Scene references material '{}', which has no .kmt file and is "
           "not in the scene's material library; leaving it unresolved.",
           name);
      return;
    }
    const std::string content = material_def_content_key(def);
    auto it = by_content.find(content);
    if (it == by_content.end()) {
      def.id = material_id_generate();
      def.display_name = material_def_suggest_name(def);
      by_content.emplace(content, def.id);
      id = def.id;
      scene.materials.push_back(std::move(def));
      ++imported;
    } else {
      id = it->second;
    }
    // The legacy name has served its purpose. Clearing it is what makes
    // the conversion one-way and total: a converted binding can never
    // silently fall back to a stale file.
    name.clear();
  };

  for (SdfLayerDef &layer : scene.layers) {
    for (SdfPrimitiveDef &primitive : layer.primitives) {
      convert(primitive.material_id, primitive.material_name);
    }
  }
  for (SdfVolumetricDef &volumetric : scene.volumetrics) {
    convert(volumetric.material_id, volumetric.material_name);
  }
  return imported;
}
