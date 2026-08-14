// Every AppScreen except Playing -- Title, Chapter Select, Settings, and
// the black click-through Intertitle screen -- plus the chapter registry
// and start_chapter()/begin_chapter_playing()/show_intertitle() flow that
// connects them to actual gameplay. See game.h's "game_menus.cpp" section.

#include "game.h"
#include "text_wrap.h"

#include <core/input.h>
#include <renderer/renderer_frontend.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace {

// One selectable chapter -- which conversation file it loads and which
// SceneState it opens on. Add an entry here (and drop the new
// .conversation file under assets/conversations/) for each further chapter
// Chapter Select should offer; nothing else needs to change; start_chapter()
// and the Chapter Select menu (see SHGame::setup_menus()) are both already
// driven off this list rather than hardcoded per-chapter.
struct ChapterDef {
  std::string display_name;
  std::string conversation_path;
  SceneState initial_state;
  // Shown one line at a time on a black screen (see SHGame::show_intertitle())
  // before this chapter's own gameplay begins -- covers both "before the
  // start of the game" (Chapter 1's own intro, shown on Start) and "in
  // between chapters" (chapter N+1's intro, shown the instant it's
  // started) with the same mechanism, since a chapter's intro is really
  // just "text shown on entry" regardless of which chapter came before it.
  // Empty means no intertitle at all -- start_chapter() skips straight to
  // begin_chapter_playing(). Never shown on Continue (see start_chapter()'s
  // own comment).
  std::vector<std::string> intro_lines;
};

// Rebuilt for the ch1_theo..ch8_photographer.conversation rework -- each
// file tags its own questions with the same chapter-local "Room_01".."
// Room_05" names (see game.h's SceneState comment and
// register_chapter_scene_states() in game_scene_state.cpp for how those
// get wired to a *different* block of scenes per chapter without needing
// to be unique game-wide) rather than a story-beat name unique across the
// whole game. Chapter 8 crosses back to Chapter 1's Theo directly (a
// loop_to= inside ch8_photographer.conversation reaches a "Theo?" question)
// but isn't a subject at all otherwise: it's the photographer turning the
// camera on himself, which is also why its intro below doesn't fit the
// "Another chair" formula every other chapter uses.
const std::vector<ChapterDef> kChapters = {
    {"Chapter 1", "assets/conversations/ch1_theo.conversation",
     SceneState::TheoRoom1,
     // Placeholder -- replace with Chapter 1's actual introduction text;
     // this exists to demonstrate/exercise the intertitle mechanism itself,
     // not to author real story content.
     {"What follows is a series of interviews conducted with victims of SH.",
      "None of the victims survived their battles with self harm.","You need to change that.","Names have been changed for privacy. "}},
    // Chapters 2-7 are reached both from Chapter Select directly and from
    // each chapter's own ending (see finish_chapter() in this file), so
    // each intro below just picks up "Another chair." rather than
    // re-explaining the premise every time. Each opens on its own
    // chapter's Room1 state (see kChapterRooms in game_scene_state.cpp)
    // rather than sharing Chapter 1's, so a dedicated model can eventually
    // replace man.sdf for one character without touching any other's --
    // every room state currently renders as the same placeholder (see
    // apply_scene_state()'s switch, game_scene_state.cpp), awaiting real
    // content.
    {"Chapter 2", "assets/conversations/ch2_thiago.conversation", SceneState::ThiagoRoom1,
     {"Another chair.", "His name is Thiago."}},
    {"Chapter 3", "assets/conversations/ch3_alex.conversation", SceneState::AlexRoom1,
     {"Another chair.", "His name is Alex."}},
    {"Chapter 4", "assets/conversations/ch4_diego.conversation", SceneState::DiegoRoom1,
     {"Another chair.", "His name is Diego."}},
    {"Chapter 5", "assets/conversations/ch5_mel.conversation", SceneState::MelRoom1,
     {"Another chair.", "Her name is Mel."}},
    {"Chapter 6", "assets/conversations/ch6_pat.conversation", SceneState::PatRoom1,
     {"Another chair.", "His name is Pat."}},
    {"Chapter 7", "assets/conversations/ch7_theresa.conversation", SceneState::TheresaRoom1,
     {"Another chair.", "Her name is Theresa."}},
    // Deliberately not "Another chair." -- there is no subject in the
    // chair this time; he has turned the camera around and sat down in it
    // himself.
    {"Chapter 8", "assets/conversations/ch8_photographer.conversation",
     SceneState::PhotographerRoom1,
     {"No more chairs.", "He turns the camera around."}},
};

