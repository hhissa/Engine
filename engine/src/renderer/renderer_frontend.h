#pragma once

#include "camera.h"
#include "renderer_types.inl"

#include <glm/glm.hpp>
#include <string>
#include <string_view>

struct static_mesh_data;
class PlatformLayer;
KAPI b8 renderer_initialize(const std::string_view, struct PlatformLayer &platform);
KAPI void renderer_shutdown();

KAPI void renderer_on_resized(u16 width, u16 height);

// Sets the camera used for the next frame's render. The game owns and
// drives the Camera (position/orientation from input); the renderer just
// forwards whatever it's given to the backend.
KAPI void renderer_set_camera(const Camera &camera);

// Queues one line of text to be drawn this frame (see
// RendererBackend::draw_text) -- call from the game's render(), not once
// at startup, since queued draws don't persist to the next frame. scale
// enlarges the glyphs about position (the baseline origin) -- the font's
// one baked atlas stretched, so large sizes come out a little soft.
KAPI void renderer_draw_text(std::string_view text, glm::vec2 position,
                       glm::vec4 colour, f32 scale = 1.0f);

// Queues the engine's built-in demo UI texture to be drawn at
// position/size (screen pixels) this frame (see
// RendererBackend::draw_ui_quad).
KAPI void renderer_draw_ui_quad(glm::vec2 position, glm::vec2 size);

// Queues a solid-colour line segment (screen pixels, fixed thickness) to
// be drawn this frame (see RendererBackend::draw_line) -- same per-frame
// queueing discipline as renderer_draw_text()/renderer_draw_ui_quad()
// above.
KAPI void renderer_draw_line(glm::vec2 start, glm::vec2 end, glm::vec4 colour);

// Queues a solid-colour, axis-aligned rectangle (screen pixels, (0,0) at
// the top-left) to be drawn this frame -- see RendererBackend::
// draw_solid_quad() for exactly why this exists (a flat UI box in an
// arbitrary colour, e.g. a censor bar) and why it's guaranteed to draw on
// top of everything else queued this frame. Same per-frame queueing
// discipline as renderer_draw_text()/renderer_draw_ui_quad()/
// renderer_draw_line() above -- call it every frame you want the box
// visible, from the game's render().
KAPI void renderer_draw_solid_quad(glm::vec2 position, glm::vec2 size,
                             glm::vec4 colour);

// renderer_draw_solid_quad()'s opposite end of the draw order: drawn over
// the scene but UNDER every text/quad/line queued this frame -- see
// RendererBackend::draw_backdrop_quad(). E.g. a black card for title text
// to sit on without unloading the scene behind it.
KAPI void renderer_draw_backdrop_quad(glm::vec2 position, glm::vec2 size,
                                glm::vec4 colour);

// Queues an image -- a path under assets/ with its extension, e.g.
// "photos/cloud_mel.jpg" -- stretched over position/size (screen pixels,
// top-left origin) this frame. Same layer and ordering as
// renderer_draw_backdrop_quad(): over the scene, under all text.
KAPI void renderer_draw_image(std::string_view path, glm::vec2 position,
                         glm::vec2 size);

// Loads an image ahead of its first renderer_draw_image() and returns its
// size in pixels ((0, 0) if it can't be loaded) -- e.g. to fit it to the
// screen without stretching it.
KAPI glm::vec2 renderer_preload_image(std::string_view path);

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
KAPI void renderer_draw_camera_overlay(glm::vec2 position, glm::vec2 size,
                                  f32 blink_time_seconds,
                                  std::string_view caption = {});

// The scene-authoring API that used to live here (SceneRef,
// renderer_load_scene()/renderer_remove_scene()/renderer_clear_scenes()/
// renderer_reconcile_scene()/renderer_set_selected_primitive()/
// renderer_get_primitive_gpu_index()) was removed in the M0 CSG deletion
// pass (see /home/h-i/.claude/plans/joyful-skipping-giraffe.md) along with
// the CSG primitive scene graph it authored -- a future sculpt-volume
// milestone gets its own, differently-shaped load/save API rather than
// reusing this one.

