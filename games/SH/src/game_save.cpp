// Disk persistence: a save file (which chapter, which scene, and which
// questions have been asked) and a settings file (render_scale_), both
// plain "key=value" lines written via engine/src/platform/filesystem.h's
// FileHandle -- see game.h's "game_save.cpp" section for the public
// surface these functions implement.

#include "game.h"

#include <core/logger.h>
#include <platform/filesystem.h>
#include <renderer/renderer_frontend.h>

#include <algorithm>
#include <format>
#include <string>
#include <string_view>

namespace {

// Where save_progress()/load_save() and load_settings()/save_settings()
// read and write, relative to the process's working directory -- the same
// root "assets/..." paths already resolve against (bin/), so these end up
// sitting alongside assets/ rather than under it (they're player-generated
// state, not shipped content).
constexpr std::string_view kSaveFilePath = "sh_save.txt";
constexpr std::string_view kSettingsFilePath = "sh_settings.txt";

// Mirrors kMinRenderScale/kMaxRenderScale in game_menus.cpp (the Settings
// screen's own Left/Right/click adjustment) -- keep both in sync with the
// Vulkan backend's actual clamp range (vulkan_raymarch_shader.cpp) if it
// ever changes.
constexpr f32 kMinRenderScale = 0.05f;
constexpr f32 kMaxRenderScale = 1.0f;

// SceneState <-> its own enumerator name (game.h), used as the save file's
// scene_state= value. Not the same string as the .conversation file's tag=
// name -- "Room_01" is shared by every chapter (see game.h's SceneState
// comment), so it can't unambiguously round-trip back to a SceneState on
// its own; chapter_index (also saved, see load_save()/save_progress()
// below) plus this chapter-and-room-specific enumerator name together
// pin down exactly one of the 40 states. to_string has no default case
// (every enumerator must be listed) so a future SceneState addition fails
// to compile here as a reminder to extend it; from_string returns nullopt
// for anything else, including a stale name from a save written before a
// scene state was renamed.
std::string scene_state_to_string(SceneState state) {
  switch (state) {
  case SceneState::TheoRoom1: return "TheoRoom1";
  case SceneState::TheoRoom2: return "TheoRoom2";
  case SceneState::TheoRoom3: return "TheoRoom3";
  case SceneState::TheoRoom4: return "TheoRoom4";
  case SceneState::TheoRoom5: return "TheoRoom5";
  case SceneState::ThiagoRoom1: return "ThiagoRoom1";
  case SceneState::ThiagoRoom2: return "ThiagoRoom2";
  case SceneState::ThiagoRoom3: return "ThiagoRoom3";
  case SceneState::ThiagoRoom4: return "ThiagoRoom4";
  case SceneState::ThiagoRoom5: return "ThiagoRoom5";
  case SceneState::AlexRoom1: return "AlexRoom1";
  case SceneState::AlexRoom2: return "AlexRoom2";
  case SceneState::AlexRoom3: return "AlexRoom3";
  case SceneState::AlexRoom4: return "AlexRoom4";
  case SceneState::AlexRoom5: return "AlexRoom5";
  case SceneState::DiegoRoom1: return "DiegoRoom1";
  case SceneState::DiegoRoom2: return "DiegoRoom2";
  case SceneState::DiegoRoom3: return "DiegoRoom3";
  case SceneState::DiegoRoom4: return "DiegoRoom4";
  case SceneState::DiegoRoom5: return "DiegoRoom5";
  case SceneState::MelRoom1: return "MelRoom1";
  case SceneState::MelRoom2: return "MelRoom2";
  case SceneState::MelRoom3: return "MelRoom3";
  case SceneState::MelRoom4: return "MelRoom4";
  case SceneState::MelRoom5: return "MelRoom5";
  case SceneState::PatRoom1: return "PatRoom1";
  case SceneState::PatRoom2: return "PatRoom2";
  case SceneState::PatRoom3: return "PatRoom3";
  case SceneState::PatRoom4: return "PatRoom4";
  case SceneState::PatRoom5: return "PatRoom5";
  case SceneState::TheresaRoom1: return "TheresaRoom1";
  case SceneState::TheresaRoom2: return "TheresaRoom2";
  case SceneState::TheresaRoom3: return "TheresaRoom3";
  case SceneState::TheresaRoom4: return "TheresaRoom4";
  case SceneState::TheresaRoom5: return "TheresaRoom5";
  case SceneState::PhotographerRoom1: return "PhotographerRoom1";
  case SceneState::PhotographerRoom2: return "PhotographerRoom2";
  case SceneState::PhotographerRoom3: return "PhotographerRoom3";
  case SceneState::PhotographerRoom4: return "PhotographerRoom4";
  case SceneState::PhotographerRoom5: return "PhotographerRoom5";
  }
  return "TheoRoom1"; // unreachable -- silences -Wreturn-type
}

std::optional<SceneState> scene_state_from_string(std::string_view name) {
  if (name == "TheoRoom1") return SceneState::TheoRoom1;
  if (name == "TheoRoom2") return SceneState::TheoRoom2;
  if (name == "TheoRoom3") return SceneState::TheoRoom3;
  if (name == "TheoRoom4") return SceneState::TheoRoom4;
  if (name == "TheoRoom5") return SceneState::TheoRoom5;
  if (name == "ThiagoRoom1") return SceneState::ThiagoRoom1;
  if (name == "ThiagoRoom2") return SceneState::ThiagoRoom2;
  if (name == "ThiagoRoom3") return SceneState::ThiagoRoom3;
  if (name == "ThiagoRoom4") return SceneState::ThiagoRoom4;
  if (name == "ThiagoRoom5") return SceneState::ThiagoRoom5;
  if (name == "AlexRoom1") return SceneState::AlexRoom1;
  if (name == "AlexRoom2") return SceneState::AlexRoom2;
  if (name == "AlexRoom3") return SceneState::AlexRoom3;
  if (name == "AlexRoom4") return SceneState::AlexRoom4;
  if (name == "AlexRoom5") return SceneState::AlexRoom5;
  if (name == "DiegoRoom1") return SceneState::DiegoRoom1;
  if (name == "DiegoRoom2") return SceneState::DiegoRoom2;
  if (name == "DiegoRoom3") return SceneState::DiegoRoom3;
  if (name == "DiegoRoom4") return SceneState::DiegoRoom4;
  if (name == "DiegoRoom5") return SceneState::DiegoRoom5;
  if (name == "MelRoom1") return SceneState::MelRoom1;
  if (name == "MelRoom2") return SceneState::MelRoom2;
  if (name == "MelRoom3") return SceneState::MelRoom3;
  if (name == "MelRoom4") return SceneState::MelRoom4;
  if (name == "MelRoom5") return SceneState::MelRoom5;
  if (name == "PatRoom1") return SceneState::PatRoom1;
  if (name == "PatRoom2") return SceneState::PatRoom2;
  if (name == "PatRoom3") return SceneState::PatRoom3;
  if (name == "PatRoom4") return SceneState::PatRoom4;
  if (name == "PatRoom5") return SceneState::PatRoom5;
  if (name == "TheresaRoom1") return SceneState::TheresaRoom1;
  if (name == "TheresaRoom2") return SceneState::TheresaRoom2;
  if (name == "TheresaRoom3") return SceneState::TheresaRoom3;
  if (name == "TheresaRoom4") return SceneState::TheresaRoom4;
  if (name == "TheresaRoom5") return SceneState::TheresaRoom5;
  if (name == "PhotographerRoom1") return SceneState::PhotographerRoom1;
  if (name == "PhotographerRoom2") return SceneState::PhotographerRoom2;
  if (name == "PhotographerRoom3") return SceneState::PhotographerRoom3;
  if (name == "PhotographerRoom4") return SceneState::PhotographerRoom4;
  if (name == "PhotographerRoom5") return SceneState::PhotographerRoom5;

  return std::nullopt;
}

// Both save/settings files are "key=value" lines -- returns the part after
// '=' if line starts with exactly "key=", nullopt otherwise (wrong key, or
// no '=' at all).
std::optional<std::string_view> parse_kv(std::string_view line,
                                         std::string_view key) {
  if (line.size() <= key.size() || line.substr(0, key.size()) != key ||
      line[key.size()] != '=') {
    return std::nullopt;
  }
  return line.substr(key.size() + 1);
}

// Parses "asked="'s value -- space-separated "1"/"0" tokens, in the same
// order QASystem::asked_flags() produced them -- back into a flag vector.
std::vector<bool> parse_asked_flags(std::string_view value) {
  std::vector<bool> flags;
  size_t pos = 0;
  while (pos < value.size()) {
    size_t space = value.find(' ', pos);
    std::string_view token = value.substr(
        pos, space == std::string_view::npos ? std::string_view::npos
                                             : space - pos);
    if (!token.empty()) {
      flags.push_back(token == "1");
    }
    if (space == std::string_view::npos) {
      break;
    }
    pos = space + 1;
  }
  return flags;
}

// Parses "flags="'s value -- space-separated flag names, in the same order
// QASystem::flags() produced them -- back into a name list. Mirrors
// parse_asked_flags() above but keeps the raw tokens instead of turning
// them into bools (a flag name isn't positional the way asked_flags()'s
// entries are, so there's no fixed slot to decode against).
std::vector<std::string> parse_flags(std::string_view value) {
  std::vector<std::string> flags;
  size_t pos = 0;
  while (pos < value.size()) {
    size_t space = value.find(' ', pos);
    std::string_view token = value.substr(
        pos, space == std::string_view::npos ? std::string_view::npos
                                             : space - pos);
    if (!token.empty()) {
      flags.emplace_back(token);
    }
    if (space == std::string_view::npos) {
      break;
    }
    pos = space + 1;
  }
  return flags;
}

} // namespace