// Mirrors kMinRenderScale/kMaxRenderScale in game_save.cpp (load_settings()'s
// own clamp) -- keep both in sync with the Vulkan backend's actual clamp
// range (vulkan_raymarch_shader.cpp) if it ever changes.
constexpr f32 kMinRenderScale = 0.05f;
constexpr f32 kMaxRenderScale = 1.0f;
constexpr f32 kRenderScaleStep = 0.05f;

// Title/Chapter Select/Settings layout (screen pixels, top-left origin).
// Anchored bottom-left, same as QASystem's question list (kListX/
// kBottomMargin there) and MenuSystem's own bottom-up row stacking -- see
// menu_bottom_left()/menu_heading_pos() below -- rather than a fixed
// absolute position, so the block stays pinned to the bottom-left corner
// (and never gets clipped off the bottom edge) regardless of window height.
constexpr f32 kMenuX = 32.0f;          // left margin, matches QASystem's kListX
constexpr f32 kMenuBottomMargin = 32.0f; // matches QASystem's kBottomMargin
constexpr f32 kMenuRowHeight = 36.0f;  // mirrors MenuSystem's own row height
                                       // -- keep in sync if that changes
constexpr f32 kMenuHeadingGap = 40.0f; // extra gap above a heading's row block

// Bottom-left anchor for a screen_height-tall window's menu block (see
// MenuSystem::update()/render()'s own bottom_left parameter).
glm::vec2 menu_bottom_left(u32 screen_height) {
  return glm::vec2(kMenuX, static_cast<f32>(screen_height) - kMenuBottomMargin);
}

// Where a heading belongs above a bottom-anchored menu block of item_count
// rows -- moves up automatically as the block grows (e.g. "Continue"
// appearing) so it never overlaps the top row.
glm::vec2 menu_heading_pos(u32 screen_height, size_t item_count) {
  return menu_bottom_left(screen_height) -
        glm::vec2(0.0f, static_cast<f32>(item_count) * kMenuRowHeight +
                            kMenuHeadingGap);
}

// Hit-test box around each of Settings' "-"/"+" glyphs -- same fixed-rect,
// eyeballed convention menu_system.cpp uses for its own rows.
constexpr glm::vec2 kSettingsClickSize(24.0f, 24.0f);

// Settings' hand-drawn rows (everything but the "Back" MenuSystem row)
// stacked bottom-up above it, mirroring how MenuSystem/QASystem build their
// own blocks upward from a bottom anchor.
struct SettingsLayout {
  glm::vec2 heading;
  glm::vec2 value;
  glm::vec2 minus;
  glm::vec2 plus;
  glm::vec2 hint;
  glm::vec2 back_bottom_left; // settings_menu_'s ("Back") anchor
};

SettingsLayout compute_settings_layout(u32 screen_height) {
  SettingsLayout layout;
  layout.back_bottom_left = menu_bottom_left(screen_height);
  layout.hint = layout.back_bottom_left - glm::vec2(0.0f, kMenuRowHeight);
  layout.value = layout.hint - glm::vec2(0.0f, kMenuRowHeight);
  layout.minus = layout.value + glm::vec2(280.0f, 0.0f);
  layout.plus = layout.value + glm::vec2(320.0f, 0.0f);
  layout.heading =
      layout.value - glm::vec2(0.0f, kMenuRowHeight + kMenuHeadingGap);
  return layout;
}