// Shows/hides the reference grid: the ground plane (y=0) subdivided into
// 1-unit cells (heavier lines every 10 units, world X/Z axes tinted),
// drawn by the raymarch pass as a modelling aid. Hidden by default so
// games never render it -- editor tooling (tools/sdf_editor) opts in
// after renderer_initialize(). Like the selection outline it's cheap and
// immediate (a push constant, not a rebake), so it can be toggled every
// frame if needed.
KAPI void renderer_set_grid_visible(b8 visible);

// Phase 4: switches the render pass's primary hit-test/normals/contact-AO/
// shadows from the fixed-cube field every caller has always used onto the
// chunked/streamed field instead (see engine/src/systems/chunk_streaming_
// manager.h and VulkanRaymarchShader::update_streaming()) -- just a push
// constant, takes effect the very next frame, no rebake needed. Off by
// default, so tools/sdf_editor and games/SH are completely unaffected
// unless a caller explicitly opts in. A caller that enables this should
// also be driving update_streaming() every frame with its camera's current
// position (see VulkanRendererBackend::begin_frame()) -- enabling this
// without that just samples whatever chunks happen to already be resident
// (likely none, since nothing streams in on its own).
KAPI void renderer_set_chunked_field_enabled(b8 enabled);

// renderer_set_dynamic_primitive()/renderer_request_cache_prewarm()/
// renderer_set_cache_prewarm_on_load()/renderer_streaming_backlog()/
// renderer_set_primitive_transform() -- the CSG-primitive-specific disk
// cache/dynamic-drag API -- were removed in the M0 CSG deletion pass (see
// /home/h-i/.claude/plans/joyful-skipping-giraffe.md).

// Selects how the chunked field's baked surface-point cloud drives primary
// visibility -- the Dreams-style point-splatting technique. See
// RendererSplatMode (renderer_types.inl) for what each mode does; takes
// effect the very next frame, no rebake needed. Only meaningful alongside
// renderer_set_chunked_field_enabled(true) -- the fixed-cube field bakes no
// point cloud, so nothing splats there whatever this is set to. Defaults to
// Prime, which cannot change the image (only how much empty space each ray
// has to march); Visibility actually shades the splats and so is opt-in.
KAPI void renderer_set_splat_mode(RendererSplatMode mode);

// Enables or disables temporal anti-aliasing (on by default). See
// RendererBackend::set_taa_enabled().
KAPI void renderer_set_taa_enabled(b8 enabled);

// Enables or disables the imperfect shadow maps for local point lights (on
// by default). See RendererBackend::set_ism_enabled().
KAPI void renderer_set_ism_enabled(b8 enabled);

// Enables or disables stochastic ambient occlusion (on by default). See
// RendererBackend::set_ao_enabled().
KAPI void renderer_set_ao_enabled(b8 enabled);

// Debug: logs what the baked field holds at a world point. See
// RendererBackend::debug_probe_field().
KAPI void renderer_debug_probe_field(glm::vec3 world_pos);

