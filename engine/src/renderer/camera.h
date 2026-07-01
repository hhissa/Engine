#pragma once
#include "../defines.h"

#include <glm/glm.hpp>

#include <cmath>

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

private:
  glm::vec3 position_{0.0f, 0.0f, -3.0f};
  f32 yaw_ = 0.0f;
  f32 pitch_ = 0.0f;
};