bool point_in_rect(glm::vec2 point, glm::vec2 top_left, glm::vec2 size) {
  return point.x >= top_left.x && point.x <= top_left.x + size.x &&
        point.y >= top_left.y && point.y <= top_left.y + size.y;
}

// Intertitle text is centered rather than bottom-anchored like the menu
// screens above -- but there's no text-measurement API anywhere in this
// engine (see MenuSystem's own comment), so true centering against the
// text's actual rendered width isn't possible. intertitle_line_x() uses
// text_wrap.h's shared kAverageCharWidth estimate to fake it -- nudge that
// constant if lines visibly drift off dead-center once you can see them
// rendered. Long lines are wrapped first (see render_intertitle(), which
// uses the same kAverageCharWidth estimate to decide wrap points) rather
// than left to run off the screen edge; each wrapped line is centered
// independently since they can differ in length.
constexpr f32 kIntertitleHintGap = 40.0f; // gap between the text block and its hint below
constexpr f32 kIntertitleLineSpacing = 36.0f; // vertical gap between wrapped lines
// Leaves a margin on both sides rather than letting wrapped lines span the
// full screen width edge-to-edge.
constexpr f32 kIntertitleMaxWidthFraction = 0.7f;

f32 intertitle_line_x(u32 screen_width, size_t char_count) {
  f32 estimated_width = static_cast<f32>(char_count) * kAverageCharWidth;
  return (static_cast<f32>(screen_width) - estimated_width) / 2.0f;
}

} // namespace

void SHGame::setup_menus() {
  // Chapter Select's item list is just kChapters plus a fixed "Back" --
  // unlike title_menu_ (see build_title_menu()) it never needs rebuilding,
  // so it's only ever set up here.
  std::vector<MenuSystem::Item> chapter_items;
  for (size_t i = 0; i < kChapters.size(); ++i) {
    chapter_items.push_back(
        {kChapters[i].display_name, [this, i] { start_chapter(i, /*fresh=*/true); }});
  }
  chapter_items.push_back({"Back", [this] {
                             screen_ = AppScreen::Title;
                             build_title_menu();
                           }});
  chapter_menu_.set_items(std::move(chapter_items));

  // Settings' only MenuSystem-managed row is "Back" -- the render-scale
  // adjustment itself is hand-tested (Left/Right + click zones) directly in
  // update_settings_screen(), since it isn't a plain selectable action.
  settings_menu_.set_items({{"Back", [this] {
                              screen_ = AppScreen::Title;
                              build_title_menu();
                            }}});

  // Title screen background -- real 3D geometry rather than just the flat
  // skybox set in initialize(): the same room every chapter's Room1 state
  // opens on, minus the man, lit the same way, so it reads as "the room,
  // before the story starts" rather than an arbitrary placeholder. Loaded
  // directly
  // here (bypassing apply_scene_state(), which is for SceneState-tagged
  // gameplay scenes only) so it isn't tied to any particular SceneState --
  // begin_chapter_playing()'s first apply_scene_state() call tears
  // loaded_scenes_ down like any other transition once a chapter actually
  // starts, same as show_intertitle() already does for its own black
  // screen. Swap the scene/pose below for whatever you'd rather show here
  // -- this is an eyeballed placeholder, same as this file's other camera
  // poses (see update_title_screen()/update_chapter_select()/
  // update_settings_screen() for what keeps this pose actually applied and
  // mouse-pannable across all three menu screens).
  loaded_scenes_.push_back(renderer_load_scene("assets/scenes/man.sdf")
                               .scale(3.0)
                               .translate(glm::vec3(0.0, 0.0, 0.0)));

  cameras_.set_poses({
      {glm::vec3(-1.57f, 9.76f, -5.82f), glm::radians(0.0f), glm::radians(-0.0f),
         0.0f, 0.0, 0.0},
  });

  build_title_menu();
}

