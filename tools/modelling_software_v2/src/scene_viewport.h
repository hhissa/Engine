#pragma once
#include "sculpt_document.h" // M4a (Move): SculptStampRecord -- see move_pickup_callback_'s own comment.
#include <renderer/camera.h>
#include <renderer/renderer_types.inl> // SculptBrushShape/SculptBrushOp

#include <QElapsedTimer>
#include <QPoint>
#include <QWindow>

#include <functional>
#include <memory>
#include <optional>

class PlatformLayer;
class QTimer;

// M3b (Clone): a frozen copy of a brush configuration, captured from
// whatever the brush panel (SceneViewport::brush_shape_/brush_size_/
// brush_smoothness_/brush_op_) held at the moment "Set Clone Source" was
// clicked (see ModellingWindow::on_set_clone_source_clicked() ->
// SceneViewport::capture_clone_source()). Deliberately does NOT include a
// center -- unlike a SculptStampRecord, a clone source is re-stamped at
// wherever the user subsequently drags, not at one fixed point. Also
// deliberately NOT updated as the live brush panel changes after capture
// (that's the whole point: Clone re-stamps the FROZEN configuration, while
// an ordinary drag with Clone mode off -- Smear -- re-reads brush_shape_/
// brush_size_/brush_smoothness_/brush_op_ live on every sub-stamp). size_
// at_capture mirrors brush_size_ at capture time; it isn't used by the
// stamp call itself (params already bakes size in via brush_params_from_
// size()) but is kept around as the "how big was this shape" number
// mouseMoveEvent's Clone spacing math needs (see kCloneSpacingFactor's own
// comment in the .cpp) and ModellingWindow's clone-source status label
// wants to display.
struct CloneSource {
  SculptBrushShape shape_type = SculptBrushShape::Sphere;
  glm::vec4 params{0.0f};
  f32 smoothness = 0.0f;
  SculptBrushOp op = SculptBrushOp::Union;
  i32 material_slot = 0;
  f32 size_at_capture = 0.0f;
  // M3 (Looseness): captured alongside everything else a clone source
  // freezes -- see brush_looseness_'s own comment for what this value
  // actually does (nothing, yet -- see SculptStamp::looseness,
  // sculpt_volume.h).
  f32 looseness = 0.0f;
};

// Embeds the engine's live Vulkan-rendered chunked/splat scene into a Qt
// widget tree (via QWidget::createWindowContainer(), see main_window.cpp)
// -- a live view with mouse-orbit camera controls (right-drag to orbit,
// wheel to zoom), a WASD-fly + Q/E-vertical keyboard camera, and
// left-click-to-stamp: the currently selected brush shape/size/smoothness/
// op (see set_brush_shape()/set_brush_size()/set_brush_smoothness()/
// set_brush_op(), driven by ModellingWindow's brush panel) is stamped into
// the sculpt volume at a fixed distance along the click ray.
//
// Adapted from tools/sdf_editor's SceneViewport (see git history, deleted
// in M0) with every CSG/gizmo/selection-specific piece removed -- no
// SdfScene member, no PrimitiveRef, no gizmo drag state. The QWindow/
// Vulkan bootstrap (borrowed-handle PlatformLayer off Qt's own XCB
// connection, deferred init to the first exposeEvent, the QTimer-driven
// tick() loop, pause_rendering()/resume_rendering() around modal dialogs)
// and the generic Camera/orbit/fly controls are reused near-verbatim --
// see this file's .cpp for exactly what carried over.
class SceneViewport : public QWindow {
  Q_OBJECT

public:
  explicit SceneViewport(QWindow *parent = nullptr);
  ~SceneViewport() override;

  SceneViewport(const SceneViewport &) = delete;
  SceneViewport &operator=(const SceneViewport &) = delete;

  // Shuts the renderer down (if it was ever initialized) while this
  // window's underlying native handle is still alive. Called explicitly
  // from ModellingWindow's destructor rather than relying on this class's
  // own destructor running before Qt tears down the native window.
  void shutdown_renderer();

  // Stops/restarts the render tick -- ModellingWindow calls
  // pause_rendering() before opening any modal dialog (QFileDialog) and
  // resume_rendering() right after, so the engine's WSI calls never spin
  // underneath a nested Qt event loop. See tools/sdf_editor's identical
  // pattern (git history) for the full rationale.
  void pause_rendering();
  void resume_rendering();

