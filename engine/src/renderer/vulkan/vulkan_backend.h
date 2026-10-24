#pragma once
#include "../../core/application.h"
#include "../../systems/geometry_system.h"
#include "../../systems/sculpt_system.h"
#include "../renderer_backend.h"
#include "vulkan_commandbuffer.h"
#include "vulkan_fence.h"
#include "vulkan_framebuffer.h"
#include "vulkan_types.inl"

#include <algorithm>
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>

class PlatformLayer;

class VulkanRendererBackend final : public RendererBackend {
public:
  explicit VulkanRendererBackend(PlatformLayer &plat_state);
  ~VulkanRendererBackend();

  b8 initialize(std::string_view application_name,
                PlatformLayer &plat_state) override;
  void shutdown() override;
  void on_resized(u16 width, u16 height) override;
  b8 begin_frame(f32 delta_time) override;
  void set_camera(const Camera &camera) override;
  void draw_text(std::string_view text, glm::vec2 position,
                glm::vec4 colour, f32 scale) override;
  void draw_ui_quad(glm::vec2 position, glm::vec2 size) override;
  void draw_line(glm::vec2 start, glm::vec2 end, glm::vec4 colour) override;
  void draw_solid_quad(glm::vec2 position, glm::vec2 size,
                      glm::vec4 colour) override;
  void draw_backdrop_quad(glm::vec2 position, glm::vec2 size,
                         glm::vec4 colour) override;
  void draw_image(std::string_view path, glm::vec2 position,
                  glm::vec2 size, glm::vec4 tint) override;
  glm::vec2 preload_image(std::string_view path) override;
  void set_grid_visible(b8 visible) override;
  void set_chunked_field_enabled(b8 enabled) override;
  void set_splat_mode(RendererSplatMode mode) override;
  void set_taa_enabled(b8 enabled) override;
  void set_ism_enabled(b8 enabled) override;
  void set_ao_enabled(b8 enabled) override;
  void debug_probe_field(glm::vec3 world_pos) override;
  void set_material(i32 slot, glm::vec3 colour, f32 roughness, f32 metallic,
                    bool waxy) override;
  i32 add_directional_light(glm::vec3 direction, glm::vec3 colour,
                            f32 intensity) override;
  void set_point_light(i32 index, glm::vec3 position, glm::vec3 colour,
                       f32 intensity, i32 source_material) override;
  i32 add_point_light(glm::vec3 position, glm::vec3 colour,
                      f32 intensity) override;
  void stamp_shape(glm::vec3 center, SculptBrushShape shape_type,
                   glm::vec4 params, f32 smoothness, SculptBrushOp shape_op,
                   i32 material_slot = 0, f32 looseness = 0.0f,
                   glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f)) override;
  bool undo_stamp() override;
  bool redo_stamp() override;
  i32 spray_paint(glm::vec3 center, SculptBrushShape shape_type,
                  glm::vec4 params, f32 opacity_per_pass) override;
  void preview_brush(bool enabled, glm::vec3 center,
                     SculptBrushShape shape_type, glm::vec4 params,
                     SculptBrushOp op, i32 material_slot,
                     glm::quat rotation) override;
  bool delete_edit(i32 index) override;
  bool move_edit(i32 index, glm::vec3 new_center) override;
  bool sculpt_bounds(glm::vec3 *out_min, glm::vec3 *out_max) const override;
  i32 begin_object() override;
  i32 current_object() const override { return current_object_id_; }
  void set_current_object(i32 object_id) override {
    current_object_id_ = object_id;
    next_object_id_ = std::max(next_object_id_, object_id + 1);
  }
  i32 pick_object(glm::vec3 point, f32 max_distance) const override;
  i32 raycast_object(glm::vec3 origin, glm::vec3 direction, f32 max_t,
                     f32 *out_t) const override;
  bool object_bounds(i32 object_id, glm::vec3 *out_min,
                     glm::vec3 *out_max) const override;
  bool transform_object(i32 object_id, glm::vec3 pivot, glm::quat rotation,
                        glm::vec3 translation, f32 scale = 1.0f) override;
  bool transform_objects(const i32 *object_ids, i32 count, glm::vec3 pivot,
                         glm::quat rotation, glm::vec3 translation,
                         f32 scale = 1.0f) override;
  bool delete_objects(const i32 *object_ids, i32 count) override;
  void set_outlined_objects(const i32 *object_ids, i32 count,
                            glm::vec4 colour,
                            const i32 *outline_groups) override;
  void set_scope_objects(const i32 *object_ids, i32 count) override;
  bool set_object_material(const i32 *object_ids, i32 count,
                           i32 slot) override;
  void set_material_glow(i32 slot, f32 glow) override;
  void set_material_effects(i32 slot, f32 boil, f32 flow, f32 wave,
                            f32 evaporate, f32 throb) override;
  void set_material_flecks(i32 slot, f32 looseness, i32 style, f32 comb,
                           f32 ruffle, f32 impasto, f32 neatness) override;
  bool set_object_looseness(const i32 *object_ids, i32 count,
                            f32 looseness) override;
  i32 object_ids(i32 *out_ids, i32 capacity) const override;
  i32 object_stamps(i32 object_id, SculptStampInfo *out_stamps,
                    i32 capacity) const override;
  i32 raycast_objects(glm::vec3 origin, glm::vec3 direction, f32 max_t,
                      const i32 *object_ids, i32 count, f32 *out_t,
                      i32 stamp_limit = -1) const override;
  i32 stamp_count() const override;
  void set_preview_smoothness(f32 k) override;
  void set_object_weld_keys(const i32 *object_ids, const i32 *keys,
                            i32 count) override;
  i32 clone_object(i32 object_id) override;
  bool add_stamps(i32 object_id, const SculptStampInfo *stamps,
                  i32 count) override;
  bool add_stamps_to_objects(const i32 *object_ids,
                             const SculptStampInfo *stamps,
                             i32 count) override;
  bool remap_object_materials(const i32 *object_ids, i32 count,
                              const i32 *from_slots, const i32 *to_slots,
                              i32 pairs) override;
  bool object_anchor(i32 object_id, glm::vec3 *out_center,
                     glm::quat *out_rotation) const override;
  i32 crop_volume(glm::vec3 center, SculptBrushShape shape_type,
                  glm::vec4 params, f32 smoothness) override;
  bool cutout_volume(glm::vec3 center, SculptBrushShape shape_type,
                     glm::vec4 params, f32 smoothness,
                     const std::string &removed_path) override;
  bool load_cutout_file(const std::string &path) override;
  void set_bloom_enabled(b8 enabled) override;
  void set_vignette_enabled(b8 enabled) override;
  void set_pixelation_enabled(b8 enabled) override;
  void set_pixelation_block_size(u32 block_size) override;
  void set_bloom_threshold(f32 threshold) override;
  void set_bloom_intensity(f32 intensity) override;
  void set_vignette_strength(f32 strength) override;
  void set_vignette_radius(f32 radius) override;
  void set_render_scale(f32 scale) override;
  void set_font(std::string_view name, f32 pixel_height) override;
  void set_skybox(std::string_view texture_name) override;
  void disable_skybox() override;
  void set_floating_origin_enabled(b8 enabled) override;
  glm::vec3 consume_origin_shift() override;
  b8 end_frame(f32 delta_time) override;