void SHGame::build_title_menu() {
  std::vector<MenuSystem::Item> items;
  items.push_back({"Start", [this] { start_chapter(0, /*fresh=*/true); }});
  if (has_save()) {
    items.push_back({"Continue", [this] {
                       std::optional<SavedProgress> saved = load_save();
                       start_chapter(saved ? saved->chapter_index : 0,
                                     /*fresh=*/false);
                     }});
  }
  items.push_back(
      {"Chapter Select", [this] { screen_ = AppScreen::ChapterSelect; }});
  items.push_back({"Settings", [this] { screen_ = AppScreen::Settings; }});
  title_menu_.set_items(std::move(items));
}

void SHGame::start_chapter(size_t index, bool fresh) {
  if (fresh && !kChapters[index].intro_lines.empty()) {
    show_intertitle(kChapters[index].intro_lines,
                    [this, index] { begin_chapter_playing(index, /*fresh=*/true); });
    return;
  }
  begin_chapter_playing(index, fresh);
}

void SHGame::show_intertitle(std::vector<std::string> lines,
                             std::function<void()> on_finish) {
  // A true black backdrop rather than a quad drawn over the scene --
  // renderer_draw_solid_quad() always paints on top of every other queued
  // 2D draw this frame (see menu_system.h's class comment for the same
  // constraint), so it can't sit *behind* the intertitle text. Clearing
  // the scene and switching to the "black" skybox (the same one
  // initialize() sets as the app's own boot-time default, before
  // setup_menus() replaces it) gets a real black background instead, with
  // text drawing over it exactly like every other screen already draws
  // text over its skybox.
  for (SceneHandle handle : loaded_scenes_) {
    renderer_remove_scene(handle);
  }
  loaded_scenes_.clear();
  renderer_enable_sky_box("black");
  // Not "already there" bookkeeping for any particular SceneState -- reset
  // so the next apply_scene_state() call (from begin_chapter_playing(),
  // once this intertitle finishes) can't wrongly no-op just because its
  // target happens to equal whatever SceneState was current before this
  // intertitle started (e.g. two chapters in a row both opening on their
  // own Room1 state).
  current_state_.reset();

  intertitle_lines_ = std::move(lines);
  intertitle_line_ = 0;
  intertitle_on_finish_ = std::move(on_finish);
  screen_ = AppScreen::Intertitle;
}

void SHGame::finish_chapter(std::vector<std::string> ending_lines) {
  if (ending_lines.empty()) {
    // A `ending` question with no `ending_text=` lines of its own -- see
    // resources/conversation.h's file format comment. Keeps every ending
    // showing *something* rather than a blank outro screen.
    ending_lines = {"The End"};
  }
  size_t next_index = active_chapter_ ? *active_chapter_ + 1 : kChapters.size();
  if (next_index < kChapters.size()) {
    // Straight into the next chapter once the outro's dismissed -- reuses
    // start_chapter() so that chapter's own intro_lines (if any) still show
    // first, exactly as if it had been picked from Chapter Select.
    show_intertitle(std::move(ending_lines),
                    [this, next_index] { start_chapter(next_index, /*fresh=*/true); });
  } else {
    // No further chapter authored yet -- fall back to Chapter Select rather
    // than looping/crashing past the end of kChapters.
    show_intertitle(std::move(ending_lines),
                    [this] { screen_ = AppScreen::ChapterSelect; });
  }
}

