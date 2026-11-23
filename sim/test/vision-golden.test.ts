import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { unpackFrames, renderFrames, packFrames, runCase, CASES } from '../scripts/vision-golden-lib';

// The vectors are what the firmware's native tests (firmware/test/test_vision) compare vision.cpp against. If this fails,
// vision.ts (or the synthetic camera) changed: run `npm run golden`, then `cd ../firmware && pio test -e native`.
const root = new URL('../../shared/', import.meta.url).pathname;
const bin = new Uint8Array(readFileSync(root + 'golden/vision_frames.bin'));
const vectors = JSON.parse(readFileSync(root + 'golden/vision_vectors.json', 'utf8'));

describe('golden vision vectors (shared with the firmware tests)', () => {
  const frames = unpackFrames(bin);
  it('the stored photos are the synthetic camera output', () => {
    const fresh = renderFrames();
    fresh.forEach((f, i) => expect(frames[i]).toEqual(f));
    expect(packFrames(fresh)).toEqual(bin);
  });
  for (const c of CASES) it(c.name, () => {
    const want = vectors.cases.find((v: { name: string }) => v.name === c.name);
    expect(want).toBeDefined();
    expect(JSON.parse(JSON.stringify(runCase(frames, c)))).toEqual(want);
  });
});
