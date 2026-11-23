// tilt.h - the tilt filter and the "is the device standing up" test.   (portable core: header-only, no Arduino)
// Port of the tilt maths in sim/src/input.ts (docs/game-design.md section 5.4).
//
//   raw accelerometer component (g)  ->  dead-zone  ->  linear up to TILT_FULL  ->  clamp to -1..1  ->  low-pass.
// The low-pass runs once per 60 Hz TICK (not per loop pass), so the feel does not depend on the display rate.
#pragma once
#include <cmath>
#include "config.h"

namespace scribble {

/** Dead-zone and linear scale, no smoothing: raw g (signed) -> -1..1. Exactly the `target` line of input.ts. */
inline double tiltTarget(double rawG) {
  const double mag = std::fmax(0.0, std::fabs(rawG) - TILT_DEADZONE) / (TILT_FULL - TILT_DEADZONE);
  const double s = rawG > 0 ? 1.0 : rawG < 0 ? -1.0 : 0.0;   // Math.sign
  return s * std::fmin(1.0, mag);
}

struct TiltFilter {
  double smoothed = 0;
  /** One 60 Hz tick: feed the raw g along the rolling axis (sign already applied); returns the filtered tilt -1..1. */
  double step(double rawG) {
    smoothed += (tiltTarget(rawG) - smoothed) * TILT_SMOOTH;
    if (std::fabs(smoothed) < 0.002) smoothed = 0;           // settle exactly to zero
    return smoothed;
  }
  void reset() { smoothed = 0; }
};

// ---- "upright": the device stands on its base with the screen facing the user (the camera stand-up screen) --------------
// ASSUMPTION (to confirm on the device, build-plan Step 2): in landscape the accelerometer sees the screen's horizontal axis
// as the ROLL axis (the one that rolls the ball), the screen's vertical axis as `along`, and the screen normal as `normal`.
//   flat on a table  : gravity is on `normal`            (|normal| ~ 1, |along| ~ 0)
//   standing upright : gravity is on `along`, either sign (|along| ~ 1, |normal| ~ 0)  -> the camera looks horizontally
// Standing is accepted up to about 40 degrees of lean back (|along| >= 0.77) and ~17 degrees of roll sideways; hysteresis (leave
// at 0.60 / 0.45 g) keeps the stand-up screen from flickering at the limits. The sign of `along` is ignored on purpose, so
// either way of standing (up or upside down) counts and nothing depends on the device's axis directions.
constexpr double UPRIGHT_ENTER_ALONG = 0.77, UPRIGHT_ENTER_ROLL = 0.30;
constexpr double UPRIGHT_LEAVE_ALONG = 0.60, UPRIGHT_LEAVE_ROLL = 0.45;

inline bool uprightFromAccel(double roll, double along, double normal, bool wasUpright) {
  (void)normal;   // implied by the other two (|a| ~ 1 g); kept in the signature for clarity and future use
  const double a = std::fabs(along), r = std::fabs(roll);
  return wasUpright ? (a >= UPRIGHT_LEAVE_ALONG && r <= UPRIGHT_LEAVE_ROLL) : (a >= UPRIGHT_ENTER_ALONG && r <= UPRIGHT_ENTER_ROLL);
}

}  // namespace scribble
