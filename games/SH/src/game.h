#pragma once
#include "camera_system.h"
#include "qa_system.h"

#include <defines.h>
#include <game_types.h>
#include <renderer/renderer_types.inl>

#include <optional>
#include <vector>

// Named, fully-specified combinations of loaded scenery/model -- see
// SHGame::apply_scene_state(). Add a new enumerator here (and a matching
// case in apply_scene_state()'s switch) for each new state the story
// needs, then wire a dialogue option to it via
// qa_.set_on_selected("...", [this] { apply_scene_state(SceneState::X); });
// in initialize() (see QASystem::set_on_selected()'s own comment).
enum class SceneState {
  Normal, // the starting room/man/lights setup -- see initialize().
  // Triggered by "Have you ever attempted suicide?" -- swaps everything
  // out for assets/scenes/door_scene.sdf alone (no room/lights/overhead
  // lights), matching what that dialogue option used to do directly via
  // renderer_clear_scenes() before it was folded into this state system.
  DoorScene1,
  DoorScene2,
  Scars1,
  Scars2,
  Scars3,
  Scars4,
};

class SHGame : public Game {
public:
  SHGame();
  ~SHGame() override = default;

  b8 initialize() override;
  b8 update(f32 delta_time) override;
  b8 render(f32 delta_time) override;
  void on_resize(u32 width, u32 height) override;

private:
  // Tears down whichever scenery/model is currently loaded and rebuilds
  // exactly the combination `state` specifies -- every case in its switch
  // is a complete description of what should be loaded, not a diff from
  // whatever was there before, so switching from any state to any other
  // is always correct without needing to know what the previous one was.
  // No-op if state is already current_state_ (e.g. re-selecting a dialogue
  // option that maps to the state already active).
  void apply_scene_state(SceneState state);

  f32 delta_time_ = 0.0f;

  QASystem qa_;
  CameraSystem cameras_;

  // Current framebuffer size -- the camera system needs it to normalize
  // the mouse position. Seeded from app_config, kept fresh by on_resize().
  u32 width_ = 0;
  u32 height_ = 0;

  // Every scene handle apply_scene_state() has currently loaded (the man/
  // room/lights for Normal, just the door for DoorScene1, etc.) -- nothing
  // here ever needs to address one of them individually after loading, so
  // apply_scene_state() just appends whatever each state's case loads and
  // tears the whole batch down uniformly on the next call. If some future
  // state needs to reach back into one specific piece (e.g. flicker just
  // one light), give that one its own named SceneHandle member instead --
  // don't try to make this vector do both jobs.
  std::vector<SceneHandle> loaded_scenes_;

  // Which SceneState is currently loaded -- nullopt only before the very
  // first apply_scene_state() call (see initialize()), so that call always
  // proceeds even though it happens to request SceneState::Normal, the
  // enum's own first (default-looking) value.
  std::optional<SceneState> current_state_;

  // The dialogue tree loaded via qa_.load_conversation() in initialize() --
  // kept around so it (or a future conversation swapped in for a different
  // room/act) can be torn down again with qa_.unload_conversation().
  ConversationHandle dialogue_ = kInvalidConversationHandle;
};
