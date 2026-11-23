// game_menus.h - state owned by Title, Picker, Chooser and the pause Menu.   OWNER: Menus agent. Edit freely; nobody else touches it.
#pragma once
namespace scribble {
struct MenusState {
  bool deleteMode = false;     // Picker: the bin is on, a filled slot needs a DELETE_HOLD_MS hold to be deleted
  int pendingSlot = -1;        // the empty slot the Picker (or Win's "New level") chose; the Chooser's Finger and the paper Review put the new level there (-1 = first empty)
};
}  // namespace scribble
