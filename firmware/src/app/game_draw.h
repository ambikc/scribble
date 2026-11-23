// game_draw.h - state owned by the Draw / Edit screen.   OWNER: Draw agent. Edit freely; nobody else touches it.
#pragma once
#include "level.h"
#include "config.h"
namespace scribble {
enum class Tool : uint8_t { Ground, Spike, Bouncy, Eraser };
struct DrawState {
  Tool tool = Tool::Ground;
  // undo ring (UNDO_LEVELS full levels is 8 x 23 KB: allocate in PSRAM lazily), stroke state, pin dragging, ghost ball ...
};
}  // namespace scribble
