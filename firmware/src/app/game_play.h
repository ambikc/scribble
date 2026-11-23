// game_play.h - state owned by the Play and Win screens (and their effects).   OWNER: Play/Win agent. Edit freely; nobody else touches it.
#pragma once
#include "physics.h"
namespace scribble {
struct PlayState {
  World world{};
  float fxStretch = 0, fxSquash = 0, fxShake = 0, fxRed = 0, fxFlash = 0; uint16_t fxFlashColor = 0xFFFF;
  float winT = 0, tiltHint = 0, spawnRing = 0; bool everPlayed = false;
  // add what you need
};
}  // namespace scribble
