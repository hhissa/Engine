#pragma once
#include <defines.h>
#include <glm/glm.hpp>
#include <renderer/camera.h>
#include <vector>

// One fixed camera station: where the camera sits, which way it faces at
// rest, and how far (in radians) the player may pan away from that facing.
struct CameraPose {
  glm::vec3 position{0.0f};
  f32 yaw = 0.0f;   // radians; 0 faces +Z (see Camera::forward())
  f32 pitch = 0.0f; // radians; positive looks up
  f32 max_pan = 0.2f; // max deviation from (yaw, pitch), both axes
};

// Holds the game's camera stations and lets the player cycle through them
// (cycle(), wired to Tab in SHGame::update()). In normal play the camera
// never free-flies: each frame the view is the current pose plus a
// mouse-driven pan that is clamped to the pose's max_pan -- moving the
// mouse toward a screen edge looks at most max_pan radians away from the
// pose's base facing, so every station is a constrained angle, not a full
// look-around.
//
// The exception is the debug camera (toggle_debug(), wired to the 0 key in
// SHGame::update() -- Escape is taken, it quits the application
// engine-side): a development aid that detaches from the stations
// entirely and free-flies -- WASD to move in the facing plane, Q/E to
// descend/rise, hold the right mouse button and drag to look, hold Shift
// to move faster. It starts from the current station's position/facing (so
// toggling it mid-play inspects exactly what you were just looking at) and
// keeps its own state across toggles within a run; leaving it snaps back
// to the stations as if nothing happened.
//
// update() must be called once per frame (from the game's update()); it
// submits the resulting camera via renderer_set_camera().
class CameraSystem {
public:
  void add_pose(const CameraPose &pose);

  // Advances to the next pose, wrapping at the end. Ignored while the
  // debug camera is active (the stations aren't in control).
  void cycle();

  // Enters/leaves the free-fly debug camera (see the class comment). On
  // entry the debug camera is re-seeded from the current station's
  // position/facing.
  void toggle_debug();
  bool debug_active() const { return debug_active_; }

  // screen_width/height are the current framebuffer size, used to turn the
  // mouse position into a normalized [-1, 1] pan amount. delta_time drives
  // the debug camera's fly speed (unused by the posed cameras, which are
  // purely positional).
  void update(u32 screen_width, u32 screen_height, f32 delta_time);

private:
  // The per-frame free-fly input handling behind debug mode -- see
  // toggle_debug()/the class comment.
  void update_debug(f32 delta_time);

  std::vector<CameraPose> poses_;
  size_t current_ = 0;

  bool debug_active_ = false;
  Camera debug_camera_; // persists across toggles within a run
};
