#include "scene_viewport.h"
#include "ray_intersect.h"

#include <core/application.h>
#include <platform/platform.h>
#include <renderer/renderer_frontend.h>

#include <QFocusEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QTimer>
#include <QWheelEvent>
#include <QtGui/qguiapplication_platform.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <xcb/xcb.h>

namespace {
constexpr f32 kYawSensitivity = 0.005f;
constexpr f32 kPitchSensitivity = 0.005f;
constexpr f32 kZoomSpeed = 0.0015f;
// units/sec -- matches TestbedGame::update()'s own tuning for this engine's
// ~2-unit-radius scene scale.
constexpr f32 kFlyMoveSpeed = 1.5f;

// Gizmo axis length (Translate) / ring radius (Rotate) scales with distance
// from the camera (screen_scale * distance), clamped to a minimum, so it
// reads as roughly the same size on screen regardless of zoom instead of
// vanishing far away or dwarfing the primitive up close.
constexpr f32 kGizmoScreenScale = 0.18f;
constexpr f32 kGizmoMinLength = 0.15f;
constexpr f32 kGizmoHitTolerancePx = 8.0f;
// How many straight segments approximate one rotate-gizmo ring -- plenty
// smooth at the size these rings are typically drawn on screen, without
// generating an excessive number of renderer_draw_line() calls per ring.
constexpr int kGizmoRingSegments = 32;
// Floor for a rotate drag's fixed tangent-tracking radius (see
// drag_radius_screen_'s comment) -- without this, grabbing the ring at a
// point that happens to already project close to the gizmo's screen-space
// origin (e.g. a heavily foreshortened ring) would start the whole drag
// unstable instead of just becoming unstable partway through it.
constexpr f32 kGizmoRotateMinRadiusPx = 40.0f;

f32 point_segment_distance(QPointF p, QPointF a, QPointF b) {
  QPointF ab = b - a;
  f32 len_sq = static_cast<f32>(ab.x() * ab.x() + ab.y() * ab.y());
  if (len_sq < 1e-6f) {
    return static_cast<f32>(QLineF(p, a).length());
  }
  QPointF ap = p - a;
  f32 t = static_cast<f32>(ap.x() * ab.x() + ap.y() * ab.y()) / len_sq;
  t = std::clamp(t, 0.0f, 1.0f);
  QPointF closest = a + t * ab;
  return static_cast<f32>(QLineF(p, closest).length());
}
} // namespace

SceneViewport::SceneViewport(QWindow *parent) : QWindow(parent) {
  // Tells Qt's xcb platform plugin not to create its own GL context/
  // backing store for this window -- the engine writes into it directly
  // via vkCreateXcbSurfaceKHR/the swapchain instead. No QVulkanInstance is
  // ever constructed; only this enum value matters, Qt's own Vulkan
  // machinery is otherwise unused.
  setSurfaceType(QSurface::VulkanSurface);
  camera_.set_position(glm::vec3(0.0f, 0.0f, -3.0f));
}

SceneViewport::~SceneViewport() { shutdown_renderer(); }

void SceneViewport::shutdown_renderer() {
  if (initialized_ && !shutdown_) {
    renderer_shutdown();
    shutdown_ = true;
  }
}

QSize SceneViewport::physical_size() const {
  return QSize(width(), height()) * devicePixelRatio();
}

void SceneViewport::ensure_engine_initialized() {
  // Load-bearing: exposeEvent can fire more than once (e.g. visibility
  // toggles), and a second renderer_initialize() would double-create the
  // Vulkan instance/debug messenger.
  if (initialized_) {
    return;
  }

  auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
  if (!x11) {
    qFatal("SceneViewport requires Qt's xcb platform plugin.");
  }
  xcb_connection_t *connection = x11->connection();
  xcb_window_t xcb_win = static_cast<xcb_window_t>(winId());

  QSize size = physical_size();
  application_set_framebuffer_size_override(static_cast<u32>(size.width()),
                                            static_cast<u32>(size.height()));

  platform_ = std::make_unique<PlatformLayer>(
      BorrowedWindowHandle{connection, static_cast<u64>(xcb_win)},
      size.width(), size.height());

  if (!renderer_initialize("SDF Editor Viewport", *platform_)) {
    qFatal("Failed to initialize renderer in SceneViewport.");
  }

  // Opt into the camera-centered chunked/clipmap field (see
  // VulkanRaymarchShader::set_chunked_field_enabled()'s doc comment) so
  // domain repetition and any other geometry keep rendering as far as the
  // viewport camera can see, instead of being clipped to the old fixed
  // [-BOUNDS,BOUNDS] cube. tick() below already calls renderer_set_camera()
  // every frame, which is all update_streaming()/update_gi_cascade() need
  // to follow this viewport around -- no other wiring required. Chunk
  // streaming/GI cascade baking already run unconditionally every frame
  // regardless of this flag (see begin_frame()'s own comment); this only
  // switches which field the render pass actually samples from.
  renderer_set_chunked_field_enabled(true);

  // The engine defaults the reference grid to hidden (games must never
  // draw it); this editor is exactly the tooling it exists for, so apply
  // whatever set_grid_visible() has stored -- true unless the Show Grid
  // button was somehow toggled before the first exposeEvent.
  renderer_set_grid_visible(grid_visible_ ? TRUE : FALSE);

  // Likewise apply whatever set_splat_visibility() has stored (see its own
  // comment) -- the engine defaults to Prime, so this only matters if the
  // toggle was flipped before the first exposeEvent.
  renderer_set_splat_mode(splat_visibility_ ? RendererSplatMode::Visibility
                                            : RendererSplatMode::Prime);

  frame_timer_.start();
  tick_timer_ = new QTimer(this);
  connect(tick_timer_, &QTimer::timeout, this, &SceneViewport::tick);
  tick_timer_->start(16); // ~60Hz

  initialized_ = true;
}

