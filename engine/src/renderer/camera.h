#pragma once
#include "../defines.h"

#include <glm/glm.hpp>

#include <cmath>
#include <optional>

// A free-fly camera: position + yaw/pitch, exposing the orthonormal basis
// (forward/right/up) a raymarch shader needs to build per-pixel ray
// directions in view space. At yaw == pitch == 0, the basis is exactly
// (right=+X, up=+Y, forward=+Z) — the same fixed axes the raymarch shader
// used before camera control existed, so this is a pure generalization, not
// a behavior change at the default orientation.
//
// Fully inline (no .cpp) — no KAPI needed, there's no out-of-line symbol
// that has to resolve across the engine/testbed shared-library boundary.
class Camera {
public:
  void set_position(glm::vec3 position) noexcept { position_ = position; }
  glm::vec3 position() const noexcept { return position_; }

  void move(glm::vec3 delta) noexcept { position_ += delta; }

  void yaw(f32 radians) noexcept { yaw_ += radians; }

  // Clamped to avoid gimbal lock (matches upstream's +-89 degrees).
  void pitch(f32 radians) noexcept {
    pitch_ += radians;
    constexpr f32 limit = 1.55334303f; // 89 degrees, in radians
    pitch_ = pitch_ < -limit ? -limit : (pitch_ > limit ? limit : pitch_);
  }

  glm::vec3 forward() const noexcept {
    return glm::normalize(glm::vec3(std::cos(pitch_) * std::sin(yaw_),
                                   std::sin(pitch_),
                                   std::cos(pitch_) * std::cos(yaw_)));
  }

  glm::vec3 right() const noexcept {
    return glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), forward()));
  }

  glm::vec3 up() const noexcept { return glm::cross(forward(), right()); }

  // Projects world_point to this camera's screen-pixel coordinates at the
  // given framebuffer size -- the exact inverse of the per-pixel ray
  // Builtin.RaymarchShader.comp.glsl's main() builds (uv derived from
  // pixel_coord, then ray_dir = normalize(uv.x*right + uv.y*up +
  // forward)). Returns nullopt if world_point is behind the camera (on or
  // beyond the plane through the camera position perpendicular to
  // forward()), since there's no sensible on-screen position for that --
  // callers pinning a UI element to a 3D point (e.g. a censor box tracking
  // a body part across camera angles) should skip drawing it that frame
  // rather than plot a garbage position.
  //
  // Derivation: world_point - position() is proportional to
  // uv.x*right + uv.y*up + forward for some positive scalar (the depth
  // along forward) -- taking the dot product of that relation with each
  // of the orthonormal forward/right/up basis vectors in turn isolates
  // uv.x and uv.y directly, without needing a full view/projection matrix.
  std::optional<glm::vec2> project_to_screen(glm::vec3 world_point,
                                             u32 screen_width,
                                             u32 screen_height) const noexcept {
    glm::vec3 relative = world_point - position_;
    glm::vec3 fwd = forward();
    f32 depth = glm::dot(relative, fwd);
    if (depth <= 0.0001f) {
      return std::nullopt;
    }

    f32 uv_x = glm::dot(relative, right()) / depth;
    f32 uv_y = glm::dot(relative, up()) / depth;

    // Inverse of main()'s "uv = (pixel_coord - 0.5*image_size) /
    // image_size.y".
    f32 height_f = static_cast<f32>(screen_height);
    return glm::vec2(uv_x * height_f + 0.5f * static_cast<f32>(screen_width),
                     uv_y * height_f + 0.5f * height_f);
  }

private:
  glm::vec3 position_{0.0f, 0.0f, -3.0f};
  f32 yaw_ = 0.0f;
  f32 pitch_ = 0.0f;
};
