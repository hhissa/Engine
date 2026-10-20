#include "scene_viewport.h"
#include "sculpt_shared.h"

#include <core/application.h>
#include <core/logger.h>
#include <platform/platform.h>
#include <renderer/renderer_frontend.h>

#include <QCursor>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QTimer>
#include <QWheelEvent>
#include <QtGui/qguiapplication_platform.h>

#include <xcb/xcb.h>

#include <algorithm>
#include <cmath>

namespace {
using sculpt_shared::apply_stretch;
using sculpt_shared::brush_params_from_size;

constexpr f32 kYawSensitivity = 0.005f;
constexpr f32 kPitchSensitivity = 0.005f;
constexpr f32 kZoomSpeed = 0.0015f;
// units/sec -- matches TestbedGame::update()'s own tuning for this engine's
// ~2-unit-radius scene scale (and tools/sdf_editor's identical constant,
// see git history).
constexpr f32 kFlyMoveSpeed = 1.5f;

// M3a (Smear): a left-drag only lays down a fresh stamp once the candidate
// center has moved at least this fraction of the current brush size since
// the stroke's last stamp -- matches the plan's own suggested default
// (joyful-skipping-giraffe.md's §4 Smear row references "0.25 x brush
// radius"). brush_size_ is used as the stand-in for "radius": per
// brush_params_from_size() above, size is every shape's dominant extent
// (the sphere's actual radius, and the primary half-extent/major axis for
// everything else), so it is the closest single number this generic one-
// slider brush panel has to a shape-agnostic radius. Keeping this a world-
// space distance (rather than a screen-space pixel delta) means the
// threshold scales correctly with brush size and stays meaningful
// regardless of camera zoom -- a bigger brush should need a
// proportionally bigger move before it re-stamps, which a fixed pixel
// threshold would not give for free. Stamping on every pixel of movement
// would be both wasteful (stamp_shape() is a synchronous, device-stalling
// GPU round trip per its own header comment) and visually redundant
// (overlapping stamps this close together add nothing to the surface).
constexpr f32 kSmearMoveThresholdFactor = 0.25f;

// M3b (Clone): a left-drag WITH Clone mode on re-stamps the captured
// clone_source_ at a FIXED spacing along the drag path, deliberately
// different from Smear's kSmearMoveThresholdFactor above -- Smear's
// threshold is tuned to be the smallest gap that avoids wasteful redundant
// stamps while still reading as one continuous blob (0.25x radius means
// heavy overlap between consecutive stamps). Clone wants the opposite
// visual: separated, individually-recognizable copies of the captured
// shape, not a blended trail. Two same-"size" shapes stop overlapping once
// their centers are roughly 2x size apart (size is each shape's own
// dominant extent -- see brush_params_from_size()'s own comment, e.g. a
// sphere's actual radius); 2.5x leaves a small, clearly visible gap on top
// of that so the eye reads "a row of copies", matching the plan's own
// suggestion of spacing "proportional to the captured shape's own size".
// Like kSmearMoveThresholdFactor, kept as a world-space (not screen-space
// pixel) distance so it scales correctly with the captured shape's size
// regardless of camera zoom.
constexpr f32 kCloneSpacingFactor = 2.5f;

// M3c (Spraypaint): a left-drag with Spraypaint mode on re-sprays once the
// candidate center has moved at least this fraction of the current brush
// size since the drag's last spray pass -- same world-space-distance-not-
// screen-space-pixels reasoning, and the same 0.25x default, as Smear's
// kSmearMoveThresholdFactor above. Spraypaint's repeated-pass opacity
// buildup wants exactly Smear's "heavy overlap reads as one continuous
// area" cadence (not Clone's deliberately-separated-copies spacing), just
// accumulating colour instead of blending geometry.
constexpr f32 kSprayMoveThresholdFactor = 0.25f;
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

  if (!renderer_initialize("Modelling Software v2 Viewport", *platform_)) {
    qFatal("Failed to initialize renderer in SceneViewport.");
  }