void SceneViewport::exposeEvent(QExposeEvent *event) {
  QWindow::exposeEvent(event);
  if (isExposed()) {
    ensure_engine_initialized();
  }
}

void SceneViewport::resizeEvent(QResizeEvent *event) {
  QWindow::resizeEvent(event);
  if (initialized_ && !shutdown_) {
    QSize size = physical_size();
    renderer_on_resized(static_cast<u16>(size.width()),
                        static_cast<u16>(size.height()));
  }
}

void SceneViewport::tick() {
  if (!initialized_ || shutdown_) {
    return;
  }
  f32 delta_time = static_cast<f32>(frame_timer_.restart()) / 1000.0f;

  // WASD flies along the camera's own forward/right (both horizontal --
  // see Camera::right()), Q/E move straight along world Y regardless of
  // where the camera is looking, matching TestbedGame::update()'s
  // Space/X convention for vertical movement.
  glm::vec3 velocity(0.0f);
  if (key_w_) {
    velocity += camera_.forward();
  }
  if (key_s_) {
    velocity -= camera_.forward();
  }
  if (key_a_) {
    velocity -= camera_.right();
  }
  if (key_d_) {
    velocity += camera_.right();
  }
  if (key_e_) {
    velocity.y += 1.0f;
  }
  if (key_q_) {
    velocity.y -= 1.0f;
  }
  if (glm::length(velocity) > 0.0002f) {
    camera_.move(glm::normalize(velocity) * (kFlyMoveSpeed * delta_time));
  }

  renderer_set_camera(camera_);
  update_gizmo(); // camera may have moved (orbit/zoom) since last tick
  // Must queue before renderer_draw_frame() below -- queued draws are
  // consumed and cleared by end_frame(), so drawing this after would only
  // show up starting next frame.
  draw_gizmo();
  draw_gizmo_drag_indicator();
  render_packet packet{delta_time};
  renderer_draw_frame(&packet);
}

void SceneViewport::set_selection(const std::vector<PrimitiveRef> &selection) {
  selected_ = selection;
  update_gizmo();
  // The renderer's selection outline only highlights one primitive -- use
  // the first entry (matches the old single-selection behaviour exactly
  // when selection.size() == 1). -1 (no outline) if nothing is selected or
  // that primitive isn't currently uploaded.
  if (selected_.empty()) {
    renderer_set_selected_primitive(-1);
  } else {
    const PrimitiveRef &first = selected_.front();
    if (first.is_light()) {
      renderer_set_selected_primitive(-1); // no outline concept for a light
      return;
    }
    if (first.layer_index >= 0 &&
        first.layer_index < static_cast<int>(scene_.layers.size())) {
      const auto &primitives = scene_.layers[first.layer_index].primitives;
      if (first.primitive_index >= 0 &&
          first.primitive_index < static_cast<int>(primitives.size())) {
        renderer_set_selected_primitive(renderer_get_primitive_gpu_index(
            primitives[first.primitive_index].name));
        return;
      }
    }
    renderer_set_selected_primitive(-1);
  }
}

void SceneViewport::set_grid_visible(bool visible) {
  grid_visible_ = visible;
  // Before the first exposeEvent there's no renderer to tell yet --
  // ensure_engine_initialized() applies grid_visible_ once there is.
  if (initialized_) {
    renderer_set_grid_visible(visible ? TRUE : FALSE);
  }
}

void SceneViewport::set_splat_visibility(bool enabled) {
  splat_visibility_ = enabled;
  // Same before-first-exposeEvent caveat as set_grid_visible() above.
  if (initialized_) {
    renderer_set_splat_mode(enabled ? RendererSplatMode::Visibility
                                    : RendererSplatMode::Prime);
  }
}

void SceneViewport::pause_rendering() {
  if (tick_timer_) {
    tick_timer_->stop();
  }
}

void SceneViewport::resume_rendering() {
  if (tick_timer_ && initialized_ && !shutdown_) {
    tick_timer_->start(16); // ~60Hz -- matches ensure_engine_initialized()'s own start()
  }
}

