// game_stubs.cpp - weak, do-nothing screens so the project always links while the real ones are written.
// A real tickX/drawX in game_<screen>.cpp overrides the stub automatically (weak symbols); delete the stub when you add it.
#include "game.h"
namespace scribble {
#define STUB(T, D) \
  __attribute__((weak)) void Game::tick##T(const InputState&) {} \
  __attribute__((weak)) void Game::draw##D() { r.clearPaper(); r.text(#T " (not built yet)", 160, 120, 18); }
STUB(Standup, Standup) STUB(Check, Check) STUB(Countdown, Countdown)
STUB(Processing, Processing) STUB(Review, Review) STUB(Draw, Draw)
}  // namespace scribble
