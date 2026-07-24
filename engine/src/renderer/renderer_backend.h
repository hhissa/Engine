#pragma once

#include "camera.h"
#include "renderer_types.inl"

#include <glm/glm.hpp>
#include <memory>
#include <string_view>

class PlatformLayer;

class RendererBackend {
public:
  virtual ~RendererBackend() = default;

  virtual b8 initialize(std::string_view application_name,
                        PlatformLayer &plat_state) = 0;
  virtual void shutdown() = 0;
  virtual void on_resized(u16 width, u16 height) = 0;
  virtual b8 begin_frame(f32 delta_time) = 0;
  virtual void set_camera(const Camera &camera) = 0;

  // Queues a UI/text draw for the frame currently being built (i.e.
  // between the game's update() and the following end_frame() -- see
  // TestbedGame::render() for the intended call site). Queued draws are
  // flushed and cleared by end_frame(), so nothing persists to the next
  // frame automatically -- callers that want something on screen every
  // frame must queue it every frame, the same way set_camera() is called
  // every frame rather than once. This is what lets an application (not
  // the engine) decide what UI/text actually appears, using the UI
  // renderpass/shaders as a service rather than the engine hardcoding
  // fixed content into it.
  virtual void draw_text(std::string_view text, glm::vec2 position,
                        glm::vec4 colour) = 0;
  // position/size are screen pixels, (0,0) at the top-left -- draws the
  // engine's single built-in demo UI texture at that rectangle.
  virtual void draw_ui_quad(glm::vec2 position, glm::vec2 size) = 0;

  // Queues a solid-colour line segment (screen pixels, (0,0) at the
  // top-left, fixed thickness) for this frame -- same queue-per-frame
  // discipline as draw_text()/draw_ui_quad() above. Added for tools that
  // embed this engine's rendering directly into their own native window
  // (see tools/sdf_editor's SceneViewport, which uses this for its move-
  // gizmo's axis lines) and need 2D overlay content guaranteed to
  // composite on top of the raymarched scene -- a separate widget/window
  // stacked over the same native surface would not reliably do so (native
  // child windows always draw over ordinary sibling widgets on X11,
  // regardless of declared widget stacking order).
  virtual void draw_line(glm::vec2 start, glm::vec2 end, glm::vec4 colour) = 0;

  // Queues a solid-colour, axis-aligned rectangle (screen pixels) to be
  // drawn this frame -- same per-frame queueing discipline as
  // draw_text()/draw_ui_quad()/draw_line() above, and drawn last of all of
  // them (see VulkanRendererBackend::end_frame()), so it's guaranteed to
  // fully occlude whatever's beneath it. Added for games that need a flat
  // UI box with an arbitrary colour (e.g. a censor bar) -- draw_ui_quad()
  // always samples a fixed demo texture, and draw_line() only draws thin
  // segments, neither of which fit that need.
  virtual void draw_solid_quad(glm::vec2 position, glm::vec2 size,
                              glm::vec4 colour) = 0;

  // Loads sdf_path (an .sdf scene file -- see sdf_scene.h for the format)
  // and registers every primitive it describes as a static SDF primitive,
  // then re-bakes the raymarch scene so the result becomes visible.
  // Multiple scenes can be loaded concurrently -- each returned handle
  // tracks only the primitives that particular call registered, entirely
  // independent of any other loaded scene. Returns kInvalidSceneHandle on
  // failure (missing/malformed file).
  virtual SceneHandle load_scene(std::string_view sdf_path) = 0;

  // Scene-wide transforms: each applies to every primitive registered by
  // the load_scene() call that returned handle, then re-bakes (a device-
  // idle wait, same cost note as load_scene()) -- meant for discrete
  // post-load placement, not per-frame animation. No-op (logs a warning)
  // if handle isn't currently loaded. Caveat shared by all three: a params
  // slot overridden by a param_expression keeps its formula untouched --
  // only the plain constants are transformed.
  //
  // Moves every primitive by delta (world units). A Plane only has a
  // height (it's always the horizontal y=height plane), so only delta.y
  // means anything for it.
  virtual void translate_scene(SceneHandle handle, glm::vec3 delta) = 0;

