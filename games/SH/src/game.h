#pragma once
#include "camera_system.h"
#include "menu_system.h"
#include "qa_system.h"

#include <defines.h>
#include <game_types.h>
#include <renderer/renderer_types.inl>

#include <functional>
#include <optional>
#include <string>
#include <vector>

// Which top-level screen the game is currently showing -- SHGame::update()/
// render() branch on this before anything else. Title/ChapterSelect/
// Settings each drive their own MenuSystem and don't touch gameplay state
// (cameras_, qa_, loaded_scenes_) at all; Intertitle is the black,
// click-through text screen shown before a fresh chapter's own gameplay
// begins (see show_intertitle()); Playing is exactly today's gameplay loop,
// gated behind actually having started a chapter (see start_chapter()).
enum class AppScreen {
  Title,
  ChapterSelect,
  Settings,
  Intertitle,
  Playing,
};

// Named, fully-specified combinations of loaded scenery/model -- see
// SHGame::apply_scene_state(). Every ch*.conversation file (assets/
// conversations/) tags its questions with the same small, chapter-local set
// of names -- "Room_01" through "Room_05", marking roughly how deep into
// that chapter's interview the player has gotten -- rather than a
// per-story-beat name unique across the whole game, so each chapter gets
// its own block of five enumerators here rather than five shared ones.
// That's also why the tags themselves are allowed to repeat across
// chapters at all: register_chapter_scene_states() (game_scene_state.cpp)
// re-registers "Room_01".."Room_05" against a *different* chapter's block
// below every time begin_chapter_playing() switches chapters, and
// QASystem::clear_scene_states() (called from unload_conversation(), which
// begin_chapter_playing() always calls first) frees the previous chapter's
// registration first -- see that function's own comment. Add a new
// five-enumerator block here (and extend apply_scene_state()'s switch and
// register_chapter_scene_states()'s table, both in game_scene_state.cpp)
// for each further chapter.
enum class SceneState {
  TheoRoom1, TheoRoom2, TheoRoom3, TheoRoom4, TheoRoom5,               // Chapter 1
  ThiagoRoom1, ThiagoRoom2, ThiagoRoom3, ThiagoRoom4, ThiagoRoom5,     // Chapter 2
  AlexRoom1, AlexRoom2, AlexRoom3, AlexRoom4, AlexRoom5,               // Chapter 3
  DiegoRoom1, DiegoRoom2, DiegoRoom3, DiegoRoom4, DiegoRoom5,          // Chapter 4
  MelRoom1, MelRoom2, MelRoom3, MelRoom4, MelRoom5,                   // Chapter 5
  PatRoom1, PatRoom2, PatRoom3, PatRoom4, PatRoom5,                   // Chapter 6
  TheresaRoom1, TheresaRoom2, TheresaRoom3, TheresaRoom4, TheresaRoom5, // Chapter 7
  PhotographerRoom1, PhotographerRoom2, PhotographerRoom3,             // Chapter 8
  PhotographerRoom4, PhotographerRoom5,
};

// Everything a save file records -- see SHGame::save_progress() (game_save.cpp)
// for exactly when this gets written and load_save() for how it's read
// back. Declared here (rather than private to game_save.cpp) since
// begin_chapter_playing() (game_menus.cpp) also needs the type to apply a
// loaded save's asked_flags/scene_state.
struct SavedProgress {
  size_t chapter_index;
  SceneState scene_state;
  std::vector<bool> asked_flags;
  // QASystem::flags() -- see QASystem::apply_flags(). Which dialogue flags
  // (see resources/conversation.h's requires=/requires_not=/sets= lines)
  // were set, so a resumed chapter has the same questions unlocked/hidden
  // it did when the save was written.
  std::vector<std::string> flags;
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
  // --- game.cpp ---
  // Shared by every screen that shows cameras_' live 3D background
  // (Playing, and -- since setup_menus() gave them one too -- Title/
  // ChapterSelect/Settings), so free-flying to check framing works the
  // same way no matter which one you're on.

  // Toggles the free-fly debug camera on an edge-triggered '0' press --
  // not Escape, which already quits the application engine-side. See
  // CameraSystem's class comment for the rest of the free-fly controls
  // (WASD move, Q/E down/up, RMB-drag look, Shift fast).
  void toggle_debug_camera_if_pressed();

  // Draws the "[DEBUG CAM] ..." HUD line (controls reminder + live
  // position/yaw/pitch) whenever the free-fly debug camera is active;
  // no-op otherwise.
  void render_debug_camera_hud() const;

