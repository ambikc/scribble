// Writes shared/levels/*.scrb (the built-in lessons) and shared/golden/physics_vectors.json.
// The firmware's native unit tests load these files and must reproduce the checkpoints within 0.5 px (docs/build-plan.md Step 3).
// Run:  npx tsx scripts/make-golden.ts
import { writeFileSync, mkdirSync } from 'node:fs';
import { starterLevel, lessonSpike, lessonBouncy, paintLine, encodeLevel, MAT_GROUND, Level } from '../src/level';
import { newWorld, physicsStep } from '../src/physics';

const root = new URL('../../shared/', import.meta.url).pathname;
mkdirSync(root + 'levels', { recursive: true }); mkdirSync(root + 'golden', { recursive: true });

const bridged = starterLevel(); bridged.name = 'Bridged';
paintLine(bridged, MAT_GROUND, 72, 116, 120, 130, 4); paintLine(bridged, MAT_GROUND, 120, 130, 200, 124, 4); paintLine(bridged, MAT_GROUND, 200, 124, 248, 116, 4);
const levels: [string, Level][] = [['lesson1-bridge', starterLevel()], ['lesson2-spikes', lessonSpike()], ['lesson3-bounce', lessonBouncy()], ['bridged', bridged]];
for (const [n, l] of levels) writeFileSync(`${root}levels/${n}.scrb`, encodeLevel(l, true));

// Scripted inputs: piecewise-constant tilt, jumps on given ticks. Deterministic, no randomness.
const scripts: Record<string, { level: string; ticks: number; tilt: [number, number][]; jumps: number[] }> = {
  'bridge-roll': { level: 'bridged', ticks: 420, tilt: [[0, 0], [30, 1]], jumps: [] },
  'spike-pop': { level: 'lesson2-spikes', ticks: 240, tilt: [[0, 0], [20, 1]], jumps: [36] },
  'bounce-launch': { level: 'lesson3-bounce', ticks: 360, tilt: [[0, 0], [20, 1], [200, 0.4]], jumps: [100] },
};
const out: unknown[] = [];
for (const [name, sc] of Object.entries(scripts)) {
  const lv = levels.find(([n]) => n === sc.level)![1], w = newWorld(lv), cps: unknown[] = []; let acc = 0;
  for (let i = 0; i < sc.ticks; i++) {
    let tilt = 0; for (const [t0, v] of sc.tilt) if (i >= t0) tilt = v;
    acc |= physicsStep(w, lv, { tilt, jumpPressed: sc.jumps.includes(i) });   // events since the last checkpoint
    if ((i + 1) % 30 === 0) { const events = acc; acc = 0; cps.push({ tick: i + 1, x: +w.ball.x.toFixed(3), y: +w.ball.y.toFixed(3), vx: +w.ball.vx.toFixed(3), vy: +w.ball.vy.toFixed(3), state: w.state, events }); }
  }
  out.push({ name, level: `levels/${sc.level}.scrb`, ticks: sc.ticks, tilt: sc.tilt, jumps: sc.jumps, checkpoints: cps });
}
writeFileSync(root + 'golden/physics_vectors.json', JSON.stringify({ _comment: 'Replay each script on the .scrb level with the 60 Hz physics; every checkpoint must match within 0.5 px (docs/system-design.md 9.2).', tolerancePx: 0.5, scripts: out }, null, 1));
console.log('wrote', levels.length, 'levels and', out.length, 'physics scripts');