  // Rotates every primitive about the *world origin* by euler_radians
  // (XYZ order, same convention as SdfPrimitiveDef::rotation): positions
  // orbit the origin and each primitive's own orientation is composed with
  // the scene rotation. Planes are skipped with a warning -- they can't
  // tilt (see translate_scene()).
  virtual void rotate_scene(SceneHandle handle, glm::vec3 euler_radians) = 0;

  // Uniformly scales every primitive about the *world origin* by factor:
  // positions and every length-like parameter (params + extra_param) are
  // multiplied. Uniform only -- non-uniform scaling would break the SDF
  // distance metric.
  virtual void scale_scene(SceneHandle handle, f32 factor) = 0;

  // Releases every primitive registered by the load_scene() call that
  // returned handle, then re-bakes. No-op (logs a warning) if handle isn't
  // currently loaded (e.g. already removed, or never valid).
  virtual void remove_scene(SceneHandle handle) = 0;

  // Releases every primitive from every currently loaded scene (as if
  // remove_scene() had been called for each one), then re-bakes once.
  virtual void clear_scenes() = 0;

  // Marks which registered static primitive (its index into the raymarch
  // field's scene_textures/scene_diffuse_colours arrays -- see
  // VulkanRaymarchShader::rebuild_static_scene()) the render pass should
  // draw a selection outline around, or -1 for none. A push constant, not
  // baked into the field, so it takes effect the very next frame with no
  // rebake -- see VulkanRaymarchShader::render_to().
  virtual void set_selected_primitive(i32 index) = 0;

  // Shows/hides the reference grid -- the subdivided ground plane (y=0)
  // the render pass draws as a modelling aid. Like set_selected_primitive()
  // it's just a push constant (see VulkanRaymarchShader::set_grid_visible()),
  // so it takes effect the very next frame with no rebake. Hidden by
  // default -- editor tooling opts in, games never see it.
  virtual void set_grid_visible(b8 visible) = 0;

  // Post-process toggles/params -- see VulkanRaymarchShader::
  // set_bloom_enabled()/set_vignette_enabled()/set_pixelation_enabled()/
  // set_pixelation_block_size() for exact semantics. All are just push
  // constants read by Builtin.PostComposite.comp.glsl, so each takes
  // effect the very next frame with no rebake.
  virtual void set_bloom_enabled(b8 enabled) = 0;
  virtual void set_vignette_enabled(b8 enabled) = 0;
  virtual void set_pixelation_enabled(b8 enabled) = 0;
  virtual void set_pixelation_block_size(u32 block_size) = 0;

  // Rebakes the bitmap font every renderer_draw_text() call uses, from
  // assets/fonts/<name>.ttf at pixel_height -- see VulkanTextShader::
  // set_font() for exact semantics (not cheap -- a full re-bake + device-
  // idle wait -- call it for a deliberate font/size change, not every
  // frame).
  virtual void set_font(std::string_view name, f32 pixel_height) = 0;

  // Enables/disables an equirectangular skybox behind all scene geometry --
  // see VulkanRaymarchShader::set_skybox()/disable_skybox() for exact
  // semantics (texture_name resolves through TextureSystem, exactly like
  // every other texture reference in this engine -- assets/textures/
  // <texture_name>.png, not an arbitrary filesystem path). Not cheap (a
  // device-idle wait to safely rewrite a descriptor binding), so call it
  // for a deliberate skybox change, not every frame.
  virtual void set_skybox(std::string_view texture_name) = 0;
  virtual void disable_skybox() = 0;

  virtual b8 end_frame(f32 delta_time) = 0;

  u64 frame_number = 0;
};

std::unique_ptr<RendererBackend>
renderer_backend_create(renderer_backend_type type, PlatformLayer &plat_state);