// M5 (Finishes) + M6 (Flecks): configures material slot `slot` (must be >=
// 2 -- 0 and 1 are reserved for the default/Spraypaint materials, see
// VulkanRaymarchShader::ensure_default_sculpt_material()) with a base
// colour, a dielectric roughness (0 = mirror-sharp, 1 = fully diffuse),
// metallic (0 = dielectric, 1 = metal -- tints the specular reflection by
// `colour` and removes the diffuse substrate, see Builtin.DeferredShade.
// comp.glsl's final combine), `waxy` (adds a second, fixed-low-roughness
// specular highlight on top -- see the shader's WAXY_LOBE_* constants),
// and a Fleck: `fleck_pattern` (0-18, one of Square/Circle/Triangle/
// Hexagon/Dots/Straight/Heavy/Hatching/Streaky/Scribble/Veins/Splat/
// Cubist/Impressionist/Expressive/Baroque/Impasto/Clay/Fuzzy, in that order
// -- see Builtin.FleckPattern.inc.glsl's own header comment for the family
// each maps to), `fleck_strength` (0 = no Fleck at all, regardless of
// `fleck_pattern`; clamped to [0, 1] otherwise) and `fleck_scale` (world
// units per pattern cycle -- larger reads as coarser/bigger features). This
// is how tools/modelling_software_v2's Finish presets (Default/Plastic/
// Shiny/Rough/Metal/Wax/Shiny Metal/Shiny Wax/Rough Metal/Rough Wax/Clay)
// and its Fleck presets (the 19 named above) are both actually realized:
// ONE freshly allocated slot carries both, via one call here -- not two independent
// allocation paths. A no-op (logs a warning) if slot is out of
// [2, kMaxScenePrimitives) -- see VulkanRaymarchShader::set_material()'s
// own comment for the full packing. Cheap and synchronous (a direct
// host-visible buffer write, NOT device-stalling like the sculpt-stamp
// family below) -- call it once per Finish/Fleck/colour pick, before the
// next renderer_sculpt_stamp_shape() call that references `slot`, not
// every frame. Pass fleck_strength 0.0f (fleck_pattern/fleck_scale are
// then ignored -- 0/1.0f is the conventional "don't care" value) for a
// slot that should carry a Finish with no Fleck, exactly what every M5
// Finish preset still does.
KAPI void renderer_sculpt_set_material(i32 slot, glm::vec3 colour,
                                       f32 roughness, f32 metallic,
                                       bool waxy, i32 fleck_pattern,
                                       f32 fleck_strength, f32 fleck_scale);

// Registers one standing directional/point light for the raymarch render
// pass's per-pixel lighting loop -- e.g. renderer_add_directional_light(
// glm::vec3(-0.4f, -0.8f, -0.3f), glm::vec3(1.0f, 0.96f, 0.9f), 2.5f) for a
// simple warm-white "sun". direction need not be pre-normalized. There is
// currently no live path from GeometrySystem::acquire_light() to the GPU
// (that bridge, rebuild_static_scene(), was deleted in the M0 CSG pass) --
// this writes the render pass's light buffer directly instead, the same
// "direct host-visible buffer write" idiom renderer_sculpt_set_material()
// above already uses. Returns the assigned light slot (0-based, in
// registration order), or -1 if the fixed 128-light cap is already full
// (logs a warning, otherwise a no-op). Cheap, synchronous, NOT device-
// stalling -- call it once per standing light a tool wants (e.g. once at
// startup for a default "sun"), not every frame. With zero lights
// registered (the engine's own default), every surface renders governed
// only by ambient_ (a flat 0.15 factor) and any skybox reflection -- see
// renderer_enable_sky_box() below -- which is why a tool that wants a
// visibly lit scene needs at least one of these.
KAPI i32 renderer_add_directional_light(glm::vec3 direction, glm::vec3 colour,
                                        f32 intensity);
KAPI i32 renderer_add_point_light(glm::vec3 position, glm::vec3 colour,
                                  f32 intensity);

// M2a: stamps one analytic brush shape, blended via union or subtraction
// with a given smoothness, directly into the chunked field and
// regenerates its splat points -- see RendererBackend::stamp_shape()/
// VulkanRaymarchShader::stamp_shape() for the full params-per-shape
// layout. Only meaningful alongside renderer_set_chunked_field_enabled(true)
// (and, to actually see it, renderer_set_splat_mode() left at its Prime/
// Visibility default -- the splat pipeline is what reads the points this
// call generates). Synchronous and device-stalling -- an authoring-time
// call, not a per-frame one. Brush ROTATION is out of scope for M2a.
// M3 (Looseness): looseness is pure CPU-side bookkeeping (saved/redoable,
// see SculptStamp::looseness's own comment) -- it has no GPU-side effect
// yet and does not change how this stamp is blended or shaded. Defaults to
// 0 (clean/uniform), matching every call site before M3 added the
// parameter.
KAPI void renderer_sculpt_stamp_shape(glm::vec3 center,
                                 SculptBrushShape shape_type,
                                 glm::vec4 params, f32 smoothness,
                                 SculptBrushOp shape_op,
                                 i32 material_slot = 0,
                                 f32 looseness = 0.0f);

