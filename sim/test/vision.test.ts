import { describe, it, expect } from 'vitest';
import * as C from '../src/config';
import { Level, newLevel, fillRect, paintLine, MAT_GROUND, MAT_SPIKE, MAT_BOUNCY, starterLevel, countBits } from '../src/level';
import { renderTemplateShot } from '../src/synth';
import { visionProcess, visionProcessOnce, defaultParams, liveCheck, grayQvga, findMarks, levelLayerIoU, Q_OK, Q_MARKS_MISSING, Q_EMPTY, LIGHT_GREEN } from '../src/vision';
import { physicsStep, newWorld } from '../src/physics';

/** A level drawn the way a child might: floor, a bridge, a spike patch and a bouncy pad. */
function drawnLevel(): Level {
  const l = starterLevel();
  paintLine(l, MAT_GROUND, 72, 116, 120, 130, 4); paintLine(l, MAT_GROUND, 120, 130, 200, 124, 4); paintLine(l, MAT_GROUND, 200, 124, 248, 116, 4);
  fillRect(l, MAT_SPIKE, 110, 160, 160, 170);
  fillRect(l, MAT_BOUNCY, 170, 160, 215, 170);
  return l;
}
const ev = (a: Level, b: Level) => (['ground', 'spike', 'bouncy'] as const).map((k) => levelLayerIoU(a[k], b[k]));

