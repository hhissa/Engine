// Scene-state switching: apply_scene_state()'s per-SceneState switch (which
// .sdf scenes/lights/camera poses each one loads) and register_scene_states()/
// register_chapter_scene_states(), which wire .conversation tag= names to
// it -- see game.h's "game_scene_state.cpp" section.

#include "game.h"

#include <renderer/renderer_frontend.h>

#include <glm/glm.hpp>

void SHGame::apply_scene_state(SceneState state) {
  if (current_state_ == state) {
    return; // already there -- e.g. re-selecting a dialogue option that
            // maps to the state already active
  }

  // Every case below is a *complete* description of what should be
  // loaded, not a diff from the previous state -- so tear down whatever
  // scenery/model is currently loaded first, unconditionally, rather than
  // trying to work out what changed between the old and new state.
  for (SceneHandle handle : loaded_scenes_) {
    renderer_remove_scene(handle);
  }
  loaded_scenes_.clear();

  // Every one of the 40 SceneState values (see game.h) currently renders
  // identically -- the same room/model/lights, just tagged distinctly per
  // chapter so a dedicated asset can later replace man.sdf for one
  // character (or give one chapter's Room_03 its own staging) without
  // touching any other state. Sharing this one switch body via a stacked
  // case label list (rather than 40 copies of the same code) keeps that
  // TODO honest: swapping in real content later is just splitting the
  // relevant label(s) off into their own case.
  switch (state) {
  case SceneState::TheoRoom1: case SceneState::TheoRoom2:
  case SceneState::TheoRoom3: case SceneState::TheoRoom4:
  case SceneState::TheoRoom5:
  case SceneState::ThiagoRoom1: case SceneState::ThiagoRoom2:
  case SceneState::ThiagoRoom3: case SceneState::ThiagoRoom4:
  case SceneState::ThiagoRoom5:
  case SceneState::AlexRoom1: case SceneState::AlexRoom2:
  case SceneState::AlexRoom3: case SceneState::AlexRoom4:
  case SceneState::AlexRoom5:
  case SceneState::DiegoRoom1: case SceneState::DiegoRoom2:
  case SceneState::DiegoRoom3: case SceneState::DiegoRoom4:
  case SceneState::DiegoRoom5:
  case SceneState::MelRoom1: case SceneState::MelRoom2:
  case SceneState::MelRoom3: case SceneState::MelRoom4:
  case SceneState::MelRoom5:
  case SceneState::PatRoom1: case SceneState::PatRoom2:
  case SceneState::PatRoom3: case SceneState::PatRoom4:
  case SceneState::PatRoom5:
  case SceneState::TheresaRoom1: case SceneState::TheresaRoom2:
  case SceneState::TheresaRoom3: case SceneState::TheresaRoom4:
  case SceneState::TheresaRoom5:
  case SceneState::PhotographerRoom1: case SceneState::PhotographerRoom2:
  case SceneState::PhotographerRoom3: case SceneState::PhotographerRoom4:
  case SceneState::PhotographerRoom5:
    renderer_enable_sky_box("skybox_to_equirect_2");

    loaded_scenes_.push_back(renderer_load_scene("assets/scenes/room.sdf")
                                 .scale(3.0)
                                 .translate(glm::vec3(0.0, 0.0, 0.0)));

    loaded_scenes_.push_back(renderer_load_scene("assets/scenes/man.sdf")
                                 .scale(0.35)
                                 .translate(glm::vec3(0.0, -2.8, 0.0)));

    loaded_scenes_.push_back(
        renderer_load_scene("assets/scenes/light.sdf")
            .rotate(glm::vec3(0.0f, glm::radians(45.0f), 0.0f))
            .scale(0.3)
            .translate(glm::vec3(2.0, -1.5, -1.0)));

    loaded_scenes_.push_back(
        renderer_load_scene("assets/scenes/light.sdf")
            .rotate(glm::vec3(0.0f, glm::radians(135.0f), 0.0f))
            .scale(0.3)
            .translate(glm::vec3(-2.0, -1.5, -1.0)));

    loaded_scenes_.push_back(
        renderer_load_scene("assets/scenes/overhead lights.sdf")
            .scale(2.0f)
            .translate(glm::vec3(0.0, -4.0, 0.0)));

    // Camera stations around the man -- Tab cycles, mouse pans within each
    // pose's max_pan (see CameraSystem).
    cameras_.set_poses({
        {glm::vec3(3.0f, -3.0f, -4.0f), glm::radians(-45.0f), 0.0f, 0.1f},
        {glm::vec3(0.0f, -3.0f, -6.0f), glm::radians(0.0f), glm::radians(-0.0f),
         0.5f},
        {glm::vec3(-3.3f, -3.0f, -4.5f), glm::radians(35.0f),
         glm::radians(20.0f), 0.1f},
    });
    break;
  }

  current_state_ = state;
}