private:
  struct TextDrawRequest {
    std::string text;
    glm::vec2 position;
    glm::vec4 colour;
    f32 scale;
  };
  struct UiQuadDrawRequest {
    glm::vec2 position;
    glm::vec2 size;
  };
  // A draw_image() request. Images share the backdrop queue's place in
  // the UI pass and its ordering, so this rides in BackdropDrawRequest.
  struct BackdropDrawRequest {
    std::string image_path; // empty: a solid quad in colour
    glm::vec2 position;
    glm::vec2 size;
    glm::vec4 colour;
  };
  struct LineDrawRequest {
    glm::vec2 start;
    glm::vec2 end;
    glm::vec4 colour;
  };
  struct SolidQuadDrawRequest {
    glm::vec2 position;
    glm::vec2 size;
    glm::vec4 colour;
  };

  // M2b: the common body of a "fresh" (user-driven) stamp AND of redo_
  // stamp()'s replay -- drives both VulkanRaymarchShader::stamp_shape()
  // (GPU work, which also captures its own undo snapshot -- see that
  // method's own comment) and sculpt_system_.apply_stamp() (CPU
  // bookkeeping), side by side, exactly like stamp_shape() always has.
  // Deliberately does NOT touch redo_stack_ -- stamp_shape() (the public,
  // RendererBackend-facing override) clears it after calling this (a fresh
  // edit invalidates redo history); redo_stamp() must NOT clear it (it
  // already popped the one entry it's replaying, and clearing here would
  // wipe out any further pending redos).
  void apply_stamp_and_record(glm::vec3 center, SculptBrushShape shape_type,
                              glm::vec4 params, f32 smoothness,
                              SculptBrushOp shape_op, i32 material_slot,
                              f32 looseness, glm::quat rotation,
                              i32 object_id);

  // After an edit that moved or removed stamps (and dropped undo history):
  // returns the chunk slots no stamp touches any more -- see
  // VulkanRaymarchShader::release_unused_sculpt_chunks().
  void release_unused_sculpt_chunks();
  // Hands the outlined objects' current stamps to the shading pass.
  void update_selection_outline();
  std::vector<i32> outlined_objects_;
  std::vector<i32> outline_keys_; // parallel to outlined_objects_
  std::vector<i32> scope_objects_;
  // set_object_weld_keys(): object -> weld key (> 0).
  std::unordered_map<i32, i32> weld_keys_;
  glm::vec4 outline_colour_{1.0f};
  bool collect_object_stamps(const i32 *object_ids, i32 count,
                             const char *caller,
                             std::vector<size_t> *out_members) const;

  // Shared by sculpt_bounds()/object_bounds(): bounds of the Union stamps
  // for which `include` returns true.
  template <typename Filter>
  bool union_stamp_bounds(Filter include, glm::vec3 *out_min,
                          glm::vec3 *out_max) const;

  VulkanContext context_{};
  PlatformLayer *plat_state_ = nullptr;

  // Set via set_camera() each frame (by the game, through the renderer
  // frontend), consumed by end_frame() when it calls the raymarch shader.
  Camera camera_;

  // M1a/M2a: the CPU-side record of every stamp stamp_shape() has applied --
  // see SculptSystem's own header comment for why the GPU-side chunk-
  // table-slot/brick bookkeeping is NOT duplicated here (it lives inside
  // VulkanRaymarchShader, which owns the actual buffers). Owned here
  // (rather than as a separate globally-registered system, the way
  // GeometrySystem/TextureSystem/MaterialSystem are) since M1a has exactly
  // one call site that needs it; promote it if a later milestone's tool
  // needs to reach it from somewhere else.
  SculptSystem sculpt_system_;
  // The object new stamps are recorded under -- see begin_object().
  i32 current_object_id_ = 0;
  i32 next_object_id_ = 1;

  // M2b: stamps popped by undo_stamp(), most-recently-undone last --
  // redo_stamp() pops from the back (LIFO), so consecutive redos restore
  // stamps in the reverse order they were undone, matching standard
  // undo/redo semantics. Cleared by stamp_shape() (never by redo_stamp()
  // itself -- see apply_stamp_and_record()'s own comment) whenever a fresh
  // stamp is applied, since diverging after an undo must discard forward
  // history. Naturally bounded by VulkanRaymarchShader::undo_stack_'s own
  // depth cap (kMaxSculptUndoDepth) -- this can never hold more entries
  // than that allows undoing in the first place, so no separate cap is
  // needed here.
  std::vector<SculptStamp> redo_stack_;

  // Queued via draw_text()/draw_ui_quad() (by the game, through the
  // renderer frontend) during its render() step; flushed into actual draw
  // calls and cleared by end_frame(), so nothing draws unless the game
  // queues it again next frame.
  std::vector<TextDrawRequest> queued_text_draws_;
  std::vector<UiQuadDrawRequest> queued_ui_quad_draws_;
  std::vector<LineDrawRequest> queued_line_draws_;
  std::vector<SolidQuadDrawRequest> queued_solid_quad_draws_;
  std::vector<BackdropDrawRequest> queued_backdrop_quad_draws_;

  // The scene-authoring members that used to live here (prewarm_on_load_,
  // loaded_scenes_/next_scene_handle_) were removed in the M0 CSG deletion
  // pass (see /home/h-i/.claude/plans/joyful-skipping-giraffe.md) along
  // with the CSG primitive-list API they backed. scene_dirty_ stays --
  // maybe_recenter_origin() (floating origin, CSG-independent) still needs
  // a deferred-rebake flag.
  //
  // Set by maybe_recenter_origin() instead of rebaking immediately -- a
  // recenter and any other same-frame mutation should coalesce into one
  // rebake, not pay for each separately. begin_frame() checks this and
  // rebakes at most once before the next frame actually draws.
  b8 scene_dirty_ = false;

  // Floating origin (see set_floating_origin_enabled()/consume_origin_shift()
  // and maybe_recenter_origin()). Off by default -- games/tools that never
  // opt in see zero behavior change. world_origin_offset_ is the absolute
  // correction accumulated so far: for anything that's been shifted,
  // absolute_position == render_space_position + world_origin_offset_,
  // exactly. Double precision even though every render-space position stays
  // f32 -- this offset alone accumulates across a whole session, so it's the
  // one quantity that actually needs to survive far more than f32's ~7
  // significant digits without itself drifting.
  b8 floating_origin_enabled_ = false;
  glm::dvec3 world_origin_offset_{0.0, 0.0, 0.0};
  // Recenter once the camera's render-space position drifts this far from
  // the origin. Derived from the baked field's finest voxel size (currently
  // COARSE_CELL_SIZE/BRICK_DIM = (2*16.0/128)/8 =~ 0.03 world units -- see
  // Builtin.SdfFieldConfig.inc.glsl) times a large safety margin: f32
  // mantissa precision only starts mattering once a position's magnitude
  // approaches a meaningful fraction of the largest value representable
  // with sub-voxel precision, which is comfortably past 1000 for a ~0.03
  // voxel -- kept well under that (not a round guess) so recenters stay
  // rare relative to how often a free-roaming camera might cross this
  // distance, while precision is never remotely at risk.
  f32 floating_origin_recenter_threshold_ = 96.0f;
  // Accumulates each recenter's render-space shift until a caller consumes
  // it (see consume_origin_shift()) -- the game must apply this same delta
  // to any Camera position it owns itself (see RendererBackend::
  // consume_origin_shift()'s comment for why this can't happen
  // automatically). Left to accumulate rather than assuming exactly one
  // recenter happens between consecutive consume_origin_shift() calls, so a
  // caller that skips a frame (or never calls it while the flag happens to
  // be off) can't silently lose part of a shift.
  glm::vec3 pending_origin_shift_{0.0f};

  // Shifts every render-space position (registered geometry/lights/
  // volumetrics, plus camera_) by -camera_.position() once the camera has
  // drifted floating_origin_recenter_threshold_ units from the render-space
  // origin, accumulating the opposite correction into world_origin_offset_
  // so nothing's true absolute position actually changes -- see the class
  // comment above camera_/world_origin_offset_. Called at the very top of
  // begin_frame(), before the scene_dirty_ check, so a recenter this frame
  // (which marks scene_dirty_ itself -- see its own comment) gets folded
  // into the very same rebake as any other pending scene mutation instead
  // of forcing a second one.
  void maybe_recenter_origin();

  u32 cached_framebuffer_width_ = 0;
  u32 cached_framebuffer_height_ = 0;

  std::vector<VulkanCommandBuffer> graphics_command_buffers_;

  // Owned fences, one per frame in flight.
  std::vector<VulkanFence> in_flight_fences_;
  // Non-owning pointers into in_flight_fences_, one per swapchain image.
  // Null means no frame is currently using that image.
  std::vector<VulkanFence *> images_in_flight_;

  // Backs main_renderpass -- color + depth, one per swapchain image.
  std::vector<VulkanFramebuffer> framebuffers_;
  // Backs ui_renderpass -- color only (no depth attachment; see
  // VulkanRenderpass's has_prev_pass/clear_flags), one per swapchain image.
  std::vector<VulkanFramebuffer> ui_framebuffers_;

  void create_commandbuffer();
  void regenerate_framebuffers();
  b8 recreate_swapchain();

  // context_.queue_complete_semaphores must be sized/indexed by swapchain
  // image (see its declaration comment in vulkan_types.inl), so both
  // initialize() and recreate_swapchain() (image_count can change across a
  // recreate) need to (re)build the whole array -- factored out here
  // instead of duplicated at each call site.
  void create_queue_complete_semaphores();
  void destroy_queue_complete_semaphores();
};
