import { describe, it, expect } from 'vitest';
import * as C from '../src/config';
import { newLevel, fillRect, MAT_GROUND, MAT_SPIKE, MAT_BOUNCY, Level } from '../src/level';
import { newWorld, physicsStep, EV_JUMP, EV_POP, EV_FELL, EV_WIN, EV_BOUNCE, ST_ROLLING, World } from '../src/physics';

function floorLevel(): Level {
  const l = newLevel();
  fillRect(l, MAT_GROUND, 0, 150, 320, 158);          // floor, top at y = 150
  l.startX = 100; l.startY = 142; l.flagX = 300; l.flagY = 20; // flag far away
  return l;
}
const REST_Y = 150 - C.BALL_RADIUS;
function run(w: World, l: Level, ticks: number, tilt = 0, jumpAt = -1, onTick?: (i: number, ev: number) => void) {
  for (let i = 0; i < ticks; i++) { const ev = physicsStep(w, l, { tilt, jumpPressed: i === jumpAt }); onTick?.(i, ev); }
}

describe('physics', () => {
  it('rests on a flat floor and is grounded', () => {
    const l = floorLevel(), w = newWorld(l);
    run(w, l, 120);
    expect(Math.abs(w.ball.y - REST_Y)).toBeLessThan(1.5);
    expect(Math.abs(w.ball.vy)).toBeLessThan(20);
    expect(w.ball.grounded).toBe(true);
  });

  it('rolls at MAX_ROLL_SPEED: 150 +/- 2 px in one second at full tilt', () => {
    const l = floorLevel(); l.startX = 20;
    const w = newWorld(l);
    run(w, l, 60);                                      // settle + nothing
    run(w, l, 40, 1);                                   // reach full speed
    const x0 = w.ball.x; run(w, l, 60, 1);
    expect(w.ball.x - x0).toBeGreaterThan(148); expect(w.ball.x - x0).toBeLessThan(152);
  });

  it('stops quickly with no tilt (friction)', () => {
    const l = floorLevel(), w = newWorld(l);
    run(w, l, 60); run(w, l, 40, 1); run(w, l, 30, 0);  // 0.5 s of friction at 500 px/s^2 stops 150 px/s in 0.3 s
    expect(Math.abs(w.ball.vx)).toBeLessThan(1);
  });

  it('jump apex is 50 +/- 2 px and only works from the ground', () => {
    const l = floorLevel(), w = newWorld(l);
    run(w, l, 60);
    let minY = w.ball.y, jumped = 0;
    run(w, l, 60, 0, 0, (_i, ev) => { minY = Math.min(minY, w.ball.y); if (ev & EV_JUMP) jumped++; });
    expect(jumped).toBe(1);
    expect(REST_Y - minY).toBeGreaterThan(48); expect(REST_Y - minY).toBeLessThan(52);
    // a tap while clearly in the air does nothing
    const w2 = newWorld(l); run(w2, l, 60); run(w2, l, 12, 0, 0);
    let again = 0; run(w2, l, 10, 0, 0, (_i, ev) => { if (ev & EV_JUMP) again++; });
    expect(again).toBe(0);
  });

  it('bouncy pad launches to about 128 px', () => {
    const l = floorLevel(); fillRect(l, MAT_BOUNCY, 60, 150, 140, 158);
    const w = newWorld(l);
    let minY = 1e9, bounced = 0;
    run(w, l, 120, 0, -1, (_i, ev) => { minY = Math.min(minY, w.ball.y); if (ev & EV_BOUNCE) bounced++; });
    expect(bounced).toBeGreaterThan(0);
    expect(REST_Y - minY).toBeGreaterThan(120); expect(REST_Y - minY).toBeLessThan(132);
  });

  it('spike pops the ball, then it comes back at Start after 0.5 s', () => {
    const l = floorLevel(); l.startX = 40; fillRect(l, MAT_SPIKE, 100, 142, 120, 150);
    const w = newWorld(l);
    let popped = 0, popTick = -1;
    run(w, l, 55, 1, -1, (i, ev) => { if (ev & EV_POP) { popped++; if (popTick < 0) popTick = i; } });
    expect(popped).toBe(1);                              // rolls into the spike once
    const w2 = newWorld(l);
    run(w2, l, popTick + 1, 1);                          // just popped
    expect(w2.state).not.toBe(ST_ROLLING);
    run(w2, l, 31, 0);                                   // 0.5 s later
    expect(w2.state).toBe(ST_ROLLING);
    expect(Math.hypot(w2.ball.x - l.startX, w2.ball.y - l.startY)).toBeLessThan(12);
  });

  it('falling into the pit respawns without a pop', () => {
    const l = newLevel(); l.startX = 100; l.startY = 100; l.flagX = 300; l.flagY = 20;
    const w = newWorld(l); let fell = 0, popped = 0;
    run(w, l, 60, 0, -1, (_i, ev) => { if (ev & EV_FELL) fell++; if (ev & EV_POP) popped++; });
    expect(fell).toBe(1); expect(popped).toBe(0);
  });

  it('touching the flag wins', () => {
    const l = floorLevel(); l.flagX = 200; l.flagY = 150;
    const w = newWorld(l); let won = 0;
    run(w, l, 60); run(w, l, 90, 1, -1, (_i, ev) => { if (ev & EV_WIN) won++; });
    expect(won).toBe(1);
  });

  it('never tunnels through an 8 px line at terminal velocity (2000 random drops)', () => {
    let seed = 12345; const rnd = () => ((seed = (seed * 1664525 + 1013904223) >>> 0) / 2 ** 32);
    const l = newLevel(); fillRect(l, MAT_GROUND, 0, 170, 320, 178);
    l.flagX = 310; l.flagY = 20;
    for (let t = 0; t < 2000; t++) {
      l.startX = 20 + rnd() * 280; l.startY = 0 + rnd() * 20;
      const w = newWorld(l); w.ball.vy = 300 + rnd() * 220; w.ball.vx = (rnd() - 0.5) * 300;
      let passed = false;
      run(w, l, 60, 0, -1, () => { if (w.ball.y > 190) passed = true; });
      expect(passed).toBe(false);
    }
  });

  it('a ball placed inside ground is ejected or respawns quickly', () => {
    const l = floorLevel(); fillRect(l, MAT_GROUND, 90, 130, 110, 150);
    const w = newWorld(l); w.ball.x = 100; w.ball.y = 140;
    run(w, l, 10);
    // either pushed out (not overlapping the block's centre) or back at start
    const inside = w.ball.x > 92 && w.ball.x < 108 && w.ball.y > 132 && w.ball.y < 148;
    expect(inside).toBe(false);
  });

  it('is deterministic', () => {
    const l = floorLevel(); fillRect(l, MAT_BOUNCY, 150, 150, 200, 158);
    const a = newWorld(l), b = newWorld(l);
    for (let i = 0; i < 600; i++) { const inp = { tilt: Math.sin(i / 20), jumpPressed: i % 97 === 0 }; physicsStep(a, l, inp); physicsStep(b, l, inp); }
    expect(a.ball.x).toBe(b.ball.x); expect(a.ball.y).toBe(b.ball.y);
  });
});
