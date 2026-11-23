// physics.ts - fixed-step ball simulation.   (portable core: no I/O, no DOM, no random, no clock)
// Mirrors firmware physics.h/.cpp. Algorithm: docs/system-design.md section 7.
//
//   * Ball = circle of radius BALL_RADIUS. Solid = ground + bouncy. Spike is NOT solid (it pops the ball).
//   * Contact normal comes from SAMPLE_COUNT points around the edge (averaging smooths pixel jaggies).
//   * Each 1/60 s tick is split into sub-steps of at most MAX_STEP pixels, so the ball can never skip
//     across an 8 px line (tunnelling).
//   * Same inputs => same outputs (deterministic), which makes ghost replays possible later.
import * as C from './config';
import { Level, getBit } from './level';

// ---- events returned by physicsStep (bit mask) ----------------------------------------
export const EV_JUMP = 1, EV_LAND = 2, EV_BOUNCE = 4, EV_POP = 8, EV_FELL = 16, EV_RESPAWN = 32, EV_WIN = 64;

export const ST_ROLLING = 0, ST_DYING = 1, ST_CELEBRATING = 2;

export interface Ball {
  x: number; y: number; vx: number; vy: number;
  angle: number;            // radians, for the rolling animation only
  grounded: boolean;
  coyote: number;           // seconds left in which a jump is still allowed after leaving the ground
  jumpBuf: number;          // seconds left in which an early tap is remembered
}
export interface World {
  ball: Ball;
  time: number;
  state: number;            // ST_*
  stateTime: number;        // seconds in the current state
  dyingKind: 0 | 1;         // 0 = popped by a spike, 1 = fell in the pit
}
export interface StepInput { tilt: number; jumpPressed: boolean }

const f = Math.fround; // mimic the device's 32-bit float at the main assignment points

export function newWorld(level: Level): World {
  const w: World = { ball: { x: 0, y: 0, vx: 0, vy: 0, angle: 0, grounded: false, coyote: 0, jumpBuf: 0 }, time: 0, state: ST_ROLLING, stateTime: 0, dyingKind: 0 };
  respawn(w, level);
  return w;
}

export function respawn(w: World, level: Level): void {
  const b = w.ball;
  b.x = level.startX; b.y = level.startY; b.vx = 0; b.vy = 0; b.angle = 0;
  b.grounded = false; b.coyote = 0; b.jumpBuf = 0;
  w.state = ST_ROLLING; w.stateTime = 0;
}

// ---- sample points around the ball's edge -------------------------------------------------
// The 16 sample directions as literals (cos, sin of 22.5 degree steps), identical in physics.cpp, so the
// simulator and the firmware never differ because of a maths library.
const COS = [1.0, 0.9238795325112867, 0.7071067811865476, 0.38268343236508984, 6.123233995736766e-17, -0.3826834323650897, -0.7071067811865475, -0.9238795325112867, -1.0, -0.9238795325112868, -0.7071067811865477, -0.38268343236509034, -1.8369701987210297e-16, 0.38268343236509, 0.7071067811865474, 0.9238795325112865];
const SIN = [0.0, 0.3826834323650898, 0.7071067811865475, 0.9238795325112867, 1.0, 0.9238795325112867, 0.7071067811865476, 0.3826834323650899, 1.2246467991473532e-16, -0.38268343236508967, -0.7071067811865475, -0.9238795325112865, -1.0, -0.9238795325112866, -0.7071067811865477, -0.3826834323650904];

/** 0 = free, 1 = ground, 2 = bouncy. Left/right of the world are solid walls; top is open; bottom is the pit. */
function solidKind(level: Level, x: number, y: number): number {
  if (x < 0 || x >= C.WORLD_W) return 1;
  if (y < 0 || y >= C.WORLD_H) return 0;
  if (getBit(level.ground, x, y)) return 1;
  if (getBit(level.bouncy, x, y)) return 2;
  return 0;
}

interface Contact { nx: number; ny: number; hits: number; bouncy: boolean }
const scratch: Contact = { nx: 0, ny: 0, hits: 0, bouncy: false };