  // Chunked field on, default sky + lights -- see sculpt_shared.h.
  sculpt_shared::setup_sculpt_environment();

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
  // where the camera is looking.
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
  // M7 (live brush-cursor preview): recomputed every tick (camera may have
  // orbited/zoomed since the last mouse move -- see update_brush_preview()'s
  // own comment); the shaded ghost itself is renderer-side persistent GPU
  // state (set via renderer_sculpt_preview_brush() inside that call), not a
  // per-frame draw queued here the way the now-removed wireframe outline
  // used to be.
  update_brush_preview();
  render_packet packet{delta_time};
  renderer_draw_frame(&packet);
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

  // Right-drag orbits; left starts a stamp stroke (M3a: Smear) -- keeping
  // them on separate buttons means neither needs to disambiguate against
  // the other.
  if (event->button() == Qt::RightButton) {
    orbiting_ = true;
    last_mouse_pos_ = event->position().toPoint();
  } else if (event->button() == Qt::LeftButton && initialized_ &&
            !shutdown_ && pending_move_edit_index_.has_value()) {
    // M4c (Move-of-an-arbitrary-edit): checked BEFORE every other left-
    // button branch below -- an armed begin_move_selected_edit() call is a
    // one-shot, explicitly-requested action (the user just clicked "Move
    // Selected Edit"), so it always wins the very next press regardless of
    // which other mode checkbox (Spraypaint/Move/Clone) happens to be on,
    // and is consumed (cleared) right here so a SECOND press without
    // another button click falls through to whatever mode is actually
    // selected. Nothing is undone/re-stamped at press time -- the target
    // edit stays exactly where it is on the GPU until mouseReleaseEvent
    // commits the new position (see its own comment for why commit-on-
    // release, same reasoning M4a's tail-only Move already uses).
    moving_edit_dragging_ = true;
    moving_edit_index_ = *pending_move_edit_index_;
    pending_move_edit_index_.reset();
  } else if (event->button() == Qt::LeftButton && initialized_ &&
            !shutdown_ && spray_mode_) {
    // M3c (Spraypaint): checked BEFORE clone_mode_ below -- see
    // set_spray_mode()'s own comment on why Spraypaint wins if a caller
    // somehow left both on. Completely separate drag-state pair
    // (spray_dragging_/last_spray_center_) from the Smear/Clone path below
    // -- left_dragging_ is deliberately left false here, so mouseMoveEvent/
    // mouseReleaseEvent's Smear/Clone branch never runs for this press.
    spray_dragging_ = true;
    glm::vec3 center = compute_ray_center(event->position().toPoint());
    place_spray(center);
    last_spray_center_ = center;
  } else if (event->button() == Qt::LeftButton && initialized_ &&
            !shutdown_ && move_mode_) {
    // M4a (Move): checked BEFORE the Smear/Clone branch below, same
    // priority reasoning as Spraypaint's own comment just above -- ON
    // means a left-press picks up the most recently placed stamp instead
    // of placing a new one. Nothing is re-stamped here: move_payload_ just
    // holds the picked-up configuration until mouseReleaseEvent commits it
    // at wherever the drag ends (see place_move_stamp()'s own comment on
    // why commit-on-release). move_dragging_/move_payload_ are a
    // completely separate pair from left_dragging_/spray_dragging_ above,
    // same "independent booleans, no interference" pattern every other
    // drag-mode pair here already follows.
    if (!move_pickup_callback_) {
      return;
    }
    std::optional<SculptStampRecord> picked_up = move_pickup_callback_();
    if (!picked_up.has_value()) {
      // Nothing to move (document_ empty, or the engine-side undo
      // unexpectedly failed) -- deliberate no-op drag, mirroring Clone's
      // clone_source_missing() precedent exactly: move_dragging_ is never
      // set, so mouseMoveEvent/mouseReleaseEvent do nothing for this press
      // either (no stray stroke_ended(0)).
      emit move_source_missing();
      return;
    }
    move_dragging_ = true;
    move_payload_ = *picked_up;
  } else if (event->button() == Qt::LeftButton && initialized_ &&
            !shutdown_) {
    // M3b: Clone mode on with nothing captured is a deliberate no-op --
    // left_dragging_ is never set, so mouseMoveEvent/mouseReleaseEvent do
    // nothing for this press either (no stray stroke_ended(0)). Tell the
    // user why, rather than silently doing nothing or silently falling
    // back to Smear (see clone_source_missing()'s own comment).
    if (clone_mode_ && !clone_source_.has_value()) {
      emit clone_source_missing();
      return;
    }
    // M3a: stamp immediately at the press point (not deferred to release
    // as pre-M3a did) so a plain click-without-moving still places exactly
    // one stamp, and so a drag's very first stamp lands right where the
    // button went down rather than skipping it. last_stamp_center_ then
    // becomes mouseMoveEvent()'s baseline for the movement threshold.
    // Guarded on initialized_/shutdown_ here (rather than relying solely
    // on place_stamp()'s own guard) so left_dragging_ never gets set --
    // and stroke_ended() never fires -- for a press the engine couldn't
    // actually service, which would otherwise report a stroke of stamps
    // that never happened.
    left_dragging_ = true;
    stroke_stamp_count_ = 0;
    // Every stroke is its own object -- the unit the controller mode's
    // grab/rotate acts on.
    renderer_sculpt_begin_object();
    glm::vec3 center = compute_ray_center(event->position().toPoint());
    // M3b: Clone mode stamps the FROZEN clone_source_ instead of the live
    // brush panel; either way the call goes through a shared helper that
    // emits stamp_placed() the same way, which is exactly what keeps
    // ModellingWindow's document_/undo_group_sizes_/stroke grouping (see
    // main_window.cpp's on_stroke_ended()) correct for Clone drags with no
    // changes needed there -- it only ever counts stamp_placed() signals
    // between press and release, and never cares which path produced them.
    if (clone_mode_) {
      place_clone_stamp(center);
    } else {
      place_stamp(center);
    }
    last_stamp_center_ = center;
    stroke_stamp_count_ = 1;
  }
}

