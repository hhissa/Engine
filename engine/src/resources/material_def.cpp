#include "material_def.h"
#include "../core/logger.h"

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <ostream>
#include <random>
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

// --- Per-type value IO ----------------------------------------------
//
// Five overloads each, matching the five member types
// for_each_material_property() can hand a visitor. Overload resolution is
// what dispatches, so the visitor lambdas below stay type-agnostic and no
// property needs an `if constexpr` chain naming its own type.

bool parse_value(const std::string &text, f32 &out) {
  std::istringstream iss(text);
  f32 v = 0.0f;
  iss >> v;
  if (iss.fail()) {
    return false;
  }
  out = v;
  return true;
}

bool parse_value(const std::string &text, bool &out) {
  if (text == "true" || text == "1") {
    out = true;
    return true;
  }
  if (text == "false" || text == "0") {
    out = false;
    return true;
  }
  return false;
}

bool parse_value(const std::string &text, glm::vec3 &out) {
  std::istringstream iss(text);
  glm::vec3 v(0.0f);
  iss >> v.x >> v.y >> v.z;
  if (iss.fail()) {
    return false;
  }
  out = v;
  return true;
}

bool parse_value(const std::string &text, glm::vec4 &out) {
  std::istringstream iss(text);
  glm::vec4 v(0.0f);
  iss >> v.x >> v.y >> v.z >> v.w;
  if (iss.fail()) {
    return false;
  }
  out = v;
  return true;
}

bool parse_value(const std::string &text, std::string &out) {
  out = text;
  return true;
}

// Enough digits that a float survives write-then-read unchanged, matching
// what save_scene() (testbed/src/sdf_authoring.cpp) already uses for every
// other float in a .sdf file. A lossy format here would reintroduce, in a
// new place, exactly the quantisation the old filename scheme forced.
void format_value(std::ostream &out, f32 v) {
  out << std::setprecision(std::numeric_limits<f32>::max_digits10) << v;
}
void format_value(std::ostream &out, bool v) { out << (v ? "true" : "false"); }
void format_value(std::ostream &out, const glm::vec3 &v) {
  out << std::setprecision(std::numeric_limits<f32>::max_digits10) << v.x << ' '
      << v.y << ' ' << v.z;
}
void format_value(std::ostream &out, const glm::vec4 &v) {
  out << std::setprecision(std::numeric_limits<f32>::max_digits10) << v.x << ' '
      << v.y << ' ' << v.z << ' ' << v.w;
}
void format_value(std::ostream &out, const std::string &v) { out << v; }

// One property, flattened to text.
struct PropertyRow {
  std::string_view key;
  std::string value;
  MaterialDirty dirty;
};

// Walks the property table once and formats every value. Because the table
// is a fixed sequence, two collect() results can be compared positionally,
// which is what lets write/diff/content-key each be a single linear pass
// rather than a nested lookup per property.
std::vector<PropertyRow> collect(const MaterialDef &def) {
  std::vector<PropertyRow> rows;
  for_each_material_property(
      def, [&](std::string_view key, const auto &member, MaterialDirty dirty) {
        std::ostringstream oss;
        format_value(oss, member);
        rows.push_back(PropertyRow{key, oss.str(), dirty});
      });
  return rows;
}

} // namespace

// --- Identity --------------------------------------------------------

std::string material_id_to_string(MaterialId id) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx",
                static_cast<unsigned long long>(id));
  return std::string(buf);
}

bool material_id_from_string(std::string_view text, MaterialId &out) {
  if (text.empty() || text.size() > 16) {
    return false;
  }
  MaterialId value = 0;
  for (char c : text) {
    u32 digit;
    if (c >= '0' && c <= '9') {
      digit = static_cast<u32>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<u32>(c - 'a') + 10;
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<u32>(c - 'A') + 10;
    } else {
      return false;
    }
    value = (value << 4) | digit;
  }
  out = value;
  return true;
}

MaterialId material_id_generate() {
  // Random, not sequential: two scenes authored independently must be
  // mergeable (and primitives copy/pasteable between them) without their
  // ids colliding, which a per-process counter cannot promise.
  static std::mt19937_64 engine{std::random_device{}()};
  static std::uniform_int_distribution<MaterialId> dist;
  MaterialId id = kInvalidMaterialId;
  while (id == kInvalidMaterialId) {
    id = dist(engine);
  }
  return id;
}

// --- Serialisation ---------------------------------------------------

bool material_def_set_property(MaterialDef &def, std::string_view key,
                              const std::string &value) {
  bool found = false;
  bool parsed = false;
  for_each_material_property(def, [&](std::string_view property_key,
                                      auto &member, MaterialDirty) {
    if (found || property_key != key) {
      return;
    }
    found = true;
    parsed = parse_value(value, member);
  });
  return found && parsed;
}

bool material_def_get_property(const MaterialDef &def, std::string_view key,
                              std::string &out) {
  for (const PropertyRow &row : collect(def)) {
    if (row.key == key) {
      out = row.value;
      return true;
    }
  }
  return false;
}

void material_def_write(const MaterialDef &def, std::ostream &out,
                       std::string_view indent) {
  out << indent << "id=" << material_id_to_string(def.id) << "\n";

  // Only properties that differ from a default-constructed def are
  // written. See material_def_write()'s declaration for why this rule is
  // load-bearing rather than cosmetic.
  const std::vector<PropertyRow> rows = collect(def);
  const std::vector<PropertyRow> defaults = collect(MaterialDef{});
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].value != defaults[i].value) {
      out << indent << rows[i].key << "=" << rows[i].value << "\n";
    }
  }

  for (const auto &[key, value] : def.unknown_keys) {
    out << indent << key << "=" << value << "\n";
  }
}