// M1a back-compat convenience wrapper, kept so testbed's own M1a smoke test
// (TestbedGame::initialize(), which only ever wants a hard-union sphere)
// doesn't need updating for M2a's generalization -- thin sugar over
// renderer_sculpt_stamp_shape(center, SculptBrushShape::Sphere,
// {radius,0,0,0}, /*smoothness=*/0.0f, SculptBrushOp::Union,
// material_slot). New call sites (tools/modelling_software_v2) should
// prefer renderer_sculpt_stamp_shape() directly.
KAPI void renderer_sculpt_stamp_sphere(glm::vec3 center, f32 radius,
                                  i32 material_slot = 0);

// M2b: reverts the most recently applied stamp -- see RendererBackend::
// undo_stamp()/VulkanRaymarchShader::undo_stamp() for the full mechanics
// (GPU voxels/material restored, or a freshly-allocated brick freed, plus
// the matching SculptSystem bookkeeping record dropped). Synchronous and
// device-stalling, like renderer_sculpt_stamp_shape() -- an authoring-time
// call, not a per-frame one. Returns false if there is nothing left to
// undo (nothing has been stamped yet, or this session's undo depth cap has
// already been exhausted -- see kMaxSculptUndoDepth).
KAPI bool renderer_sculpt_undo();

// M2b: re-applies the most recently undone stamp -- see RendererBackend::
// redo_stamp(). Returns false if there is nothing to redo (nothing has
// been undone, or a fresh renderer_sculpt_stamp_shape() call since the
// last undo cleared the redo history).
KAPI bool renderer_sculpt_redo();

// M3c: Spraypaint -- a paint-ONLY brush. Accumulates `opacity_per_pass`
// (clamped into [0,1], and added -- not overwritten -- onto whatever
// opacity a voxel already holds, also clamped) into a per-voxel paint
// buffer for every voxel INSIDE the brush shape's volume (see Builtin.
// SculptSpraypaint.comp.glsl's PAINT_THRESHOLD comment for exactly what
// "inside" covers -- the whole enclosed volume, not just a thin shell
// around the existing surface, which is what lets it "spray through"
// geometry instead of only tinting the side facing the brush), in every
// touched cell that already has a brick -- cells with none (nothing
// stamped there yet) are silently skipped, same as a chunk nothing has
// ever been stamped into: there is no surface, and nothing to paint.
// Never touches chunk_bricks[]/chunk_brick_primitive[] (the SDF distance/
// per-brick-material data) at all -- see RendererBackend::spray_paint()/
// VulkanRaymarchShader::spray_paint() for the full mechanics. `params`
// uses the exact same per-shape layout table as renderer_sculpt_stamp_
// shape() (see its own comment) -- there is no `smoothness`/`op`/
// `material_slot` parameter here: paint has no geometry blend to smooth,
// no union/subtraction concept, and always targets one fixed "paint
// colour" material slot (see VulkanRaymarchShader::
// ensure_default_sculpt_material()'s own comment -- no colour-picker UI
// exists yet, out of scope for this milestone). Synchronous and device-
// stalling, like renderer_sculpt_stamp_shape() -- an authoring-time call,
// not a per-frame one. Returns how many coarse cells were actually
// touched (0 if the shape's bounding radius is non-positive, or if the
// brush's whole AABB falls over chunks nothing has ever been stamped
// into -- spraying over empty space is a correct, silent no-op).
//
// Known limitation: a Spraypaint stroke is NOT currently undoable/
// redoable -- renderer_sculpt_undo()/renderer_sculpt_redo() only ever
// affect geometry stamps (SculptSystem's own bookkeeping, which this call
// deliberately bypasses -- see VulkanRendererBackend::spray_paint()'s own
// comment). A future milestone wanting Spraypaint undo should extend
// VulkanRaymarchShader's undo_stack_ with an optional per-voxel paint
// snapshot, mirroring SculptUndoCell::old_voxels' own shape.
KAPI i32 renderer_sculpt_spray(glm::vec3 center, SculptBrushShape shape_type,
                               glm::vec4 params, f32 opacity_per_pass);

