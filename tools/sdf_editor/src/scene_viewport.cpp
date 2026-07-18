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

  // The engine defaults the reference grid to hidden (games must never
  // draw it); this editor is exactly the tooling it exists for, so apply
  // whatever set_grid_visible() has stored -- true unless the Show Grid
  // button was somehow toggled before the first exposeEvent.
  renderer_set_grid_visible(grid_visible_ ? TRUE : FALSE);

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

void SceneViewport::set_selected_layer(int layer_index) {
  selected_layer_index_ = layer_index;
  update_gizmo();
  renderer_set_selected_primitive(layer_index);
}

void SceneViewport::set_grid_visible(bool visible) {
  grid_visible_ = visible;
  // Before the first exposeEvent there's no renderer to tell yet --
  // ensure_engine_initialized() applies grid_visible_ once there is.
  if (initialized_) {
    renderer_set_grid_visible(visible ? TRUE : FALSE);
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
    GizmoAxis axis = hit_test_gizmo(pos);
    if (axis != GizmoAxis::None) {
      begin_gizmo_drag(axis, pos);
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
      pick_at(event->position().toPoint());
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

void SceneViewport::pick_at(QPoint pos) {
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
  int layer_index = hit ? hit->layer_index : -1;
  set_selected_layer(layer_index);
  emit primitive_picked(layer_index);
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

SdfPrimitiveDef *SceneViewport::selected_primitive() {
  if (selected_layer_index_ < 0 ||
      selected_layer_index_ >= static_cast<int>(scene_.layers.size())) {
    return nullptr;
  }
  auto &primitives = scene_.layers[selected_layer_index_].primitives;
  if (primitives.empty()) {
    return nullptr;
  }
  return &primitives.front();
}

void SceneViewport::update_gizmo() {
  gizmo_ring_x_.clear();
  gizmo_ring_y_.clear();
  gizmo_ring_z_.clear();

  SdfPrimitiveDef *primitive = selected_primitive();
  if (!primitive) {
    gizmo_visible_ = false;
    return;
  }

  // Rotating an infinite horizontal plane is meaningless (see
  // SdfPrimitiveDef::rotation) -- no rotate gizmo for one at all, matching
  // how the translate gizmo below already restricts a plane to Y only.
  bool is_plane = primitive->type == SdfPrimitiveType::Plane;
  if (gizmo_mode_ == GizmoMode::Rotate && is_plane) {
    gizmo_visible_ = false;
    return;
  }

  glm::vec3 origin_world = gizmo_effective_position(*primitive);
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
      is_plane ? std::nullopt
              : project_to_screen(origin_world + glm::vec3(axis_length, 0, 0));
  std::optional<QPointF> y_screen =
      project_to_screen(origin_world + glm::vec3(0, axis_length, 0));
  std::optional<QPointF> z_screen =
      is_plane ? std::nullopt
              : project_to_screen(origin_world + glm::vec3(0, 0, axis_length));

  if (!y_screen || (!is_plane && (!x_screen || !z_screen))) {
    gizmo_visible_ = false;
    return;
  }

  gizmo_visible_ = true;
  gizmo_origin_ = *origin_screen;
  gizmo_x_ = x_screen.value_or(*origin_screen);
  gizmo_y_ = *y_screen;
  gizmo_z_ = z_screen.value_or(*origin_screen);
}

void SceneViewport::draw_gizmo() const {
  if (!gizmo_visible_) {
    return;
  }
  f32 dpr = static_cast<f32>(devicePixelRatio());
  auto to_physical = [dpr](QPointF p) {
    return glm::vec2(static_cast<f32>(p.x()) * dpr, static_cast<f32>(p.y()) * dpr);
  };

  if (gizmo_mode_ == GizmoMode::Rotate) {
    auto draw_ring = [&](const std::vector<QPointF> &ring, glm::vec4 colour) {
      for (size_t i = 1; i < ring.size(); ++i) {
        renderer_draw_line(to_physical(ring[i - 1]), to_physical(ring[i]),
                           colour);
      }
    };
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
  SdfPrimitiveDef *primitive = selected_primitive();
  if (!primitive) {
    return;
  }

  glm::vec3 origin_world = gizmo_effective_position(*primitive);
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
    drag_start_rotation_ = primitive->rotation;
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
  drag_start_position_ = primitive->position;
  drag_start_params_ = primitive->params;
  drag_screen_axis_dir_ = QPointF(screen_delta.x() / span, screen_delta.y() / span);
  drag_world_per_pixel_ = 1.0f / span;
}

void SceneViewport::update_gizmo_drag(QPointF mouse_pos) {
  SdfPrimitiveDef *primitive = selected_primitive();
  if (!primitive) {
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

    glm::vec3 rotation = drag_start_rotation_;
    switch (dragging_axis_) {
    case GizmoAxis::X:
      rotation.x += drag_accumulated_angle_;
      break;
    case GizmoAxis::Y:
      rotation.y += drag_accumulated_angle_;
      break;
    case GizmoAxis::Z:
      rotation.z += drag_accumulated_angle_;
      break;
    case GizmoAxis::None:
      break;
    }
    primitive->rotation = rotation;

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

  if (primitive->type == SdfPrimitiveType::Plane) {
    // Only Y is meaningful for a plane -- height lives in params.x, not
    // position.y (see gizmo_effective_position()).
    if (dragging_axis_ == GizmoAxis::Y) {
      primitive->params.x = drag_start_params_.x + world_delta;
    }
  } else {
    glm::vec3 axis_dir = axis_world_direction(dragging_axis_);
    primitive->position = drag_start_position_ + axis_dir * world_delta;
  }

  update_gizmo(); // reflect the new position/height in the gizmo lines now,
                  // not just next tick -- keeps the drag feeling responsive
}

void SceneViewport::end_gizmo_drag() {
  if (dragging_axis_ == GizmoAxis::None) {
    return;
  }
  dragging_axis_ = GizmoAxis::None;

  SdfPrimitiveDef *primitive = selected_primitive();
  if (primitive) {
    emit primitive_transformed(selected_layer_index_, primitive->position,
                               primitive->rotation, primitive->params);
  }
}