void SceneViewport::mouseReleaseEvent(QMouseEvent *event) {
  if (event->button() == Qt::RightButton) {
    orbiting_ = false;
  } else if (event->button() == Qt::LeftButton && moving_edit_dragging_) {
    // M4c: commit-on-release, same reasoning M4a's tail-only Move already
    // documents (no gizmo/live-preview mechanism exists in this tool, so a
    // single commit at release time loses nothing it had). Unlike M4a's
    // Move, nothing was undone at press time -- the target edit is still
    // exactly where it was until this one renderer_sculpt_move_edit() call,
    // which restores-and-replays the engine's checkpoint ring around
    // `moving_edit_index_` with the new center, same machinery delete_edit()
    // established. Only emits edit_moved() (which ModellingWindow uses to
    // update its own document_) if the engine confirms the move actually
    // happened -- same "never claim more than actually happened" discipline
    // every other sculpt-family call site in this tool follows.
    moving_edit_dragging_ = false;
    const int index = moving_edit_index_;
    moving_edit_index_ = -1;
    glm::vec3 center = compute_ray_center(event->position().toPoint());
    if (renderer_sculpt_move_edit(index, center)) {
      emit edit_moved(index, center);
    }
  } else if (event->button() == Qt::LeftButton && spray_dragging_) {
    // M3c: no stroke_ended() -- see place_spray()'s own comment on why a
    // spray drag never reports through the undo-grouping signals.
    spray_dragging_ = false;
  } else if (event->button() == Qt::LeftButton && move_dragging_) {
    // M4a (Move): commit-on-release, not continuously during the drag --
    // simpler and cheaper than re-stamping on every qualifying mouseMove
    // (which would mean an undo+restamp GPU round trip per sub-step, on
    // top of stamp_shape()'s own documented device-stalling cost -- see
    // renderer_sculpt_stamp_shape()'s own comment), at the cost of no live
    // preview while dragging (there is no gizmo/preview mechanism in this
    // tool at all to begin with, so this loses nothing this tool already
    // had). The picked-up shape re-appears exactly once, at wherever the
    // cursor is when the button comes up.
    move_dragging_ = false;
    glm::vec3 center = compute_ray_center(event->position().toPoint());
    place_move_stamp(center);
    move_payload_.reset();
    // A Move is exactly ONE committed stamp regardless of how far the drag
    // travelled (see place_move_stamp()'s own comment) -- always reports a
    // stroke of 1, same as a plain click, so ModellingWindow's undo_group_
    // sizes_ treats "undo the Move" as a single renderer_sculpt_undo()
    // call.
    emit stroke_ended(1);
  } else if (event->button() == Qt::LeftButton && left_dragging_) {
    // M3a: ends the stroke and reports its total stamp count so
    // ModellingWindow can group every stamp placed between the matching
    // press and this release into one undoable unit (see stroke_ended()'s
    // own comment in scene_viewport.h).
    left_dragging_ = false;
    emit stroke_ended(stroke_stamp_count_);
    stroke_stamp_count_ = 0;
  }
}