  // Replays a previously-saved stamp (e.g. during file load) without
  // going through the click path -- same renderer_sculpt_stamp_shape()
  // call left-click uses, but does NOT emit stamp_placed(), since the
  // caller (ModellingWindow::load_document()) already has the record from
  // the file and would otherwise double-record it into the document.
  // No-op (silently) if the engine hasn't initialized yet -- callers
  // should only invoke this once the viewport is up.
  void replay_stamp(glm::vec3 center, SculptBrushShape shape_type,
                    glm::vec4 params, f32 smoothness, SculptBrushOp op,
                    int material_slot, f32 looseness = 0.0f);

  // M2b: reverts/re-applies the most recent stamp -- thin wrappers over
  // renderer_sculpt_undo()/renderer_sculpt_redo() (see renderer_frontend.h)
  // with the same "no-op (returns false) before the engine has initialized"
  // guard replay_stamp() above already has. ModellingWindow calls these
  // from its Edit menu's Undo/Redo actions and, on true, keeps its own
  // SculptDocument (and its own small redo stack of popped records) in
  // lockstep with the engine's SculptVolume/undo stack -- see main_window.
  // cpp's on_undo_triggered()/on_redo_triggered() for exactly how.
  bool undo_last_stamp();
  bool redo_last_stamp();

  // M4b (Delete-of-an-arbitrary-edit): thin wrapper over
  // renderer_sculpt_delete_edit(), same "no-op (returns false) before the
  // engine has initialized" guard as undo_last_stamp()/redo_last_stamp()
  // above. ModellingWindow's edit-history panel calls this with the
  // selected row's index, and on true, removes that same index from its
  // own document_ (and does the bookkeeping that keeps undo_group_sizes_/
  // redo_groups_ honest about a non-tail removal -- see main_window.cpp's
  // on_delete_edit_clicked()).
  bool delete_edit_at(int index);

  // M4c (Move-of-an-arbitrary-edit): arms a one-shot "move this edit" drag
  // -- called from ModellingWindow's "Move Selected Edit" button with
  // whatever row is currently selected in its edit-history list. Does NOT
  // touch the engine or document_ itself (unlike Move mode's press-time
  // pickup, there is nothing to undo up front -- the target edit stays
  // exactly where it is on the GPU until the user actually commits a new
  // position); it only arms pending_move_edit_index_, which the very next
  // left-press/release pair in the viewport consumes (see
  // mousePressEvent()/mouseReleaseEvent()'s own comments) to drive one
  // renderer_sculpt_move_edit() call on release. A no-op (silently) if
  // index is negative -- callers are expected to have already validated the
  // selection.
  void begin_move_selected_edit(int index) {
    if (index >= 0) {
      pending_move_edit_index_ = index;
    }
  }

  // M2a brush-panel controls (see ModellingWindow's brush panel) -- each
  // left-click stamp uses whatever these are currently set to. Defaults
  // match M1b's old fixed behaviour (Sphere, size 1.0, smoothness 0 =
  // hard, Union) so the tool behaves identically until a user actually
  // touches the panel.
  void set_brush_shape(SculptBrushShape shape) { brush_shape_ = shape; }
  void set_brush_size(f32 size) { brush_size_ = size; }
  void set_brush_smoothness(f32 smoothness) { brush_smoothness_ = smoothness; }
  void set_brush_op(SculptBrushOp op) { brush_op_ = op; }

  SculptBrushShape brush_shape() const noexcept { return brush_shape_; }
  f32 brush_size() const noexcept { return brush_size_; }
  f32 brush_smoothness() const noexcept { return brush_smoothness_; }
  SculptBrushOp brush_op() const noexcept { return brush_op_; }

  // M5 (Finishes): which material slot (see renderer_sculpt_set_material(),
  // engine/src/renderer/renderer_frontend.h) the NEXT place_stamp()/
  // capture_clone_source() call uses -- NOT retroactive, exactly like every
  // other brush-panel control (changing Size doesn't resize already-placed
  // stamps either). ModellingWindow owns the actual slot-allocation
  // counter and the preset-name -> (roughness, metallic, waxy) mapping
  // (see its own apply_finish_and_colour()); this class just remembers
  // whichever slot index was configured most recently. Defaults to 0 (the
  // plain white default material every stamp used before this milestone),
  // so a tool session nobody has touched the Finish/colour controls on
  // behaves identically to before.
  void set_brush_material_slot(i32 slot) { brush_material_slot_ = slot; }
  i32 brush_material_slot() const noexcept { return brush_material_slot_; }

