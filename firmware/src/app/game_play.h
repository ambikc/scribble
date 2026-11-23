// game_play.h - state owned by the Play and Win screens (and their effects).   OWNER: Play/Win agent. Edit freely; nobody else touches it.
#pragma once
#include <cstdint>
#include "physics.h"
namespace scribble {
class Game;
struct PlayState {
  World world{};
  float fxStretch = 0, fxSquash = 0, fxShake = 0, fxRed = 0, fxFlash = 0; uint16_t fxFlashColor = 0xFFFF;
  float winT = 0, tiltHint = 0, spawnRing = 0; bool everPlayed = false;

  // "oops puffs": where the last 5 pops happened; cleared when the level's pixels change (docs/game-design.md section 8)
  struct Puff { float x, y; };
  Puff puffs[5]; int nPuffs = 0;

  // The static part of the scene (paper + the three material layers), rendered once per level into a full-frame copy and blitted every
  // frame instead of clearPaper() + drawLayers(). Rebuilt when Play is entered and the layers' CRC differs from `sceneCrc`.
  uint16_t* scene = nullptr;       // 320 x 240 RGB565, byte-swapped like Renderer::px(); null = could not allocate (draw uncached)
  bool sceneOk = false;
  uint32_t sceneCrc = 0; bool haveCrc = false;
};

/** The Play scene alone (level, puffs, flag, ball, particles, tilt hint, red flash) without buttons: the pause menu paints it behind its dimmer. */
void drawPlayScene(Game& g);
}  // namespace scribble