void SHGame::register_scene_states() {
  // Fallback for the stretch of the chain no tag has covered yet -- see
  // QASystem::set_base_scene_state()'s doc comment. set_on_returned_to_root
  // is a second, independent trigger: it fires once, specifically when a
  // nested follow-up branch is fully explored and the view pops all the
  // way back up to the top-level list. Both go through
  // current_chapter_base_state() (game_menus.cpp) rather than a single
  // hardcoded SceneState, so each chapter falls back to *its own* Room1
  // state no matter which chapter is actually playing.
  qa_.set_base_scene_state(
      [this] { apply_scene_state(current_chapter_base_state()); });
  qa_.set_on_returned_to_root(
      [this] { apply_scene_state(current_chapter_base_state()); });

  // Reaching a question marked `ending` (see resources/conversation.h)
  // stops dialogue navigation right where it is -- this is what actually
  // ends the chapter: a short outro beat (reusing the same black
  // click-through screen chapter intros use -- see show_intertitle() in
  // game_menus.cpp), using that specific ending's own ending_lines (see
  // resources/conversation.h's `ending_text=` lines), then straight on to
  // the next chapter -- see finish_chapter() (game_menus.cpp).
  qa_.set_on_ending_reached([this](const QASystem::Entry &entry) {
    finish_chapter(entry.ending_lines);
  });
}

void SHGame::register_chapter_scene_states(size_t chapter_index) {
  // Every ch*.conversation file tags its questions with the same five
  // names ("Room_01".."Room_05") -- see game.h's SceneState comment for
  // why that's fine despite QASystem::register_scene_state() otherwise
  // warning about a name colliding: begin_chapter_playing() always
  // unloads the previous conversation first, which clears every
  // previously-registered tag name (QASystem::clear_scene_states()),
  // before this re-registers the same five names against
  // chapter_index's own block of SceneState values below.
  struct ChapterRooms {
    SceneState room1, room2, room3, room4, room5;
  };
  constexpr ChapterRooms kChapterRooms[] = {
      {SceneState::TheoRoom1, SceneState::TheoRoom2, SceneState::TheoRoom3,
       SceneState::TheoRoom4, SceneState::TheoRoom5},
      {SceneState::ThiagoRoom1, SceneState::ThiagoRoom2, SceneState::ThiagoRoom3,
       SceneState::ThiagoRoom4, SceneState::ThiagoRoom5},
      {SceneState::AlexRoom1, SceneState::AlexRoom2, SceneState::AlexRoom3,
       SceneState::AlexRoom4, SceneState::AlexRoom5},
      {SceneState::DiegoRoom1, SceneState::DiegoRoom2, SceneState::DiegoRoom3,
       SceneState::DiegoRoom4, SceneState::DiegoRoom5},
      {SceneState::MelRoom1, SceneState::MelRoom2, SceneState::MelRoom3,
       SceneState::MelRoom4, SceneState::MelRoom5},
      {SceneState::PatRoom1, SceneState::PatRoom2, SceneState::PatRoom3,
       SceneState::PatRoom4, SceneState::PatRoom5},
      {SceneState::TheresaRoom1, SceneState::TheresaRoom2, SceneState::TheresaRoom3,
       SceneState::TheresaRoom4, SceneState::TheresaRoom5},
      {SceneState::PhotographerRoom1, SceneState::PhotographerRoom2,
       SceneState::PhotographerRoom3, SceneState::PhotographerRoom4,
       SceneState::PhotographerRoom5},
  };

  const ChapterRooms &rooms = kChapterRooms[chapter_index];
  qa_.register_scene_state("Room_01", [this, rooms] { apply_scene_state(rooms.room1); });
  qa_.register_scene_state("Room_02", [this, rooms] { apply_scene_state(rooms.room2); });
  qa_.register_scene_state("Room_03", [this, rooms] { apply_scene_state(rooms.room3); });
  qa_.register_scene_state("Room_04", [this, rooms] { apply_scene_state(rooms.room4); });
  qa_.register_scene_state("Room_05", [this, rooms] { apply_scene_state(rooms.room5); });
}