void SHGame::begin_chapter_playing(size_t index, bool fresh) {
  if (dialogue_ != kInvalidConversationHandle) {
    qa_.unload_conversation(dialogue_);
  }
  const ChapterDef &chapter = kChapters[index];
  dialogue_ = qa_.load_conversation(chapter.conversation_path);
  active_chapter_ = index;
  // Wires this chapter's own "Room_01".."Room_05" tags -- the previous
  // chapter's (if any) were just freed by qa_.unload_conversation() above,
  // via QASystem::clear_scene_states() -- see register_chapter_scene_
  // states()'s own comment (game.h).
  register_chapter_scene_states(index);

  SceneState state_to_apply = chapter.initial_state;
  if (!fresh) {
    // Continue -- restore which questions were already asked and which
    // scene the player was actually looking at, rather than starting the
    // chapter fresh. QASystem's own navigation position (which list is
    // open, cursor_, etc.) isn't restored -- see
    // QASystem::apply_asked_flags()'s own comment -- so this lands back at
    // the top-level question list with previously-asked questions
    // darkened, not the exact mid-branch spot the player left off at.
    if (std::optional<SavedProgress> saved = load_save()) {
      qa_.apply_asked_flags(saved->asked_flags);
      qa_.apply_flags(saved->flags);
      state_to_apply = saved->scene_state;
    }
  }
  apply_scene_state(state_to_apply);

  screen_ = AppScreen::Playing;
}

SceneState SHGame::current_chapter_base_state() const {
  if (!active_chapter_) {
    return SceneState::TheoRoom1; // shouldn't happen mid-Playing -- see
                                  // this function's own declaration comment
  }
  return kChapters[*active_chapter_].initial_state;
}

void SHGame::update_title_screen() {
  toggle_debug_camera_if_pressed();
  cameras_.update(width_, height_, delta_time_); // keeps the title
                                                 // background's pose live
  // Frozen while free-flying, same reasoning as update_playing() freezing
  // qa_ -- stray Enter/arrow presses made while inspecting the background
  // scene shouldn't also navigate/select a menu item behind your back.
  if (!cameras_.debug_active()) {
    title_menu_.update(menu_bottom_left(height_));
  }
}

