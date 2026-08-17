#include "geometry_chunk_tests.h"
#include "../expect.h"
#include "../test_manager.h"

#include <defines.h>
#include <systems/geometry_system.h>

#include <algorithm>

namespace {

bool contains(const std::vector<ChunkKey> &chunks, ChunkKey key) {
  return std::find(chunks.begin(), chunks.end(), key) != chunks.end();
}

// geometry_bounding_radius() -- a handful of known per-type formulas
// (mirrors the exact per-type table in geometry_system.cpp; these values
// are picked so the expected result is easy to hand-verify, not to
// exhaustively cover every PrimitiveType).

bool bounding_radius_sphere_is_its_own_radius() {
  Geometry sphere;
  sphere.type = PrimitiveType::Sphere;
  sphere.params = glm::vec3(2.0f, 0.0f, 0.0f);

  EXPECT_FLOAT_EQ(2.0f, geometry_bounding_radius(sphere));
  return true;
}

bool bounding_radius_box_is_half_extent_length() {
  Geometry box;
  box.type = PrimitiveType::Box;
  box.params = glm::vec3(1.0f, 2.0f, 2.0f); // length = 3

  EXPECT_FLOAT_EQ(3.0f, geometry_bounding_radius(box));
  return true;
}

bool bounding_radius_plane_is_unbounded() {
  Geometry plane;
  plane.type = PrimitiveType::Plane;

  EXPECT_FLOAT_EQ(kUnboundedBoundingRadius, geometry_bounding_radius(plane));
  return true;
}

bool bounding_radius_infinite_repetition_is_unbounded() {
  Geometry sphere;
  sphere.type = PrimitiveType::Sphere;
  sphere.params = glm::vec3(1.0f, 0.0f, 0.0f);
  sphere.repetition_mode = RepetitionMode::Infinite;

  EXPECT_FLOAT_EQ(kUnboundedBoundingRadius, geometry_bounding_radius(sphere));
  return true;
}

bool bounding_radius_active_param_expression_is_unbounded() {
  Geometry sphere;
  sphere.type = PrimitiveType::Sphere;
  sphere.params = glm::vec3(1.0f, 0.0f, 0.0f);
  sphere.param_expressions[0] = "1.0"; // any non-empty formula

  EXPECT_FLOAT_EQ(kUnboundedBoundingRadius, geometry_bounding_radius(sphere));
  return true;
}

bool bounding_radius_limited_repetition_widens_by_reach() {
  Geometry sphere;
  sphere.type = PrimitiveType::Sphere;
  sphere.params = glm::vec3(1.0f, 0.0f, 0.0f); // local_bound = 1
  sphere.repetition_mode = RepetitionMode::Limited;
  sphere.repetition_cell = glm::vec3(4.0f, 1.0f, 1.0f);
  sphere.repetition_count = glm::vec3(3.0f, 1.0f, 1.0f); // half_span.x = 1 -> reach 4

  EXPECT_FLOAT_EQ(5.0f, geometry_bounding_radius(sphere)); // 1 + 4
  return true;
}

// chunks_touched_by()

// y/z are deliberately 2.0 (mid-chunk), not 0.0, in every case below except
// the boundary-straddling one: 0.0 is itself a chunk boundary (chunks tile
// from multiples of chunk_size), so a primitive placed there straddles the
// origin on every axis at once -- exactly what
// chunks_touched_by_primitive_straddling_a_boundary_touches_both() tests on
// purpose, but not what these other cases mean to exercise.

bool chunks_touched_by_small_primitive_at_origin_touches_one_chunk() {
  Geometry sphere;
  sphere.type = PrimitiveType::Sphere;
  sphere.position = glm::vec3(2.0f, 2.0f, 2.0f); // chunk_size/2 on every axis
  sphere.params = glm::vec3(0.5f, 0.0f, 0.0f);

  std::vector<ChunkKey> touched = chunks_touched_by(sphere, 4.0f);

  EXPECT_EQ(1u, touched.size());
  EXPECT_TRUE(contains(touched, ChunkKey{0, 0, 0, 0}));
  return true;
}

bool chunks_touched_by_primitive_entirely_inside_one_chunk() {
  Geometry sphere;
  sphere.type = PrimitiveType::Sphere;
  sphere.position = glm::vec3(2.0f, 2.0f, 2.0f); // well inside chunk (0,0,0)
  sphere.params = glm::vec3(0.5f, 0.0f, 0.0f);

  std::vector<ChunkKey> touched = chunks_touched_by(sphere, 4.0f);

  EXPECT_EQ(1u, touched.size());
  EXPECT_TRUE(contains(touched, ChunkKey{0, 0, 0, 0}));
  return true;
}

bool chunks_touched_by_primitive_straddling_a_boundary_touches_both() {
  Geometry sphere;
  sphere.type = PrimitiveType::Sphere;
  // Exactly on the x=4 chunk boundary, y/z mid-chunk so only x straddles.
  sphere.position = glm::vec3(4.0f, 2.0f, 2.0f);
  sphere.params = glm::vec3(0.5f, 0.0f, 0.0f); // x range [3.5, 4.5]

  std::vector<ChunkKey> touched = chunks_touched_by(sphere, 4.0f);

  EXPECT_EQ(2u, touched.size());
  EXPECT_TRUE(contains(touched, ChunkKey{0, 0, 0, 0}));
  EXPECT_TRUE(contains(touched, ChunkKey{0, 1, 0, 0}));
  return true;
}

bool chunks_touched_by_negative_position_uses_floor_not_truncation() {
  Geometry sphere;
  sphere.type = PrimitiveType::Sphere;
  // chunk index floor(-1/4) = -1, not 0 (a naive truncating cast would give
  // 0, wrongly placing this in the same chunk as the positive side).
  sphere.position = glm::vec3(-1.0f, 2.0f, 2.0f);
  sphere.params = glm::vec3(0.5f, 0.0f, 0.0f);

  std::vector<ChunkKey> touched = chunks_touched_by(sphere, 4.0f);

  EXPECT_EQ(1u, touched.size());
  EXPECT_TRUE(contains(touched, ChunkKey{0, -1, 0, 0}));
  return true;
}

bool chunks_touched_by_unbounded_primitive_returns_empty() {
  Geometry plane;
  plane.type = PrimitiveType::Plane;

  std::vector<ChunkKey> touched = chunks_touched_by(plane, 4.0f);

  EXPECT_EQ(0u, touched.size());
  return true;
}

bool chunks_touched_by_non_positive_chunk_size_returns_empty() {
  Geometry sphere;
  sphere.type = PrimitiveType::Sphere;
  sphere.params = glm::vec3(0.5f, 0.0f, 0.0f);

  EXPECT_EQ(0u, chunks_touched_by(sphere, 0.0f).size());
  EXPECT_EQ(0u, chunks_touched_by(sphere, -1.0f).size());
  return true;
}

} // namespace

