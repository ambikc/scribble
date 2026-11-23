// input.h - touch + tilt, one InputState per 60 Hz tick.   Port of sim/src/input.ts.   (platform adapter: M5Unified)
//
//   touch : ONE finger (single touch). Edges (pressed / released) are LATCHED until consumeEdges() so a tap is never
//           lost on a loop pass that happens to run no tick (docs/system-design.md 6.1).
//   tilt  : BMI270 accelerometer -> -1..1 with dead-zone, linear to TILT_FULL, low-pass (same maths as input.ts).
//           Which axis / sign rolls the ball is set by TILT_AXIS / TILT_SIGN below (measure with the hardware-check app).
//   upright: "the device is standing up" (IMU says the screen faces the user), used by the camera stand-up flow.
#pragma once
#include <cstdint>
#include "config.h"
#include "tilt.h"

namespace scribble {

constexpr int TILT_AXIS = 0;        // 0 = x, 1 = y, 2 = z of the accelerometer; confirm on the device (build-plan Step 2)
constexpr int TILT_SIGN = 1;        // +1 or -1

struct TouchState {
  bool down = false, pressed = false, released = false;   // pressed/released are one-tick edges
  int x = 0, y = 0;                                        // 320 x 240 screen pixels
  int startX = 0, startY = 0;                              // where this touch began
  uint32_t heldMs = 0;
};
struct InputState {
  TouchState touch;
  double tilt = 0;                // -1..1, filtered
  bool upright = true;
  bool keyJump = false;           // emulator only: Space; always false on the device
  bool keyBack = false;           // emulator only: Escape
};

class Input {
 public:
  void begin();
  /** Read M5.Touch (and the emulator keys) and latch the edges. main() calls it once per loop pass, right after M5.update(), so a
   *  tap is caught even on a pass that runs no tick. Idempotent: poll() calls it too, and edges are never doubled. */
  void sample(uint32_t nowMs);
  /** Call once per 60 Hz tick (reads M5.Imu, applies the tilt filter once, returns the state with the latched edges). */
  InputState poll(uint32_t nowMs);
  /** Call after a tick has used the edges. */
  void consumeEdges();
  /** Hold a virtual tilt (used by the emulator's on-screen keys and by tests). -1, 0, +1. */
  void setVirtualTilt(int dir) { virtualDir_ = dir; }
  /** Raw accelerometer x/y/z in g from the last update (for the calibration screen). */
  const float* rawAccel() const { return acc_; }
 private:
  bool down_ = false, pressedEdge_ = false, releasedEdge_ = false;
  int x_ = 0, y_ = 0, sx_ = 0, sy_ = 0; uint32_t downAt_ = 0;
  float acc_[3] = {0, 0, 0};
  TiltFilter tilt_;                // dead-zone + low-pass, stepped once per tick (lib/core/src/tilt.h)
  double smoothed_ = 0;
  bool upright_ = true;
  bool jumpEdge_ = false, backEdge_ = false, jumpHeld_ = false, backHeld_ = false;   // emulator keys
  int virtualDir_ = 0;
};

}  // namespace scribble