// --- Comparison ------------------------------------------------------

MaterialDirty material_def_diff(const MaterialDef &a, const MaterialDef &b) {
  const std::vector<PropertyRow> rows_a = collect(a);
  const std::vector<PropertyRow> rows_b = collect(b);
  MaterialDirty worst = MaterialDirty::None;
  for (size_t i = 0; i < rows_a.size(); ++i) {
    if (rows_a[i].value != rows_b[i].value && rows_a[i].dirty > worst) {
      worst = rows_a[i].dirty;
    }
  }
  return worst;
}

std::string material_def_content_key(const MaterialDef &def) {
  std::ostringstream oss;
  for (const PropertyRow &row : collect(def)) {
    oss << row.key << '=' << row.value << ';';
  }
  return oss.str();
}

// --- Overrides -------------------------------------------------------

MaterialDef material_def_apply_overrides(
    const MaterialDef &def, const std::vector<MaterialOverride> &overrides) {
  MaterialDef result = def;
  for (const MaterialOverride &override_entry : overrides) {
    if (!material_def_set_property(result, override_entry.key,
                                   override_entry.value)) {
      KWARN("Material override names an unknown or unparseable property "
           "'{}={}' on material '{}'; ignoring it.",
           override_entry.key, override_entry.value,
           def.display_name.empty() ? material_id_to_string(def.id)
                                    : def.display_name);
    }
  }
  return result;
}

f32 material_effective_ior(const MaterialDef &def, bool &clamped) {
  clamped = false;
  if (def.ior <= 0.0f) {
    return 0.0f; // opaque
  }
  if (def.ior < 1.0f) {
    clamped = true;
    return 1.0f;
  }
  return def.ior;
}

// --- Legacy import ---------------------------------------------------

bool material_def_load_kmt(std::string_view name, MaterialDef &out) {
  std::ifstream file("assets/materials/" + std::string(name) + ".kmt");
  if (!file.is_open()) {
    return false;
  }

  MaterialDef def;
  std::string line;
  while (std::getline(file, line)) {
    std::string trimmed = trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }
    auto eq = trimmed.find('=');
    if (eq == std::string::npos) {
      KWARN("Material file '{}': '=' not found, skipping line '{}'.", name,
           trimmed);
      continue;
    }
    const std::string key = trim(trimmed.substr(0, eq));
    const std::string value = trim(trimmed.substr(eq + 1));

    // The .kmt key set, mapped onto MaterialDef's fields. "version" and
    // "name" are read and discarded: a legacy file's name is a value
    // encoding rather than a label, so the importer derives a real
    // display name instead (see material_def_suggest_name()).
    bool ok = true;
    if (key == "version" || key == "name") {
      continue;
    } else if (key == "diffuse_colour" || key == "diffuse_color") {
      ok = parse_value(value, def.base_colour);
    } else if (key == "diffuse_map_name") {
      ok = parse_value(value, def.base_map);
    } else if (key == "texture_scale") {
      f32 scale = 0.0f;
      ok = parse_value(value, scale) && scale > 0.0f;
      if (ok) {
        def.uv_scale = scale;
      }
    } else if (key == "texture_offset") {
      ok = parse_value(value, def.uv_offset);
    } else if (key == "texture_rotation") {
      ok = parse_value(value, def.uv_rotation);
    } else if (key == "bump_map_name") {
      ok = parse_value(value, def.bump_map);
    } else if (key == "bump_strength") {
      ok = parse_value(value, def.bump_strength);
    } else if (key == "emissive_colour" || key == "emissive_color") {
      ok = parse_value(value, def.emissive_colour);
    } else if (key == "emissive_intensity") {
      ok = parse_value(value, def.emissive_intensity);
    } else if (key == "casts_shadow") {
      ok = parse_value(value, def.casts_shadow);
    } else if (key == "pixelation_exempt") {
      ok = parse_value(value, def.pixelation_exempt);
    } else {
      // Unrecognised keys are preserved rather than dropped, same as for a
      // scene-embedded material -- a .kmt written by a newer build should
      // survive a round trip through an older one.
      def.unknown_keys.emplace_back(key, value);
      continue;
    }

    if (!ok) {
      KWARN("Material file '{}': could not parse '{}={}'; leaving that "
           "property at its default.",
           name, key, value);
    }
  }

  out = std::move(def);
  return true;
}

std::string material_def_suggest_name(const MaterialDef &def) {
  // A hex tint suffix, but only when the tint is actually doing something
  // -- an untinted textured material is just "officecarpet", not
  // "officecarpet (ffffff)".
  const bool tinted = def.base_colour.x < 0.999f || def.base_colour.y < 0.999f ||
                     def.base_colour.z < 0.999f;
  char tint[8] = "";
  if (tinted) {
    auto channel = [](f32 v) {
      f32 clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
      return static_cast<int>(clamped * 255.0f + 0.5f);
    };
    std::snprintf(tint, sizeof(tint), "%02x%02x%02x", channel(def.base_colour.x),
                  channel(def.base_colour.y), channel(def.base_colour.z));
  }

  if (!def.base_map.empty()) {
    return tinted ? def.base_map + " (" + tint + ")" : def.base_map;
  }
  if (def.emissive_intensity > 0.0f) {
    return tinted ? std::string("emissive (") + tint + ")" : "emissive";
  }
  return tinted ? std::string("colour ") + tint : "white";
}