/** Sums the directions from every sample that lies inside a solid pixel toward the centre. */
function contact(level: Level, bx: number, by: number, r: number): Contact {
  let nx = 0, ny = 0, hits = 0, bouncy = false;
  for (let i = 0; i < C.SAMPLE_COUNT; i++) {
    const k = solidKind(level, Math.floor(bx + r * COS[i]), Math.floor(by + r * SIN[i]));
    if (k) { nx -= COS[i]; ny -= SIN[i]; hits++; if (k === 2) bouncy = true; }
  }
  if (hits) {
    const l = Math.sqrt(nx * nx + ny * ny);
    if (l > 1e-3) { nx /= l; ny /= l; } else { nx = 0; ny = -1; } // pinched between two surfaces: push up
  }
  scratch.nx = nx; scratch.ny = ny; scratch.hits = hits; scratch.bouncy = bouncy;
  return scratch;
}

function spikeHit(level: Level, bx: number, by: number): boolean {
  const r = C.BALL_RADIUS * C.SPIKE_HIT_SCALE;          // forgiving: smaller than the ball
  for (let i = 0; i < C.SAMPLE_COUNT; i++) {
    const x = Math.floor(bx + r * COS[i]), y = Math.floor(by + r * SIN[i]);
    if (x >= 0 && y >= 0 && x < C.WORLD_W && y < C.WORLD_H && getBit(level.spike, x, y)) return true;
  }
  return false;
}

/** Ball stuck inside ground (drawn onto): look for the nearest free spot. */
function unstick(level: Level, b: Ball): boolean {
  for (let r = 1; r <= C.UNSTICK_RADIUS; r++) for (let i = 0; i < C.SAMPLE_COUNT; i++) {
    const x = b.x + r * COS[i], y = b.y + r * SIN[i];
    if (contact(level, x, y, C.BALL_RADIUS).hits === 0) { b.x = x; b.y = y; return true; }
  }
  return false;
}

const clamp = (v: number, lo: number, hi: number) => (v < lo ? lo : v > hi ? hi : v);
const len = (x: number, y: number) => Math.sqrt(x * x + y * y); // sqrt is exactly rounded in every language; hypot is not

