// game_paper.h - state owned by the paper-mode screens (stand-up, check, countdown, processing, review).   OWNER: Paper agent.
// Edit freely; nobody else touches it. The screens: game_paper.cpp. The camera: camera.h. The vision core: lib/core/src/vision.h.
#pragma once
#include <cstdint>
#include "level.h"
#include "vision.h"

namespace scribble {

struct PaperState {
  // ---- buffers, allocated once on the first Check (PSRAM on the device; docs/system-design.md 8.2) -----------------------------
  VisionWork* work = nullptr;              // 770 KB: the straightened page, ink, classes and all scratch; Review re-thresholds from it
  uint16_t* snap = nullptr;                // 640 x 480 RGB565: the averaged photo taken at "snap!" (Processing shows it dimmed)
  uint8_t* gray = nullptr;                 // 320 x 240: the newest preview frame for the live check
  Level* res = nullptr;                    // the recovered level, zero-initialised on purpose (the golden CRC covers the whole struct)
  Level* tmp = nullptr;                    // a re-threshold writes here first, so the screen never draws a half-made level; also the save scratch
  bool allocFailed = false;

  // ---- stand-up ----
  bool wasUpright = false;                 // the device was standing the last tick (plays one tick sound when it gets stood up)

  // ---- check ----
  LiveCheck lc{}; bool lcValid = false;    // the newest live-check result (lc.marks says whether the four marks were found)
  uint32_t lcAt = 0, lcFrame = 0;          // when the last check started; the camera frame it was made from
  float greenFor = 0;                      // seconds all four lights have been green (GO pulses from GREEN_HOLD_S)
  bool debugNums = false;                  // hold the ? chip 2 s: calibration numbers

  // ---- countdown ----
  float cdT = 0; bool said[4] = {false, false, false, false}; bool snapAsked = false, snapped = false;

  // ---- processing / review ----
  bool haveSnap = false;                   // `snap` holds a photo
  bool procStarted = false, procDone = false; float procT = 0;
  bool haveResult = false;                 // `res` / `report` / `work` hold a processed photo (Review has something to show)
  VisionParams params{};                   // the parameters of `res` (the ink thickness buttons move inkThresh)
  VisionReport report{};
  float reveal = 0;                        // seconds on Review: ground, spikes, bouncy appear one by one
  bool busy = false;                       // a re-threshold is running
  int pinDrag = 0;                         // 0 none, 1 start pin, 2 flag pin
};

}  // namespace scribble
