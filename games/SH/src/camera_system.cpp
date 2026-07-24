#include "camera_system.h"

#include <core/input.h>
#include <renderer/camera.h>
#include <renderer/renderer_frontend.h>

#include <algorithm>

void CameraSystem::add_pose(const CameraPose &pose) {
  poses_.push_back(pose);
}

void CameraSystem::cycle() {
  if (!poses_.empty() && !debug_active_) {
    current_ = (current_ + 1) % poses_.size();
    zoom_offset_ = 0.0f; // each station starts at its authored distance
  }
}

void CameraSystem::toggle_debug() {
  debug_active_ = !debug_active_;
  if (debug_active_ && !poses_.empty()) {
    // Start where the player was just looking from. A fresh Camera has
    // yaw = pitch = 0, so the relative yaw()/pitch() calls set absolute
    // angles -- same idiom as the posed path in update() below. The
    // mouse-pan offset is deliberately dropped: the pose's base facing is
    // the stable reference, the pan wanders frame to frame.
    const CameraPose &pose = poses_[current_];
    debug_camera_ = Camera();
    debug_camera_.set_position(pose.position);
    debug_camera_.yaw(pose.yaw);
    debug_camera_.pitch(pose.pitch);
  }
}

void CameraSystem::update_debug(f32 delta_time) {
  // Hold right mouse button and drag to look -- click-to-look rather than
  // captured-mouse, since the window never grabs the cursor and Q&A play
  // still uses the same mouse.
  if (input::is_button_down(input::Button::Right)) {
    input::MousePosition now = input::mouse_position();
    input::MousePosition prev = input::previous_mouse_position();
    constexpr f32 kLookSensitivity = 0.004f; // radians per pixel
    debug_camera_.yaw(static_cast<f32>(now.x - prev.x) * kLookSensitivity);
    // Dragging up (dy negative) looks up (positive pitch).
    debug_camera_.pitch(static_cast<f32>(prev.y - now.y) * kLookSensitivity);
  }

  glm::vec3 move(0.0f);
  if (input::is_key_down(input::Key::W)) {
    move += debug_camera_.forward();
  }
  if (input::is_key_down(input::Key::S)) {
    move -= debug_camera_.forward();
  }
  if (input::is_key_down(input::Key::D)) {
    move += debug_camera_.right();
  }
  if (input::is_key_down(input::Key::A)) {
    move -= debug_camera_.right();
  }
  // World-vertical rather than camera-up, so E always rises regardless of
  // where the camera is pitched -- the same convention the sdf_editor
  // viewport uses.
  if (input::is_key_down(input::Key::E)) {
    move += glm::vec3(0.0f, 1.0f, 0.0f);
  }
  if (input::is_key_down(input::Key::Q)) {
    move -= glm::vec3(0.0f, 1.0f, 0.0f);
  }

  if (move != glm::vec3(0.0f)) {
    constexpr f32 kFlySpeed = 3.0f;      // world units per second
    constexpr f32 kFastMultiplier = 4.0f; // while Shift is held
    f32 speed = kFlySpeed *
                (input::is_key_down(input::Key::Shift) ? kFastMultiplier : 1.0f);
    debug_camera_.move(glm::normalize(move) * speed * delta_time);
  }

  current_camera_ = debug_camera_;
  renderer_set_camera(debug_camera_);
}

void CameraSystem::update(u32 screen_width, u32 screen_height, f32 delta_time) {
  if (debug_active_) {
    update_debug(delta_time);
    return;
  }

  if (poses_.empty() || screen_width == 0 || screen_height == 0) {
    return;
  }
  const CameraPose &pose = poses_[current_];

  // Mouse position -> [-1, 1] across each screen axis (0 at the centre),
  // scaled by the pose's pan limit. Clamped so a mouse position reported
  // outside the window (possible mid-drag on X11) can't exceed max_pan.
  input::MousePosition mouse = input::mouse_position();
  f32 nx = std::clamp(
      static_cast<f32>(mouse.x) / static_cast<f32>(screen_width) * 2.0f - 1.0f,
      -1.0f, 1.0f);
  f32 ny = std::clamp(
      static_cast<f32>(mouse.y) / static_cast<f32>(screen_height) * 2.0f - 1.0f,
      -1.0f, 1.0f);

  // Scroll-to-zoom: dolly along wherever the camera is *currently* looking
  // (base facing plus pan, computed below), so scrolling zooms in on
  // whatever's actually under the view, not a fixed direction that ignores
  // where the mouse has panned it. Scrolling up (a positive wheel delta)
  // zooms in -- moves forward.
  constexpr f32 kZoomStep = 0.15f;    // world units per wheel notch
  constexpr f32 kMinZoomOffset = -1.5f; // furthest zoomed out (back away)
  constexpr f32 kMaxZoomOffset = 3.0f;  // furthest zoomed in (move forward)
  zoom_offset_ = std::clamp(
      zoom_offset_ +
          static_cast<f32>(input::mouse_wheel_delta()) * kZoomStep,
      kMinZoomOffset, kMaxZoomOffset);

  // A fresh Camera starts at yaw = pitch = 0, so the relative yaw()/pitch()
  // calls below set absolute angles: base facing plus the clamped pan.
  Camera camera;
  camera.set_position(pose.position);
  camera.yaw(pose.yaw + nx * pose.max_pan);
  camera.pitch(pose.pitch + ny * pose.max_pan);

  // Dolly along the camera's own forward() -- already the panned direction
  // set just above -- so zooming moves toward exactly where you're looking
  // right now.
  camera.set_position(pose.position + camera.forward() * zoom_offset_);

  current_camera_ = camera;
  renderer_set_camera(camera);
}