/** Advance one fixed tick (1/PHYSICS_HZ). Returns EV_* bits. */
export function physicsStep(w: World, level: Level, input: StepInput): number {
  const dt = 1 / C.PHYSICS_HZ;
  let ev = 0;
  w.time += dt; w.stateTime += dt;
  const b = w.ball;

  if (w.state === ST_DYING) {                                    // wait, then come back at Start
    if (w.stateTime >= (w.dyingKind === 0 ? C.RESPAWN_DELAY : C.FELL_DELAY)) { respawn(w, level); ev |= EV_RESPAWN; }
    return ev;
  }
  if (w.state === ST_CELEBRATING) return ev;                     // modes.ts plays the celebration

  // ---- jump: only from the ground, with forgiveness (coyote time + jump buffer) ----
  b.jumpBuf = input.jumpPressed ? C.JUMP_BUFFER : Math.max(0, b.jumpBuf - dt);
  b.coyote = b.grounded ? C.COYOTE_TIME : Math.max(0, b.coyote - dt);
  if (b.jumpBuf > 0 && (b.grounded || b.coyote > 0)) {
    b.vy = -C.JUMP_SPEED; b.jumpBuf = 0; b.coyote = 0; b.grounded = false; ev |= EV_JUMP;
  }

  // ---- rolling model (docs/game-design.md 6) ----
  const tilt = clamp(input.tilt, -1, 1);
  const target = tilt * C.MAX_ROLL_SPEED;
  if (b.grounded || tilt !== 0) {
    const sameWay = Math.sign(b.vx) === Math.sign(target) && Math.abs(b.vx) >= Math.abs(target);
    if (!(!b.grounded && sameWay)) {                              // in the air never slow down on purpose
      const rate = b.grounded ? (tilt === 0 ? C.GROUND_FRICTION : C.ROLL_ACCEL) : C.ROLL_ACCEL * C.AIR_CONTROL;
      b.vx += clamp(target - b.vx, -rate * dt, rate * dt);
    }
  }
  b.vy += C.GRAVITY * dt * 0.5;                                  // leapfrog: half a gravity kick before the move ...
  const sp = len(b.vx, b.vy);
  if (sp > C.MAX_SPEED) { b.vx *= C.MAX_SPEED / sp; b.vy *= C.MAX_SPEED / sp; }

  // ---- sub-steps: never move more than MAX_STEP px at once ----
  const n = Math.max(1, Math.ceil((Math.max(Math.abs(b.vx), Math.abs(b.vy)) * dt) / C.MAX_STEP));
  const sdt = dt / n;
  let groundedNow = false;
  for (let s = 0; s < n; s++) {
    b.x = f(b.x + b.vx * sdt); b.y = f(b.y + b.vy * sdt);

    // 1. push out of solids along the surface normal
    let touched = false, bouncy = false, lnx = 0, lny = -1;
    for (let it = 0; it < C.MAX_PUSH; it++) {
      const c = contact(level, b.x, b.y, C.BALL_RADIUS);
      if (!c.hits) break;
      b.x = f(b.x + c.nx * C.PUSH_STEP); b.y = f(b.y + c.ny * C.PUSH_STEP);
      touched = true; if (c.bouncy) bouncy = true; lnx = c.nx; lny = c.ny;
    }
    if (touched && contact(level, b.x, b.y, C.BALL_RADIUS).hits) {   // still inside after MAX_PUSH
      if (!unstick(level, b)) { respawn(w, level); return ev | EV_RESPAWN; }
    }
    if (touched) {
      const vn = b.vx * lnx + b.vy * lny;
      if (vn < 0) {                                                  // moving into the surface
        if (bouncy) {                                                // launch along the normal
          const tx = b.vx - vn * lnx, ty = b.vy - vn * lny;
          b.vx = lnx * C.BOUNCY_SPEED + tx * C.BOUNCY_TANGENT_KEEP;
          b.vy = lny * C.BOUNCY_SPEED + ty * C.BOUNCY_TANGENT_KEEP;
          ev |= EV_BOUNCE;
        } else {
          const e = -vn < C.BOUNCE_MIN_SPEED ? 0 : C.RESTITUTION;
          b.vx -= (1 + e) * vn * lnx; b.vy -= (1 + e) * vn * lny;
          if (lny < -0.5 && -vn > C.LAND_MIN_SPEED) ev |= EV_LAND;
        }
        b.vx = f(b.vx); b.vy = f(b.vy);
      }
    }

    // 2. resting contact (for "grounded" and friction)
    const pr = contact(level, b.x, b.y, C.BALL_RADIUS + C.GROUND_PROBE_EXTRA);
    groundedNow = pr.hits > 0 && pr.ny < -0.5;

    // 3. non-solid checks
    if (spikeHit(level, b.x, b.y)) { w.state = ST_DYING; w.dyingKind = 0; w.stateTime = 0; b.grounded = false; return ev | EV_POP; }
    if (b.y > C.WORLD_H + C.PIT_MARGIN_R * C.BALL_RADIUS) { w.state = ST_DYING; w.dyingKind = 1; w.stateTime = 0; b.grounded = false; return ev | EV_FELL; }
    if (len(b.x - level.flagX, b.y - (level.flagY - C.FLAG_CENTER_DY)) < C.FLAG_HIT_RADIUS) {
      w.state = ST_CELEBRATING; w.stateTime = 0; b.vx = 0; b.vy = 0; return ev | EV_WIN;
    }
  }
  b.vy += C.GRAVITY * dt * 0.5;                                  // ... and half after, so a 300 px/s jump peaks at exactly 50 px
  b.grounded = groundedNow;
  b.angle += (b.vx * dt) / C.BALL_RADIUS;
  return ev;
}