void register_geometry_chunk_tests() {
  TestManager::register_test(bounding_radius_sphere_is_its_own_radius,
                             "geometry_bounding_radius: sphere is its own radius");
  TestManager::register_test(bounding_radius_box_is_half_extent_length,
                             "geometry_bounding_radius: box is half-extent length");
  TestManager::register_test(bounding_radius_plane_is_unbounded,
                             "geometry_bounding_radius: plane is unbounded");
  TestManager::register_test(
      bounding_radius_infinite_repetition_is_unbounded,
      "geometry_bounding_radius: infinite repetition is unbounded");
  TestManager::register_test(
      bounding_radius_active_param_expression_is_unbounded,
      "geometry_bounding_radius: active param expression is unbounded");
  TestManager::register_test(
      bounding_radius_limited_repetition_widens_by_reach,
      "geometry_bounding_radius: limited repetition widens by reach");

  TestManager::register_test(
      chunks_touched_by_small_primitive_at_origin_touches_one_chunk,
      "chunks_touched_by: small primitive at origin touches one chunk");
  TestManager::register_test(
      chunks_touched_by_primitive_entirely_inside_one_chunk,
      "chunks_touched_by: primitive entirely inside one chunk");
  TestManager::register_test(
      chunks_touched_by_primitive_straddling_a_boundary_touches_both,
      "chunks_touched_by: primitive straddling a boundary touches both");
  TestManager::register_test(
      chunks_touched_by_negative_position_uses_floor_not_truncation,
      "chunks_touched_by: negative position uses floor, not truncation");
  TestManager::register_test(
      chunks_touched_by_unbounded_primitive_returns_empty,
      "chunks_touched_by: unbounded primitive returns empty (not all-chunks)");
  TestManager::register_test(
      chunks_touched_by_non_positive_chunk_size_returns_empty,
      "chunks_touched_by: non-positive chunk_size returns empty");
}
