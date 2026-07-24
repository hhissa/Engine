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

// Queues a solid-colour, axis-aligned rectangle (screen pixels, (0,0) at
// the top-left) to be drawn this frame -- see RendererBackend::
// draw_solid_quad() for exactly why this exists (a flat UI box in an
// arbitrary colour, e.g. a censor bar) and why it's guaranteed to draw on
// top of everything else queued this frame. Same per-frame queueing
// discipline as renderer_draw_text()/renderer_draw_ui_quad()/
// renderer_draw_line() above -- call it every frame you want the box
// visible, from the game's render().
void renderer_draw_solid_quad(glm::vec2 position, glm::vec2 size,
                             glm::vec4 colour);

// Queues a "looking through a camera" viewfinder-style HUD overlay within
// the given screen rect (position/size in screen pixels, matching
// renderer_draw_ui_quad()'s convention): rule-of-thirds grid lines, corner
// brackets, a small blinking REC indicator, and an optional caption line
// (e.g. an exposure-style readout) near the bottom-left corner. Built
// entirely from renderer_draw_line()/renderer_draw_text() above -- no new
// shader or pipeline -- so it's just a convenience for games that want
// that look without hand-rolling the same handful of lines/text
// themselves. Same per-frame queueing discipline as every other
// renderer_draw_*() call: queue it every frame from the game's render(),
// not once at startup. blink_time_seconds drives the REC dot's blink (feed
// it a running clock, e.g. accumulated delta_time -- the dot is solid for
// the first half of each 1-second cycle and hidden for the second half).
void renderer_draw_camera_overlay(glm::vec2 position, glm::vec2 size,
                                  f32 blink_time_seconds,
                                  std::string_view caption = {});

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

// Enables/disables the bloom post-process -- a soft glow bloomed off
// bright/emissive surfaces (see Material::emissive_intensity), added back
// on top of the scene. On by default, subtly; call with false to disable
// entirely (e.g. for a performance-constrained target, or a game whose
// look doesn't want it).
void renderer_set_bloom_enabled(b8 enabled);

// Enables/disables the vignette post-process -- a smooth radial darkening
// toward the screen edges. On by default, subtly.
void renderer_set_vignette_enabled(b8 enabled);

// Enables/disables the pixelation post-process -- quantizes the screen
// into flat-coloured blocks (the classic "pixel art" mosaic look), except
// for any primitive whose material opted out via Material::
// pixelation_exempt (see the .kmt format), which stays crisp/full-
// resolution regardless. Off by default -- it's a deliberate stylistic
// choice, not something every game using this engine would want without
// asking.
void renderer_set_pixelation_enabled(b8 enabled);

// Sets the pixelation block size (edge length, in full-resolution screen
// pixels) -- larger blocks read as chunkier/lower-fidelity. Has no visible
// effect unless pixelation is also enabled (see
// renderer_set_pixelation_enabled() above). Defaults to 6.
void renderer_set_pixelation_block_size(u32 block_size);

// Rebakes the bitmap font every renderer_draw_text() call uses (SH's Q&A
// list, HUD text, the debug camera hint, etc.), from
// assets/fonts/<name>.ttf at pixel_height -- e.g.
// renderer_set_font("DejaVuSans", 20.0f) for a smaller size using the
// engine's built-in font, or point name at any other .ttf dropped into
// assets/fonts/. Not cheap (a full atlas re-bake plus a device-idle wait,
// same cost class as a scene rebake) -- call it for a deliberate font/
// size change (e.g. once at startup, or from a settings screen), not
// every frame. Leaves the current font in place (logging an error) if
// name.ttf can't be baked.
void renderer_set_font(std::string_view name, f32 pixel_height);

// Enables an equirectangular (lat/long, NOT a 6-face cubemap) skybox behind
// all scene geometry, replacing the flat two-colour background gradient
// this engine uses when none is set -- e.g.
// renderer_enable_sky_box("sunset_sky") to use assets/textures/
// sunset_sky.png. texture_name resolves through TextureSystem exactly like
// every other texture reference in this engine (Material::diffuse_map_name,
// etc.) -- a name, not an arbitrary filesystem path. Not cheap (a device-
// idle wait to safely rewrite a descriptor binding), so call it for a
// deliberate skybox change (e.g. once per level/scene), not every frame.
void renderer_enable_sky_box(std::string_view texture_name);

// Disables the skybox, falling back to the flat gradient background.
void renderer_disable_sky_box();

b8 renderer_draw_frame(struct render_packet *packet);
