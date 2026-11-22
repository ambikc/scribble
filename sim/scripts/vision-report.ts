// Sweep the synthetic camera model over distance, tilt, light and noise and print how well the vision pipeline
// recovers a known level. Run:  npx tsx scripts/vision-report.ts      (docs/build-plan.md, "before the device arrives")
import { starterLevel, paintLine, fillRect, MAT_GROUND, MAT_SPIKE, MAT_BOUNCY } from '../src/level';
import { renderTemplateShot, ShotOptions } from '../src/synth';
import { visionProcess, liveCheck, grayQvga, levelLayerIoU } from '../src/vision';

const truth = starterLevel();
paintLine(truth, MAT_GROUND, 72, 116, 120, 130, 4); paintLine(truth, MAT_GROUND, 120, 130, 200, 124, 4); paintLine(truth, MAT_GROUND, 200, 124, 248, 116, 4);
fillRect(truth, MAT_SPIKE, 110, 160, 160, 170); fillRect(truth, MAT_BOUNCY, 170, 160, 215, 170);

const cases: [string, ShotOptions][] = [
  ['35 cm', { distanceCm: 35 }], ['40 cm', { distanceCm: 40 }], ['45 cm', { distanceCm: 45 }], ['50 cm', { distanceCm: 50 }],
  ['40 cm, yaw 10', { yawDeg: 10 }], ['40 cm, yaw 20', { yawDeg: 20 }], ['40 cm, yaw 30', { yawDeg: 30 }],
  ['40 cm, yaw 15 pitch -10', { yawDeg: 15, pitchDeg: -10 }], ['40 cm, rolled 12 deg', { rollDeg: 12 }], ['upside down', { rollDeg: 180 }],
  ['dim (0.45)', { brightness: 0.45 }], ['uneven light 40 %', { gradient: 0.4 }], ['hard shadow', { shadow: true }],
  ['noise sigma 12', { noise: 12 }], ['blur 3 px', { blurPx: 3 }], ['no marks (fallback)', { drawMarks: false, distanceCm: 33 }],
];
console.log('case'.padEnd(26), 'marks status  ground spike bouncy | live: dist  sharp light level');
for (const [name, o] of cases) {
  const shot = renderTemplateShot(truth, { distanceCm: 40, seed: 7, ...o });
  const r = visionProcess(shot);
  const lc = liveCheck(grayQvga(shot));
  const f = (k: 'ground' | 'spike' | 'bouncy') => levelLayerIoU(truth[k], r.level[k]).toFixed(2);
  console.log(name.padEnd(26), String(r.report.marksFound).padEnd(5), String(r.report.status).padEnd(7), f('ground').padStart(6), f('spike').padStart(5), f('bouncy').padStart(6), '|',
    lc.marks ? `${lc.distCm.toFixed(0).padStart(4)}cm ${lc.edgePx.toFixed(1).padStart(5)}px ${lc.lights.join(' ')}`.padEnd(30) : '   (no marks)');
}
