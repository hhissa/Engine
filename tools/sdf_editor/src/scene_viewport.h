#pragma once
#include <renderer/camera.h>
#include <resources/sdf_scene.h>

#include <QElapsedTimer>
#include <QPoint>
#include <QPointF>
#include <QWindow>

#include <memory>
#include <optional>
#include <vector>

class PlatformLayer;

// Which world axis a gizmo drag is currently constrained to (None means
// "not dragging").
enum class GizmoAxis { None, X, Y, Z };

// Which of the two gizmo tools is currently shown/interactive -- toggled by
// SdfEditorWindow's Move/Rotate buttons (see set_gizmo_mode() below).
enum class GizmoMode { Translate, Rotate };

// Identifies one selected item within scene_: either a primitive (which
// layer, and which entry within that layer's primitives[] -- a layer can
// hold more than one primitive, see SdfLayerDef's own comment, so a layer
// index alone no longer names a unique primitive the way it used to when
// this editor enforced one-primitive-per-layer) OR a light (light_index
// into scene_.lights, with layer_index/primitive_index left at -1). Only
// a Point light is ever referenced this way -- a Directional light has no
// position for the gizmo to show/drag (see SdfLightDef).
struct PrimitiveRef {
  int layer_index = -1;
  int primitive_index = -1;
  int light_index = -1;

  bool is_light() const { return light_index >= 0; }

  bool operator==(const PrimitiveRef &other) const {
    return layer_index == other.layer_index &&
          primitive_index == other.primitive_index &&
          light_index == other.light_index;
  }
};

// One selected item's final position/rotation/params once a gizmo drag
// (possibly across a multi-item selection) ends -- see
// primitives_transformed() below. For a light ref (see PrimitiveRef above),
// only position is meaningful -- rotation/params are sent as zero and
// ignored by the receiving end (a point light has neither).
struct GizmoTransformResult {
  PrimitiveRef ref;
  glm::vec3 position;
  glm::vec3 rotation;
  glm::vec3 params;
};

// Embeds the engine's live Vulkan-rendered raymarch scene into a Qt
// widget tree (via QWidget::createWindowContainer(), see main_window.cpp)
// -- a live view with mouse-orbit camera controls (right-drag to orbit,
// wheel to zoom), a WASD-fly + Q/E-vertical keyboard camera (see
// keyPressEvent()/keyReleaseEvent()/tick() -- requires the viewport to have
// keyboard focus, which mousePressEvent() requests on every click),
// click-to-select (left-click on empty space), and
// click-and-drag-to-transform via a gizmo: either a 3-axis translate gizmo
// (3 straight lines) or a 3-ring rotate gizmo (3 circles, one per axis,
// each drawn as a polyline), whichever set_gizmo_mode() last selected --
// see update_gizmo()/hit_test_gizmo() below for the projection/hit-test
// math for both. The gizmo's lines are drawn by the engine itself every
// tick() via renderer_draw_line() (see renderer_frontend.h), NOT by a
// separate Qt-painted overlay widget -- a first attempt used one, but a
// QWindow wrapped via QWidget::createWindowContainer() (which this class
// is, out of necessity: it needs a real native window to create a
// VkSurfaceKHR from) is a genuine native child window, and on X11 those
// always draw on top of ordinary sibling widgets regardless of Qt's own
// declared widget stacking order -- so the overlay was permanently hidden
// underneath the render surface. Drawing the gizmo *inside* the same
// native surface via the engine's own UI renderpass sidesteps that
// entirely. A scale gizmo isn't implemented (primitive size has its own
// spinbox in the side panel instead). Drives the renderer with its own
// QTimer rather than going through Application/Game at all: this tool
// isn't a Game (no update()/render()/on_resize() semantics needed), and
// Application's constructor would create a second, owning PlatformLayer --
// exactly the XCB-window conflict this class exists to avoid.
//
// Dragging never re-bakes the actual voxel scene on every mouse-move (that
// would be a GPU stall per pixel of mouse movement -- see
// VulkanRaymarchShader::rebake()'s comment) -- only the gizmo's own 2D
// lines move live, following an in-memory-only edit to scene_. The real
// renderer_load_scene()/rebake only happens once, in SdfEditorWindow, when
// primitives_transformed() fires on mouse release.
class SceneViewport : public QWindow {
  Q_OBJECT

public:
  explicit SceneViewport(QWindow *parent = nullptr);
  ~SceneViewport() override;

