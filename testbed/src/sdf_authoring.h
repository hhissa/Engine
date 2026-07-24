#pragma once
#include <resources/sdf_scene.h>

#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <string_view>

// Testbed-side .sdf scene authoring: reading is already covered by the
// engine's load_sdf_scene() (see <resources/sdf_scene.h>) -- read_scene()
// below just forwards to it, for symmetry with the write side. Writing has
// no engine-side equivalent (by design -- see sdf_scene.h's own comment:
// it's "a pure data/parsing module" with no writer), so this is where a
// game builds an SdfScene in memory (via add_layer()/add_sphere()/
// add_box()/add_plane() below) and persists it with save_scene().
//
// None of this touches GeometrySystem/MaterialSystem or the renderer at
// all -- it's pure data, exactly like sdf_scene.h itself. Turning a saved
// file into rendered geometry is a separate step (renderer_load_scene(),
// see renderer_frontend.h).

// Parses path (see sdf_scene.h for the file format). Returns std::nullopt
// on failure (missing file) -- forwards directly to load_sdf_scene().
std::optional<SdfScene> read_scene(std::string_view path);

// Writes scene to path in exactly the format read_scene()/load_sdf_scene()
// parse. Returns false if path couldn't be opened for writing.
bool save_scene(std::string_view path, const SdfScene &scene);

// Appends a new, empty layer to scene and returns a reference to it --
// pass that reference to add_sphere()/add_box()/add_plane() below to
// populate it. NOTE: scene.layers is a std::vector, so any previously
// taken SdfLayerDef& becomes invalid the instant another add_layer() call
// (on the same scene) reallocates it -- re-fetch (or finish populating
// each layer before adding the next) rather than holding one across calls.
SdfLayerDef &add_layer(SdfScene &scene, std::string name,
                      SdfLayerOperation operation, f32 smoothness = 0.0f);

// Appends one sphere/box/plane primitive to layer and returns a reference
// to it (e.g. to tweak material_name further before saving). Same
// reallocation caveat as add_layer() applies to layer.primitives.
SdfPrimitiveDef &add_sphere(SdfLayerDef &layer, std::string name,
                           glm::vec3 position, glm::vec3 rotation,
                           f32 radius, std::string material_name);
SdfPrimitiveDef &add_box(SdfLayerDef &layer, std::string name,
                        glm::vec3 position, glm::vec3 rotation,
                        glm::vec3 half_extents, std::string material_name);
// Always the horizontal plane y = height, matching GeometryConfig::plane()
// engine-side -- position is meaningless for a plane, so there's no
// position parameter here.
SdfPrimitiveDef &add_plane(SdfLayerDef &layer, std::string name, f32 height,
                          std::string material_name);

// Appends one primitive of any other SdfPrimitiveType (Torus, CappedCylinder,
// CappedCone, RoundBox, BoxFrame, Octahedron, Pyramid, HexPrism, RoundCone,
// Capsule, Link, Ellipsoid -- see SdfPrimitiveType's comment for what params/
// extra_param mean for each) -- a generic counterpart to add_sphere()/
// add_box()/add_plane() above for types that don't have their own named
// helper. Same reallocation caveat as add_layer() applies to
// layer.primitives.
SdfPrimitiveDef &add_primitive(SdfLayerDef &layer, std::string name,
                              SdfPrimitiveType type, glm::vec3 position,
                              glm::vec3 rotation, glm::vec3 params,
                              f32 extra_param, std::string material_name);

// Appends a directional/point light to scene (see SdfLightType's comment)
// and returns a reference to it. Same reallocation caveat as add_layer()
// applies to scene.lights.
SdfLightDef &add_directional_light(SdfScene &scene, std::string name,
                                   glm::vec3 direction, glm::vec3 colour,
                                   f32 intensity);
SdfLightDef &add_point_light(SdfScene &scene, std::string name,
                            glm::vec3 position, glm::vec3 colour,
                            f32 intensity);

// Appends a transparent, textured "volumetric light" shape (see
// SdfVolumetricDef's comment in sdf_scene.h) and returns a reference to it.
// Same reallocation caveat as add_layer() applies to scene.volumetrics.
SdfVolumetricDef &add_volumetric(SdfScene &scene, std::string name,
                                 SdfPrimitiveType type, glm::vec3 position,
                                 glm::vec3 rotation, glm::vec3 params,
                                 f32 extra_param, f32 density,
                                 std::string material_name);