void SceneViewport::mousePressEvent(QMouseEvent *event) {
  // A QWindow doesn't automatically take keyboard focus just from being
  // clicked (unlike a QWidget under a focus policy) -- without this, WASD/
  // Q/E only work once some other event happens to hand this window focus.
  requestActivate();

  // Right-drag orbits; left is reserved entirely for click-to-select/
  // gizmo-drag below -- keeping them on separate buttons means neither
  // needs a click-vs-drag movement-threshold heuristic.
  if (event->button() == Qt::RightButton) {
    orbiting_ = true;
    last_mouse_pos_ = event->position().toPoint();
    return;
  }
  if (event->button() == Qt::LeftButton) {
    QPointF pos = event->position();
    // Ctrl held always means "additive-select," never "drag the gizmo" --
    // skip the hit-test entirely so a ctrl-click on/near the gizmo (common
    // once something's selected: the gizmo sits right where you're likely
    // to be ctrl-clicking a neighbouring/overlapping primitive from another
    // layer) still reaches mouseReleaseEvent's pick_at() instead of being
    // swallowed as a drag start.
    GizmoAxis axis = event->modifiers().testFlag(Qt::ControlModifier)
                         ? GizmoAxis::None
                         : hit_test_gizmo(pos);
    if (axis != GizmoAxis::None) {
      begin_gizmo_drag(axis, pos);
      // Announced from here rather than inside begin_gizmo_drag(), which
      // has several early returns that leave the drag un-started -- this is
      // the one place that knows a drag actually began.
      if (dragging_axis_ != GizmoAxis::None) {
        // Only a lone primitive can go dynamic: the renderer tracks exactly
        // one, and a light has no baked geometry to take out of the bake.
        drag_dynamic_ref_ = PrimitiveRef{};
        if (selected_.size() == 1 && !selected_[0].is_light()) {
          drag_dynamic_ref_ = selected_[0];
        }
        emit gizmo_drag_started(drag_dynamic_ref_);
      }
    }
    // Not a gizmo hit: do nothing on press -- mouseReleaseEvent's pick_at()
    // handles a plain click.
  }
}

void SceneViewport::mouseReleaseEvent(QMouseEvent *event) {
  if (event->button() == Qt::RightButton) {
    orbiting_ = false;
  } else if (event->button() == Qt::LeftButton) {
    if (dragging_axis_ != GizmoAxis::None) {
      end_gizmo_drag();
    } else {
      pick_at(event->position().toPoint(),
             event->modifiers().testFlag(Qt::ControlModifier));
    }
  }
}

void SceneViewport::mouseMoveEvent(QMouseEvent *event) {
  if (orbiting_) {
    QPoint pos = event->position().toPoint();
    QPoint delta = pos - last_mouse_pos_;
    last_mouse_pos_ = pos;
    camera_.yaw(-static_cast<f32>(delta.x()) * kYawSensitivity);
    camera_.pitch(-static_cast<f32>(delta.y()) * kPitchSensitivity);
    return;
  }
  if (dragging_axis_ != GizmoAxis::None) {
    update_gizmo_drag(event->position());
  }
}

void SceneViewport::wheelEvent(QWheelEvent *event) {
  f32 amount = static_cast<f32>(event->angleDelta().y()) * kZoomSpeed;
  camera_.move(camera_.forward() * amount);
}

void SceneViewport::keyPressEvent(QKeyEvent *event) {
  switch (event->key()) {
  case Qt::Key_W:
    key_w_ = true;
    break;
  case Qt::Key_A:
    key_a_ = true;
    break;
  case Qt::Key_S:
    key_s_ = true;
    break;
  case Qt::Key_D:
    key_d_ = true;
    break;
  case Qt::Key_Q:
    key_q_ = true;
    break;
  case Qt::Key_E:
    key_e_ = true;
    break;
  default:
    QWindow::keyPressEvent(event);
    return;
  }
  event->accept();
}

void SceneViewport::keyReleaseEvent(QKeyEvent *event) {
  switch (event->key()) {
  case Qt::Key_W:
    key_w_ = false;
    break;
  case Qt::Key_A:
    key_a_ = false;
    break;
  case Qt::Key_S:
    key_s_ = false;
    break;
  case Qt::Key_D:
    key_d_ = false;
    break;
  case Qt::Key_Q:
    key_q_ = false;
    break;
  case Qt::Key_E:
    key_e_ = false;
    break;
  default:
    QWindow::keyReleaseEvent(event);
    return;
  }
  event->accept();
}

void SceneViewport::focusOutEvent(QFocusEvent *event) {
  key_w_ = key_a_ = key_s_ = key_d_ = key_q_ = key_e_ = false;
  QWindow::focusOutEvent(event);
}

void SceneViewport::pick_at(QPoint pos, bool additive) {
  // Matches Builtin.RaymarchShader.comp.glsl's main() exactly: uv centered
  // on the image, scaled by height only (so it's undistorted regardless of
  // aspect ratio), then the ray direction is built from the camera's own
  // basis rather than fixed world axes.
  QSize size = physical_size();
  f32 px = static_cast<f32>(pos.x()) * static_cast<f32>(devicePixelRatio());
  f32 py = static_cast<f32>(pos.y()) * static_cast<f32>(devicePixelRatio());
  f32 uv_x = (px - 0.5f * static_cast<f32>(size.width())) /
            static_cast<f32>(size.height());
  f32 uv_y = (py - 0.5f * static_cast<f32>(size.height())) /
            static_cast<f32>(size.height());

  glm::vec3 ray_dir = glm::normalize(uv_x * camera_.right() +
                                     uv_y * camera_.up() + camera_.forward());

  std::optional<SceneRayHit> hit =
      raycast_scene(scene_, camera_.position(), ray_dir);

  std::vector<PrimitiveRef> new_selection = selected_;
  if (hit) {
    PrimitiveRef ref{hit->layer_index, hit->primitive_index};
    if (additive) {
      auto it = std::find(new_selection.begin(), new_selection.end(), ref);
      if (it != new_selection.end()) {
        new_selection.erase(it); // ctrl-click an already-selected primitive
                                 // removes it from the selection
      } else {
        new_selection.push_back(ref);
      }
    } else {
      new_selection = {ref};
    }
  } else if (!additive) {
    new_selection.clear(); // plain click on empty space deselects everything
  }
  // A ctrl-click on empty space leaves the existing selection untouched.

  set_selection(new_selection);
  emit selection_changed(selected_);
}

