// game_draw.h - state owned by the Draw / Edit screen.   OWNER: Draw agent. Edit freely; nobody else touches it.
// The rules (strokes, undo ring, pins, ghost) live in lib/core/src/editing.h so the PC tests can run them.
#pragma once
#include "editing.h"
#include "level.h"
#include "config.h"
namespace scribble {

struct DrawState {
  Tool tool = Tool::Ground;
  UndoRing undo;                     // UNDO_LEVELS snapshots, allocated lazily in PSRAM (8 x 23 KB) on the first edit
  Stroke stroke;
  Pin pinDrag = Pin::None;
  bool ghostDone = false;            // the ghost finger stops for good once he touches the screen
  uint32_t idleSince = 0;            // millis() of the last touch (or of entering Draw)
  uint32_t serial = 0;               // Game::levelSerial of the level the undo ring belongs to
  bool menuSnapValid = false;        // set when Draw hands over to the menu overlay: lets a Clear done there be undone
  LevelSnap* menuSnap = nullptr;     // the level as it was when the menu opened

  /** Remember the level before an edit (call it BEFORE changing the level). Any screen that edits `level` (e.g. the menu's Clear) may call it. */
  void pushUndo(const Level& l);
  bool ensureUndo();                 // allocate the ring on first use; false if out of memory
  bool canUndo() const { return undo.size() > 0; }
};


class Game;
/** The playfield alone (no buttons): the menu overlay may call this under its dimmer, or just call Game::drawDraw(). */
void drawDrawScene(Game& g);

}  // namespace scribble
