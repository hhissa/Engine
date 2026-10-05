#include "scene_skybox_tests.h"
#include "../expect.h"
#include "../test_manager.h"

#include <defines.h>
#include <resources/sdf_scene.h>
#include <sdf_authoring.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

// SdfScene::skybox names the equirectangular image drawn behind a scene.
// Its one subtlety is that EMPTY MEANS UNSPECIFIED, not "no skybox" -- a
// scene file written before the field existed must not switch off a skybox
// the game set from code (see SdfScene::skybox, and where
// VulkanRendererBackend::load_scene() applies it). These pin that down: the
// name survives a round trip, and an unset one leaves no trace in the file
// at all.

namespace {

const char *kTempPath = "assets/scenes/.skybox_test.sdf";

std::string read_file(const char *path) {
  std::ifstream file{path};
  std::ostringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

SdfScene one_primitive_scene() {
  SdfScene scene;
  SdfLayerDef &layer = add_layer(scene, "layer0", SdfLayerOperation::Union);
  add_box(layer, "box", glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.5f), "");
  return scene;
}

bool skybox_round_trips() {
  SdfScene scene = one_primitive_scene();
  scene.skybox = "sunset_sky";

  EXPECT_TRUE(save_scene(kTempPath, scene));
  const std::string written = read_file(kTempPath);
  auto reloaded = read_scene(kTempPath);
  std::remove(kTempPath);
  EXPECT_TRUE(reloaded.has_value());

  EXPECT_EQ(std::string("sunset_sky"), reloaded->skybox);
  // Top-level, beside ambient= -- not nested in a layer or a material
  // block, where the reader would never look for it.
  EXPECT_TRUE(written.find("\nskybox=sunset_sky\n") != std::string::npos);
  return true;
}

// The compatibility guarantee. A scene that names no skybox must write no
// skybox line -- both so files authored before the field existed stay
// byte-for-byte unchanged when re-saved, and because an empty line would
// read back as a value the loader would then have to invent a meaning for.
bool an_unset_skybox_writes_nothing() {
  SdfScene scene = one_primitive_scene();
  EXPECT_TRUE(scene.skybox.empty());

  EXPECT_TRUE(save_scene(kTempPath, scene));
  const std::string written = read_file(kTempPath);
  auto reloaded = read_scene(kTempPath);
  std::remove(kTempPath);
  EXPECT_TRUE(reloaded.has_value());

  EXPECT_TRUE(written.find("skybox") == std::string::npos);
  EXPECT_TRUE(reloaded->skybox.empty());
  return true;
}

// Every scene shipped in this checkout predates the field, so re-saving one
// must not start naming a skybox it never had -- the check that the writer's
// "only when set" rule actually holds against real files, not just
// hand-built ones.
bool shipped_scenes_gain_no_skybox() {
  const char *kScenes[] = {
      "assets/scenes/dayroom.sdf",
      "assets/scenes/hikikimoriroom.sdf",
      "assets/scenes/TheresasRoom.sdf",
  };
  u32 checked = 0;
  for (const char *path : kScenes) {
    auto loaded = read_scene(path);
    if (!loaded) {
      continue; // not present in this checkout
    }
    EXPECT_TRUE(loaded->skybox.empty());
    EXPECT_TRUE(save_scene(kTempPath, *loaded));
    const std::string written = read_file(kTempPath);
    std::remove(kTempPath);
    EXPECT_TRUE(written.find("skybox") == std::string::npos);
    ++checked;
  }
  EXPECT_TRUE(checked > 0); // guards against the paths having moved
  return true;
}

} // namespace

void register_scene_skybox_tests() {
  TestManager::register_test(skybox_round_trips,
                             "A scene's skybox survives save_scene()/"
                             "read_scene() as a top-level key");
  TestManager::register_test(an_unset_skybox_writes_nothing,
                             "An unset skybox writes no line at all");
  TestManager::register_test(shipped_scenes_gain_no_skybox,
                             "Re-saving a shipped scene does not give it a "
                             "skybox it never had");
}