  // M3 (Looseness): 0 (clean/uniform, the default) to 1 (maximally loose).
  // Purely authored/recorded right now -- see SculptStamp::looseness's own
  // comment (engine/src/resources/sculpt_volume.h) for why this has no
  // visible effect on a stamp's shape or shading yet: there is no Fleck
  // evaluator (M6) to consume it. Threaded through the brush panel/stamp
  // calls anyway so a value set now round-trips correctly through save/
  // load/Undo/Redo and is already in place the moment M6 lands.
  void set_brush_looseness(f32 looseness) { brush_looseness_ = looseness; }
  f32 brush_looseness() const noexcept { return brush_looseness_; }

  // M4a (Stretch): three independent multipliers (default 1.0 = no
  // stretch), applied on top of whatever brush_params_from_size() already
  // computed for params.x/y/z -- see place_stamp()'s own comment for
  // exactly where/how, and the per-shape table in brush_params_from_size()
  // 's own comment for which shapes these meaningfully affect vs. no-op on
  // (e.g. Sphere/Plane/Octahedron only ever populate params.x, so stretch_y/
  // stretch_z are inert for them -- by design, not a bug). Deliberately
  // THREE separate sliders rather than one vec3-valued control: this tool
  // has no gizmo/local-axis-drag concept at all (confirmed explicitly
  // deferred, see the plan file), so three plain spin boxes are the
  // simplest control surface that still lets each axis move independently.
  // params.w is NEVER touched by these (no fourth slider exists) -- see
  // apply_stretch()'s own comment, scene_viewport.cpp.
  void set_brush_stretch_x(f32 stretch) { brush_stretch_x_ = stretch; }
  void set_brush_stretch_y(f32 stretch) { brush_stretch_y_ = stretch; }
  void set_brush_stretch_z(f32 stretch) { brush_stretch_z_ = stretch; }
  f32 brush_stretch_x() const noexcept { return brush_stretch_x_; }
  f32 brush_stretch_y() const noexcept { return brush_stretch_y_; }
  f32 brush_stretch_z() const noexcept { return brush_stretch_z_; }
  static constexpr f32 kDefaultBrushStretch = 1.0f;

  // M3b (Clone): "Clone mode" is a distinct, explicitly-toggled drag mode --
  // OFF (the default) means a left-drag behaves exactly as it always has
  // (Smear: live brush-panel settings, movement-threshold spacing, see
  // mouseMoveEvent()). ON means a left-drag instead re-stamps whatever
  // capture_clone_source() last froze into clone_source_, at a FIXED
  // spacing (kCloneSpacingFactor in the .cpp), not Smear's movement
  // threshold. Driven by ModellingWindow's "Enable Clone Mode" checkbox.
  void set_clone_mode(bool enabled) { clone_mode_ = enabled; }
  bool clone_mode() const noexcept { return clone_mode_; }

  // M3b: captures the CURRENT brush panel state (shape/size-derived params/
  // smoothness/op/material_slot -- M5: now whatever brush_material_slot_
  // currently holds, matching place_stamp(); was unconditionally 0 before
  // the Finish/colour controls existed) into clone_source_,
  // overwriting whatever was captured before. Called from ModellingWindow's
  // "Set Clone Source" button. Works regardless of whether Clone mode is
  // currently on -- capturing a source and turning Clone mode on are two
  // independent actions, so a user can pre-stage a source before flipping
  // the mode (or change the live brush and re-capture a different source
  // while Clone mode is already on, between drags).
  void capture_clone_source();

  bool has_clone_source() const noexcept { return clone_source_.has_value(); }

  // M3c (Spraypaint): a THIRD, mutually-exclusive drag mode alongside
  // Smear (the default, both this and Clone off) and Clone -- ON means a
  // left-drag calls renderer_sculpt_spray() repeatedly (see place_spray())
  // instead of stamping/cloning geometry at all. ModellingWindow's brush
  // panel is responsible for keeping this and clone_mode() mutually
  // exclusive (unchecking one when the other is checked) -- this class
  // itself does not enforce that; if a caller somehow turned both on,
  // mousePressEvent()/mouseMoveEvent() check spray_mode() FIRST (see their
  // own bodies), so Spraypaint would simply win.
  void set_spray_mode(bool enabled) { spray_mode_ = enabled; }
  bool spray_mode() const noexcept { return spray_mode_; }