void SHGame::render_title_screen() const {
  render_debug_camera_hud();
  renderer_draw_text("SH", menu_heading_pos(height_, title_menu_.item_count()),
                     glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
  title_menu_.render(menu_bottom_left(height_));
}

void SHGame::update_chapter_select() {
  toggle_debug_camera_if_pressed();
  cameras_.update(width_, height_, delta_time_);
  if (!cameras_.debug_active()) {
    chapter_menu_.update(menu_bottom_left(height_));
  }
}

void SHGame::render_chapter_select() const {
  render_debug_camera_hud();
  renderer_draw_text(
      "Chapter Select", menu_heading_pos(height_, chapter_menu_.item_count()),
      glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
  chapter_menu_.render(menu_bottom_left(height_));
}

void SHGame::update_settings_screen() {
  toggle_debug_camera_if_pressed();
  cameras_.update(width_, height_, delta_time_);
  if (cameras_.debug_active()) {
    return; // frozen while free-flying -- see update_title_screen()'s
            // own comment
  }

  SettingsLayout layout = compute_settings_layout(height_);
  if (input::is_key_down(input::Key::Left) &&
      !input::was_key_down(input::Key::Left)) {
    render_scale_ = std::clamp(render_scale_ - kRenderScaleStep,
                               kMinRenderScale, kMaxRenderScale);
    renderer_set_render_scale(render_scale_);
    save_settings();
  }
  if (input::is_key_down(input::Key::Right) &&
      !input::was_key_down(input::Key::Right)) {
    render_scale_ = std::clamp(render_scale_ + kRenderScaleStep,
                               kMinRenderScale, kMaxRenderScale);
    renderer_set_render_scale(render_scale_);
    save_settings();
  }
  if (input::is_button_down(input::Button::Left) &&
      !input::was_button_down(input::Button::Left)) {
    input::MousePosition mouse = input::mouse_position();
    glm::vec2 m(static_cast<f32>(mouse.x), static_cast<f32>(mouse.y));
    if (point_in_rect(m, layout.minus, kSettingsClickSize)) {
      render_scale_ = std::clamp(render_scale_ - kRenderScaleStep,
                                 kMinRenderScale, kMaxRenderScale);
      renderer_set_render_scale(render_scale_);
      save_settings();
    } else if (point_in_rect(m, layout.plus, kSettingsClickSize)) {
      render_scale_ = std::clamp(render_scale_ + kRenderScaleStep,
                                 kMinRenderScale, kMaxRenderScale);
      renderer_set_render_scale(render_scale_);
      save_settings();
    }
  }
  settings_menu_.update(layout.back_bottom_left); // just "Back"
}

void SHGame::render_settings_screen() const {
  render_debug_camera_hud();
  SettingsLayout layout = compute_settings_layout(height_);
  renderer_draw_text("Settings", layout.heading,
                     glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
  renderer_draw_text(
      std::format("Render Scale: {:.0f}%", render_scale_ * 100.0f),
      layout.value, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
  renderer_draw_text("-", layout.minus, glm::vec4(1.0f, 0.9f, 0.5f, 1.0f));
  renderer_draw_text("+", layout.plus, glm::vec4(1.0f, 0.9f, 0.5f, 1.0f));
  renderer_draw_text("[Left/Right] or click -/+ to adjust", layout.hint,
                     glm::vec4(0.5f, 0.5f, 0.5f, 1.0f));
  settings_menu_.render(layout.back_bottom_left);
}

void SHGame::update_intertitle() {
  // A click or [Enter] advances -- mirrors QASystem's answer-line advance
  // (Enter only) plus every menu screen's click support.
  bool advance = (input::is_key_down(input::Key::Enter) &&
                 !input::was_key_down(input::Key::Enter)) ||
                (input::is_button_down(input::Button::Left) &&
                 !input::was_button_down(input::Button::Left));
  if (!advance) {
    return;
  }
  ++intertitle_line_;
  if (intertitle_line_ >= intertitle_lines_.size()) {
    // Move on_finish out first -- it's about to run and typically changes
    // screen_ itself (e.g. to Playing), so there's nothing left in
    // intertitle_on_finish_ worth keeping around regardless of what it does.
    std::function<void()> on_finish = std::move(intertitle_on_finish_);
    intertitle_on_finish_ = nullptr;
    if (on_finish) {
      on_finish();
    }
  }
}

void SHGame::render_intertitle() const {
  // The scene/skybox were already cleared to black in show_intertitle() --
  // nothing else to draw behind the text here.
  //
  // Defaults to plain screen-center -- only overridden below once the
  // current line's own wrapped block height is known, so the hint still
  // lands somewhere sane in the (shouldn't-normally-happen) case
  // intertitle_line_ is out of range.
  f32 hint_y = static_cast<f32>(height_) / 2.0f + kIntertitleHintGap;
  if (intertitle_line_ < intertitle_lines_.size()) {
    // Long lines wrap onto further lines rather than running off the
    // screen edge (see text_wrap.h's wrap_text()) -- each wrapped line is
    // centered independently (they can differ in length), and the whole
    // block is vertically centered as a unit so a longer line doesn't
    // just grow downward past the hint.
    std::vector<std::string> wrapped =
        wrap_text(intertitle_lines_[intertitle_line_],
                  static_cast<f32>(width_) * kIntertitleMaxWidthFraction);
    f32 block_height =
        static_cast<f32>(wrapped.size() - 1) * kIntertitleLineSpacing;
    f32 top_y = static_cast<f32>(height_) / 2.0f - block_height / 2.0f;
    for (size_t i = 0; i < wrapped.size(); ++i) {
      f32 line_y = top_y + static_cast<f32>(i) * kIntertitleLineSpacing;
      renderer_draw_text(
          wrapped[i],
          glm::vec2(intertitle_line_x(width_, wrapped[i].size()), line_y),
          glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
    }
    hint_y = top_y + block_height + kIntertitleHintGap;
  }
  constexpr std::string_view kHint = "Click or [Enter] to continue";
  renderer_draw_text(
      kHint, glm::vec2(intertitle_line_x(width_, kHint.size()), hint_y),
      glm::vec4(0.5f, 0.5f, 0.5f, 1.0f));
}