// M7 polish: the live brush-cursor "ghost" preview -- shows the ACTUAL
// shaded material (colour, roughness/metallic/waxy Finish, Fleck pattern)
// a renderer_sculpt_stamp_shape() call at `center` with these exact
// parameters would place, replacing the plain wireframe outline tools/
// modelling_software_v2's scene_viewport.cpp drew before this existed. See
// RendererBackend::preview_brush()/VulkanRaymarchShader::set_brush_
// preview() for the full mechanics: one analytic shape is sphere-traced
// per pixel and shaded through the SAME lighting path every real surface
// uses, then blended as a translucent ghost over whatever that pixel
// already rendered. `shape_type`/`params`/`op`/`material_slot` use the
// exact same per-shape layout and enum values as renderer_sculpt_stamp_
// shape() -- the preview can never drift from what a real stamp right now
// would place. Cheap, synchronous, and NOT device-stalling (a direct
// host-visible buffer write) -- unlike every sculpt-stamp-family call
// above, this one is meant to be called every frame the preview should be
// visible (tools/modelling_software_v2's SceneViewport::tick(), alongside
// its own wireframe-line bookkeeping), not just on an authoring-time
// action. Pass `enabled=false` the instant the preview should stop
// showing (pointer left the viewport, a drag started, etc.) -- center/
// params/op/material_slot are then ignored, and the shader's own first
// read of the uniform skips the trace entirely, so the false path costs
// one float compare per pixel, not a wasted march.
KAPI void renderer_sculpt_preview_brush(bool enabled, glm::vec3 center,
                                        SculptBrushShape shape_type,
                                        glm::vec4 params, SculptBrushOp op,
                                        i32 material_slot);

// M4b: deletes the stamp at `index` (0 = oldest, matching the order
// renderer_sculpt_stamp_shape() calls landed in) WITHOUT discarding
// anything stamped before or after it -- see RendererBackend::delete_edit()/
// VulkanRendererBackend::delete_edit() for the full checkpoint-restore-and-
// replay mechanics (VulkanRaymarchShader::restore_sculpt_checkpoint_before()/
// capture_sculpt_checkpoint()). Synchronous and device-stalling, like every
// other sculpt-family call -- an authoring-time call, not a per-frame one.
// Returns false if index is out of range for however many stamps currently
// exist.
KAPI bool renderer_sculpt_delete_edit(i32 index);

// M4c (Move-of-an-arbitrary-edit): re-centers the stamp at `index` (0 =
// oldest, matching the order renderer_sculpt_stamp_shape() calls landed in)
// to new_center, WITHOUT changing its shape/params/smoothness/op/
// material_slot/looseness and WITHOUT changing its position in the stamp
// list -- see RendererBackend::move_edit()/VulkanRendererBackend::
// move_edit() for the full restore-then-replay mechanics, which reuse
// exactly the same checkpoint/replay engine renderer_sculpt_delete_edit()
// does (VulkanRaymarchShader::restore_sculpt_checkpoint_before()/
// capture_sculpt_checkpoint()), just re-stamping `index` at the new center
// instead of skipping it. Synchronous and device-stalling, like every other
// sculpt-family call -- an authoring-time call, not a per-frame one.
// Returns false if index is out of range for however many stamps currently
// exist.
//
// Deliberately additive alongside the existing tail-only Move (driven by
// SceneViewport's Move mode, which undoes the most recent stamp and
// re-stamps it via renderer_sculpt_stamp_shape() -- no checkpoint/replay
// involved at all, since there is nothing after it to reconstruct) -- that
// path is untouched and remains the cheap, common-case way to move
// whatever was JUST stamped. This call is for moving an ARBITRARY past
// edit, selected from the tool's edit-history list.
KAPI bool renderer_sculpt_move_edit(i32 index, glm::vec3 new_center);