  // M3c: how much opacity ONE spray pass adds (clamped into [0,1] on the
  // GPU side -- see Builtin.SculptSpraypaint.comp.glsl) -- the brush panel's
  // "Spray Opacity" slider drives this. Deliberately independent of
  // brush_smoothness_/brush_op_, which Spraypaint has no use for at all
  // (see place_spray()'s own comment).
  void set_spray_opacity_per_pass(f32 opacity) {
    spray_opacity_per_pass_ = opacity;
  }
  f32 spray_opacity_per_pass() const noexcept {
    return spray_opacity_per_pass_;
  }
  static constexpr f32 kDefaultSprayOpacityPerPass = 0.25f;

  // M3b: read-only accessors for ModellingWindow's clone-source status
  // label -- deliberately narrow (just what the label displays), not a
  // general getter for the whole CloneSource (nothing outside this class
  // needs the full struct; place_clone_stamp() reads clone_source_
  // directly).
  SculptBrushShape clone_source_shape() const noexcept {
    return clone_source_ ? clone_source_->shape_type : SculptBrushShape::Sphere;
  }
  f32 clone_source_size() const noexcept {
    return clone_source_ ? clone_source_->size_at_capture : 0.0f;
  }
  SculptBrushOp clone_source_op() const noexcept {
    return clone_source_ ? clone_source_->op : SculptBrushOp::Union;
  }

  // M4a (Move): a FOURTH, mutually-exclusive drag mode alongside Smear
  // (default), Clone, and Spraypaint -- ON means a left-PRESS picks up the
  // most recently placed stamp (via move_pickup_callback_ below, which
  // ModellingWindow installs so this class never needs its own copy of
  // SculptDocument -- see that callback's own comment) instead of placing
  // a new one; the picked-up stamp is held in move_payload_ and re-stamped
  // at wherever the matching mouseReleaseEvent lands (see place_move_
  // stamp()'s own comment on why commit-on-RELEASE, not continuously
  // during the drag, like Smear/Clone/Spraypaint all do). ModellingWindow's
  // brush panel keeps this mutually exclusive with clone_mode()/spray_
  // mode() (unchecking the others when this is checked, and vice versa) --
  // this class itself does not enforce that; mousePressEvent() checks
  // spray_mode() first, then move_mode(), then falls through to Smear/
  // Clone (see its own body), so if a caller somehow left more than one on,
  // that priority order is what actually wins.
  void set_move_mode(bool enabled) { move_mode_ = enabled; }
  bool move_mode() const noexcept { return move_mode_; }

  // M4a: installs the callback mousePressEvent()'s Move branch calls to
  // pick up "the most recently placed stamp" -- NOT a Qt signal (unlike
  // every other cross-class hook on this class) because this one needs a
  // synchronous RETURN VALUE, not a one-way notification: picking a stamp
  // up means retracting it from BOTH the engine's own undo stack (via
  // undo_last_stamp(), already public on this class) AND ModellingWindow's
  // SculptDocument (document_, owned there -- see sculpt_document.h's own
  // comment on why that split exists), and the caller needs the popped
  // record's exact shape/params/smoothness/op/material_slot/looseness
  // back before it can re-stamp anything. A std::function is the simplest
  // way to get a return value out of a call this class itself triggers;
  // ModellingWindow installs it once, right after constructing this
  // viewport (see main_window.cpp's constructor). Returns std::nullopt if
  // there is nothing to pick up (document_ empty, or the engine-side undo
  // unexpectedly failed) -- mousePressEvent() treats that as a no-op drag
  // and emits move_source_missing(), mirroring clone_source_missing()'s
  // own precedent.
  using MovePickupFn = std::function<std::optional<SculptStampRecord>()>;
  void set_move_pickup_callback(MovePickupFn callback) {
    move_pickup_callback_ = std::move(callback);
  }