std::optional<QPointF> SceneViewport::project_to_screen(glm::vec3 world_point) const {
  // Inverts pick_at()'s ray construction: given camera_.forward()/right()/
  // up() are orthonormal, dot(uv.x*right + uv.y*up + forward, forward) is
  // always exactly 1 regardless of uv, so v_forward alone gives the scale
  // factor needed to recover uv from an arbitrary world-space vector.
  glm::vec3 v = world_point - camera_.position();
  f32 v_forward = glm::dot(v, camera_.forward());
  if (v_forward <= 0.001f) {
    return std::nullopt; // behind the camera
  }
  f32 uv_x = glm::dot(v, camera_.right()) / v_forward;
  f32 uv_y = glm::dot(v, camera_.up()) / v_forward;

  QSize phys = physical_size();
  f32 px = uv_x * static_cast<f32>(phys.height()) +
          0.5f * static_cast<f32>(phys.width());
  f32 py = uv_y * static_cast<f32>(phys.height()) +
          0.5f * static_cast<f32>(phys.height());

  f32 dpr = static_cast<f32>(devicePixelRatio());
  return QPointF(px / dpr, py / dpr); // back to logical pixels
}

glm::vec3 SceneViewport::gizmo_effective_position(const SdfPrimitiveDef &primitive) {
  if (primitive.type == SdfPrimitiveType::Plane) {
    return glm::vec3(0.0f, primitive.params.x, 0.0f);
  }
  return primitive.position;
}

glm::vec3 SceneViewport::axis_world_direction(GizmoAxis axis) {
  switch (axis) {
  case GizmoAxis::X:
    return glm::vec3(1.0f, 0.0f, 0.0f);
  case GizmoAxis::Y:
    return glm::vec3(0.0f, 1.0f, 0.0f);
  case GizmoAxis::Z:
    return glm::vec3(0.0f, 0.0f, 1.0f);
  case GizmoAxis::None:
  default:
    return glm::vec3(0.0f);
  }
}

std::vector<QPointF> SceneViewport::build_gizmo_ring(glm::vec3 origin_world,
                                                     GizmoAxis axis,
                                                     f32 radius) const {
  constexpr f32 kTwoPi = 6.28318530717958647692f;

  // Two orthonormal directions spanning the plane perpendicular to axis --
  // rotating "around X" sweeps through the YZ plane, etc.
  glm::vec3 u, v;
  switch (axis) {
  case GizmoAxis::X:
    u = glm::vec3(0.0f, 1.0f, 0.0f);
    v = glm::vec3(0.0f, 0.0f, 1.0f);
    break;
  case GizmoAxis::Y:
    u = glm::vec3(0.0f, 0.0f, 1.0f);
    v = glm::vec3(1.0f, 0.0f, 0.0f);
    break;
  case GizmoAxis::Z:
  default:
    u = glm::vec3(1.0f, 0.0f, 0.0f);
    v = glm::vec3(0.0f, 1.0f, 0.0f);
    break;
  }

  std::vector<QPointF> points;
  points.reserve(kGizmoRingSegments + 1);
  for (int i = 0; i <= kGizmoRingSegments; ++i) {
    f32 t = kTwoPi * static_cast<f32>(i) / static_cast<f32>(kGizmoRingSegments);
    glm::vec3 world_point =
        origin_world + radius * (std::cos(t) * u + std::sin(t) * v);
    std::optional<QPointF> screen_point = project_to_screen(world_point);
    if (screen_point) {
      points.push_back(*screen_point);
    }
  }
  return points;
}

std::vector<SdfPrimitiveDef *> SceneViewport::selected_primitives() {
  std::vector<SdfPrimitiveDef *> result;
  result.reserve(selected_.size());
  for (const PrimitiveRef &ref : selected_) {
    if (ref.is_light()) {
      continue;
    }
    if (ref.layer_index < 0 ||
        ref.layer_index >= static_cast<int>(scene_.layers.size())) {
      continue;
    }
    auto &primitives = scene_.layers[ref.layer_index].primitives;
    if (ref.primitive_index < 0 ||
        ref.primitive_index >= static_cast<int>(primitives.size())) {
      continue;
    }
    result.push_back(&primitives[ref.primitive_index]);
  }
  return result;
}

std::vector<SdfLightDef *> SceneViewport::selected_lights() {
  std::vector<SdfLightDef *> result;
  result.reserve(selected_.size());
  for (const PrimitiveRef &ref : selected_) {
    if (!ref.is_light()) {
      continue;
    }
    if (ref.light_index >= static_cast<int>(scene_.lights.size())) {
      continue;
    }
    result.push_back(&scene_.lights[ref.light_index]);
  }
  return result;
}

