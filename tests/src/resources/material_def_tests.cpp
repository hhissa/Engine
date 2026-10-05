#include "material_def_tests.h"
#include "../expect.h"
#include "../test_manager.h"

#include <defines.h>
#include <resources/material_def.h>

#include <sstream>

namespace {

// The property table is the single declaration every consumer derives
// from, so the properties worth testing are the ones that would break
// silently if a consumer drifted away from it: that a written def reads
// back identically, that defaults are omitted, that unknown keys survive,
// and that the content key and the dirty classification agree with the
// table rather than with a hand-maintained copy of it.

MaterialDef sample_def() {
  MaterialDef def;
  def.id = 0x0123456789abcdefull;
  def.display_name = "Bottle Glass";
  def.base_colour = glm::vec4(0.8f, 0.95f, 0.85f, 1.0f);
  def.base_map = "glass_diffuse";
  def.uv_scale = 1.25f;
  def.uv_offset = glm::vec3(0.5f, -0.25f, 0.125f);
  def.uv_rotation = 0.7853982f;
  def.bump_map = "glass_bump";
  def.bump_strength = 1.4f;
  def.roughness = 0.05f;
  def.emissive_colour = glm::vec3(0.2f, 0.4f, 0.6f);
  def.emissive_intensity = 2.5f;
  def.ior = 1.52f;
  def.absorption_tint = glm::vec3(0.72f, 0.93f, 0.78f);
  def.absorption_ref_thickness = 0.008f;
  def.thin_walled = true;
  def.casts_shadow = false;
  def.pixelation_exempt = true;
  return def;
}

// Reads back what material_def_write() produced, the way the scene parser
// does: split each line on '=', route id/display_name specially, and hand
// everything else to material_def_set_property().
MaterialDef read_back(const std::string &text) {
  MaterialDef def;
  std::istringstream iss(text);
  std::string line;
  while (std::getline(iss, line)) {
    auto eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    std::string key = line.substr(0, eq);
    std::string value = line.substr(eq + 1);
    if (key == "id") {
      material_id_from_string(value, def.id);
    } else if (key == "display_name") {
      def.display_name = value;
    } else if (!material_def_set_property(def, key, value)) {
      def.unknown_keys.emplace_back(key, value);
    }
  }
  return def;
}

bool material_def_round_trips_every_property() {
  const MaterialDef original = sample_def();

  std::ostringstream oss;
  material_def_write(original, oss, "");
  const MaterialDef restored = read_back(oss.str());

  EXPECT_EQ(original.id, restored.id);
  // display_name is intentionally absent from the written form -- the
  // container that persists a def carries the label (a .sdf material block
  // writes it in its header), so the writer must not emit it.
  EXPECT_TRUE(oss.str().find("display_name") == std::string::npos);
  // The content key covers every VALUE property in one comparison, so a
  // property added to the table without being added here is still checked.
  EXPECT_EQ(material_def_content_key(original),
            material_def_content_key(restored));
  EXPECT_EQ(static_cast<u32>(MaterialDirty::None),
            static_cast<u32>(material_def_diff(original, restored)));

  return true;
}

bool material_def_write_omits_defaults() {
  MaterialDef def;
  def.id = 0x11ull;
  def.uv_scale = 2.0f; // the only non-default value

  std::ostringstream oss;
  material_def_write(def, oss, "    ");
  const std::string text = oss.str();

  EXPECT_TRUE(text.find("uv_scale=2") != std::string::npos);
  // Everything else is at its default and must not appear at all -- this
  // is the rule the old filename scheme re-implemented by hand per
  // property, and the reason adding a property stays a one-line change.
  EXPECT_TRUE(text.find("base_colour") == std::string::npos);
  EXPECT_TRUE(text.find("casts_shadow") == std::string::npos);
  EXPECT_TRUE(text.find("ior") == std::string::npos);
  EXPECT_TRUE(text.find("    ") == 0); // indent applied

  return true;
}

bool material_def_preserves_unknown_keys() {
  MaterialDef def;
  def.id = 0x22ull;
  def.unknown_keys.emplace_back("subsurface_radius", "0.4 0.2 0.1");

  std::ostringstream oss;
  material_def_write(def, oss, "");
  const MaterialDef restored = read_back(oss.str());

  EXPECT_EQ(1u, static_cast<u32>(restored.unknown_keys.size()));
  EXPECT_EQ(std::string("subsurface_radius"), restored.unknown_keys[0].first);
  EXPECT_EQ(std::string("0.4 0.2 0.1"), restored.unknown_keys[0].second);
  // An uninterpretable key must not read as a rendering change, or every
  // load of a forward-versioned scene would look like an edit.
  EXPECT_EQ(static_cast<u32>(MaterialDirty::None),
            static_cast<u32>(material_def_diff(MaterialDef{}, restored)));

  return true;
}

bool material_def_content_key_ignores_identity() {
  MaterialDef a = sample_def();
  MaterialDef b = sample_def();
  b.id = material_id_generate();
  b.display_name = "Something Else Entirely";

  // Two separately-authored materials that would render identically share
  // one runtime entry. Content addressing belongs here -- and only here.
  EXPECT_EQ(material_def_content_key(a), material_def_content_key(b));

  b.base_colour.x += 0.5f;
  EXPECT_NE(material_def_content_key(a), material_def_content_key(b));

  return true;
}

bool material_def_diff_classifies_by_severity() {
  const MaterialDef base;

  MaterialDef shading = base;
  shading.base_colour = glm::vec4(0.1f, 0.2f, 0.3f, 1.0f);
  EXPECT_EQ(static_cast<u32>(MaterialDirty::Shading),
            static_cast<u32>(material_def_diff(base, shading)));

  MaterialDef lighting = base;
  lighting.emissive_intensity = 3.0f;
  EXPECT_EQ(static_cast<u32>(MaterialDirty::Lighting),
            static_cast<u32>(material_def_diff(base, lighting)));

  // A shading change alongside a lighting change must report the worse of
  // the two, not the first one found.
  MaterialDef both = shading;
  both.emissive_intensity = 3.0f;
  EXPECT_EQ(static_cast<u32>(MaterialDirty::Lighting),
            static_cast<u32>(material_def_diff(base, both)));

  // Renaming is not a rendering change.
  MaterialDef renamed = base;
  renamed.display_name = "Renamed";
  renamed.id = material_id_generate();
  EXPECT_EQ(static_cast<u32>(MaterialDirty::None),
            static_cast<u32>(material_def_diff(base, renamed)));

  return true;
}

bool material_def_overrides_apply_over_a_shared_definition() {
  MaterialDef shared;
  shared.id = material_id_generate();
  shared.display_name = "Oak";
  shared.base_colour = glm::vec4(0.87f, 0.86f, 0.75f, 1.0f);
  shared.uv_scale = 0.6f;

  const std::vector<MaterialOverride> overrides = {
      {"base_colour", "0.61 0.52 0.38 1"},
      {"uv_scale", "0.35"},
  };
  const MaterialDef resolved = material_def_apply_overrides(shared, overrides);

  EXPECT_FLOAT_EQ(0.61f, resolved.base_colour.x);
  EXPECT_FLOAT_EQ(0.38f, resolved.base_colour.z);
  EXPECT_FLOAT_EQ(0.35f, resolved.uv_scale);
  // The override does not fork the material: identity is untouched, so the
  // primitive still tracks edits to the shared definition for every
  // property it did not override.
  EXPECT_EQ(shared.id, resolved.id);
  EXPECT_EQ(shared.display_name, resolved.display_name);

  // Two primitives overriding the same material identically must still
  // resolve to one runtime entry.
  const MaterialDef twin = material_def_apply_overrides(shared, overrides);
  EXPECT_EQ(material_def_content_key(resolved), material_def_content_key(twin));

  return true;
}

bool material_def_ignores_unknown_overrides() {
  MaterialDef def;
  def.uv_scale = 0.6f;
  const MaterialDef resolved =
      material_def_apply_overrides(def, {{"not_a_property", "7"}});

  // An override naming a property this build does not have is dropped with
  // a warning rather than corrupting the def.
  EXPECT_EQ(material_def_content_key(def), material_def_content_key(resolved));
  return true;
}

bool material_id_round_trips_through_text() {
  const MaterialId id = 0x0123456789abcdefull;
  EXPECT_EQ(std::string("0123456789abcdef"), material_id_to_string(id));

  MaterialId parsed = kInvalidMaterialId;
  EXPECT_TRUE(material_id_from_string("0123456789abcdef", parsed));
  EXPECT_EQ(id, parsed);

  // Leading zeroes must survive -- an id is fixed-width hex, not a number.
  EXPECT_EQ(std::string("0000000000000011"), material_id_to_string(0x11ull));

  // A generated id is never the "no material" sentinel.
  EXPECT_NE(kInvalidMaterialId, material_id_generate());

  // Anything that is not hex fails rather than silently producing an id --
  // which is what lets the scene loader tell a real id from a legacy
  // material name in the same "material=" field.
  MaterialId rejected = 0;
  EXPECT_FALSE(material_id_from_string("qt_colour_dddbbeff_ts060_white",
                                       rejected));
  EXPECT_FALSE(material_id_from_string("", rejected));
  EXPECT_FALSE(material_id_from_string("00112233445566778", rejected));

  return true;
}

bool material_effective_ior_rejects_the_mirror_band() {
  // Regression: the editor exposed a single 0-to-3 IOR spin box, so one
  // step off "opaque" landed on 0.01 -- which is not weak glass. It is a
  // near-perfect mirror by two independent routes: F0 = ((1-n)/(1+n))^2
  // approaches 1 as n approaches 0, AND eta = 1/n total-internal-reflects
  // every ray, so the transmitted branch collapses into the reflected one.
  // The first thing anyone saw on enabling glass was a mirror.
  MaterialDef def;

  // Opaque stays opaque -- 0 is the sentinel, not a small IOR.
  def.ior = 0.0f;
  EXPECT_FLOAT_EQ(0.0f, material_effective_ior(def));

  bool clamped = true;
  def.ior = 1.52f;
  EXPECT_FLOAT_EQ(1.52f, material_effective_ior(def, clamped));
  EXPECT_FALSE(clamped);

  // Anything in (0, 1) is corrected, and says so.
  def.ior = 0.01f;
  EXPECT_FLOAT_EQ(1.0f, material_effective_ior(def, clamped));
  EXPECT_TRUE(clamped);

  def.ior = 0.999f;
  EXPECT_FLOAT_EQ(1.0f, material_effective_ior(def));

  // A clamped material is still transmissive -- it must not silently fall
  // out of the transmissive tail range and stop being drawn at all.
  def.ior = 0.01f;
  EXPECT_TRUE(material_effective_ior(def) > 0.0f);

  // Negative is nonsense from a hand-edited file; treated as opaque
  // rather than clamped into visibility.
  def.ior = -2.0f;
  EXPECT_FLOAT_EQ(0.0f, material_effective_ior(def));

  return true;
}

bool material_def_suggests_readable_names() {
  MaterialDef textured;
  textured.base_map = "officecarpet";
  EXPECT_EQ(std::string("officecarpet"), material_def_suggest_name(textured));

  MaterialDef tinted = textured;
  tinted.base_colour = glm::vec4(2.0f / 3.0f, 1.0f / 3.0f, 0.0f, 1.0f);
  EXPECT_EQ(std::string("officecarpet (aa5500)"),
            material_def_suggest_name(tinted));

  MaterialDef plain;
  EXPECT_EQ(std::string("white"), material_def_suggest_name(plain));

  return true;
}

} // namespace