  // Default brush size (the generic "size" the brush panel's spin box
  // controls) used until a user changes it -- see brush_params_from_size()
  // in the .cpp for how this one scalar maps onto each shape's actual SDF
  // parameters.
  static constexpr f32 kDefaultBrushSize = 1.0f;

  // Fixed distance along the click ray a stamp is placed at -- there is no
  // "existing surface" data structure to pick against yet in this
  // milestone (CPU-side picking against the sculpt volume is a later
  // task), so every click places its shape at this fixed depth from the
  // camera regardless of what's actually rendered under the cursor.
  static constexpr f32 kStampDistance = 5.0f;

signals:
  // Emitted after a successful left-click/left-drag stamp (i.e. NOT during
  // replay_stamp()), so ModellingWindow can append it to its own
  // SculptDocument for save/load. M3a: now fires once per stamp placed
  // during a Smear drag, not just once per click -- see mouseMoveEvent().
  void stamp_placed(glm::vec3 center, SculptBrushShape shape_type,
                    glm::vec4 params, f32 smoothness, SculptBrushOp op,
                    int material_slot, f32 looseness);

  // M3a: emitted once a left-button drag ends (mouseReleaseEvent), carrying
  // how many stamp_placed() signals fired between the matching press and
  // this release (always >= 1 -- mousePressEvent stamps immediately so a
  // plain click-without-moving still counts as one). ModellingWindow uses
  // this to group however many stamps one Smear stroke produced into a
  // single undoable unit -- see main_window.cpp's on_stroke_ended()/
  // undo_group_sizes_ for exactly how. Deliberately tool-side rather than
  // an engine-side stroke concept: the engine's undo_stack_ (vulkan_
  // raymarch_shader.h) already pops exactly one stamp per undo_stamp()
  // call, which is sufficient primitive -- grouping N of those calls into
  // one user-visible Ctrl+Z is pure bookkeeping that doesn't need to know
  // anything about bricks/voxels/chunk slots, so it lives here instead of
  // teaching the engine a new "stroke" concept it doesn't otherwise need.
  void stroke_ended(int stamp_count);

  // M3b: emitted from capture_clone_source() every time it runs, so
  // ModellingWindow can refresh its clone-source status label immediately
  // (rather than the label only updating lazily next time something else
  // happens to repaint it).
  void clone_source_captured();

  // M3b: emitted from mousePressEvent() when a left-press starts with
  // Clone mode on but no clone source captured yet. Deliberately a no-op
  // drag in that case (left_dragging_ is never set, so no stamps are
  // placed and mouseReleaseEvent/stroke_ended never fire either) rather
  // than silently falling back to Smear's live-brush-settings behaviour,
  // which would be a surprising, undocumented-feeling substitution for a
  // user who explicitly asked for Clone. ModellingWindow shows this as a
  // transient status-bar message.
  void clone_source_missing();

  // M4a: emitted from mousePressEvent() when a left-press starts with Move
  // mode on but move_pickup_callback_ reports nothing to pick up (document_
  // is empty -- nothing has been stamped yet this session) -- mirrors
  // clone_source_missing()'s own precedent exactly, including leaving
  // move_dragging_ unset so mouseReleaseEvent/stroke_ended never fire for
  // this press either. ModellingWindow shows this as a transient status-
  // bar message.
  void move_source_missing();

  // M4c (Move-of-an-arbitrary-edit): emitted from mouseReleaseEvent() once
  // a begin_move_selected_edit()-armed drag commits successfully (i.e.
  // renderer_sculpt_move_edit() returned true) -- `index` is the same index
  // begin_move_selected_edit() was called with, `new_center` is wherever
  // the release landed. ModellingWindow connects this to update its own
  // document_ (SculptDocument::update_stamp_center()) so the tool-side
  // mirror and the engine's SculptVolume never disagree about this edit's
  // position. Not emitted if the move failed (e.g. index somehow went
  // stale between arming and release) -- document_ is left untouched in
  // that case, same "never claim more than actually happened" discipline
  // every other sculpt-family call site in this tool already follows.
  void edit_moved(int index, glm::vec3 new_center);

public:
  // M4 (Crop/Cutout): triggers a whole-volume Crop using the CURRENT brush
  // panel's shape/size-derived params/smoothness/stretch (same "the
  // current brush configuration is a reasonable crop shape source"
  // reasoning capture_clone_source() already establishes for Clone),
  // centered at the viewport's own center-ray point (compute_ray_center()
  // at the widget's logical center pixel) -- the same fixed-distance-
  // along-the-ray placement an ordinary click already uses, since there
  // is no "existing surface" pick to anchor on instead (see compute_ray_
  // center()'s own header comment). Keeps only geometry inside the shape,
  // discards everything outside, across the WHOLE volume (not just the
  // shape's AABB) -- see renderer_sculpt_crop()'s own comment. NOT
  // undoable this pass (documented, accepted limitation -- same precedent
  // Spraypaint already set). Returns false (no-op) before the engine has
  // initialized.
  bool do_crop();