glm::vec3
SceneViewport::selection_centroid(const std::vector<SdfPrimitiveDef *> &primitives,
                                  const std::vector<SdfLightDef *> &lights) const {
  glm::vec3 sum(0.0f);
  for (const SdfPrimitiveDef *primitive : primitives) {
    sum += gizmo_effective_position(*primitive);
  }
  for (const SdfLightDef *light : lights) {
    sum += light->position;
  }
  size_t count = primitives.size() + lights.size();
  return count == 0 ? sum : sum / static_cast<f32>(count);
}

void SceneViewport::update_gizmo() {
  gizmo_ring_x_.clear();
  gizmo_ring_y_.clear();
  gizmo_ring_z_.clear();
  gizmo_light_marker_visible_ = false;
  gizmo_light_marker_x_.clear();
  gizmo_light_marker_y_.clear();
  gizmo_light_marker_z_.clear();

  std::vector<SdfPrimitiveDef *> primitives = selected_primitives();
  std::vector<SdfLightDef *> lights = selected_lights();
  if (primitives.empty() && lights.empty()) {
    gizmo_visible_ = false;
    return;
  }

  // Rotating an infinite horizontal plane -- or a light, which has no
  // orientation at all (see SdfLightDef) -- is meaningless (see
  // SdfPrimitiveDef::rotation): no rotate gizmo shows at all unless at
  // least one selected primitive can actually be rotated. For a mixed
  // group, the rotate gizmo still shows as long as ANY selected primitive
  // is rotatable (the non-rotatable members -- planes, lights -- just don't
  // move during the drag; see update_gizmo_drag()).
  bool all_planes = !primitives.empty() && lights.empty() &&
                    std::all_of(primitives.begin(), primitives.end(),
                                [](const SdfPrimitiveDef *p) {
                                  return p->type == SdfPrimitiveType::Plane;
                                });
  bool any_rotatable = std::any_of(primitives.begin(), primitives.end(),
                                   [](const SdfPrimitiveDef *p) {
                                     return p->type != SdfPrimitiveType::Plane;
                                   });
  if (gizmo_mode_ == GizmoMode::Rotate && !any_rotatable) {
    gizmo_visible_ = false;
    return;
  }

  glm::vec3 origin_world = selection_centroid(primitives, lights);
  f32 distance = glm::dot(origin_world - camera_.position(), camera_.forward());
  if (distance <= 0.01f) {
    gizmo_visible_ = false;
    return;
  }
  f32 axis_length = std::max(kGizmoMinLength, distance * kGizmoScreenScale);

  std::optional<QPointF> origin_screen = project_to_screen(origin_world);
  if (!origin_screen) {
    gizmo_visible_ = false;
    return;
  }

  if (gizmo_mode_ == GizmoMode::Rotate) {
    gizmo_ring_x_ = build_gizmo_ring(origin_world, GizmoAxis::X, axis_length);
    gizmo_ring_y_ = build_gizmo_ring(origin_world, GizmoAxis::Y, axis_length);
    gizmo_ring_z_ = build_gizmo_ring(origin_world, GizmoAxis::Z, axis_length);
    gizmo_origin_ = *origin_screen;
    gizmo_visible_ = gizmo_ring_x_.size() > 1 || gizmo_ring_y_.size() > 1 ||
                     gizmo_ring_z_.size() > 1;
    return;
  }

  std::optional<QPointF> x_screen =
      all_planes ? std::nullopt
                : project_to_screen(origin_world + glm::vec3(axis_length, 0, 0));
  std::optional<QPointF> y_screen =
      project_to_screen(origin_world + glm::vec3(0, axis_length, 0));
  std::optional<QPointF> z_screen =
      all_planes ? std::nullopt
                : project_to_screen(origin_world + glm::vec3(0, 0, axis_length));

  if (!y_screen || (!all_planes && (!x_screen || !z_screen))) {
    gizmo_visible_ = false;
    return;
  }

  gizmo_visible_ = true;
  gizmo_origin_ = *origin_screen;
  gizmo_x_ = x_screen.value_or(*origin_screen);
  gizmo_y_ = *y_screen;
  gizmo_z_ = z_screen.value_or(*origin_screen);

  if (!lights.empty()) {
    f32 marker_radius = axis_length * 0.3f;
    gizmo_light_marker_x_ = build_gizmo_ring(origin_world, GizmoAxis::X, marker_radius);
    gizmo_light_marker_y_ = build_gizmo_ring(origin_world, GizmoAxis::Y, marker_radius);
    gizmo_light_marker_z_ = build_gizmo_ring(origin_world, GizmoAxis::Z, marker_radius);
    gizmo_light_marker_visible_ = gizmo_light_marker_x_.size() > 1 ||
                                  gizmo_light_marker_y_.size() > 1 ||
                                  gizmo_light_marker_z_.size() > 1;
  }
}