void SceneViewport::mouseMoveEvent(QMouseEvent *event) {
  if (orbiting_) {
    QPoint pos = event->position().toPoint();
    QPoint delta = pos - last_mouse_pos_;
    last_mouse_pos_ = pos;
    camera_.yaw(-static_cast<f32>(delta.x()) * kYawSensitivity);
    camera_.pitch(-static_cast<f32>(delta.y()) * kPitchSensitivity);
  }

  // M3c (Spraypaint): while the left button is held in a spray drag,
  // re-spray along the drag path once the candidate center has moved far
  // enough since the drag's last pass -- see kSprayMoveThresholdFactor's
  // own comment. Independent of orbiting_/left_dragging_ below, same
  // "separate booleans, no interference" reasoning as those two already
  // have relative to each other.
  if (spray_dragging_) {
    glm::vec3 candidate = compute_ray_center(event->position().toPoint());
    const f32 threshold = kSprayMoveThresholdFactor * brush_size_;
    if (glm::length(candidate - last_spray_center_) >= threshold) {
      place_spray(candidate);
      last_spray_center_ = candidate;
    }
  }

  // M3a (Smear): while the left button is held, re-stamp along the drag
  // path, but only once the candidate center has moved far enough since
  // the stroke's last stamp (see kSmearMoveThresholdFactor's own comment).
  // orbiting_ and left_dragging_ are independent booleans (one per button),
  // so both branches above/below can fire off the same move event without
  // interfering -- e.g. a right-drag orbit never trips this, and this
  // never touches camera_.
  if (left_dragging_) {
    glm::vec3 candidate = compute_ray_center(event->position().toPoint());
    // M3b: clone_mode_ is read live here (same as brush_size_ is for
    // Smear) rather than latched at press time, so toggling the checkbox
    // mid-drag takes effect on the very next sub-stamp. The only edge case
    // that needs an explicit guard is clone_mode_ having been flipped ON
    // mid-drag with no source ever captured -- mousePressEvent's own guard
    // only runs at press time, so a drag that STARTED in Smear (Clone mode
    // off, no source needed) could reach here with clone_mode_ now true
    // and clone_source_ still empty. Rather than dereferencing an empty
    // optional, that combination is treated as "nothing to stamp this
    // move" and skipped -- the drag keeps running (left_dragging_ stays
    // true), it just places no further stamps until either a source gets
    // captured or Clone mode is turned back off.
    if (clone_mode_ && !clone_source_.has_value()) {
      return;
    }
    const f32 threshold = clone_mode_
                              ? kCloneSpacingFactor * clone_source_->size_at_capture
                              : kSmearMoveThresholdFactor * brush_size_;
    if (glm::length(candidate - last_stamp_center_) >= threshold) {
      if (clone_mode_) {
        place_clone_stamp(candidate);
      } else {
        place_stamp(candidate);
      }
      last_stamp_center_ = candidate;
      ++stroke_stamp_count_;
    }
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

glm::vec3 SceneViewport::compute_ray_center(QPoint pos) const {
  // Matches Builtin.RaymarchShader.comp.glsl's main() exactly: uv centered
  // on the image, scaled by height only (so it's undistorted regardless of
  // aspect ratio), then the ray direction is built from the camera's own
  // basis rather than fixed world axes -- carried over unchanged from
  // tools/sdf_editor's pick_at() (git history).
  QSize size = physical_size();
  f32 px = static_cast<f32>(pos.x()) * static_cast<f32>(devicePixelRatio());
  f32 py = static_cast<f32>(pos.y()) * static_cast<f32>(devicePixelRatio());
  f32 uv_x = (px - 0.5f * static_cast<f32>(size.width())) /
            static_cast<f32>(size.height());
  f32 uv_y = (py - 0.5f * static_cast<f32>(size.height())) /
            static_cast<f32>(size.height());

  glm::vec3 ray_dir = glm::normalize(uv_x * camera_.right() +
                                     uv_y * camera_.up() + camera_.forward());

  // No "existing surface" data structure to pick against yet in this
  // milestone (see this class's header comment and kStampDistance) --
  // every stamp places its shape at a fixed depth along the ray instead
  // of at wherever the rendered surface actually is under the cursor.
  return camera_.position() + ray_dir * kStampDistance;
}

void SceneViewport::place_stamp(glm::vec3 center) {
  if (!initialized_ || shutdown_) {
    return;
  }
  // M4a (Stretch): applied AFTER brush_params_from_size()'s uniform-size
  // mapping, ON TOP of it -- see apply_stretch()'s own comment for why
  // params.w is never touched and why this is a positional, not shape-
  // aware, multiply.
  const glm::vec4 params =
      apply_stretch(brush_params_from_size(brush_shape_, brush_size_),
                    brush_stretch_x_, brush_stretch_y_, brush_stretch_z_);
  // M5 (Finishes): brush_material_slot_ -- whatever ModellingWindow's
  // Finish/colour controls last configured (defaults to 0, the plain
  // white default material, so a session that never touches those
  // controls stamps exactly as every prior milestone did).
  renderer_sculpt_stamp_shape(center, brush_shape_, params, brush_smoothness_,
                              brush_op_, brush_material_slot_,
                              brush_looseness_);
  emit stamp_placed(center, brush_shape_, params, brush_smoothness_,
                    brush_op_, brush_material_slot_, brush_looseness_);
}

void SceneViewport::place_clone_stamp(glm::vec3 center) {
  if (!initialized_ || shutdown_ || !clone_source_.has_value()) {
    return;
  }
  const CloneSource &src = *clone_source_;
  renderer_sculpt_stamp_shape(center, src.shape_type, src.params,
                              src.smoothness, src.op, src.material_slot,
                              src.looseness);
  emit stamp_placed(center, src.shape_type, src.params, src.smoothness,
                    src.op, src.material_slot, src.looseness);
}

void SceneViewport::place_spray(glm::vec3 center) {
  if (!initialized_ || shutdown_) {
    return;
  }
  const glm::vec4 params = brush_params_from_size(brush_shape_, brush_size_);
  // M3c verification-only: same rationale as place_stamp()'s own M3a log --
  // lets a Spraypaint drag's repeated-pass behaviour be confirmed from the
  // engine log.
  KINFO("M3c place_spray: center=({:.3f}, {:.3f}, {:.3f}) opacity_per_pass={:.3f}",
        center.x, center.y, center.z, spray_opacity_per_pass_);
  renderer_sculpt_spray(center, brush_shape_, params, spray_opacity_per_pass_);
  // Deliberately no stamp_placed()/stroke_ended() emission -- see this
  // method's own header comment, scene_viewport.h.
}

void SceneViewport::place_move_stamp(glm::vec3 center) {
  if (!initialized_ || shutdown_ || !move_payload_.has_value()) {
    return;
  }
  const SculptStampRecord &rec = *move_payload_;
  // Back into the object it was picked up from, keeping its rotation. The
  // signal is emitted before switching back so ModellingWindow records the
  // right object id.
  const i32 previous_object = renderer_sculpt_current_object();
  renderer_sculpt_set_current_object(rec.object_id);
  renderer_sculpt_stamp_shape(center, rec.shape_type, rec.params,
                              rec.smoothness, rec.op, rec.material_slot,
                              rec.looseness, rec.rotation);
  last_stamp_rotation_ = rec.rotation;
  emit stamp_placed(center, rec.shape_type, rec.params, rec.smoothness,
                    rec.op, rec.material_slot, rec.looseness);
  last_stamp_rotation_ = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
  renderer_sculpt_set_current_object(previous_object);
}

void SceneViewport::capture_clone_source() {
  // M4a (Stretch): baked into the frozen params AT CAPTURE TIME, same as
  // shape/smoothness/op/looseness already are -- Stretch is just another
  // brush-panel dimension, so freezing it the moment "Set Clone Source" is
  // clicked (rather than re-reading brush_stretch_x_/y_/z_ live on every
  // re-stamp, the way place_stamp() does for an ordinary Smear drag) keeps
  // every field of CloneSource consistently "whatever the panel held at
  // capture", not a mix of frozen and live. place_clone_stamp() therefore
  // never needs to know Stretch exists at all -- it already just re-stamps
  // clone_source_.params verbatim.
  clone_source_ = CloneSource{
      brush_shape_,
      apply_stretch(brush_params_from_size(brush_shape_, brush_size_),
                    brush_stretch_x_, brush_stretch_y_, brush_stretch_z_),
      brush_smoothness_,
      brush_op_,
      // M5 (Finishes): matches place_stamp()'s own brush_material_slot_ --
      // was hardcoded 0 before this milestone.
      brush_material_slot_,
      brush_size_,
      brush_looseness_,
  };
  emit clone_source_captured();
}

// M4 (Crop/Cutout): shared by do_crop()/do_cutout() below -- the viewport's
// own logical-center pixel, fed through the same compute_ray_center() an
// ordinary click uses, is the crop shape's anchor (there is no "existing
// surface" pick to anchor on instead -- see that method's own comment).
namespace {
glm::vec4 crop_params_from_brush(SculptBrushShape shape_type, f32 size,
                                 f32 stretch_x, f32 stretch_y, f32 stretch_z) {
  return apply_stretch(brush_params_from_size(shape_type, size), stretch_x,
                       stretch_y, stretch_z);
}
} // namespace

bool SceneViewport::do_crop() {
  if (!initialized_ || shutdown_) {
    return false;
  }
  const glm::vec3 center =
      compute_ray_center(QPoint(width() / 2, height() / 2));
  const glm::vec4 params = crop_params_from_brush(
      brush_shape_, brush_size_, brush_stretch_x_, brush_stretch_y_,
      brush_stretch_z_);
  const i32 touched =
      renderer_sculpt_crop(center, brush_shape_, params, brush_smoothness_);
  KINFO("M4 do_crop: center=({:.3f}, {:.3f}, {:.3f}) touched {} brick(s).",
       center.x, center.y, center.z, touched);
  return true;
}

bool SceneViewport::do_cutout(const QString &path) {
  if (!initialized_ || shutdown_) {
    return false;
  }
  const glm::vec3 center =
      compute_ray_center(QPoint(width() / 2, height() / 2));
  const glm::vec4 params = crop_params_from_brush(
      brush_shape_, brush_size_, brush_stretch_x_, brush_stretch_y_,
      brush_stretch_z_);
  const bool ok = renderer_sculpt_cutout(center, brush_shape_, params,
                                         brush_smoothness_,
                                         path.toStdString());
  KINFO("M4 do_cutout: center=({:.3f}, {:.3f}, {:.3f}) path='{}' -> {}",
       center.x, center.y, center.z, path.toStdString(), ok);
  return ok;
}

bool SceneViewport::load_cutout_file(const QString &path) {
  if (!initialized_ || shutdown_) {
    return false;
  }
  return renderer_sculpt_load_cutout(path.toStdString());
}

void SceneViewport::configure_material(i32 slot, glm::vec3 colour,
                                       f32 roughness, f32 metallic,
                                       bool waxy) {
  if (!initialized_ || shutdown_) {
    return;
  }
  renderer_sculpt_set_material(slot, colour, roughness, metallic, waxy);
}

void SceneViewport::replay_stamp(glm::vec3 center,
                                 SculptBrushShape shape_type,
                                 glm::vec4 params, f32 smoothness,
                                 SculptBrushOp op, int material_slot,
                                 f32 looseness) {
  if (!initialized_ || shutdown_) {
    return;
  }
  renderer_sculpt_stamp_shape(center, shape_type, params, smoothness, op,
                              material_slot, looseness);
}

bool SceneViewport::undo_last_stamp() {
  if (!initialized_ || shutdown_) {
    return false;
  }
  return renderer_sculpt_undo();
}

bool SceneViewport::redo_last_stamp() {
  if (!initialized_ || shutdown_) {
    return false;
  }
  return renderer_sculpt_redo();
}

bool SceneViewport::delete_edit_at(int index) {
  if (!initialized_ || shutdown_) {
    return false;
  }
  return renderer_sculpt_delete_edit(index);
}

void SceneViewport::update_brush_preview() {
  preview_visible_ = false;

  if (!initialized_ || shutdown_) {
    return;
  }
  // Hidden during any active drag -- see this method's own header comment
  // in scene_viewport.h for why every drag mode already has its own
  // feedback and doesn't need this on top of it. Explicitly tells the
  // renderer the shaded ghost preview (M7 polish) should stop showing too
  // -- the renderer-side preview is separate persistent GPU state
  // (VulkanRaymarchShader::set_brush_preview()'s UBO) that stays showing
  // whatever it was last told until something explicitly turns it off --
  // a `return` here with no matching call would leave last frame's ghost
  // stuck on screen for the whole drag.
  if (orbiting_ || left_dragging_ || spray_dragging_ || move_dragging_ ||
      moving_edit_dragging_) {
    renderer_sculpt_preview_brush(false, glm::vec3(0.0f), brush_shape_,
                                  glm::vec4(0.0f), brush_op_,
                                  brush_material_slot_);
    return;
  }

  // Polled via QCursor::pos() + mapFromGlobal() every tick(), rather than
  // cached from the last mouseMoveEvent -- two reasons. First, it's a more
  // literal fit for "updates every frame as the mouse moves" (a tick()-
  // driven poll inherently can't go stale between moves the way a cached
  // last-event position theoretically could if some platform ever
  // coalesced/dropped a move). Second, and more load-bearing: a bare
  // QWindow embedded via createWindowContainer (see this class's own
  // header comment on the borrowed-handle PlatformLayer bootstrap) is not
  // guaranteed to receive a plain hover-only mouseMoveEvent (no button
  // held) from every input path a real desktop delivers one from --
  // QCursor::pos() asks the X server directly (XQueryPointer under the
  // xcb platform) and so is independent of whichever event-delivery path
  // did or didn't fire. mapFromGlobal() + an explicit bounds check against
  // width()/height() is this class's own equivalent of "is the mouse over
  // me" -- there is no QWidget-style underMouse() on a bare QWindow.
  const QPoint local = mapFromGlobal(QCursor::pos());
  if (local.x() < 0 || local.y() < 0 || local.x() >= width() ||
      local.y() >= height()) {
    // Same "explicitly turn off persistent GPU state" reasoning as the
    // drag-state early return above -- the pointer having left this
    // viewport (over a dock, menu, etc.) must not leave last frame's ghost
    // showing at whatever world position it was last computed at.
    renderer_sculpt_preview_brush(false, glm::vec3(0.0f), brush_shape_,
                                  glm::vec4(0.0f), brush_op_,
                                  brush_material_slot_);
    return;
  }

  const glm::vec3 center = compute_ray_center(local);
  // Same params a real stamp at this position would actually use --
  // brush_params_from_size() + apply_stretch(), exactly what place_stamp()
  // computes -- so the preview's size/stretch always matches what a click
  // right now would place, not an approximation of it.
  const glm::vec4 params =
      apply_stretch(brush_params_from_size(brush_shape_, brush_size_),
                    brush_stretch_x_, brush_stretch_y_, brush_stretch_z_);

  // Nothing past this point can hide the preview -- both early-return
  // cases above (not initialized/shut down, an active drag, cursor outside
  // the viewport) already did that. This used to also gate a 2D wireframe
  // outline (append_wire_box()/append_wire_ellipse() via project_to_
  // screen()) built here and drawn every tick() by a since-removed draw_
  // brush_preview(); now that the shaded ghost below is the only preview
  // cue, preview_visible_ simply reflects "did update_brush_preview() make
  // it this far" (see git history for the wireframe this replaced).
  preview_visible_ = true;

  // M7 polish: the real shaded "ghost" preview -- center/params/brush_
  // shape_/brush_op_/brush_material_slot_ are exactly what a click right
  // now would stamp with (see this method's own comment on `params`
  // above), so the preview never shows a different brush than the one
  // that would actually land.
  renderer_sculpt_preview_brush(preview_visible_, center, brush_shape_,
                                params, brush_op_, brush_material_slot_);
}
