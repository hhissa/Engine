#pragma once

#include "camera.h"
#include "renderer_types.inl"

#include <glm/glm.hpp>
#include <memory>
#include <string>
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
  // scale enlarges the glyphs about position -- see renderer_draw_text().
  virtual void draw_text(std::string_view text, glm::vec2 position,
                        glm::vec4 colour, f32 scale) = 0;
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

  // The same solid-colour rectangle as draw_solid_quad(), but drawn FIRST
  // in the UI pass rather than last -- over the raymarched scene, under
  // every quad/text/line queued this frame. For a backdrop that text has
  // to sit on (e.g. a black title card over a still-loaded scene), which
  // draw_solid_quad() can't be, since it occludes text too.
  virtual void draw_backdrop_quad(glm::vec2 position, glm::vec2 size,
                                 glm::vec4 colour) = 0;

  // Queues the image at path (a texture name -- "photos/cloud_mel.jpg" is
  // assets/photos/cloud_mel.jpg, see TextureSystem) stretched over
  // position/size this frame. Drawn with the backdrop quads: over the
  // scene, under every text/quad/line, in queue order with them.
  virtual void draw_image(std::string_view path, glm::vec2 position,
                          glm::vec2 size) = 0;

  // Loads path's image now rather than on its first draw_image(), and
  // returns its size in pixels -- (0, 0) if it can't be loaded. For a
  // caller that needs the aspect ratio to lay it out, or wants the decode
  // done ahead of time.
  virtual glm::vec2 preload_image(std::string_view path) = 0;

  // Shows/hides the reference grid -- the subdivided ground plane (y=0)
  // the render pass draws as a modelling aid. Like set_selected_primitive()
  // it's just a push constant (see VulkanRaymarchShader::set_grid_visible()),
  // so it takes effect the very next frame with no rebake. Hidden by
  // default -- editor tooling opts in, games never see it.
  virtual void set_grid_visible(b8 visible) = 0;

  // Phase 4: switches the render pass's primary hit-test/normals/contact-AO/
  // shadows from the fixed-cube field every caller has always used onto the
  // chunked/streamed field instead -- see VulkanRaymarchShader::
  // set_chunked_field_enabled(). Just a push constant, takes effect the
  // very next frame, no rebake needed. Off by default, so nothing currently
  // working changes unless a caller explicitly opts in.
  virtual void set_chunked_field_enabled(b8 enabled) = 0;

  // Selects how the chunked field's baked point cloud drives primary
  // visibility -- see RendererSplatMode (renderer_types.inl) and
  // VulkanRaymarchShader::set_splat_mode(). Just a push constant (plus
  // whether the splat prepass is recorded), takes effect the very next
  // frame with no rebake.
  virtual void set_splat_mode(RendererSplatMode mode) = 0;

  // Temporal anti-aliasing -- see VulkanRaymarchShader::set_taa_enabled().
  // On by default. Costs one full-resolution resolve pass either way (with
  // it off the pass degenerates to a copy), so this is a pure quality
  // switch, not a pipeline reconfiguration.
  virtual void set_taa_enabled(b8 enabled) = 0;

  // Imperfect shadow maps for local point lights -- see
  // VulkanRaymarchShader::set_ism_enabled(). On by default.
  virtual void set_ism_enabled(b8 enabled) = 0;

  // Stochastic ambient occlusion over the binary voxel cascades -- see
  // VulkanRaymarchShader::set_ao_enabled(). On by default, and it wants TAA
  // on too: it traces one ray per pixel per frame.
  virtual void set_ao_enabled(b8 enabled) = 0;

  // Debug: samples the baked field at a world point and logs what it holds
  // -- see VulkanRaymarchShader::debug_probe_field(). Synchronous and
  // device-stalling; for diagnosis, not for a running game.
  virtual void debug_probe_field(glm::vec3 world_pos) = 0;

  // M5 (Finishes) + M6 (Flecks): configures material slot `slot` (must be
  // >= 2 -- 0/1 are reserved, see VulkanRaymarchShader::set_material()'s
  // own comment) with a base colour/roughness/metallic/waxy "Finish" AND a
  // fleck_pattern (0-15)/fleck_strength (0-1)/fleck_scale "Fleck" -- see
  // that method for the exact packing. One slot carries both together
  // (not two independent allocation paths), matching how tools/
  // modelling_software_v2's apply_finish_and_colour() allocates a slot. A
  // no-op (logs a warning) if slot is out of range. Synchronous, cheap,
  // and NOT device-stalling (a direct host-visible buffer write, same as
  // ensure_default_sculpt_material()) -- meant to be called once per
  // Finish/Fleck/colour pick in a tool, before the next stamp_shape()/call
  // that references this slot, not per-frame. Pass fleck_strength 0 (with
  // any fleck_pattern/fleck_scale -- they are ignored) for "no Fleck".
  virtual void set_material(i32 slot, glm::vec3 colour, f32 roughness,
                            f32 metallic, bool waxy, i32 fleck_pattern,
                            f32 fleck_strength, f32 fleck_scale) = 0;

  // Registers one standing light -- see VulkanRaymarchShader::
  // add_directional_light()/add_point_light() for exactly what this does
  // and why it bypasses GeometrySystem::acquire_light() (that CPU-side
  // registry is currently a dead API: nothing has fed light_snapshot()
  // into the render pass's GPU light buffer since rebuild_static_scene(),
  // its only bridge, was deleted in the M0 CSG pass). direction need not
  // be pre-normalized. Returns the assigned light slot, or -1 if the
  // fixed light cap is already full (logs a warning). Cheap, synchronous,
  // NOT device-stalling -- call it once per standing light a tool wants,
  // not per frame.
  virtual i32 add_directional_light(glm::vec3 direction, glm::vec3 colour,
                                    f32 intensity) = 0;
  virtual i32 add_point_light(glm::vec3 position, glm::vec3 colour,
                             f32 intensity) = 0;

  // M2a: stamps one analytic brush shape (any of SculptBrushShape's 15
  // values), blended via shape_op (union or subtraction) with blend radius
  // smoothness, directly into the chunked field's existing GPU buffers,
  // and regenerates the splat points it needs to actually render -- see
  // VulkanRaymarchShader::stamp_shape(). Only meaningful alongside
  // renderer_set_chunked_field_enabled(true); the fixed-cube field has no
  // stamp path. Synchronous and device-stalling, like debug_probe_field()
  // above -- an authoring-time operation, not a per-frame one. params'
  // meaning depends on shape_type -- see VulkanRaymarchShader::
  // stamp_shape()'s header comment for the full per-shape layout table.
  // material_slot indexes the per-primitive material arrays; defaults to
  // 0, the one material every M1a/M2a stamp uses so far. Brush ROTATION is
  // out of scope for M2a -- shape_type always stamps in its canonical,
  // axis-aligned local orientation.
  virtual void stamp_shape(glm::vec3 center, SculptBrushShape shape_type,
                           glm::vec4 params, f32 smoothness,
                           SculptBrushOp shape_op, i32 material_slot = 0,
                           f32 looseness = 0.0f) = 0;

  // M2b: reverts the most recently applied stamp (GPU voxels/material
  // restored or its freshly-allocated brick freed, plus the matching
  // bookkeeping record dropped) -- see VulkanRendererBackend::undo_stamp().
  // Returns false if there is nothing left to undo.
  virtual bool undo_stamp() = 0;

  // M2b: re-applies the most recently undone stamp (by re-invoking
  // stamp_shape() with its original parameters) -- see VulkanRendererBackend
  // ::redo_stamp(). Returns false if there is nothing to redo (either
  // nothing has been undone, or a fresh stamp_shape() call since the last
  // undo has cleared the redo history -- see its own comment for why that
  // invalidation is correct).
  virtual bool redo_stamp() = 0;

  // M3c: Spraypaint -- a paint-ONLY brush, see VulkanRaymarchShader::
  // spray_paint() for the full mechanics. Accumulates opacity_per_pass
  // (clamped) into a per-voxel paint buffer for every voxel INSIDE the
  // brush shape's volume, in every touched cell that already has a brick
  // (no geometry is read or written at all -- unlike stamp_shape(), there
  // is no `op`/`smoothness`/blend here). Synchronous and device-stalling,
  // like stamp_shape(). Returns how many coarse cells were actually
  // touched (0 if the shape's bounding radius is non-positive, or if the
  // brush's whole AABB falls over chunks nothing has ever been stamped
  // into -- spraying over empty space is a correct, silent no-op, not an
  // error). Not currently undoable/redoable -- see spray_paint()'s own
  // header comment for why, and what a future milestone adding that would
  // need to extend.
  virtual i32 spray_paint(glm::vec3 center, SculptBrushShape shape_type,
                          glm::vec4 params, f32 opacity_per_pass) = 0;

  // M7 polish: the live brush-cursor "ghost" preview -- see
  // VulkanRaymarchShader::set_brush_preview()'s own header comment for the
  // full mechanics (a shaded sphere-trace of ONE analytic shape, composited
  // over whatever render_to() already produced for that pixel, read from a
  // small UBO on the shading pipeline's existing additive descriptor set).
  // `shape_type`/`params`/`op`/`material_slot` use the exact same per-shape
  // layout/enum values as stamp_shape() above -- the preview always matches
  // what a real stamp at `center` right now would place. Cheap, synchronous,
  // NOT device-stalling (a direct host-visible buffer write) -- unlike
  // stamp_shape()/spray_paint(), this IS meant to be called every frame the
  // preview is visible, not just on an authoring-time action; pass
  // `enabled=false` (center/params/op/material_slot are then ignored) the
  // instant the preview should stop showing (pointer left the viewport, a
  // drag started, etc.) so the ghost never sticks around a stale frame.
  virtual void preview_brush(bool enabled, glm::vec3 center,
                             SculptBrushShape shape_type, glm::vec4 params,
                             SculptBrushOp op, i32 material_slot) = 0;

  // M4b: deletes the stamp at `index` (0 = oldest, matching SculptVolume::
  // stamps()'s own order) WITHOUT discarding anything stamped before or
  // after it -- the checkpoint/replay counterpart to undo_stamp()'s own
  // tail-only revert. See VulkanRendererBackend::delete_edit() for the full
  // restore-then-replay mechanics. Returns false if index is out of range.
  // Synchronous and device-stalling, like every other sculpt-family call.
  virtual bool delete_edit(i32 index) = 0;

  // M4c (Move-of-an-arbitrary-edit): re-centers the stamp at `index` to
  // new_center WITHOUT changing its shape/params/smoothness/op/
  // material_slot/looseness, and WITHOUT changing its position in
  // SculptVolume::stamps() -- the in-place counterpart to delete_edit()'s
  // own restore-then-replay, reusing exactly the same checkpoint/replay
  // engine (restore the nearest checkpoint strictly before `index`, replay
  // everything between it and `index` unchanged, re-stamp `index` itself at
  // new_center instead of skipping it, then replay everything after `index`
  // unchanged). See VulkanRendererBackend::move_edit() for the full
  // mechanics. Returns false if index is out of range. Synchronous and
  // device-stalling, like every other sculpt-family call. Deliberately
  // additive alongside the tail-only Move (stamp_shape()'s own re-stamp-the-
  // most-recent-undo path, driven tool-side by SceneViewport's Move mode) --
  // that path is untouched and remains the quick, no-checkpoint-replay-
  // needed way to move whatever was JUST stamped.
  virtual bool move_edit(i32 index, glm::vec3 new_center) = 0;

  // M4 (Crop): a whole-volume boolean filter -- keeps only the geometry
  // currently inside shape_type/params/center (centered/smoothed exactly
  // like a stamp's own params -- see VulkanRaymarchShader::crop_volume()'s
  // own header comment for the keep-inside SDF identity), discarding
  // everything outside. Unlike every prior tool (stamp_shape()/spray_
  // paint()/undo_stamp()), this is NOT scoped to the shape's own AABB --
  // it enumerates and filters EVERY resident brick in the whole volume.
  // Synchronous and device-stalling, like every other sculpt-family call.
  //
  // Deliberately NOT integrated into SculptVolume/the checkpoint-replay
  // engine, and NOT undoable/redoable this pass -- same documented,
  // accepted scope Spraypaint shipped with (see VulkanRaymarchShader::
  // crop_volume()'s own header comment for the full writeup, including
  // what that means for a LATER delete_edit()/move_edit() targeting an
  // edit that landed before a Crop ran). Returns how many resident bricks
  // were actually touched (0 if the volume is empty, or the shape's
  // bounding radius is non-positive).
  virtual i32 crop_volume(glm::vec3 center, SculptBrushShape shape_type,
                          glm::vec4 params, f32 smoothness) = 0;

  // M4 (Cutout): identical live effect to crop_volume() above (the kept-
  // inside geometry ends up the same), but FIRST captures everything the
  // crop is about to discard and writes it to `removed_path` as a
  // standalone snapshot file a FRESH tool session can open separately
  // (see VulkanRaymarchShader::export_crop_outside()/load_crop_outside_
  // file() for the file format and why it is not the tool's ordinary
  // .stamps format). There is no live second sculpt volume in this process
  // -- "preserved as a new separate sculpture" means a second FILE, opened
  // in a second session, not simultaneous live rendering of both halves
  // (see the plan file's own explicitly-accepted scoping note, /home/h-i/
  // .claude/plans/joyful-skipping-giraffe.md's §4 table). Returns false
  // (and does NOT modify the live volume) if the file write failed;
  // otherwise performs the same live crop crop_volume() does and returns
  // true.
  virtual bool cutout_volume(glm::vec3 center, SculptBrushShape shape_type,
                             glm::vec4 params, f32 smoothness,
                             const std::string &removed_path) = 0;

  // M4 (Cutout): opens a snapshot file export_sculpt_cutout()/cutout_
  // volume() wrote and injects every brick in it into the LIVE volume --
  // see VulkanRaymarchShader::load_crop_outside_file()'s own comment.
  // ADDITIVE onto whatever the volume currently holds (same documented
  // limitation the ordinary .stamps Load path already carries -- there is
  // still no "clear the chunked field" API). Returns false if the file
  // couldn't be read or is malformed.
  virtual bool load_cutout_file(const std::string &path) = 0;

  // Post-process toggles/params -- see VulkanRaymarchShader::
  // set_bloom_enabled()/set_vignette_enabled()/set_pixelation_enabled()/
  // set_pixelation_block_size() for exact semantics. All are just push
  // constants read by Builtin.PostComposite.comp.glsl, so each takes
  // effect the very next frame with no rebake.
  virtual void set_bloom_enabled(b8 enabled) = 0;
  virtual void set_vignette_enabled(b8 enabled) = 0;
  virtual void set_pixelation_enabled(b8 enabled) = 0;
  virtual void set_pixelation_block_size(u32 block_size) = 0;

  // Post-process tuning knobs -- see VulkanRaymarchShader::
  // set_bloom_threshold()/set_bloom_intensity()/set_vignette_strength()/
  // set_vignette_radius() for exact semantics/clamping. Same push-constant
  // mechanics as the toggles above.
  virtual void set_bloom_threshold(f32 threshold) = 0;
  virtual void set_bloom_intensity(f32 intensity) = 0;
  virtual void set_vignette_strength(f32 strength) = 0;
  virtual void set_vignette_radius(f32 radius) = 0;

  // Scales pass 3/4's internal render resolution relative to the actual
  // framebuffer size (upscaled back up via a linear blit) -- see
  // VulkanRaymarchShader::set_render_scale(). The lever for trading
  // sharpness for frame rate on a large/fullscreen window, since every
  // pass here costs O(pixel count). Takes effect immediately (a device-
  // idle wait to safely recreate render targets and rewrite the
  // descriptor bindings pointing at them), so prefer calling it for a
  // deliberate quality/perf change, not every frame. Clamped to (0, 1].
  virtual void set_render_scale(f32 scale) = 0;

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

  // Enables/disables floating-origin recentring: once enabled, begin_frame()
  // periodically shifts every render-space position (registered geometry/
  // lights/volumetrics, plus this backend's own camera copy) back near the
  // origin as the camera drifts away, to avoid float-precision loss far from
  // (0,0,0) -- see VulkanRendererBackend::maybe_recenter_origin(). Off by
  // default so nothing currently working changes; a game opts in only if it
  // actually free-roams far enough to need it.
  virtual void set_floating_origin_enabled(b8 enabled) = 0;

  // Returns the render-space shift (world units) any floating-origin
  // recenter has applied since the last call, then clears it back to zero
  // -- (0,0,0) if none happened. The game owns and drives its own Camera
  // (see set_camera()'s comment), so a recenter that only shifts this
  // backend's transient copy would be silently overwritten the next time
  // the game calls set_camera(). A game that opts into floating origin must
  // call this once per frame, before computing/using its own camera
  // position, and apply the same shift to every camera-position state it
  // owns (see Camera::shift()) to stay in the same render-space frame as
  // the geometry.
  virtual glm::vec3 consume_origin_shift() = 0;

  virtual b8 end_frame(f32 delta_time) = 0;

  u64 frame_number = 0;
};

std::unique_ptr<RendererBackend>
renderer_backend_create(renderer_backend_type type, PlatformLayer &plat_state);