void SceneViewport::draw_gizmo() const {
  if (!gizmo_visible_) {
    return;
  }
  f32 dpr = static_cast<f32>(devicePixelRatio());
  auto to_physical = [dpr](QPointF p) {
    return glm::vec2(static_cast<f32>(p.x()) * dpr, static_cast<f32>(p.y()) * dpr);
  };
  auto draw_ring = [&](const std::vector<QPointF> &ring, glm::vec4 colour) {
    for (size_t i = 1; i < ring.size(); ++i) {
      renderer_draw_line(to_physical(ring[i - 1]), to_physical(ring[i]),
                         colour);
    }
  };

  if (gizmo_mode_ == GizmoMode::Rotate) {
    draw_ring(gizmo_ring_x_, glm::vec4(0.9f, 0.24f, 0.24f, 1.0f));  // X = red
    draw_ring(gizmo_ring_y_, glm::vec4(0.24f, 0.78f, 0.35f, 1.0f)); // Y = green
    draw_ring(gizmo_ring_z_, glm::vec4(0.27f, 0.51f, 0.9f, 1.0f));  // Z = blue
    return;
  }

  glm::vec2 origin = to_physical(gizmo_origin_);
  if (gizmo_x_ != gizmo_origin_) {
    renderer_draw_line(origin, to_physical(gizmo_x_),
                       glm::vec4(0.9f, 0.24f, 0.24f, 1.0f)); // X = red
  }
  if (gizmo_y_ != gizmo_origin_) {
    renderer_draw_line(origin, to_physical(gizmo_y_),
                       glm::vec4(0.24f, 0.78f, 0.35f, 1.0f)); // Y = green
  }
  if (gizmo_z_ != gizmo_origin_) {
    renderer_draw_line(origin, to_physical(gizmo_z_),
                       glm::vec4(0.27f, 0.51f, 0.9f, 1.0f)); // Z = blue
  }

  if (gizmo_light_marker_visible_) {
    glm::vec4 bulb_colour(1.0f, 0.85f, 0.2f, 1.0f); // warm yellow, like a bulb
    draw_ring(gizmo_light_marker_x_, bulb_colour);
    draw_ring(gizmo_light_marker_y_, bulb_colour);
    draw_ring(gizmo_light_marker_z_, bulb_colour);
  }
}

void SceneViewport::draw_gizmo_drag_indicator() const {
  if (dragging_axis_ == GizmoAxis::None || drag_mode_ != GizmoMode::Rotate) {
    return;
  }

  const char *axis_name = "?";
  glm::vec4 colour(1.0f);
  switch (dragging_axis_) {
  case GizmoAxis::X:
    axis_name = "X";
    colour = glm::vec4(0.9f, 0.24f, 0.24f, 1.0f);
    break;
  case GizmoAxis::Y:
    axis_name = "Y";
    colour = glm::vec4(0.24f, 0.78f, 0.35f, 1.0f);
    break;
  case GizmoAxis::Z:
    axis_name = "Z";
    colour = glm::vec4(0.27f, 0.51f, 0.9f, 1.0f);
    break;
  case GizmoAxis::None:
    return;
  }

  char text[32];
  std::snprintf(text, sizeof(text), "%s %+.1f deg", axis_name,
               static_cast<double>(drag_delta_degrees_));

  f32 dpr = static_cast<f32>(devicePixelRatio());
  // Offset a little down-right of the cursor so the text doesn't sit
  // directly under it.
  glm::vec2 position(static_cast<f32>(drag_indicator_pos_.x()) * dpr + 18.0f,
                     static_cast<f32>(drag_indicator_pos_.y()) * dpr + 18.0f);
  renderer_draw_text(text, position, colour);
}

namespace {
f32 polyline_distance(QPointF p, const std::vector<QPointF> &polyline) {
  f32 best = std::numeric_limits<f32>::infinity();
  for (size_t i = 1; i < polyline.size(); ++i) {
    best = std::min(best, point_segment_distance(p, polyline[i - 1], polyline[i]));
  }
  return best;
}
} // namespace

GizmoAxis SceneViewport::hit_test_gizmo(QPointF pos) const {
  if (!gizmo_visible_) {
    return GizmoAxis::None;
  }

  if (gizmo_mode_ == GizmoMode::Rotate) {
    struct RingCandidate {
      GizmoAxis axis;
      const std::vector<QPointF> *ring;
    };
    const RingCandidate candidates[] = {{GizmoAxis::X, &gizmo_ring_x_},
                                       {GizmoAxis::Y, &gizmo_ring_y_},
                                       {GizmoAxis::Z, &gizmo_ring_z_}};
    GizmoAxis best = GizmoAxis::None;
    f32 best_dist = kGizmoHitTolerancePx;
    for (const RingCandidate &candidate : candidates) {
      f32 dist = polyline_distance(pos, *candidate.ring);
      if (dist < best_dist) {
        best_dist = dist;
        best = candidate.axis;
      }
    }
    return best;
  }

  struct Candidate {
    GizmoAxis axis;
    QPointF end;
  };
  const Candidate candidates[] = {
      {GizmoAxis::X, gizmo_x_}, {GizmoAxis::Y, gizmo_y_}, {GizmoAxis::Z, gizmo_z_}};

  GizmoAxis best = GizmoAxis::None;
  f32 best_dist = kGizmoHitTolerancePx;
  for (const Candidate &candidate : candidates) {
    if (candidate.end == gizmo_origin_) {
      continue; // hidden (degenerate) axis -- e.g. X/Z for a Plane
    }
    f32 dist = point_segment_distance(pos, gizmo_origin_, candidate.end);
    if (dist < best_dist) {
      best_dist = dist;
      best = candidate.axis;
    }
  }
  return best;
}

