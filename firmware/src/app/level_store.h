// level_store.h - the ten level slots, settings and progress.   Port of sim/src/level_store.ts.   (platform adapter)
//
// Files live in LittleFS (on the device) or a directory (the emulator / native tests), used through plain stdio:
//   <root>/slot0.scrb ... slot9.scrb     the .scrb bytes (docs/system-design.md section 4)
//   <root>/settings.json                 {"sound":1,"lastSlot":0,"solvedMask":0,"firstRunDone":false,"standupSeen":false}
// Saves are atomic: write slotN.tmp, fflush + fsync, rename over slotN.scrb (docs/system-design.md 4.4). A corrupt file shows
// as an empty slot with a warning dot (`broken[i]`) and is never deleted by the loader. Slots live in PSRAM.
#pragma once
#include <cstdint>
#include "config.h"
#include "level.h"

namespace scribble {

struct Settings { int sound = 1; int lastSlot = 0; uint32_t solvedMask = 0; bool firstRunDone = false; bool standupSeen = false; };

/** A new blank level: a floor, a Start ball and a Flag, so Play works at once. Port of blankLevel(). */
void blankLevel(Level& out);

class LevelStore {
 public:
  Level* slots[SLOTS] = {};                       // nullptr = empty (allocated lazily, PSRAM)
  uint8_t* thumbs[SLOTS] = {};                    // THUMB_BYTES each, or nullptr
  bool broken[SLOTS] = {};
  Settings settings;

  /** `root` has no trailing slash, e.g. "/littlefs" on the device. Mounts nothing itself; main() mounts LittleFS first. */
  bool begin(const char* root);
  bool save(int slot, Level& level);              // bumps level.rev; returns false if the write failed (the old file stays intact)
  void remove(int slot);
  int firstEmpty() const;                         // -1 if all full
  bool isSolved(int slot) const { return (settings.solvedMask >> slot) & 1; }
  void markSolved(int slot);
  void saveSettings();
  /** Put a decoded .scrb into the first empty slot (or the given one). Returns the slot or -1. */
  int importBytes(const uint8_t* bytes, size_t n, int slot = -1);
  /** Copy slot -> out (out must be non-null). Returns false if the slot is empty. */
  bool copyTo(int slot, Level& out) const;
 private:
  char root_[96] = "";
  bool writeAtomic(const char* path, const uint8_t* data, size_t n);
  void loadSlot(int i);
};

}  // namespace scribble
