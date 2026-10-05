#include "layer_transform_tests.h"
#include "../expect.h"
#include "../test_manager.h"

#include <defines.h>
#include <resources/sdf_scene.h>
#include <sdf_authoring.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdio>

// A layer's own transform (see SdfLayerDef::position) is not carried to the
// shader -- it is COMPOSED onto each of its primitives at the point the
// authored scene becomes runtime geometry, so that "world space" stays the
// only space anything downstream of that point has to know about.
//
// That makes sdf_layer_world_transform() the single definition of what a
// layer transform means: the voxel bake, the primitive buffer, chunk
// culling and the editor's picker all read the transform it produces. So
// these tests are all about that one function -- that it is the identity
// when nothing has been transformed, that it composes in the documented
// order, that sdf_layer_local_transform() undoes it exactly (the editor's
// gizmo drags in world space and writes authored values back through it),
// and that the file round-trips.

namespace {

const char *kTempPath = "assets/scenes/.layer_transform_test.sdf";

bool vec3_near(glm::vec3 expected, glm::vec3 actual) {
  EXPECT_FLOAT_EQ(expected.x, actual.x);
  EXPECT_FLOAT_EQ(expected.y, actual.y);
  EXPECT_FLOAT_EQ(expected.z, actual.z);
  return true;
}

// Orientations are compared as quaternions, never as Euler triples:
// sdf_layer_world_transform() composes rotations and reads the result back
// out with eulerAngles(), which is free to return a different triple than
// either input as long as it encodes the same rotation. The rotation is
// what renders; the angles are just how it is stored.
//
// q and -q are the same rotation, hence the abs() on the dot product.
bool rotation_near(glm::vec3 expected_euler, glm::vec3 actual_euler) {
  glm::quat expected(expected_euler);
  glm::quat actual(actual_euler);
  EXPECT_FLOAT_EQ(1.0f, std::fabs(glm::dot(expected, actual)));
  return true;
}

SdfPrimitiveDef make_box(glm::vec3 position, glm::vec3 rotation) {
  SdfPrimitiveDef box;
  box.name = "box";
  box.type = SdfPrimitiveType::Box;
  box.position = position;
  box.rotation = rotation;
  box.params = glm::vec3(0.5f);
  return box;
}

// The regression that matters most: an untransformed layer -- every layer
// in every scene authored before this existed -- must leave its primitives
// exactly as they were, not merely close. A composition that drifted by an
// epsilon here would report every primitive in the scene as changed on
// every reconcile (see primitive_shape_matches(), geometry_system.cpp) and
// re-bake the whole thing on every edit.
bool identity_layer_changes_nothing() {
  SdfLayerDef layer;
  const SdfPrimitiveDef box =
      make_box(glm::vec3(2.0f, -1.0f, 0.5f), glm::vec3(0.3f, 1.1f, -0.7f));

  const SdfTransform world = sdf_layer_world_transform(layer, box);
  EXPECT_TRUE(world.position == box.position);
  EXPECT_TRUE(world.rotation == box.rotation);
  return true;
}

bool translation_moves_the_whole_layer() {
  SdfLayerDef layer;
  layer.position = glm::vec3(10.0f, 0.0f, -4.0f);

  // Two primitives a fixed distance apart: a layer translation must move
  // both by the same vector, i.e. leave the arrangement between them
  // untouched. That is the entire point of the feature.
  const SdfPrimitiveDef a = make_box(glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(0.0f));
  const SdfPrimitiveDef b = make_box(glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f));

  const SdfTransform world_a = sdf_layer_world_transform(layer, a);
  const SdfTransform world_b = sdf_layer_world_transform(layer, b);
  EXPECT_TRUE(vec3_near(glm::vec3(11.0f, 2.0f, -1.0f), world_a.position));
  EXPECT_TRUE(vec3_near(glm::vec3(9.0f, 0.0f, -4.0f), world_b.position));
  EXPECT_TRUE(vec3_near(a.position - b.position,
                        world_a.position - world_b.position));
  // Translating a layer never turns anything.
  EXPECT_TRUE(rotation_near(glm::vec3(0.0f), world_a.rotation));
  return true;
}

bool rotation_orbits_and_composes() {
  SdfLayerDef layer;
  layer.rotation = glm::vec3(0.0f, glm::half_pi<f32>(), 0.0f);

  // A quarter turn about +Y takes the +X axis to -Z (right-handed), so a
  // primitive standing at (1,0,0) in the layer ends up at (0,0,-1) -- it
  // ORBITS the layer's origin rather than spinning where it stands.
  const SdfPrimitiveDef box = make_box(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f));
  const SdfTransform world = sdf_layer_world_transform(layer, box);
  EXPECT_TRUE(vec3_near(glm::vec3(0.0f, 0.0f, -1.0f), world.position));
  // ...and it is itself turned by the same quarter turn, which is what
  // keeps the arrangement rigid rather than smearing it.
  EXPECT_TRUE(rotation_near(layer.rotation, world.rotation));

  // Rotate about the layer's own origin, THEN translate -- the order
  // SdfLayerDef::position documents. Composing the other way round would
  // put this primitive at (1,0,-1) instead.
  layer.position = glm::vec3(5.0f, 0.0f, 0.0f);
  const SdfTransform moved = sdf_layer_world_transform(layer, box);
  EXPECT_TRUE(vec3_near(glm::vec3(5.0f, 0.0f, -1.0f), moved.position));
  return true;
}