void SceneViewport::begin_gizmo_drag(GizmoAxis axis, QPointF mouse_pos) {
  std::vector<SdfPrimitiveDef *> primitives = selected_primitives();
  std::vector<SdfLightDef *> lights = selected_lights();
  if (primitives.empty() && lights.empty()) {
    return;
  }

  glm::vec3 origin_world = selection_centroid(primitives, lights);
  std::optional<QPointF> origin_screen = project_to_screen(origin_world);
  if (!origin_screen) {
    return; // degenerate (origin behind the camera)
  }

  if (gizmo_mode_ == GizmoMode::Rotate) {
    QPointF to_mouse = mouse_pos - *origin_screen;
    f32 dist = static_cast<f32>(std::hypot(to_mouse.x(), to_mouse.y()));
    if (dist < 0.5f) {
      return; // degenerate -- mouse right on top of the gizmo's origin
    }
    dragging_axis_ = axis;
    drag_mode_ = GizmoMode::Rotate;
    drag_start_mouse_ = mouse_pos;
    drag_group_pivot_ = origin_world;
    drag_start_positions_.clear();
    drag_start_rotations_.clear();
    drag_start_light_positions_.clear(); // unused in Rotate -- see update_gizmo_drag()
    for (SdfPrimitiveDef *primitive : primitives) {
      drag_start_positions_.push_back(primitive->position);
      drag_start_rotations_.push_back(primitive->rotation);
    }
    drag_start_angle_ = std::atan2(static_cast<f32>(to_mouse.y()),
                                   static_cast<f32>(to_mouse.x()));
    // Fixed for the whole drag -- see drag_radius_screen_'s comment for why
    // this must not be recomputed from the mouse's live distance to the
    // origin every frame.
    drag_radius_screen_ = std::max(dist, kGizmoRotateMinRadiusPx);
    drag_last_mouse_pos_ = mouse_pos;
    drag_accumulated_angle_ = 0.0f;
    drag_delta_degrees_ = 0.0f;
    drag_indicator_pos_ = mouse_pos;
    return;
  }

  glm::vec3 axis_dir = axis_world_direction(axis);
  std::optional<QPointF> p1 = project_to_screen(origin_world + axis_dir);
  if (!p1) {
    return; // degenerate (axis pointing straight at/away from the camera)
  }
  QPointF screen_delta = *p1 - *origin_screen;
  f32 span = static_cast<f32>(
      std::hypot(screen_delta.x(), screen_delta.y()));
  if (span < 0.5f) {
    return;
  }

  dragging_axis_ = axis;
  drag_mode_ = GizmoMode::Translate;
  drag_start_mouse_ = mouse_pos;
  drag_start_positions_.clear();
  drag_start_params_.clear();
  for (SdfPrimitiveDef *primitive : primitives) {
    drag_start_positions_.push_back(primitive->position);
    drag_start_params_.push_back(primitive->params);
  }
  drag_start_light_positions_.clear();
  for (SdfLightDef *light : lights) {
    drag_start_light_positions_.push_back(light->position);
  }
  drag_screen_axis_dir_ = QPointF(screen_delta.x() / span, screen_delta.y() / span);
  drag_world_per_pixel_ = 1.0f / span;
}