// M4 (Crop): a whole-volume boolean filter -- keeps only the geometry
// currently inside shape_type/params/center (same per-shape params table as
// renderer_sculpt_stamp_shape()), discarding everything outside. Unlike
// every other sculpt-family call, this is NOT scoped to the shape's own
// AABB -- see RendererBackend::crop_volume()/VulkanRaymarchShader::
// crop_volume() for the whole-volume enumeration and the keep-inside SDF
// identity. Synchronous and device-stalling, like every other sculpt-
// family call.
//
// Known limitation (documented, not a bug): NOT currently undoable/
// redoable, and NOT integrated into SculptVolume/the checkpoint-replay
// engine -- same documented, accepted scope Spraypaint shipped with (see
// VulkanRaymarchShader::crop_volume()'s own header comment). A later
// renderer_sculpt_delete_edit()/renderer_sculpt_move_edit() targeting an
// edit that landed before a Crop ran will reconstruct that edit WITHOUT
// reapplying the crop. Returns how many resident bricks were actually
// touched (0 if the volume is empty, or the shape's bounding radius is
// non-positive).
KAPI i32 renderer_sculpt_crop(glm::vec3 center, SculptBrushShape shape_type,
                              glm::vec4 params, f32 smoothness);

// M4 (Cutout): the same live effect as renderer_sculpt_crop() above, but
// FIRST captures everything the crop is about to discard and writes it to
// `removed_path` as a standalone snapshot file (NOT the tool's .stamps
// format) a fresh tool session can open separately via
// renderer_sculpt_load_cutout() -- see RendererBackend::cutout_volume()'s
// own comment for why a second FILE, not a second live volume, is this
// pass's accepted scope for "preserved as a new separate sculpture".
// Returns false (and leaves the live volume untouched) if the file write
// failed; otherwise performs the live crop and returns true.
KAPI bool renderer_sculpt_cutout(glm::vec3 center, SculptBrushShape shape_type,
                                 glm::vec4 params, f32 smoothness,
                                 const std::string &removed_path);

// M4 (Cutout): opens a snapshot file renderer_sculpt_cutout() wrote and
// injects every brick in it into the LIVE volume -- see RendererBackend::
// load_cutout_file(). ADDITIVE onto whatever the volume currently holds,
// same documented limitation the ordinary .stamps Load path already
// carries. Returns false if the file couldn't be read or is malformed.
KAPI bool renderer_sculpt_load_cutout(const std::string &path);

// Enables/disables the bloom post-process -- a soft glow bloomed off
// bright/emissive surfaces (see Material::emissive_intensity), added back
// on top of the scene. On by default, subtly; call with false to disable
// entirely (e.g. for a performance-constrained target, or a game whose
// look doesn't want it).
KAPI void renderer_set_bloom_enabled(b8 enabled);

// Enables/disables the vignette post-process -- a smooth radial darkening
// toward the screen edges. On by default, subtly.
KAPI void renderer_set_vignette_enabled(b8 enabled);

// Enables/disables the pixelation post-process -- quantizes the screen
// into flat-coloured blocks (the classic "pixel art" mosaic look), except
// for any primitive whose material opted out via Material::
// pixelation_exempt (see the .kmt format), which stays crisp/full-
// resolution regardless. Off by default -- it's a deliberate stylistic
// choice, not something every game using this engine would want without
// asking.
KAPI void renderer_set_pixelation_enabled(b8 enabled);

// Sets the pixelation block size (edge length, in full-resolution screen
// pixels) -- larger blocks read as chunkier/lower-fidelity. Has no visible
// effect unless pixelation is also enabled (see
// renderer_set_pixelation_enabled() above). Defaults to 6.
KAPI void renderer_set_pixelation_block_size(u32 block_size);

