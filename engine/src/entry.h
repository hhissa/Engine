#pragma once
#include "core/application.h"
#include "core/hmemory.h"
#include "core/logger.h"
#include "game_types.h"
#include "platform/filesystem.h"
#include "platform/platform.h"
#include <format>
#include <memory>

// Externally-defined factory function provided by the game.
extern std::unique_ptr<Game> create_game();

int main() {
  // Before anything opens a file -- see the function's comment.
  Platform::set_working_directory_to_executable();
  Platform::install_crash_handler();
  if (!Filesystem::exists("assets")) {
    // Most often: SH.exe run straight out of the zip, which Windows
    // extracts on its own -- without the DLLs or assets beside it.
    Platform::show_error_dialog(
        "Couldn't find the game's 'assets' folder.\n\n"
        "Extract the whole zip to a folder first, then run SH.exe from "
        "there.");
    return 3;
  }
  Memory::initialize();
  auto game = create_game();
  if (!game) {
    KFATAL("Could not create game!");
    return -1;
  }

  Application app(game.get());

  if (!app.application_start()) {
    KINFO("Application did not shut down gracefully.");
    Platform::show_error_dialog(std::format(
        "SH failed to start:\n\n{}\n\nconsole.log in the game's folder has "
        "the details.",
        Logger::last_fatal_message()));
    return 2;
  }
  Memory::shutdown();
  return 0;
}
