#include "scene_material_tests.h"
#include "../expect.h"
#include "../test_manager.h"

#include <defines.h>
#include <resources/material_def.h>
#include <resources/sdf_scene.h>
#include <sdf_authoring.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

// The migration acceptance test.
//
// The whole point of the material library is that it changes how materials
// are IDENTIFIED, not what anything looks like. So the property worth
// testing is exactly that: for every scene shipped in assets/scenes, take
// the resolved material of every primitive before conversion and after a
// full save/reload cycle, and require them to be identical.
//
// Resolution is a pure function of the definition and the override set,
// which is what makes this a cheap test of a wide-reaching change.
//
// These run from bin/ (like the renderer itself), so the asset paths are
// relative to the working directory. A scene that will not open is
// skipped rather than failed -- the test is about conversion fidelity, not
// about which scenes happen to be present in a given checkout.

const char *kScenes[] = {
    "assets/scenes/DiegosOffice.sdf", "assets/scenes/TheosRoom.sdf",
    "assets/scenes/room.sdf",         "assets/scenes/demo_scene.sdf",
    "assets/scenes/man_sitting.sdf",  "assets/scenes/hikikimoriroom.sdf",
};
const char *kTempPath = "assets/scenes/.material_migration_test.sdf";

// Every primitive's and volumetric's resolved material, in scene order.
std::vector<std::string> resolved_material_keys(const SdfScene &scene) {
  std::vector<std::string> keys;
  for (const SdfLayerDef &layer : scene.layers) {
    for (const SdfPrimitiveDef &primitive : layer.primitives) {
      keys.push_back(
          material_def_content_key(sdf_scene_resolve_material(scene, primitive)));
    }
  }
  for (const SdfVolumetricDef &volumetric : scene.volumetrics) {
    keys.push_back(material_def_content_key(
        sdf_scene_resolve_material(scene, volumetric)));
  }
  return keys;
}

bool scene_materials_survive_conversion() {
  u32 scenes_checked = 0;

  for (const char *path : kScenes) {
    auto loaded = read_scene(path);
    if (!loaded) {
      continue; // not present in this checkout
    }
    const std::vector<std::string> before = resolved_material_keys(*loaded);
    if (before.empty()) {
      continue;
    }

    EXPECT_TRUE(save_scene(kTempPath, *loaded));
    auto reloaded = read_scene(kTempPath);
    std::remove(kTempPath);
    EXPECT_TRUE(reloaded.has_value());

    const std::vector<std::string> after = resolved_material_keys(*reloaded);
    EXPECT_EQ(before.size(), after.size());
    for (size_t i = 0; i < before.size(); ++i) {
      if (before[i] != after[i]) {
        KERROR("--> '{}' primitive {}: material changed across conversion.\\n"
              "    before: {}\\n    after:  {}",
              path, i, before[i], after[i]);
        return false;
      }
    }

    // Conversion must actually have happened -- a test that passed because
    // nothing was converted would be worthless. Every binding ends up on
    // an id, and the library holds one entry per DISTINCT material rather
    // than one per primitive.
    EXPECT_FALSE(reloaded->materials.empty());
    EXPECT_TRUE(reloaded->materials.size() <= before.size());
    for (const SdfLayerDef &layer : reloaded->materials.empty()
                                        ? std::vector<SdfLayerDef>{}
                                        : reloaded->layers) {
      for (const SdfPrimitiveDef &primitive : layer.primitives) {
        EXPECT_NE(kInvalidMaterialId, primitive.material_id);
        EXPECT_TRUE(primitive.material_name.empty());
      }
    }

    ++scenes_checked;
  }

  // Guards against the whole test silently doing nothing because the asset
  // paths moved.
  EXPECT_TRUE(scenes_checked > 0);
  return true;
}

bool scene_conversion_deduplicates_materials() {
  auto loaded = read_scene("assets/scenes/DiegosOffice.sdf");
  if (!loaded) {
    return true; // not present in this checkout
  }

  u32 primitive_count = 0;
  for (const SdfLayerDef &layer : loaded->layers) {
    primitive_count += static_cast<u32>(layer.primitives.size());
  }

  // Dedup is by content, so a scene whose primitives share value tuples
  // imports far fewer materials than it has primitives. This is the
  // property that turns a folder of 949 files into a library of a few
  // dozen -- if it regressed to one material per primitive the library
  // would be as useless as the folder was.
  EXPECT_TRUE(primitive_count > 0);
  EXPECT_TRUE(loaded->materials.size() < primitive_count);

  // Every imported material got a real id and a readable label.
  for (const MaterialDef &material : loaded->materials) {
    EXPECT_NE(kInvalidMaterialId, material.id);
    EXPECT_FALSE(material.display_name.empty());
  }
  return true;
}

