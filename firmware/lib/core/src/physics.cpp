// physics.cpp - see physics.h. Every expression mirrors sim/src/physics.ts in the same order.
#include "physics.h"
#include <cmath>

namespace scribble {

// The 16 sample directions as literals (cos, sin of 22.5 degree steps), identical in physics.ts.
static const double COS[16] = { 1.0, 0.9238795325112867, 0.7071067811865476, 0.38268343236508984, 6.123233995736766e-17, -0.3826834323650897, -0.7071067811865475, -0.9238795325112867, -1.0, -0.9238795325112868, -0.7071067811865477, -0.38268343236509034, -1.8369701987210297e-16, 0.38268343236509, 0.7071067811865474, 0.9238795325112865 };
static const double SIN[16] = { 0.0, 0.3826834323650898, 0.7071067811865475, 0.9238795325112867, 1.0, 0.9238795325112867, 0.7071067811865476, 0.3826834323650899, 1.2246467991473532e-16, -0.38268343236508967, -0.7071067811865475, -0.9238795325112865, -1.0, -0.9238795325112866, -0.7071067811865477, -0.3826834323650904 };

static inline double f(double v) { return double(float(v)); }   // == Math.fround in the simulator
static inline double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline double len(double x, double y) { return std::sqrt(x * x + y * y); }
static inline double sgn(double v) { return v > 0 ? 1.0 : v < 0 ? -1.0 : 0.0; }   // == Math.sign (for comparisons)
static inline int ifloor(double v) { return int(std::floor(v)); }

void respawn(World& w, const Level& level) {
  Ball& b = w.ball;
  b.x = level.startX; b.y = level.startY; b.vx = 0; b.vy = 0; b.angle = 0;
  b.grounded = false; b.coyote = 0; b.jumpBuf = 0;
  w.state = ST_ROLLING; w.stateTime = 0;
}
void newWorld(World& w, const Level& level) { w = World{}; respawn(w, level); }

/** 0 = free, 1 = ground, 2 = bouncy. Left/right of the world are solid walls; top is open; bottom is the pit. */
static int solidKind(const Level& level, int x, int y) {
  if (x < 0 || x >= WORLD_W) return 1;
  if (y < 0 || y >= WORLD_H) return 0;
  if (getBit(level.ground, x, y)) return 1;
  if (getBit(level.bouncy, x, y)) return 2;
  return 0;
}

struct Contact { double nx, ny; int hits; bool bouncy; };

/** Sums the directions from every sample that lies inside a solid pixel toward the centre. */
static Contact contact(const Level& level, double bx, double by, double r) {
  double nx = 0, ny = 0; int hits = 0; bool bouncy = false;
  for (int i = 0; i < SAMPLE_COUNT; i++) {
    const int k = solidKind(level, ifloor(bx + r * COS[i]), ifloor(by + r * SIN[i]));
    if (k) { nx -= COS[i]; ny -= SIN[i]; hits++; if (k == 2) bouncy = true; }
  }
  if (hits) {
    const double l = std::sqrt(nx * nx + ny * ny);
    if (l > 1e-3) { nx /= l; ny /= l; } else { nx = 0; ny = -1; }   // pinched between two surfaces: push up
  }
  return { nx, ny, hits, bouncy };
}

static bool spikeHit(const Level& level, double bx, double by) {
  const double r = BALL_RADIUS * SPIKE_HIT_SCALE;                      // forgiving: smaller than the ball
  for (int i = 0; i < SAMPLE_COUNT; i++) {
    const int x = ifloor(bx + r * COS[i]), y = ifloor(by + r * SIN[i]);
    if (x >= 0 && y >= 0 && x < WORLD_W && y < WORLD_H && getBit(level.spike, x, y)) return true;
  }
  return false;
}

/** Ball stuck inside ground (drawn onto): look for the nearest free spot. */
static bool unstick(const Level& level, Ball& b) {
  for (int r = 1; r <= UNSTICK_RADIUS; r++) for (int i = 0; i < SAMPLE_COUNT; i++) {
    const double x = b.x + r * COS[i], y = b.y + r * SIN[i];
    if (contact(level, x, y, BALL_RADIUS).hits == 0) { b.x = x; b.y = y; return true; }
  }
  return false;
}

uint32_t physicsStep(World& w, const Level& level, const StepInput& input) {
  const double dt = 1.0 / PHYSICS_HZ;
  uint32_t ev = 0;
  w.time += dt; w.stateTime += dt;
  Ball& b = w.ball;

  if (w.state == ST_DYING) {                                             // wait, then come back at Start
    if (w.stateTime >= (w.dyingKind == 0 ? RESPAWN_DELAY : FELL_DELAY)) { respawn(w, level); ev |= EV_RESPAWN; }
    return ev;
  }
  if (w.state == ST_CELEBRATING) return ev;                              // modes plays the celebration

  // ---- jump: only from the ground, with forgiveness (coyote time + jump buffer) ----
  b.jumpBuf = input.jumpPressed ? JUMP_BUFFER : (b.jumpBuf - dt > 0 ? b.jumpBuf - dt : 0);
  b.coyote = b.grounded ? COYOTE_TIME : (b.coyote - dt > 0 ? b.coyote - dt : 0);
  if (b.jumpBuf > 0 && (b.grounded || b.coyote > 0)) {
    b.vy = -JUMP_SPEED; b.jumpBuf = 0; b.coyote = 0; b.grounded = false; ev |= EV_JUMP;
  }

  // ---- rolling model (docs/game-design.md 6) ----
  const double tilt = clampd(input.tilt, -1, 1);
  const double target = tilt * MAX_ROLL_SPEED;
  if (b.grounded || tilt != 0) {
    const bool sameWay = sgn(b.vx) == sgn(target) && std::fabs(b.vx) >= std::fabs(target);
    if (!(!b.grounded && sameWay)) {                                     // in the air never slow down on purpose
      const double rate = b.grounded ? (tilt == 0 ? double(GROUND_FRICTION) : double(ROLL_ACCEL)) : ROLL_ACCEL * AIR_CONTROL;
      b.vx += clampd(target - b.vx, -rate * dt, rate * dt);
    }
  }
  b.vy += GRAVITY * dt * 0.5;                                            // leapfrog: half a gravity kick before the move ...
  const double sp = len(b.vx, b.vy);
  if (sp > MAX_SPEED) { b.vx *= MAX_SPEED / sp; b.vy *= MAX_SPEED / sp; }

  // ---- sub-steps: never move more than MAX_STEP px at once ----
  const double mv = std::fabs(b.vx) > std::fabs(b.vy) ? std::fabs(b.vx) : std::fabs(b.vy);
  const int nSteps = (int)std::ceil((mv * dt) / MAX_STEP) > 1 ? (int)std::ceil((mv * dt) / MAX_STEP) : 1;
  const double sdt = dt / nSteps;
  bool groundedNow = false;
  for (int s = 0; s < nSteps; s++) {
    b.x = f(b.x + b.vx * sdt); b.y = f(b.y + b.vy * sdt);

    // 1. push out of solids along the surface normal
    bool touched = false, bouncy = false; double lnx = 0, lny = -1;
    for (int it = 0; it < MAX_PUSH; it++) {
      const Contact c = contact(level, b.x, b.y, BALL_RADIUS);
      if (!c.hits) break;
      b.x = f(b.x + c.nx * PUSH_STEP); b.y = f(b.y + c.ny * PUSH_STEP);
      touched = true; if (c.bouncy) bouncy = true; lnx = c.nx; lny = c.ny;
    }
    if (touched && contact(level, b.x, b.y, BALL_RADIUS).hits) {          // still inside after MAX_PUSH
      if (!unstick(level, b)) { respawn(w, level); return ev | EV_RESPAWN; }
    }
    if (touched) {
      const double vn = b.vx * lnx + b.vy * lny;
      if (vn < 0) {                                                      // moving into the surface
        if (bouncy) {                                                    // launch along the normal
          const double tx = b.vx - vn * lnx, ty = b.vy - vn * lny;
          b.vx = lnx * BOUNCY_SPEED + tx * BOUNCY_TANGENT_KEEP;
          b.vy = lny * BOUNCY_SPEED + ty * BOUNCY_TANGENT_KEEP;
          ev |= EV_BOUNCE;
        } else {
          const double e = -vn < BOUNCE_MIN_SPEED ? 0 : RESTITUTION;
          b.vx -= (1 + e) * vn * lnx; b.vy -= (1 + e) * vn * lny;
          if (lny < -0.5 && -vn > LAND_MIN_SPEED) ev |= EV_LAND;
        }
        b.vx = f(b.vx); b.vy = f(b.vy);
      }
    }

    // 2. resting contact (for "grounded" and friction)
    const Contact pr = contact(level, b.x, b.y, BALL_RADIUS + GROUND_PROBE_EXTRA);
    groundedNow = pr.hits > 0 && pr.ny < -0.5;

    // 3. non-solid checks
    if (spikeHit(level, b.x, b.y)) { w.state = ST_DYING; w.dyingKind = 0; w.stateTime = 0; b.grounded = false; return ev | EV_POP; }
    if (b.y > WORLD_H + PIT_MARGIN_R * BALL_RADIUS) { w.state = ST_DYING; w.dyingKind = 1; w.stateTime = 0; b.grounded = false; return ev | EV_FELL; }
    if (len(b.x - level.flagX, b.y - (level.flagY - FLAG_CENTER_DY)) < FLAG_HIT_RADIUS) {
      w.state = ST_CELEBRATING; w.stateTime = 0; b.vx = 0; b.vy = 0; return ev | EV_WIN;
    }
  }
  b.vy += GRAVITY * dt * 0.5;                                            // ... and half after (a 300 px/s jump peaks at 50 px)
  b.grounded = groundedNow;
  b.angle += (b.vx * dt) / BALL_RADIUS;
  return ev;
}

}  // namespace scribble
