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
// primitive_transformed() fires on mouse release.
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
  // divergence between the two during a drag (see primitive_transformed()).
  void set_scene(const SdfScene &scene) {
    scene_ = scene;
    update_gizmo();
  }

  // Selects (or, for a negative index, deselects) which primitive the
  // gizmo is shown on -- called both from this class's own pick_at() and
  // externally by SdfEditorWindow when the side panel's own list selection
  // changes, so either place can drive the other. Also drives the
  // renderer's selection outline (see renderer_set_selected_primitive() in
  // the .cpp): layer_index doubles as the raymarch field's
  // scene_textures/scene_diffuse_colours index, which holds *only* because
  // this editor puts exactly one primitive per layer and always reloads
  // the entire scene_ on every change (see sync_viewport_scene()) --
  // GeometrySystem::rebuild_static_scene() then uploads primitives in
  // layer order, so a layer's ordinal position among the currently-loaded
  // layers is exactly its GPU primitive index too.
  void set_selected_layer(int layer_index);

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

signals:
  // Emitted on every left-click release that isn't a gizmo drag:
  // layer_index is the SdfScene layer index the ray hit nearest (this
  // editor puts exactly one primitive per layer -- see
  // SdfEditorWindow::on_add_clicked()), or -1 if the click hit nothing
  // (deselect).
  void primitive_picked(int layer_index);

  // Emitted once, when a gizmo drag ends (mouse release) -- position/
  // rotation/params are the primitive's final values (only one of them
  // actually changed, depending on which axis/gizmo mode/primitive type,
  // but all three are always sent so SdfEditorWindow can just overwrite its
  // own copy unconditionally).
  void primitive_transformed(int layer_index, glm::vec3 position,
                             glm::vec3 rotation, glm::vec3 params);

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
  // actually sees at that pixel. Also drives set_selected_layer() and
  // emits primitive_picked().
  void pick_at(QPoint pos);

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

  // Returns the currently-selected primitive, or nullptr if nothing valid
  // is selected (index out of range, or that layer somehow has no
  // primitives -- shouldn't happen given this editor's one-primitive-per-
  // layer convention, but checked defensively).
  SdfPrimitiveDef *selected_primitive();

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

  int selected_layer_index_ = -1;

  GizmoMode gizmo_mode_ = GizmoMode::Translate;

  // See set_grid_visible() -- defaults to shown, this being precisely the
  // modelling tool the engine's (hidden-by-default) grid exists for.
  bool grid_visible_ = true;

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

  GizmoAxis dragging_axis_ = GizmoAxis::None;
  // Which gizmo was active when the drag started -- locked in for the
  // whole drag so a mode toggle mid-drag (Move/Rotate buttons) can't change
  // how update_gizmo_drag()/end_gizmo_drag() interpret dragging_axis_.
  GizmoMode drag_mode_ = GizmoMode::Translate;
  QPointF drag_start_mouse_;
  glm::vec3 drag_start_position_{0.0f};
  glm::vec3 drag_start_rotation_{0.0f};
  glm::vec3 drag_start_params_{0.0f};
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