// Luminance above which a pixel starts contributing to bloom -- lower
// reads as "more of the scene blooms," not just the brightest highlights.
// Has no visible effect while bloom is disabled (see
// renderer_set_bloom_enabled() above). Clamped to non-negative. Defaults
// to 0.85.
KAPI void renderer_set_bloom_threshold(f32 threshold);

// How strongly the blurred bright-pass adds back on top of the scene --
// higher reads as a stronger glow. Same mechanics as
// renderer_set_bloom_threshold() above. Clamped to non-negative. Defaults
// to 0.35.
KAPI void renderer_set_bloom_intensity(f32 intensity);

// How much the vignette darkens the screen edges -- 0 = no visible effect
// even while vignette is enabled, 1 = fully black at the furthest corner.
// Same mechanics as renderer_set_bloom_threshold() above. Clamped to
// [0, 1]. Defaults to 0.35.
KAPI void renderer_set_vignette_strength(f32 strength);

// Normalized distance from screen center (0 = center, 1 = a full-height/
// width edge, sqrt(2) = the furthest corner) where the vignette's falloff
// begins -- smaller pulls the darkening in toward the center, larger
// pushes it out toward the corners. Same mechanics as
// renderer_set_bloom_threshold() above. Clamped to [0, sqrt(2)]. Defaults
// to 0.55.
KAPI void renderer_set_vignette_radius(f32 radius);

// Scales the raymarch/post-process passes' internal render resolution
// relative to the actual framebuffer size -- e.g. 0.75 renders at 75%
// linear resolution (~56% the pixel count) and upscales into the
// framebuffer via a linear blit. Every one of those passes costs O(pixel
// count), so this is the lever for trading sharpness for frame rate on a
// large or fullscreen window instead of paying full native-resolution
// cost unconditionally. Takes effect immediately. Clamped to (0, 1].
// Defaults to 1.0 (native resolution, unchanged from before this existed).
KAPI void renderer_set_render_scale(f32 scale);

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
KAPI void renderer_set_font(std::string_view name, f32 pixel_height);

// Enables an equirectangular (lat/long, NOT a 6-face cubemap) skybox behind
// all scene geometry, replacing the flat two-colour background gradient
// this engine uses when none is set -- e.g.
// renderer_enable_sky_box("sunset_sky") to use assets/textures/
// sunset_sky.png. texture_name resolves through TextureSystem exactly like
// every other texture reference in this engine (Material::diffuse_map_name,
// etc.) -- a name, not an arbitrary filesystem path. Not cheap (a device-
// idle wait to safely rewrite a descriptor binding), so call it for a
// deliberate skybox change (e.g. once per level/scene), not every frame.
KAPI void renderer_enable_sky_box(std::string_view texture_name);

// Disables the skybox, falling back to the flat gradient background.
KAPI void renderer_disable_sky_box();

// Enables/disables floating-origin recentring -- see
// RendererBackend::set_floating_origin_enabled() for exact semantics. Off by
// default; a game that free-roams far enough from (0,0,0) to risk float
// precision loss opts in explicitly. A game that enables this MUST call
// renderer_consume_origin_shift() once per frame (see below) or its own
// camera will drift out of sync with the (correctly recentred) scene.
KAPI void renderer_set_floating_origin_enabled(b8 enabled);

// Returns the render-space shift (world units) a floating-origin recenter
// has applied since the last call, then clears it -- (0,0,0) if none
// happened (including whenever floating origin is disabled). Call this once
// per frame, before using/updating any Camera position the game itself
// owns (e.g. at the very start of the game's update()), and apply the
// returned delta to every such Camera via Camera::shift() -- see
// RendererBackend::consume_origin_shift()'s comment for why this can't just
// happen automatically.
KAPI glm::vec3 renderer_consume_origin_shift();

KAPI b8 renderer_draw_frame(struct render_packet *packet);