void SceneViewport::update_gizmo_drag(QPointF mouse_pos) {
  std::vector<SdfPrimitiveDef *> primitives = selected_primitives();
  std::vector<SdfLightDef *> lights = selected_lights();
  if ((primitives.empty() && lights.empty()) ||
      primitives.size() != drag_start_positions_.size()) {
    dragging_axis_ = GizmoAxis::None;
    return;
  }
  // drag_start_light_positions_ is only ever populated for a Translate drag
  // (see begin_gizmo_drag() -- a light never rotates), so only check it
  // against the live light selection in that mode; a Rotate drag never
  // reads it at all.
  if (drag_mode_ == GizmoMode::Translate &&
      lights.size() != drag_start_light_positions_.size()) {
    dragging_axis_ = GizmoAxis::None;
    return;
  }

  if (drag_mode_ == GizmoMode::Rotate) {
    // Project this frame's raw mouse movement onto the tangent direction
    // of a fixed-radius circle (drag_radius_screen_, set once at drag
    // start) at the current accumulated angle, then divide by that same
    // fixed radius to get an angular step -- see drag_radius_screen_'s
    // comment in the header for why this must not recompute
    // atan2(mouse - origin) fresh from the mouse's live (and possibly
    // near-zero) distance to the origin every frame instead.
    QPointF delta_mouse = mouse_pos - drag_last_mouse_pos_;
    f32 current_angle = drag_start_angle_ + drag_accumulated_angle_;
    f32 tangent_x = -std::sin(current_angle);
    f32 tangent_y = std::cos(current_angle);
    f32 tangential_pixels = static_cast<f32>(delta_mouse.x()) * tangent_x +
                           static_cast<f32>(delta_mouse.y()) * tangent_y;
    drag_accumulated_angle_ += tangential_pixels / drag_radius_screen_;
    drag_last_mouse_pos_ = mouse_pos;

    // One delta rotation, shared by every selected primitive: each one's
    // own orientation is composed with it, and its position orbits the
    // group's pivot by the same amount -- mirrors
    // VulkanRendererBackend::rotate_scene()'s own "orbit + compose"
    // approach for rotating a set of primitives together. For a single-
    // primitive selection this reduces to exactly the old behaviour: the
    // pivot equals that primitive's own position, so orbiting it is a
    // no-op and only its own rotation changes.
    glm::quat delta_quat =
        glm::angleAxis(drag_accumulated_angle_, axis_world_direction(dragging_axis_));

    for (size_t i = 0; i < primitives.size(); ++i) {
      SdfPrimitiveDef *primitive = primitives[i];
      if (primitive->type == SdfPrimitiveType::Plane) {
        continue; // an infinite plane has no orientation to rotate -- see
                  // update_gizmo()'s all_planes handling
      }
      primitive->position =
          drag_group_pivot_ +
          delta_quat * (drag_start_positions_[i] - drag_group_pivot_);
      primitive->rotation =
          glm::eulerAngles(delta_quat * glm::quat(drag_start_rotations_[i]));
    }

    drag_delta_degrees_ = glm::degrees(drag_accumulated_angle_);
    drag_indicator_pos_ = mouse_pos;

    update_gizmo(); // reflect the new rotation in the gizmo rings now, not
                    // just next tick -- keeps the drag feeling responsive
    return;
  }

  QPointF delta = mouse_pos - drag_start_mouse_;
  f32 pixels_along =
      static_cast<f32>(delta.x()) * static_cast<f32>(drag_screen_axis_dir_.x()) +
      static_cast<f32>(delta.y()) * static_cast<f32>(drag_screen_axis_dir_.y());
  f32 world_delta = pixels_along * drag_world_per_pixel_;
  glm::vec3 axis_dir = axis_world_direction(dragging_axis_);

  for (size_t i = 0; i < primitives.size(); ++i) {
    SdfPrimitiveDef *primitive = primitives[i];
    if (primitive->type == SdfPrimitiveType::Plane) {
      // Only Y is meaningful for a plane -- height lives in params.x, not
      // position.y (see gizmo_effective_position()).
      if (dragging_axis_ == GizmoAxis::Y) {
        primitive->params.x = drag_start_params_[i].x + world_delta;
      }
    } else {
      primitive->position = drag_start_positions_[i] + axis_dir * world_delta;
    }
  }
  for (size_t i = 0; i < lights.size(); ++i) {
    // A point light's position is a plain vec3 -- no plane-style restriction
    // to worry about, every axis applies.
    lights[i]->position = drag_start_light_positions_[i] + axis_dir * world_delta;
  }

  update_gizmo(); // reflect the new position/height in the gizmo lines now,
                  // not just next tick -- keeps the drag feeling responsive
  // Read the just-updated transform out of THIS class's scene copy and send
  // it -- the window's copy is not updated until the drag ends, so anything
  // that re-serialises that copy mid-drag would send the pre-drag transform
  // and nothing would appear to move.
  if (drag_dynamic_ref_.layer_index >= 0 &&
      drag_dynamic_ref_.layer_index < static_cast<int>(scene_.layers.size())) {
    const auto &layer_primitives =
        scene_.layers[drag_dynamic_ref_.layer_index].primitives;
    if (drag_dynamic_ref_.primitive_index >= 0 &&
        drag_dynamic_ref_.primitive_index <
            static_cast<int>(layer_primitives.size())) {
      const SdfPrimitiveDef &moved =
          layer_primitives[drag_dynamic_ref_.primitive_index];
      emit gizmo_drag_moved(GizmoTransformResult{
          drag_dynamic_ref_, moved.position, moved.rotation, moved.params});
    }
  }
}

void SceneViewport::end_gizmo_drag() {
  if (dragging_axis_ == GizmoAxis::None) {
    return;
  }
  dragging_axis_ = GizmoAxis::None;
  // Before the transform is committed below, so the primitive is handed
  // back to the bake first and the commit re-bakes it exactly once.
  emit gizmo_drag_ended();
  drag_dynamic_ref_ = PrimitiveRef{};

  std::vector<GizmoTransformResult> results;
  for (const PrimitiveRef &ref : selected_) {
    if (ref.is_light()) {
      if (ref.light_index >= static_cast<int>(scene_.lights.size())) {
        continue;
      }
      const SdfLightDef &light = scene_.lights[ref.light_index];
      results.push_back(
          GizmoTransformResult{ref, light.position, glm::vec3(0.0f), glm::vec3(0.0f)});
      continue;
    }
    if (ref.layer_index < 0 || ref.layer_index >= static_cast<int>(scene_.layers.size())) {
      continue;
    }
    const auto &layer_primitives = scene_.layers[ref.layer_index].primitives;
    if (ref.primitive_index < 0 ||
        ref.primitive_index >= static_cast<int>(layer_primitives.size())) {
      continue;
    }
    const SdfPrimitiveDef &primitive = layer_primitives[ref.primitive_index];
    results.push_back(
        GizmoTransformResult{ref, primitive.position, primitive.rotation, primitive.params});
  }
  if (!results.empty()) {
    emit primitives_transformed(results);
  }
}