  // --- game_scene_state.cpp ---

  // Tears down whichever scenery/model is currently loaded and rebuilds
  // exactly the combination `state` specifies -- every case in its switch
  // is a complete description of what should be loaded, not a diff from
  // whatever was there before, so switching from any state to any other
  // is always correct without needing to know what the previous one was.
  // No-op if state is already current_state_ (e.g. re-selecting a dialogue
  // option that maps to the state already active).
  void apply_scene_state(SceneState state);

  // Wires the conversation-agnostic fallbacks -- set_base_scene_state()/
  // set_on_returned_to_root()/set_on_ending_reached() -- called once from
  // initialize(). Doesn't touch the "Room_01".."Room_05" tag names
  // themselves -- those are chapter-scoped, see
  // register_chapter_scene_states() below -- so this has nothing left to
  // do once a chapter is actually playing.
  void register_scene_states();

  // Wires "Room_01".."Room_05" (see apply_scene_state()'s own comment and
  // SceneState's) to kChapters[chapter_index]'s own block of five
  // SceneState values via qa_.register_scene_state() -- called from
  // begin_chapter_playing() (game_menus.cpp) right after that chapter's
  // load_conversation(), every time a chapter starts or resumes. Safe to
  // call repeatedly with a different chapter_index across chapter
  // switches: begin_chapter_playing() always unloads the previous
  // conversation first, and QASystem::unload_conversation() clears every
  // previously-registered tag name (see QASystem::clear_scene_states())
  // before this re-registers the same five names against the new
  // chapter's own scenes -- that's what lets every chapter's
  // .conversation file reuse the same tag names without them needing to
  // stay unique game-wide.
  void register_chapter_scene_states(size_t chapter_index);

  // --- game_playing.cpp ---

  void update_playing();
  void render_playing() const;

  // --- game_menus.cpp ---
  // Everything for the Title/ChapterSelect/Settings/Intertitle screens --
  // i.e. every AppScreen except Playing -- plus the chapter-starting flow
  // that connects them to it.

  // Sets up chapter_menu_/settings_menu_'s fixed item lists, loads the
  // title screen's background scene/camera pose, and calls
  // build_title_menu() -- called once from initialize().
  void setup_menus();

  void update_title_screen();
  void render_title_screen() const;
  void update_chapter_select();
  void render_chapter_select() const;
  void update_settings_screen();
  void render_settings_screen() const;
  void update_intertitle();
  void render_intertitle() const;

  // Rebuilds title_menu_'s item list -- called from initialize() and
  // whenever returning to AppScreen::Title, so "Continue" only appears
  // when a save file actually exists right now (e.g. it wouldn't yet on
  // the very first launch, but would after backing out to the title screen
  // once some progress has been made).
  void build_title_menu();

  // Entry point for actually playing kChapters[index] (see game_menus.cpp)
  // -- shows that chapter's intro text first (see show_intertitle()) if
  // it's a fresh start and the chapter has any, otherwise goes straight to
  // begin_chapter_playing(). Shared by Start (fresh), Continue (!fresh),
  // and every Chapter Select entry (fresh). Continue never shows intro
  // text even if the chapter has some -- it's resuming an already-started
  // chapter, not beginning it.
  void start_chapter(size_t index, bool fresh);

  // Does the actual work start_chapter() defers behind the intertitle:
  // unloads whichever conversation was previously loaded (if any), loads
  // kChapters[index]'s, applies it, and switches screen_ to Playing. If
  // fresh is false, restores asked-flag progress from the save file
  // instead of starting blank -- see save_progress()/load_save()
  // (game_save.cpp).
  void begin_chapter_playing(size_t index, bool fresh);

  // kChapters[*active_chapter_]'s own initial_state (game_menus.cpp) -- the
  // per-chapter SceneState (e.g. SceneState::ThiagoRoom1 for Chapter 2,
  // SceneState::TheoRoom1 for Chapter 1) that chapter should fall back to
  // whenever no tag governs the current position. Used by
  // register_scene_states()'s set_base_scene_state()/set_on_returned_to_
  // root() callbacks (game_scene_state.cpp) instead of a single hardcoded
  // state, so those fallbacks show the *right* chapter's base state no
  // matter which one is actually playing -- without this, every chapter's
  // "returned to root" moment would incorrectly show Chapter 1's own
  // room/man.sdf. Falls back to SceneState::TheoRoom1 itself if
  // active_chapter_ is nullopt (shouldn't happen in practice -- see
  // save_progress()'s own guard -- but avoids an out-of-bounds kChapters
  // read if it somehow did).
  SceneState current_chapter_base_state() const;

