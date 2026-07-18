#pragma once

#include "camera.h"
#include "renderer_types.inl"

#include <glm/glm.hpp>
#include <string_view>

struct static_mesh_data;
class PlatformLayer;
b8 renderer_initialize(const std::string_view, struct PlatformLayer &platform);
void renderer_shutdown();

void renderer_on_resized(u16 width, u16 height);

// Sets the camera used for the next frame's render. The game owns and
// drives the Camera (position/orientation from input); the renderer just
// forwards whatever it's given to the backend.
void renderer_set_camera(const Camera &camera);

// Queues one line of text to be drawn this frame (see
// RendererBackend::draw_text) -- call from the game's render(), not once
// at startup, since queued draws don't persist to the next frame.
void renderer_draw_text(std::string_view text, glm::vec2 position,
                       glm::vec4 colour);

// Queues the engine's built-in demo UI texture to be drawn at
// position/size (screen pixels) this frame (see
// RendererBackend::draw_ui_quad).
void renderer_draw_ui_quad(glm::vec2 position, glm::vec2 size);

// Queues a solid-colour line segment (screen pixels, fixed thickness) to
// be drawn this frame (see RendererBackend::draw_line) -- same per-frame
// queueing discipline as renderer_draw_text()/renderer_draw_ui_quad()
// above.
void renderer_draw_line(glm::vec2 start, glm::vec2 end, glm::vec4 colour);

// Thin chainable wrapper around a SceneHandle, returned by
// renderer_load_scene() below. Implicitly converts to SceneHandle, so
// existing code that stores the handle keeps working unchanged. The
// transform methods apply to every primitive the scene registered and
// each one re-bakes the raymarch scene (a device-idle wait -- same cost
// note as renderer_load_scene()), so they're for discrete post-load
// placement, chained fluent-style:
//
//   demo_scene_ = renderer_load_scene("assets/scenes/demo_scene.sdf")
//                     .translate(glm::vec3(0.0f, 1.0f, 0.0f))
//                     .rotate(glm::vec3(0.0f, glm::radians(45.0f), 0.0f))
//                     .scale(0.5f);
//
// All three are safe no-ops on an invalid handle (a failed load), so a
// chain never needs a validity check in the middle. See the matching
// RendererBackend::translate_scene()/rotate_scene()/scale_scene() doc
// comments for exact semantics (rotation/scale are about the world
// origin -- so scale() also scales any translation applied before it in
// the chain: scale first, then translate, to place a resized model at an
// exact spot; planes can only translate vertically; param_expression
// formulas scale correctly via Geometry::param_expr_scale).
class KAPI SceneRef {
public:
  explicit SceneRef(SceneHandle handle) : handle_(handle) {}

  operator SceneHandle() const { return handle_; }
  SceneHandle handle() const { return handle_; }

  // Moves the scene by delta (world units).
  SceneRef &translate(glm::vec3 delta);
  // Rotates the scene about the world origin by Euler angles (radians,
  // XYZ order -- same convention as .sdf "rotation=" lines).
  SceneRef &rotate(glm::vec3 euler_radians);
  // Uniformly scales the scene about the world origin. Uniform only --
  // non-uniform scaling would break the SDF distance metric.
  SceneRef &scale(f32 factor);

private:
  SceneHandle handle_;
};

// Loads sdf_path (an .sdf scene file -- see engine/src/resources/
// sdf_scene.h for the file format) and registers every primitive it
// describes as a static SDF primitive, then re-bakes the raymarch scene
// so the result becomes visible immediately (see RendererBackend::
// load_scene() -- this call is not cheap, a full device-idle wait, so
// call it for discrete scene loads, not every frame). Multiple scenes can
// be loaded at the same time -- each one is an independent "model": the
// returned handle tracks only the primitives that particular call
// registered, so removing one scene never affects any other. Returns a
// SceneRef holding kInvalidSceneHandle on failure (missing/malformed
// file); see SceneRef above for the chainable post-load transforms.
SceneRef renderer_load_scene(std::string_view sdf_path);

// Unloads exactly the scene identified by handle (as returned by an
// earlier renderer_load_scene() call), releasing every primitive it
// registered and re-baking. No-op (logs a warning) if handle isn't
// currently loaded.
void renderer_remove_scene(SceneHandle handle);

// Unloads every currently loaded scene (as if renderer_remove_scene() had
// been called for each one), then re-bakes once.
void renderer_clear_scenes();

// Marks which registered static primitive to draw a selection outline
// around this frame (see RendererBackend::set_selected_primitive() for what
// `index` means), or -1 for none. Cheap and immediate (a push constant, not
// a rebake) -- call every time the selection changes, not once at startup.
void renderer_set_selected_primitive(i32 index);

// Shows/hides the reference grid: the ground plane (y=0) subdivided into
// 1-unit cells (heavier lines every 10 units, world X/Z axes tinted),
// drawn by the raymarch pass as a modelling aid. Hidden by default so
// games never render it -- editor tooling (tools/sdf_editor) opts in
// after renderer_initialize(). Like the selection outline it's cheap and
// immediate (a push constant, not a rebake), so it can be toggled every
// frame if needed.
void renderer_set_grid_visible(b8 visible);

b8 renderer_draw_frame(struct render_packet *packet);