  SceneViewport(const SceneViewport &) = delete;
  SceneViewport &operator=(const SceneViewport &) = delete;

  // Shuts the renderer down (if it was ever initialized) while this
  // window's underlying native handle is still alive. Called explicitly
  // from SdfEditorWindow's destructor rather than relying on this class's
  // own destructor running before Qt tears down the native window --
  // see the plan's shutdown-ordering note. Safe to call more than once
  // (e.g. once explicitly, once again from the destructor as a backstop).
  void shutdown_renderer();

  // Keeps a copy of the current scene for left-click picking and gizmo
  // dragging -- called by SdfEditorWindow::sync_viewport_scene() right
  // alongside its renderer_load_scene() calls, so both always operate on
  // the same scene the viewport is actually showing. Also the point where
  // any transform a just-finished drag applied gets overwritten with
  // SdfEditorWindow's authoritative merged copy, resolving the temporary
  // divergence between the two during a drag (see primitives_transformed()).
  void set_scene(const SdfScene &scene) {
    scene_ = scene;
    update_gizmo();
  }

  // Selects (or, for an empty vector, deselects) which item(s) -- primitives
  // and/or lights (see PrimitiveRef) -- the gizmo is shown on/drags
  // together -- called both from this class's own pick_at() and externally
  // by SdfEditorWindow when the side panel's own tree/lights-list selection
  // changes, so either place can drive the other. A single-element
  // selection behaves exactly like the old one-primitive gizmo; a multi-
  // element one shows/drags the gizmo at the group's centroid (see
  // update_gizmo()). Only the first entry drives the renderer's selection
  // outline (see renderer_set_selected_primitive() in the .cpp) -- that
  // outline is a single-primitive highlight, not a group one, and doesn't
  // apply to a light selection at all (no outline concept for one).
  void set_selection(const std::vector<PrimitiveRef> &selection);

  // Switches which gizmo tool is shown/interactive -- called by
  // SdfEditorWindow's Move/Rotate toggle buttons.
  void set_gizmo_mode(GizmoMode mode) {
    gizmo_mode_ = mode;
    update_gizmo();
  }

  // Shows/hides the engine's reference grid (the subdivided y=0 plane --
  // see renderer_set_grid_visible() in renderer_frontend.h) -- called by
  // SdfEditorWindow's Show Grid toggle button. The engine-wide default is
  // hidden (games must never draw it), so ensure_engine_initialized()
  // re-applies the stored value right after renderer_initialize(); until
  // then this just records the desired state (the renderer may not exist
  // yet -- the engine only comes up on the first exposeEvent).
  void set_grid_visible(bool visible);

  // Switches primary visibility between marching the chunked field and
  // shading its baked point cloud directly -- the Dreams-style splatting
  // technique (see RendererSplatMode in renderer_types.inl). Called by
  // SdfEditorWindow's Splat Visibility toggle. Same stored-until-
  // initialized contract as set_grid_visible() above: the engine's own
  // default is Prime (which cannot change the image), so this only ever
  // selects between that and Visibility.
  void set_splat_visibility(bool enabled);

  // Stops/restarts tick_timer_ -- SdfEditorWindow calls pause_rendering()
  // before opening any modal dialog (QFileDialog/QColorDialog/...) and
  // resume_rendering() right after. A modal dialog spins its own nested
  // Qt event loop, but a QTimer keeps firing into that nested loop unless
  // stopped -- so without this, tick() (and therefore a Vulkan present
  // every ~16ms) kept running the whole time the dialog was up, on a
  // window that's now partially/fully occluded by it. This engine's WSI
  // calls (vkAcquireNextImageKHR, the in-flight fence wait) use infinite
  // timeouts with no occlusion guard, and everything -- render loop, GUI
  // event loop, the dialog itself -- shares this one thread, so if
  // presentation ever stalls while occluded (a real possibility on
  // Linux/X11), the whole process appears to freeze, dialog included,
  // until that indefinite wait eventually returns (if it ever does).
  // Simply not rendering at all while a dialog owns the event loop removes
  // the only thing that could stall it. Safe to call resume_rendering()
  // before the engine has even initialized (e.g. no dialog opened yet) --
  // it's a no-op then, same as tick_timer_ being null-checked throughout.
  void pause_rendering();
  void resume_rendering();

signals:
  // Emitted on every left-click release that isn't a gizmo drag, reflecting
  // this class's own (possibly just-updated) selection: a plain click
  // replaces the whole selection with whatever the ray hit nearest (or
  // clears it, for a click that hit nothing); a ctrl-click toggles just the
  // hit primitive in/out of whatever was already selected. SdfEditorWindow
  // mirrors this back onto contents_tree_'s own item selection so both
  // stay in sync no matter which side drove the change.
  void selection_changed(std::vector<PrimitiveRef> selection);

