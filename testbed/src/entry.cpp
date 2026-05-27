#include "game.h"
#include <core/hmemory.h>
#include <entry.h>
#include <memory>

std::unique_ptr<Game> create_game() {
  TestbedGame *game = Memory::create<TestbedGame>(MemoryTag::Game);
  return std::unique_ptr<Game>(game);
}