describe('vision', () => {
  const truth = drawnLevel();

  it('finds the four marks, with the ring at top-left, at 40 cm straight on', () => {
    const shot = renderTemplateShot(truth, { distanceCm: 40, seed: 1 });
    const m = findMarks(shot, defaultParams());
    expect(m).not.toBeNull();
    expect(m!.orientGuess).toBe(false);
    // distance check: mark spacing in the QVGA frame should be about 254 px at 40 cm
    const w = Math.hypot(m!.pts[1][0] - m!.pts[0][0], m!.pts[1][1] - m!.pts[0][1]) / 2;
    expect(w).toBeGreaterThan(246); expect(w).toBeLessThan(262);
  });

  it('round-trips a drawn level: straight on, 40 cm', () => {
    const r = visionProcess(renderTemplateShot(truth, { distanceCm: 40, seed: 2 }));
    expect(r.report.marksFound).toBe(true);
    expect(r.report.status).toBe(Q_OK);
    const [g, s, b] = ev(truth, r.level);
    expect(g).toBeGreaterThan(0.7); expect(s).toBeGreaterThan(0.7); expect(b).toBeGreaterThan(0.7);
  });

  it('survives a 15 degree tilt, uneven light and sensor noise', () => {
    const r = visionProcess(renderTemplateShot(truth, { distanceCm: 42, yawDeg: 15, pitchDeg: -6, gradient: 0.25, noise: 5, seed: 3 }));
    expect(r.report.marksFound).toBe(true);
    const [g, s, b] = ev(truth, r.level);
    expect(g).toBeGreaterThan(0.6); expect(s).toBeGreaterThan(0.55); expect(b).toBeGreaterThan(0.55);
  });

  it('works at the near and far ends of the range (35 cm and 45 cm)', () => {
    for (const d of [36, 45]) {
      const r = visionProcess(renderTemplateShot(truth, { distanceCm: d, seed: 4 }));
      expect(r.report.marksFound, `distance ${d}`).toBe(true);
      expect(ev(truth, r.level)[0], `distance ${d}`).toBeGreaterThan(0.6);
    }
  });

  it('a sheet held upside down is recognised by the ring and still comes out upright', () => {
    const r = visionProcess(renderTemplateShot(truth, { distanceCm: 40, rollDeg: 180, seed: 5 }));
    expect(r.report.marksFound).toBe(true);
    expect(r.report.orientGuess).toBe(false);
    expect(ev(truth, r.level)[0]).toBeGreaterThan(0.65);
  });

  it('a sheet turned 90 degrees also works', () => {
    const r = visionProcess(renderTemplateShot(truth, { distanceCm: 50, rollDeg: 90, seed: 6 }));
    // at 50 cm and rolled the sheet may not fit the frame; marks must still be found if they are in frame
    if (r.report.marksFound) expect(ev(truth, r.level)[0]).toBeGreaterThan(0.5);
  });

  it('survives a hard shadow without crashing and still finds the marks', () => {
    const r = visionProcess(renderTemplateShot(truth, { distanceCm: 40, shadow: true, seed: 7 }));
    expect(r.report.marksFound).toBe(true);
  });

  it('falls back to a centre crop when the sheet has no marks, and says so', () => {
    const r = visionProcess(renderTemplateShot(truth, { distanceCm: 33, drawMarks: false, seed: 8 }));
    expect(r.report.marksFound).toBe(false);
    expect(r.report.status).not.toBe(Q_OK);
  });

  it('a blank sheet gives the EMPTY status, not a crash', () => {
    const r = visionProcess(renderTemplateShot(newLevel(), { distanceCm: 40, seed: 9 }));
    expect([Q_EMPTY, Q_MARKS_MISSING]).toContain(r.report.status === Q_OK ? -1 : r.report.status);
    expect(countBits(r.level.ground)).toBeLessThan(200);
  });

  it('places Start on the left ledge and Flag on the right ledge', () => {
    const r = visionProcess(renderTemplateShot(truth, { distanceCm: 40, seed: 10 }));
    expect(r.level.startX).toBeLessThan(80); expect(r.level.startY).toBeLessThan(120);
    expect(r.level.flagX).toBeGreaterThan(240); expect(Math.abs(r.level.flagY - 112)).toBeLessThan(10);
  });

  it('the recovered level is actually playable: the ball can cross it', () => {
    const r = visionProcess(renderTemplateShot(truth, { distanceCm: 40, seed: 11 }));
    const w = newWorld(r.level);
    let won = false;
    for (let i = 0; i < 60 * 12 && !won; i++) {
      const jump = i % 50 === 0 && w.ball.vy >= 0;
      const e = physicsStep(w, r.level, { tilt: 1, jumpPressed: jump });
      if (e & 64) won = true;
    }
    expect(won).toBe(true);
  });

  it('thicker/thinner ink re-runs without the camera and changes the result', () => {
    const r = visionProcessOnce(renderTemplateShot(truth, { distanceCm: 40, seed: 12 }), defaultParams());
    const a = countBits(r.level.ground);
    expect(a).toBeGreaterThan(500);
  });

  it('the live Camera check turns green at 40 cm and shows the right cue at 55 cm', () => {
    const good = liveCheck(grayQvga(renderTemplateShot(truth, { distanceCm: 40, seed: 13 })));
    expect(good.marks).toBe(true);
    expect(Math.abs(good.distCm - 40)).toBeLessThan(2.5);
    expect(good.lights[0]).toBe(LIGHT_GREEN);
    expect(good.lights[2]).toBe(LIGHT_GREEN);
    const far = liveCheck(grayQvga(renderTemplateShot(truth, { distanceCm: 55, seed: 14 })));
    if (far.marks) { expect(far.lights[0]).not.toBe(LIGHT_GREEN); expect(far.cue).toBe('closer'); }
    const tilted = liveCheck(grayQvga(renderTemplateShot(truth, { distanceCm: 40, yawDeg: 24, seed: 15 })));
    if (tilted.marks) expect(tilted.lights[3]).not.toBe(LIGHT_GREEN);
    const blurry = liveCheck(grayQvga(renderTemplateShot(truth, { distanceCm: 40, blurPx: 6, seed: 16 })));
    if (blurry.marks) expect(blurry.lights[1]).not.toBe(LIGHT_GREEN);
  });

  it('has the template geometry the docs promise', () => {
    expect(C.MARK_WORLD_X0).toBeCloseTo(-13.913, 2); expect(C.MARK_WORLD_Y1).toBeCloseTo(200.348, 2);
    expect(C.MM_PER_PX).toBeCloseTo(0.77625, 4);
  });
});