  // Emitted once, when a gizmo drag ends (mouse release) -- one entry per
  // primitive that was part of the drag (a single-primitive selection
  // sends exactly one), each carrying that primitive's final position/
  // rotation/params (only one of the three actually changed, depending on
  // axis/gizmo mode/primitive type, but all three are always sent so
  // SdfEditorWindow can just overwrite its own copies unconditionally).
  void primitives_transformed(std::vector<GizmoTransformResult> results);

protected:
  void exposeEvent(QExposeEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  // Tracks which of W/A/S/D/Q/E are currently held (see the key_*_ bools
  // below) -- applied continuously in tick(), like a game's per-frame
  // input poll, rather than moving a fixed amount per keypress.
  void keyPressEvent(QKeyEvent *event) override;
  void keyReleaseEvent(QKeyEvent *event) override;
  // Clears every key_*_ flag on focus loss (e.g. alt-tab, or clicking
  // another window while a key is held) -- otherwise a key held down when
  // focus leaves never gets its keyReleaseEvent, and the camera would keep
  // flying in that direction forever.
  void focusOutEvent(QFocusEvent *event) override;

private slots:
  void tick();

private:
  // width()/height() are logical pixels; the Vulkan swapchain extent needs
  // physical pixels -- every place size reaches the renderer must go
  // through this, not width()/height() directly.
  QSize physical_size() const;

  void ensure_engine_initialized();

  // Builds a world-space ray for the given widget-local pixel position,
  // using the exact same construction Builtin.RaymarchShader.comp.glsl
  // uses per-pixel (uv from pixel/image size, then
  // uv.x*right + uv.y*up + forward) -- so a click picks whatever the user
  // actually sees at that pixel. additive true (ctrl-click) toggles the hit
  // primitive in/out of the current selection instead of replacing it.
  // Drives set_selection() and emits selection_changed().
  void pick_at(QPoint pos, bool additive);

  // Inverts the same ray construction: projects a world point to this
  // window's logical-pixel coordinates (matching mouse events/
  // GizmoOverlay's coordinate space). Returns nullopt if the point is
  // behind the camera.
  std::optional<QPointF> project_to_screen(glm::vec3 world_point) const;

  // A Plane's SdfPrimitiveDef::position is always (0,0,0) -- see
  // GeometryConfig::plane()/add_plane() -- only its height (params.x)
  // means anything, so the gizmo's world position for a Plane is
  // synthesized as (0, height, 0) instead of using position directly.
  static glm::vec3 gizmo_effective_position(const SdfPrimitiveDef &primitive);
  static glm::vec3 axis_world_direction(GizmoAxis axis);

  // Builds one rotate-gizmo ring (axis's rotation circle) as a closed
  // screen-space polyline: kGizmoRingSegments points around origin_world at
  // the given world-space radius, in the plane perpendicular to axis,
  // projected to screen space via project_to_screen(). Points that project
  // behind the camera are simply omitted (leaves a gap in that stretch of
  // the ring rather than hiding it entirely).
  std::vector<QPointF> build_gizmo_ring(glm::vec3 origin_world,
                                        GizmoAxis axis, f32 radius) const;

  // Resolves selected_ to actual primitive pointers (skipping any light
  // entries, and any entry that's gone stale -- index out of range, e.g. a
  // removal elsewhere shrank scene_ since the selection was made).
  std::vector<SdfPrimitiveDef *> selected_primitives();
  // Mirrors selected_primitives() for the light entries in selected_ (see
  // PrimitiveRef::is_light()) instead.
  std::vector<SdfLightDef *> selected_lights();

  // The centroid of gizmo_effective_position() across every currently-
  // selected primitive, plus each selected light's own position -- where
  // the gizmo is drawn/dragged from. Equals a single item's own position
  // when exactly one is selected, so this is a strict generalization of the
  // old single-primitive behaviour, not a different one.
  glm::vec3 selection_centroid(const std::vector<SdfPrimitiveDef *> &primitives,
                               const std::vector<SdfLightDef *> &lights) const;

  // Recomputes the gizmo's screen-space geometry (logical pixels, matching
  // mouse events) from the current selection/camera/gizmo_mode_ and caches
  // it (gizmo_visible_/gizmo_origin_/gizmo_x_/y_/z_ for Translate,
  // gizmo_ring_x_/y_/z_ for Rotate -- used by both hit_test_gizmo() and
  // draw_gizmo()). Called every tick() (camera/drag movement) and
  // immediately on selection/mode change.
  void update_gizmo();

  // Queues this frame's renderer_draw_line() calls (3 straight axes for
  // Translate, or 3 ring polylines for Rotate) from the geometry
  // update_gizmo() last cached, converting logical pixels to the physical
  // pixels renderer_draw_line()/the swapchain actually use (see
  // physical_size()). No-op if gizmo_visible_ is false. Called every
  // tick(), after update_gizmo() -- queued draws don't persist to the next
  // frame, so this must run unconditionally every tick, not just when the
  // geometry changes.
  void draw_gizmo() const;

  // Queues a renderer_draw_text() showing the active rotate drag's axis and
  // signed delta in degrees since the drag started (e.g. "Y +42.3"), near
  // wherever the mouse currently is -- a Translate drag has no equivalent
  // (worldspace distance moved is already visible from the primitive itself
  // sliding along the axis, but a rotation's magnitude isn't nearly as
  // legible just from watching it spin). No-op unless a Rotate drag is
  // currently in progress. Called every tick(), same discipline as
  // draw_gizmo().
  void draw_gizmo_drag_indicator() const;

  // Returns which axis (if any) pos is within a small pixel tolerance of,
  // using the geometry update_gizmo() last cached (axis lines for
  // Translate, ring polylines for Rotate). None if nothing is
  // selected/visible or pos isn't close enough to any axis.
  GizmoAxis hit_test_gizmo(QPointF pos) const;

  void begin_gizmo_drag(GizmoAxis axis, QPointF mouse_pos);
  void update_gizmo_drag(QPointF mouse_pos);
  void end_gizmo_drag();

  std::unique_ptr<PlatformLayer> platform_;
  bool initialized_ = false;
  bool shutdown_ = false;

  Camera camera_;
  QElapsedTimer frame_timer_;
  QTimer *tick_timer_ = nullptr;
  SdfScene scene_; // see set_scene()

  bool orbiting_ = false;
  QPoint last_mouse_pos_;

  // Held-key state for WASD-fly/QE-vertical camera movement, applied every
  // tick() rather than per-keypress -- see keyPressEvent()/keyReleaseEvent().
  bool key_w_ = false;
  bool key_a_ = false;
  bool key_s_ = false;
  bool key_d_ = false;
  bool key_q_ = false;
  bool key_e_ = false;

  // See set_selection()/selected_primitives(). Empty means nothing
  // selected -- the gizmo is hidden.
  std::vector<PrimitiveRef> selected_;

  GizmoMode gizmo_mode_ = GizmoMode::Translate;

  // See set_grid_visible() -- defaults to shown, this being precisely the
  // modelling tool the engine's (hidden-by-default) grid exists for.
  bool grid_visible_ = true;

  // See set_splat_visibility() -- defaults off, so the viewport opens on
  // the thoroughly-exercised raymarched path and splat visibility is
  // something you deliberately switch on to compare against it.
  bool splat_visibility_ = true;

  bool gizmo_visible_ = false;
  QPointF gizmo_origin_;
  QPointF gizmo_x_;
  QPointF gizmo_y_;
  QPointF gizmo_z_;

  // Rotate-mode gizmo geometry: each axis's rotation ring as a screen-space
  // polyline (see build_gizmo_ring()) -- empty when gizmo_mode_ isn't
  // Rotate, or the axis is hidden (e.g. nothing selected).
  std::vector<QPointF> gizmo_ring_x_;
  std::vector<QPointF> gizmo_ring_y_;
  std::vector<QPointF> gizmo_ring_z_;

  // A small wireframe-sphere marker (three orthogonal rings, reusing
  // build_gizmo_ring()) drawn at the Translate gizmo's origin whenever the
  // selection includes a light -- a light has no raymarched geometry of its
  // own, so without this its position would otherwise only be visible as
  // three bare axis lines crossing empty space. Recomputed alongside the
  // Translate gizmo in update_gizmo(); empty/false whenever gizmo_mode_
  // isn't Translate or nothing selected is a light.
  bool gizmo_light_marker_visible_ = false;
  std::vector<QPointF> gizmo_light_marker_x_;
  std::vector<QPointF> gizmo_light_marker_y_;
  std::vector<QPointF> gizmo_light_marker_z_;

  GizmoAxis dragging_axis_ = GizmoAxis::None;
  // Which gizmo was active when the drag started -- locked in for the
  // whole drag so a mode toggle mid-drag (Move/Rotate buttons) can't change
  // how update_gizmo_drag()/end_gizmo_drag() interpret dragging_axis_.
  GizmoMode drag_mode_ = GizmoMode::Translate;
  QPointF drag_start_mouse_;
  // Per-selected-primitive snapshot taken at drag start (parallel to
  // selected_, same order/size) -- a single-element selection reduces to
  // exactly the old single-primitive drag; a multi-element one lets
  // update_gizmo_drag() apply the same delta (Translate) or orbit every
  // primitive around drag_group_pivot_ (Rotate) without losing where each
  // one started.
  std::vector<glm::vec3> drag_start_positions_;
  std::vector<glm::vec3> drag_start_rotations_;
  std::vector<glm::vec3> drag_start_params_;
  // Mirrors drag_start_positions_ for the selection's light entries
  // (parallel to selected_lights(), Translate-mode only -- a light has no
  // rotation/params, so Rotate-mode drags simply never touch it).
  std::vector<glm::vec3> drag_start_light_positions_;
  // Rotate-mode only: the group's centroid at drag start (see
  // selection_centroid()), fixed for the whole drag -- every selected
  // primitive's position orbits this same point, and its own rotation is
  // composed with the drag's delta quaternion, mirroring
  // VulkanRendererBackend::rotate_scene()'s own "orbit + compose" approach
  // for rotating a group of primitives together.
  glm::vec3 drag_group_pivot_{0.0f};
  // A linear approximation held fixed for the whole drag: how the screen-
  // space position changes per world unit moved along the dragged axis,
  // computed once at drag start from the axis's screen-space direction/
  // length. Good enough for typical gizmo-drag distances; large drags
  // accumulate some perspective error, a reasonable tradeoff for how much
  // simpler it keeps this over a full inverse-Jacobian correction.
  QPointF drag_screen_axis_dir_;
  f32 drag_world_per_pixel_ = 0.0f;
  // Rotate-mode drag state. The total signed rotation (radians) is tracked
  // by projecting each frame's raw mouse movement (delta_mouse, screen
  // pixels) onto the *tangent* direction of a circle of fixed radius
  // drag_radius_screen_ (established once at drag start) centered on the
  // gizmo's screen-space origin, then dividing by that same fixed radius
  // to get an angular step -- NOT by recomputing atan2(mouse - origin)
  // fresh from the mouse's live distance to the origin every frame. The
  // naive atan2 approach is scale-invariant in exact math, but with
  // pixel-quantized mouse input it becomes wildly unstable (huge angle
  // swings for a single pixel of movement) whenever the cursor passes
  // close to the origin during the drag -- which happens often, since a
  // ring viewed close to edge-on foreshortens to a thin line running right
  // through the origin, and dragging along that line means crossing near
  // it. Using a fixed radius the whole drag (rather than the live,
  // possibly near-zero one) keeps the angular sensitivity constant
  // regardless of where the cursor actually wanders on screen. A 2D
  // screen-space technique rather than a true 3D arcball/trackball
  // rotation: simple, and matches the translate gizmo's own "good enough"
  // screen-space philosophy above, though it means the ring nearer/farther
  // from the camera along its own axis doesn't change the drag's feel the
  // way a true trackball would.
  f32 drag_start_angle_ = 0.0f;
  f32 drag_radius_screen_ = 0.0f;
  QPointF drag_last_mouse_pos_;
  f32 drag_accumulated_angle_ = 0.0f;
  // Live readout for draw_gizmo_drag_indicator(): drag_accumulated_angle_
  // in degrees, and where to draw it (logical pixels) -- both updated every
  // update_gizmo_drag() call while drag_mode_ == Rotate, so tick()'s redraw
  // always shows the latest value even though it doesn't itself run on
  // every mouse move.
  f32 drag_delta_degrees_ = 0.0f;
  QPointF drag_indicator_pos_;
};