// The primitive's own rotation composes UNDER the layer's, not alongside
// it: what renders is layer_rotation * primitive_rotation.
bool composed_rotation_is_the_quaternion_product() {
  SdfLayerDef layer;
  layer.rotation = glm::vec3(0.4f, -0.9f, 0.2f);
  const SdfPrimitiveDef box = make_box(glm::vec3(0.0f), glm::vec3(-1.2f, 0.5f, 0.8f));

  const SdfTransform world = sdf_layer_world_transform(layer, box);
  const glm::quat expected = glm::quat(layer.rotation) * glm::quat(box.rotation);
  EXPECT_TRUE(rotation_near(glm::eulerAngles(expected), world.rotation));
  return true;
}

// sdf_layer_local_transform() is what the editor's gizmo writes through on
// every mouse-move of a drag: it takes the world-space result of the
// gesture and turns it back into the layer-local values a primitive
// actually stores. If it were not an exact inverse, dragging a primitive
// in a transformed layer would make it creep.
bool local_transform_inverts_world_transform() {
  SdfLayerDef layer;
  layer.position = glm::vec3(-3.0f, 7.5f, 1.25f);
  layer.rotation = glm::vec3(0.35f, 1.4f, -0.6f);

  const SdfPrimitiveDef box =
      make_box(glm::vec3(2.0f, -1.0f, 0.5f), glm::vec3(0.3f, 1.1f, -0.7f));

  const SdfTransform world = sdf_layer_world_transform(layer, box);
  const SdfTransform local = sdf_layer_local_transform(
      layer, box.type, world.position, world.rotation);

  EXPECT_TRUE(vec3_near(box.position, local.position));
  EXPECT_TRUE(rotation_near(box.rotation, local.rotation));
  return true;
}

// A Plane is always the horizontal y=height plane and never rotates (see
// GeometryConfig::plane()), so there is nothing for a layer transform to
// compose onto. Both directions must pass it straight through -- the
// editor's gizmo relies on that to keep dragging a plane's height the one
// thing that moves it.
bool a_plane_ignores_its_layer_transform() {
  SdfLayerDef layer;
  layer.position = glm::vec3(4.0f, 9.0f, -2.0f);
  layer.rotation = glm::vec3(0.0f, glm::half_pi<f32>(), 0.0f);

  SdfPrimitiveDef plane;
  plane.name = "ground";
  plane.type = SdfPrimitiveType::Plane;
  plane.params = glm::vec3(1.5f, 0.0f, 0.0f);

  const SdfTransform world = sdf_layer_world_transform(layer, plane);
  EXPECT_TRUE(world.position == plane.position);
  EXPECT_TRUE(world.rotation == plane.rotation);

  const SdfTransform local = sdf_layer_local_transform(
      layer, plane.type, glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(0.1f));
  EXPECT_TRUE(vec3_near(glm::vec3(1.0f, 2.0f, 3.0f), local.position));
  return true;
}

bool layer_transform_round_trips_through_a_file() {
  SdfScene scene;
  {
    SdfLayerDef &moved = add_layer(scene, "moved", SdfLayerOperation::Union);
    moved.position = glm::vec3(1.5f, -2.25f, 0.125f);
    moved.rotation = glm::vec3(0.1f, -0.7f, 1.3f);
    add_box(moved, "box", glm::vec3(1.0f), glm::vec3(0.0f), glm::vec3(0.5f), "");
  }
  {
    // A layer left alone, to prove the writer's "only emit a transform
    // that is not the identity" rule reads back as the identity rather
    // than as something omitted-and-therefore-undefined.
    SdfLayerDef &still = add_layer(scene, "still", SdfLayerOperation::Union);
    add_box(still, "box", glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.5f), "");
  }

  EXPECT_TRUE(save_scene(kTempPath, scene));
  auto reloaded = read_scene(kTempPath);
  std::remove(kTempPath);
  EXPECT_TRUE(reloaded.has_value());
  EXPECT_EQ(2u, static_cast<u32>(reloaded->layers.size()));

  EXPECT_TRUE(vec3_near(scene.layers[0].position, reloaded->layers[0].position));
  EXPECT_TRUE(vec3_near(scene.layers[0].rotation, reloaded->layers[0].rotation));
  EXPECT_TRUE(reloaded->layers[1].position == glm::vec3(0.0f));
  EXPECT_TRUE(reloaded->layers[1].rotation == glm::vec3(0.0f));

  // What actually renders has to survive the file, not just the fields.
  const SdfTransform before =
      sdf_layer_world_transform(scene.layers[0], scene.layers[0].primitives[0]);
  const SdfTransform after = sdf_layer_world_transform(
      reloaded->layers[0], reloaded->layers[0].primitives[0]);
  EXPECT_TRUE(vec3_near(before.position, after.position));
  EXPECT_TRUE(rotation_near(before.rotation, after.rotation));
  return true;
}

} // namespace

void register_layer_transform_tests() {
  TestManager::register_test(identity_layer_changes_nothing,
                             "An identity layer transform leaves its "
                             "primitives bit-for-bit unchanged");
  TestManager::register_test(translation_moves_the_whole_layer,
                             "A layer translation moves every primitive by "
                             "the same vector");
  TestManager::register_test(rotation_orbits_and_composes,
                             "A layer rotation orbits its primitives about "
                             "the layer origin, then translates");
  TestManager::register_test(composed_rotation_is_the_quaternion_product,
                             "A primitive's rotation composes under its "
                             "layer's");
  TestManager::register_test(local_transform_inverts_world_transform,
                             "sdf_layer_local_transform() exactly inverts "
                             "sdf_layer_world_transform()");
  TestManager::register_test(a_plane_ignores_its_layer_transform,
                             "A Plane passes through its layer's transform "
                             "untouched");
  TestManager::register_test(layer_transform_round_trips_through_a_file,
                             "A layer transform round-trips through "
                             "save_scene()/read_scene()");
}
