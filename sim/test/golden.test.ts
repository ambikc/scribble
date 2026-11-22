import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { decodeLevel } from '../src/level';
import { newWorld, physicsStep } from '../src/physics';

const root = new URL('../../shared/', import.meta.url).pathname;
const vectors = JSON.parse(readFileSync(root + 'golden/physics_vectors.json', 'utf8'));

describe('golden physics vectors (shared with the firmware tests)', () => {
  for (const sc of vectors.scripts) it(sc.name, () => {
    const lv = decodeLevel(new Uint8Array(readFileSync(root + sc.level))).level, w = newWorld(lv);
    let k = 0;
    for (let i = 0; i < sc.ticks; i++) {
      let tilt = 0; for (const [t0, v] of sc.tilt as [number, number][]) if (i >= t0) tilt = v;
      physicsStep(w, lv, { tilt, jumpPressed: (sc.jumps as number[]).includes(i) });
      if ((i + 1) % 30 === 0) { const cp = sc.checkpoints[k++]; expect(Math.abs(w.ball.x - cp.x)).toBeLessThanOrEqual(vectors.tolerancePx); expect(Math.abs(w.ball.y - cp.y)).toBeLessThanOrEqual(vectors.tolerancePx); expect(w.state).toBe(cp.state); }
    }
  });
});