bool scene_round_trips_library_and_overrides() {
  SdfScene scene;
  MaterialDef oak;
  oak.id = material_id_generate();
  oak.display_name = "Oak";
  oak.base_colour = glm::vec4(0.87f, 0.86f, 0.75f, 1.0f);
  oak.base_map = "oak_diffuse";
  oak.roughness = 0.55f;
  oak.unknown_keys.emplace_back("subsurface_radius", "0.4 0.2 0.1");
  scene.materials.push_back(oak);

  SdfLayerDef &layer = add_layer(scene, "layer0", SdfLayerOperation::Union, 0.0f);
  SdfPrimitiveDef &plain =
      add_box(layer, "desk_top", glm::vec3(0.0f), glm::vec3(0.0f),
              glm::vec3(1.0f), "");
  plain.material_id = oak.id;
  SdfPrimitiveDef &tweaked =
      add_box(layer, "drawer_front", glm::vec3(1.0f), glm::vec3(0.0f),
              glm::vec3(1.0f), "");
  tweaked.material_id = oak.id;
  tweaked.material_overrides.push_back({"base_colour", "0.61 0.52 0.38 1"});
  tweaked.material_overrides.push_back({"uv_scale", "0.35"});

  const std::vector<std::string> before = resolved_material_keys(scene);

  EXPECT_TRUE(save_scene(kTempPath, scene));
  auto reloaded = read_scene(kTempPath);
  std::remove(kTempPath);
  EXPECT_TRUE(reloaded.has_value());

  EXPECT_EQ(1u, static_cast<u32>(reloaded->materials.size()));
  EXPECT_EQ(oak.id, reloaded->materials[0].id);
  // The display name rides in the block header, so a name with no spaces
  // must come back intact.
  EXPECT_EQ(std::string("Oak"), reloaded->materials[0].display_name);
  // A key this build does not interpret survives the round trip.
  EXPECT_EQ(1u, static_cast<u32>(reloaded->materials[0].unknown_keys.size()));

  // Two primitives, one shared definition, one of them overridden -- and
  // the overridden one resolves differently without forking the material.
  const std::vector<std::string> after = resolved_material_keys(*reloaded);
  EXPECT_EQ(before.size(), after.size());
  EXPECT_EQ(before[0], after[0]);
  EXPECT_EQ(before[1], after[1]);
  EXPECT_NE(after[0], after[1]);

  return true;
}

bool scene_editing_a_material_is_not_a_swap() {
  SdfScene scene;
  MaterialDef paint;
  paint.id = material_id_generate();
  paint.display_name = "Wall Paint";
  scene.materials.push_back(paint);

  SdfLayerDef &layer = add_layer(scene, "layer0", SdfLayerOperation::Union, 0.0f);
  SdfPrimitiveDef &wall = add_box(layer, "wall", glm::vec3(0.0f),
                                  glm::vec3(0.0f), glm::vec3(1.0f), "");
  wall.material_id = paint.id;

  const std::string before = resolved_material_keys(scene)[0];

  // Renaming must not change the resolved key -- that is what stops
  // GeometrySystem::reconcile_scene() treating a rename as a destructive
  // material swap (and draining the GPU queue for it).
  scene.materials[0].display_name = "Feature Wall";
  EXPECT_EQ(before, resolved_material_keys(scene)[0]);

  // Changing a VALUE does change it, so a real edit still re-resolves.
  scene.materials[0].base_colour = glm::vec4(0.2f, 0.3f, 0.4f, 1.0f);
  EXPECT_NE(before, resolved_material_keys(scene)[0]);

  return true;
}

} // namespace

void register_scene_material_tests() {
  TestManager::register_test(
      scene_materials_survive_conversion,
      "Shipped scenes render identically after material-library conversion");
  TestManager::register_test(
      scene_conversion_deduplicates_materials,
      "Legacy material import deduplicates by content");
  TestManager::register_test(
      scene_round_trips_library_and_overrides,
      "Scene round-trips its material library, overrides and unknown keys");
  TestManager::register_test(
      scene_editing_a_material_is_not_a_swap,
      "Renaming a material does not change its resolved key");
}