void register_material_def_tests() {
  TestManager::register_test(material_def_round_trips_every_property,
                             "MaterialDef round-trips every property through "
                             "write/read");
  TestManager::register_test(material_def_write_omits_defaults,
                             "MaterialDef write omits default-valued "
                             "properties");
  TestManager::register_test(material_def_preserves_unknown_keys,
                             "MaterialDef preserves unknown keys and ignores "
                             "them in the diff");
  TestManager::register_test(material_def_content_key_ignores_identity,
                             "Material content key ignores id and display "
                             "name");
  TestManager::register_test(material_def_diff_classifies_by_severity,
                             "Material diff reports the worst invalidation "
                             "class");
  TestManager::register_test(
      material_def_overrides_apply_over_a_shared_definition,
      "Material overrides resolve without forking the definition");
  TestManager::register_test(material_def_ignores_unknown_overrides,
                             "Material overrides naming unknown properties are "
                             "ignored");
  TestManager::register_test(material_id_round_trips_through_text,
                             "MaterialId round-trips as fixed-width hex and "
                             "rejects legacy names");
  TestManager::register_test(
      material_effective_ior_rejects_the_mirror_band,
      "Transmissive IOR below 1 is clamped instead of rendering a mirror");
  TestManager::register_test(material_def_suggests_readable_names,
                             "Legacy material import suggests readable names");
}
