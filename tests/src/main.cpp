#include "test_manager.h"

#include "memory/linear_allocator_tests.h"
#include "resources/layer_transform_tests.h"
#include "resources/material_def_tests.h"
#include "resources/scene_material_tests.h"
#include "resources/scene_skybox_tests.h"
#include "systems/chunk_streaming_manager_tests.h"
#include "systems/geometry_chunk_tests.h"

#include <core/hmemory.h>
#include <core/logger.h>

int main() {
  Memory::initialize();
  Logger::initialize_logging();

  register_linear_allocator_tests();
  register_material_def_tests();
  register_layer_transform_tests();
  register_scene_material_tests();
  register_scene_skybox_tests();
  register_geometry_chunk_tests();
  register_chunk_streaming_manager_tests();

  KDEBUG("Starting tests...");

  TestManager::run_tests();

  Memory::shutdown();
  return 0;
}
