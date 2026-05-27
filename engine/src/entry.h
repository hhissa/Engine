#pragma once
#include "core/application.h"
#include "core/hmemory.h"
#include "core/logger.h"
#include "game_types.h"
#include <memory>

// Externally-defined factory function provided by the game.
extern std::unique_ptr<Game> create_game();

int main() {
  Memory::initialize();
  auto game = create_game();
  if (!game) {
    KFATAL("Could not create game!");
    return -1;
  }

  Application app(game.get());

  if (!app.application_start()) {
    KINFO("Application did not shut down gracefully.");
    return 2;
  }
  Memory::shutdown();
  return 0;
}