  // M4 (Cutout): the same crop shape/center as do_crop() above, but FIRST
  // saves everything the crop is about to discard to `removed_path` -- a
  // standalone snapshot file (NOT the tool's .stamps format) a later
  // "File > Open Cutout..." can load into a fresh session -- see
  // renderer_sculpt_cutout()'s own comment for why a second FILE, not a
  // second live volume, is this pass's accepted scope. Returns false (and
  // leaves the live volume untouched) if the file write failed, or before
  // the engine has initialized.
  bool do_cutout(const QString &path);

  // M4 (Cutout): opens a snapshot file do_cutout() wrote and injects it
  // into the LIVE volume -- see renderer_sculpt_load_cutout()'s own
  // comment for the ADDITIVE-onto-current-volume caveat (same one the
  // ordinary .stamps Load path already has -- there is still no "clear the
  // chunked field" API). Returns false if the file couldn't be read/is
  // malformed, or before the engine has initialized.
  bool load_cutout_file(const QString &path);

  // M5 (Finishes) + M6 (Flecks): thin wrapper over
  // renderer_sculpt_set_material() -- the same "SceneViewport is the
  // engine-API boundary, ModellingWindow stays Qt-only business logic"
  // split every other sculpt-family call on this class already follows
  // (do_crop()/undo_last_stamp()/etc. above). Deliberately does NOT also
  // call set_brush_material_slot(slot) itself -- ModellingWindow does that
  // as a separate, explicit second step (apply_finish_and_colour(),
  // main_window.cpp), the same two-step shape every other brush-panel
  // control already has (a setter configuring state, read by the next
  // place_stamp()/capture_clone_source() call). fleck_strength 0.0f means
  // "no Fleck" (fleck_pattern/fleck_scale are then ignored engine-side) --
  // see renderer_sculpt_set_material()'s own comment. No-op before the
  // engine has initialized.
  void configure_material(i32 slot, glm::vec3 colour, f32 roughness,
                          f32 metallic, bool waxy, i32 fleck_pattern,
                          f32 fleck_strength, f32 fleck_scale);

protected:
  void exposeEvent(QExposeEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void keyReleaseEvent(QKeyEvent *event) override;
  void focusOutEvent(QFocusEvent *event) override;

private slots:
  void tick();

private:
  // width()/height() are logical pixels; the Vulkan swapchain extent needs
  // physical pixels -- every place size reaches the renderer must go
  // through this, not width()/height() directly.
  QSize physical_size() const;

  void ensure_engine_initialized();

  // Builds the world-space stamp center for the given widget-local pixel
  // position, using the exact same per-pixel ray construction
  // Builtin.RaymarchShader.comp.glsl's main() uses (uv from pixel/image
  // size, scaled by height only so it's undistorted regardless of aspect
  // ratio, then ray_dir = normalize(uv.x*right + uv.y*up + forward) off
  // the camera's own orthonormal basis) -- carried over from
  // tools/sdf_editor's pick_at()/ray_intersect.h (git history), minus the
  // CPU sphere-trace against a primitive list that used to follow it
  // (that data structure no longer exists post-M0; this milestone stamps
  // at a fixed distance along the ray instead, see kStampDistance). Pure
  // calculation, no side effects -- both the click path (mousePressEvent)
  // and the M3a Smear drag path (mouseMoveEvent) use this to get a
  // candidate center before deciding whether to actually stamp there.
  glm::vec3 compute_ray_center(QPoint pos) const;

  // Issues one actual stamp at a world-space center already computed by
  // compute_ray_center() -- calls renderer_sculpt_stamp_shape() with the
  // current brush panel state and emits stamp_placed(), exactly what the
  // old stamp_at() did inline. Factored out so mousePressEvent (the
  // stroke's first stamp) and mouseMoveEvent (every subsequent Smear
  // sub-stamp) share one call site instead of duplicating it.
  void place_stamp(glm::vec3 center);

  // M3b: Clone's analogue of place_stamp() -- issues one
  // renderer_sculpt_stamp_shape() call using clone_source_'s FROZEN
  // configuration (not the live brush panel) and emits stamp_placed() with
  // that same frozen configuration, so ModellingWindow's document_/undo-
  // redo bookkeeping records exactly what was actually stamped regardless
  // of whatever the live brush panel has since drifted to. No-op (like
  // place_stamp()) before the engine is initialized, and also a no-op if
  // clone_source_ is empty (defensive -- callers are expected to have
  // already checked has_clone_source() before entering a Clone drag; see
  // mousePressEvent()).
  void place_clone_stamp(glm::vec3 center);

  // M3c (Spraypaint): issues one renderer_sculpt_spray() call at `center`
  // using the live brush shape/size (same brush_params_from_size() mapping
  // place_stamp() uses) and spray_opacity_per_pass_ -- see Builtin.
  // SculptSpraypaint.comp.glsl for what actually happens on the GPU.
  // Deliberately does NOT emit stamp_placed()/stroke_ended(): those signals
  // feed ModellingWindow's document_/undo_group_sizes_ bookkeeping, which
  // assumes every stamp it records has a matching entry on the engine's
  // own undo_stack_ (vulkan_raymarch_shader.h) -- spray_paint() never
  // touches that stack at all (see its own header comment on why
  // Spraypaint isn't undoable yet), so reporting a spray stroke through
  // those signals would let a later Ctrl+Z "undo" an unrelated geometry
  // stamp instead of doing nothing, which is far more confusing than
  // Spraypaint simply not being undoable. No-op before the engine is
  // initialized, same guard every other stamp/spray entry point has.
  void place_spray(glm::vec3 center);

  // M4a (Move): issues one renderer_sculpt_stamp_shape() call using move_
  // payload_'s picked-up configuration (not the live brush panel -- same
  // "frozen configuration" idea as place_clone_stamp(), except the
  // configuration was captured by undoing an existing stamp rather than by
  // reading the brush panel) at `center`, and emits stamp_placed() with
  // that same configuration -- so ModellingWindow's document_/undo_group_
  // sizes_ bookkeeping records this as an ordinary fresh stamp at its new
  // location, with no special-casing needed on that side (see on_stamp_
  // placed()'s own comment: it already clears redo_groups_ for every fresh
  // stamp, which is exactly what makes "Undo after a Move removes the
  // shape entirely" the natural, no-extra-code outcome -- see this class's
  // mouseReleaseEvent() for the full write-up). No-op if move_payload_ is
  // empty (defensive -- callers are expected to have already confirmed a
  // pickup succeeded before setting move_dragging_; see mousePressEvent()).
  void place_move_stamp(glm::vec3 center);

  std::unique_ptr<PlatformLayer> platform_;
  bool initialized_ = false;
  bool shutdown_ = false;

  Camera camera_;
  QElapsedTimer frame_timer_;
  QTimer *tick_timer_ = nullptr;

  bool orbiting_ = false;
  QPoint last_mouse_pos_;

  // M3a (Smear): tracks a left-button drag independently of orbiting_'s
  // right-button drag -- see mousePressEvent()/mouseMoveEvent()/
  // mouseReleaseEvent(). left_dragging_ is true for the whole press-to-
  // release span; last_stamp_center_ is the world-space center of the
  // most recent stamp placed in the current stroke (used to measure
  // movement since then); stroke_stamp_count_ counts how many stamps this
  // stroke has placed so far, reported via stroke_ended() on release.
  bool left_dragging_ = false;
  glm::vec3 last_stamp_center_{0.0f};
  int stroke_stamp_count_ = 0;

  // Held-key state for WASD-fly/QE-vertical camera movement, applied every
  // tick() rather than per-keypress.
  bool key_w_ = false;
  bool key_a_ = false;
  bool key_s_ = false;
  bool key_d_ = false;
  bool key_q_ = false;
  bool key_e_ = false;

  // M2a brush-panel state -- see set_brush_shape()/set_brush_size()/
  // set_brush_smoothness()/set_brush_op() above.
  SculptBrushShape brush_shape_ = SculptBrushShape::Sphere;
  f32 brush_size_ = kDefaultBrushSize;
  f32 brush_smoothness_ = 0.0f;
  SculptBrushOp brush_op_ = SculptBrushOp::Union;
  // M3 (Looseness) -- see set_brush_looseness()'s own comment above.
  f32 brush_looseness_ = 0.0f;
  // M4a (Stretch) -- see set_brush_stretch_x()/y()/z()'s own comment above.
  f32 brush_stretch_x_ = kDefaultBrushStretch;
  f32 brush_stretch_y_ = kDefaultBrushStretch;
  f32 brush_stretch_z_ = kDefaultBrushStretch;
  // M5 (Finishes) -- see set_brush_material_slot()'s own comment above.
  i32 brush_material_slot_ = 0;

  // M3b (Clone) state -- see set_clone_mode()/capture_clone_source()/
  // CloneSource's own comments. clone_mode_ defaults to false so a tool
  // launched fresh behaves exactly as M3a did (Smear) until a user
  // explicitly opts into Clone; clone_source_ defaults to empty (nothing
  // captured yet) until "Set Clone Source" is clicked at least once.
  bool clone_mode_ = false;
  std::optional<CloneSource> clone_source_;

  // M3c (Spraypaint) state -- see set_spray_mode()/set_spray_opacity_per_
  // pass()'s own comments. spray_dragging_/last_spray_center_ are a
  // DELIBERATELY SEPARATE pair from left_dragging_/last_stamp_center_/
  // stroke_stamp_count_ above (even though at most one of a Smear/Clone
  // drag and a Spraypaint drag can be active at once, since spray_mode_
  // gates which path mousePressEvent() takes) -- keeping them apart means
  // a spray drag never touches stroke_stamp_count_ and so never risks
  // emitting stroke_ended() for it (see place_spray()'s own comment on why
  // that must not happen).
  bool spray_mode_ = false;
  f32 spray_opacity_per_pass_ = kDefaultSprayOpacityPerPass;
  bool spray_dragging_ = false;
  glm::vec3 last_spray_center_{0.0f};

  // M4a (Move) state -- see set_move_mode()/set_move_pickup_callback()/
  // place_move_stamp()'s own comments. move_mode_ defaults to false (Smear)
  // like clone_mode_/spray_mode_ above. move_dragging_/move_payload_ are
  // DELIBERATELY SEPARATE from every other drag-state pair above (same
  // reasoning as spray_dragging_'s own comment) -- a Move drag never
  // touches last_stamp_center_/stroke_stamp_count_/last_spray_center_, and
  // vice versa, since mousePressEvent()'s mode checks are mutually
  // exclusive branches. move_payload_ is only ever populated for the
  // lifetime of one drag (set at pickup in mousePressEvent, consumed and
  // cleared by place_move_stamp()/mouseReleaseEvent) -- it is NOT a
  // persistent "last moved thing" the way clone_source_ is a persistent
  // clipboard; each Move drag picks up fresh from move_pickup_callback_.
  bool move_mode_ = false;
  bool move_dragging_ = false;
  std::optional<SculptStampRecord> move_payload_;
  MovePickupFn move_pickup_callback_;

  // M4c (Move-of-an-arbitrary-edit) state -- see begin_move_selected_edit()/
  // edit_moved()'s own comments. pending_move_edit_index_ is armed by
  // begin_move_selected_edit() and consumed by the very next left-press
  // (which moves it into moving_edit_index_ and sets moving_edit_dragging_,
  // then clears pending_move_edit_index_ -- so a second press without a
  // fresh button click does an ordinary Smear/Clone/Move stamp again, not
  // another arbitrary-edit move). Deliberately a THIRD, independent drag-
  // state pair alongside move_dragging_/move_payload_ (M4a's tail-only
  // Move) -- the two are never confused with each other: mousePressEvent()
  // checks pending_move_edit_index_ before move_mode_, so an armed
  // arbitrary-edit move always wins the very next press regardless of
  // which other mode checkbox happens to be on, and move_mode_'s own
  // pickup-on-press mechanism never runs for that one press.
  std::optional<int> pending_move_edit_index_;
  bool moving_edit_dragging_ = false;
  int moving_edit_index_ = -1;
};