  // Switches to AppScreen::Intertitle: clears whatever scene is currently
  // loaded and switches to a black backdrop (see game_menus.cpp --
  // solid_quad can't sit *behind* text, so this gets a true black screen
  // by clearing the scene and skybox instead of drawing over one), then
  // shows lines one at a time -- a click or [Enter] advances to the next,
  // and once the last line is dismissed on_finish runs (typically
  // begin_chapter_playing()). Mirrors QASystem's one-line-at-a-time answer
  // display (kAnswer/kHint colours, Enter to advance) for a consistent
  // feel, plus the same click support every other screen already has.
  void show_intertitle(std::vector<std::string> lines,
                       std::function<void()> on_finish);

  // Called from register_scene_states()'s set_on_ending_reached() callback
  // (game_scene_state.cpp) once a chapter's dialogue reaches an `ending`
  // question -- shows that ending's own outro text (ending_lines, falling
  // back to a generic line if the .conversation file gave it none) via
  // show_intertitle(), then moves straight on to the next entry in
  // kChapters (game_menus.cpp) once it's dismissed, exactly as if the
  // player had picked it from Chapter Select themselves -- rather than
  // dropping back to Chapter Select. Falls back to Chapter Select only once
  // active_chapter_ was the last chapter in kChapters.
  void finish_chapter(std::vector<std::string> ending_lines);

  // --- game_save.cpp ---
  // Disk persistence -- a save file (chapter/scene/asked-flag progress)
  // and a settings file (render_scale_), both plain "key=value" lines
  // written via engine/src/platform/filesystem.h's FileHandle.

  // Writes the current chapter/scene/asked-flag progress to disk. Wired as
  // QASystem::set_on_any_asked()'s callback in initialize(), so this fires
  // automatically every time the player asks any question -- an autosave,
  // since Game has no shutdown hook to save on quit from instead (see
  // game_types.h).
  void save_progress();

  // True if a save file exists on disk right now -- build_title_menu()
  // uses this to decide whether "Continue" should appear at all.
  bool has_save() const;

  // Reads and parses the save file -- nullopt if none exists, or if it
  // exists but is malformed (hand-edited, or written by a since-changed
  // save format) rather than guessing at a partial read.
  std::optional<SavedProgress> load_save() const;

  // Reads render_scale_ from disk, leaving it at its current value if no
  // settings file exists yet (e.g. first launch). Called once from
  // initialize().
  void load_settings();

  // Writes render_scale_ to disk -- called immediately whenever the
  // Settings screen changes it, so the change survives even a hard quit.
  void save_settings() const;

  f32 delta_time_ = 0.0f;

  AppScreen screen_ = AppScreen::Title;
  MenuSystem title_menu_;
  MenuSystem chapter_menu_;
  MenuSystem settings_menu_; // just the single "Back" row -- see game.cpp

  // Applied at boot (see load_settings()) and live-adjusted from the
  // Settings screen; renderer_set_render_scale() itself has no getter, so
  // this is the one place that value is tracked.
  f32 render_scale_ = 0.75f;

  // Index into kChapters (game.cpp) for whichever chapter is currently
  // being played -- nullopt while on Title/ChapterSelect/Settings, before
  // start_chapter() has run. Recorded so save_progress() knows which
  // chapter a save belongs to.
  std::optional<size_t> active_chapter_;

  // AppScreen::Intertitle's state -- see show_intertitle(). intertitle_line_
  // indexes into intertitle_lines_, exactly like QASystem's answer_line_
  // indexes into an Entry's answer_lines; intertitle_on_finish_ runs once
  // the last line is dismissed.
  std::vector<std::string> intertitle_lines_;
  size_t intertitle_line_ = 0;
  std::function<void()> intertitle_on_finish_;

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
  // first apply_scene_state() call (see begin_chapter_playing()), so that
  // call always proceeds even though it happens to request
  // SceneState::TheoRoom1, the enum's own first (default-looking) value.
  std::optional<SceneState> current_state_;

  // The dialogue tree loaded via qa_.load_conversation() in initialize() --
  // kept around so it (or a future conversation swapped in for a different
  // room/act) can be torn down again with qa_.unload_conversation().
  ConversationHandle dialogue_ = kInvalidConversationHandle;
};
