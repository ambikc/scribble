// physics.h - fixed-step ball simulation.   (portable core: no Arduino, no I/O, no random, no clock)
// Line-by-line port of sim/src/physics.ts; algorithm in docs/system-design.md section 7.
// The maths is done in double with the same rounding points as the simulator (it rounds to 32-bit float where the
// simulator calls Math.fround), so the golden tests can demand the SAME numbers. Cost on the ESP32-S3 (soft double):
// a few hundred operations per tick, estimated well under 1 ms; measured in build Step 3.
#pragma once
#include <cstdint>
#include "level.h"

namespace scribble {

enum : uint32_t { EV_JUMP = 1, EV_LAND = 2, EV_BOUNCE = 4, EV_POP = 8, EV_FELL = 16, EV_RESPAWN = 32, EV_WIN = 64 };
enum : int { ST_ROLLING = 0, ST_DYING = 1, ST_CELEBRATING = 2 };

struct Ball {
  double x, y, vx, vy;
  double angle;       // radians, for the rolling animation only
  bool grounded;
  double coyote;      // seconds left in which a jump is still allowed after leaving the ground
  double jumpBuf;     // seconds left in which an early tap is remembered
};
struct World {
  Ball ball;
  double time;
  int state;          // ST_*
  double stateTime;
  int dyingKind;      // 0 = popped by a spike, 1 = fell in the pit
};
struct StepInput { double tilt; bool jumpPressed; };

void newWorld(World& w, const Level& level);
void respawn(World& w, const Level& level);
/** Advance one fixed tick (1/PHYSICS_HZ). Returns EV_* bits. */
uint32_t physicsStep(World& w, const Level& level, const StepInput& input);

}  // namespace scribble