bool SHGame::has_save() const { return Filesystem::exists(kSaveFilePath); }

std::optional<SavedProgress> SHGame::load_save() const {
  if (!Filesystem::exists(kSaveFilePath)) {
    return std::nullopt;
  }
  FileHandle file(kSaveFilePath, FileMode::Read, /*binary=*/false);
  if (!file) {
    KWARN("SHGame: couldn't open '{}' for reading.", kSaveFilePath);
    return std::nullopt;
  }

  std::optional<std::string> chapter_line = file.read_line();
  std::optional<std::string> scene_line = file.read_line();
  std::optional<std::string> asked_line = file.read_line();
  std::optional<std::string> flags_line = file.read_line();
  if (!chapter_line || !scene_line || !asked_line || !flags_line) {
    KWARN("SHGame: '{}' is missing a line -- ignoring.", kSaveFilePath);
    return std::nullopt;
  }

  std::optional<std::string_view> chapter_value =
      parse_kv(*chapter_line, "chapter_index");
  std::optional<std::string_view> scene_value =
      parse_kv(*scene_line, "scene_state");
  std::optional<std::string_view> asked_value =
      parse_kv(*asked_line, "asked");
  std::optional<std::string_view> flags_value =
      parse_kv(*flags_line, "flags");
  if (!chapter_value || !scene_value || !asked_value || !flags_value) {
    KWARN("SHGame: '{}' has an unexpected line format -- ignoring.",
         kSaveFilePath);
    return std::nullopt;
  }

  std::optional<SceneState> scene_state =
      scene_state_from_string(*scene_value);
  if (!scene_state) {
    KWARN("SHGame: '{}' names an unknown scene state '{}' -- ignoring.",
         kSaveFilePath, *scene_value);
    return std::nullopt;
  }

  SavedProgress progress;
  progress.chapter_index =
      static_cast<size_t>(std::stoul(std::string(*chapter_value)));
  progress.scene_state = *scene_state;
  progress.asked_flags = parse_asked_flags(*asked_value);
  progress.flags = parse_flags(*flags_value);
  return progress;
}

void SHGame::save_progress() {
  // Only meaningful mid-chapter -- set_on_any_asked() only fires from
  // qa_.update(), which itself only runs while screen_ == Playing (see
  // update_playing() in game_playing.cpp), so active_chapter_/
  // current_state_ are always set by the time this runs; the checks below
  // are cheap defensive guards rather than an expected path.
  if (!active_chapter_ || !current_state_) {
    return;
  }
  FileHandle file(kSaveFilePath, FileMode::Write, /*binary=*/false);
  if (!file) {
    KWARN("SHGame::save_progress: couldn't open '{}' for writing.",
         kSaveFilePath);
    return;
  }
  file.write_line(std::format("chapter_index={}", *active_chapter_));
  file.write_line(
      std::format("scene_state={}", scene_state_to_string(*current_state_)));

  std::string asked_line = "asked=";
  const std::vector<bool> asked = qa_.asked_flags();
  for (size_t i = 0; i < asked.size(); ++i) {
    if (i > 0) {
      asked_line += ' ';
    }
    asked_line += asked[i] ? '1' : '0';
  }
  file.write_line(asked_line);

  std::string flags_line = "flags=";
  const std::vector<std::string> flags = qa_.flags();
  for (size_t i = 0; i < flags.size(); ++i) {
    if (i > 0) {
      flags_line += ' ';
    }
    flags_line += flags[i];
  }
  file.write_line(flags_line);
}

void SHGame::load_settings() {
  if (!Filesystem::exists(kSettingsFilePath)) {
    return;
  }
  FileHandle file(kSettingsFilePath, FileMode::Read, /*binary=*/false);
  if (!file) {
    return;
  }
  std::optional<std::string> line = file.read_line();
  if (!line) {
    return;
  }
  if (std::optional<std::string_view> value = parse_kv(*line, "render_scale")) {
    render_scale_ = std::clamp(std::stof(std::string(*value)),
                               kMinRenderScale, kMaxRenderScale);
  }
}

void SHGame::save_settings() const {
  FileHandle file(kSettingsFilePath, FileMode::Write, /*binary=*/false);
  if (!file) {
    KWARN("SHGame::save_settings: couldn't open '{}' for writing.",
         kSettingsFilePath);
    return;
  }
  file.write_line(std::format("render_scale={:.2f}", render_scale_));
}
