// Shared by scripts/make-vision-golden.ts (writes the golden data) and test/vision-golden.test.ts (checks it still holds).
// The golden vision data lets the firmware's native tests prove that vision.cpp gives the SAME marks, warp, ink, layers and
// pins as vision.ts (docs/system-design.md 9.2). It is: a few synthetic template photos (VGA RGB565, compressed) and, for
// each "case" (a photo plus a portable integer transform such as dimming or noise), the expected outputs.
import { Level, newLevel, starterLevel, paintLine, fillRect, encodeLevel, crc32, MAT_GROUND, MAT_SPIKE, MAT_BOUNCY } from '../src/level';
import { renderTemplateShot } from '../src/synth';
import { visionProcess, visionRethreshold, defaultParams, VisionParams, liveCheck, grayQvga, worldToVga, pack565 } from '../src/vision';

export const VGA_N = 640 * 480;

/** The level every photo shows: floor, a bridge, a spike patch and a bouncy pad (same as test/vision.test.ts). */
export function drawnLevel(): Level {
  const l = starterLevel();
  paintLine(l, MAT_GROUND, 72, 116, 120, 130, 4); paintLine(l, MAT_GROUND, 120, 130, 200, 124, 4); paintLine(l, MAT_GROUND, 200, 124, 248, 116, 4);
  fillRect(l, MAT_SPIKE, 110, 160, 160, 170);
  fillRect(l, MAT_BOUNCY, 170, 160, 215, 170);
  return l;
}

// ---- the stored photos (noise free, so they compress to ~20 KB each) --------------------------------------------------
export const FRAME_NAMES = ['clean', 'tilt', 'nomarks', 'blank'];
export function renderFrames(): Uint16Array[] {
  const truth = drawnLevel();
  return [
    renderTemplateShot(truth, { distanceCm: 40 }),                                             // straight on, 40 cm
    renderTemplateShot(truth, { distanceCm: 42, yawDeg: 15, pitchDeg: -6, gradient: 0.25 }),   // tilted, uneven light
    renderTemplateShot(truth, { distanceCm: 33, drawMarks: false }),                           // a sheet without marks: centre-crop fallback
    renderTemplateShot(newLevel(), { distanceCm: 40 }),                                        // a blank sheet
  ];
}

// ---- portable integer transforms (the C++ test applies the same ones) ---------------------------------------------------
export type Xform = { kind: 'none' } | { kind: 'dim'; k: number } | { kind: 'fade'; k: number } | { kind: 'noise'; amp: number; seed: number } | { kind: 'rot180' };
const r8 = (p: number) => ((p >> 11) << 3) | (p >> 13), g8 = (p: number) => (((p >> 5) & 63) << 2) | ((p >> 9) & 3), b8 = (p: number) => ((p & 31) << 3) | ((p >> 2) & 7);
const c255 = (v: number) => (v < 0 ? 0 : v > 255 ? 255 : v);
export function applyXform(f: Uint16Array, x: Xform): Uint16Array {
  const out = new Uint16Array(f.length);
  if (x.kind === 'none') out.set(f);
  else if (x.kind === 'rot180') for (let i = 0; i < f.length; i++) out[i] = f[f.length - 1 - i];
  else if (x.kind === 'fade') for (let i = 0; i < f.length; i++) { const p = f[i], q = (v: number) => 255 - (((255 - v) * x.k + 128) >> 8); out[i] = pack565(q(r8(p)), q(g8(p)), q(b8(p))); }   // pale crayon: ink strength x k / 256
  else if (x.kind === 'dim') for (let i = 0; i < f.length; i++) { const p = f[i]; out[i] = pack565((r8(p) * x.k + 128) >> 8, (g8(p) * x.k + 128) >> 8, (b8(p) * x.k + 128) >> 8); }
  else {
    let s = x.seed >>> 0;                                                     // LCG, one step per pixel; the same noise on r, g and b
    for (let i = 0; i < f.length; i++) {
      s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
      const n = (((s >>> 24) * (2 * x.amp + 1)) >> 8) - x.amp, p = f[i];
      out[i] = pack565(c255(r8(p) + n), c255(g8(p) + n), c255(b8(p) + n));
    }
  }
  return out;
}

// ---- compact lossless photo codec: residual against the pixel above, zero runs, varints ------------------------------------
export function encodeFrame(f: Uint16Array): number[] {
  const out: number[] = []; const vi = (v: number) => { while (v >= 128) { out.push((v & 127) | 128); v >>>= 7; } out.push(v); };
  let run = 0;
  for (let i = 0; i < f.length; i++) {
    const above = i >= 640 ? f[i - 640] : i > 0 ? f[i - 1] : 0, d = ((f[i] - above) << 16) >> 16;
    if (d === 0) { run++; continue; }
    vi(run); run = 0; vi(d < 0 ? -2 * d - 1 : 2 * d);
  }
  vi(run);
  return out;
}
export function decodeFrame(b: Uint8Array, pos: number, end: number): Uint16Array {
  const f = new Uint16Array(VGA_N); let i = 0;
  const vi = () => { let v = 0, sh = 0, c: number; do { c = b[pos++]; v |= (c & 127) << sh; sh += 7; } while (c & 128); return v >>> 0; };
  for (;;) {
    const run = vi(); for (let k = 0; k < run && i < VGA_N; k++, i++) f[i] = predict(f, i);
    if (pos >= end) break;                                                    // that was the trailing run
    const z = vi(); if (i < VGA_N) { const d = z & 1 ? -((z + 1) >> 1) : z >> 1; f[i] = (predict(f, i) + d) & 0xffff; i++; }
  }
  while (i < VGA_N) { f[i] = predict(f, i); i++; }
  return f;
}
const predict = (f: Uint16Array, i: number) => (i >= 640 ? f[i - 640] : i > 0 ? f[i - 1] : 0);

/** "VGF1", u32 count, then per frame u32 byte length + data. */
export function packFrames(frames: Uint16Array[]): Uint8Array {
  const parts = frames.map(encodeFrame), total = 8 + parts.reduce((s, p) => s + 4 + p.length, 0), out = new Uint8Array(total), dv = new DataView(out.buffer);
  out.set([0x56, 0x47, 0x46, 0x31]); dv.setUint32(4, frames.length, true);
  let pos = 8; for (const p of parts) { dv.setUint32(pos, p.length, true); out.set(p, pos + 4); pos += 4 + p.length; }
  return out;
}
export function unpackFrames(b: Uint8Array): Uint16Array[] {
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength), n = dv.getUint32(4, true), out: Uint16Array[] = [];
  let pos = 8; for (let k = 0; k < n; k++) { const len = dv.getUint32(pos, true); out.push(decodeFrame(b, pos + 4, pos + 4 + len)); pos += 4 + len; }
  return out;
}

// ---- the cases -----------------------------------------------------------------------------------------------------------
export interface Case { name: string; frame: number; xform: Xform; rethresh?: Partial<VisionParams>[]; }
export const CASES: Case[] = [
  { name: 'clean', frame: 0, xform: { kind: 'none' }, rethresh: [{ inkThresh: 64 }, { inkThresh: 90 }, { closeR: 0, thickenR: 0 }, { minBlob: 60, thickenR: 1 }] },
  { name: 'tilt', frame: 1, xform: { kind: 'none' } },
  { name: 'dim', frame: 0, xform: { kind: 'dim', k: 115 } },
  { name: 'dark', frame: 0, xform: { kind: 'dim', k: 80 } },
  { name: 'faded', frame: 0, xform: { kind: 'fade', k: 80 } },
  { name: 'noisy', frame: 0, xform: { kind: 'noise', amp: 14, seed: 1 } },
  { name: 'harsh', frame: 0, xform: { kind: 'noise', amp: 70, seed: 3 } },
  { name: 'upside-down', frame: 0, xform: { kind: 'rot180' } },
  { name: 'tilt-noisy', frame: 1, xform: { kind: 'noise', amp: 10, seed: 7 } },
  { name: 'nomarks', frame: 2, xform: { kind: 'none' } },
  { name: 'blank', frame: 3, xform: { kind: 'none' } },
];

const CUE = { '': 0, closer: 1, back: 2, tilt: 3 } as const;
const bytes = (a: Uint8Array | Uint16Array) => new Uint8Array(a.buffer, a.byteOffset, a.byteLength);
const scrbCrc = (l: Level) => crc32(encodeLevel(l, false));
const flat = (pts: [number, number][] | null) => (pts ? pts.flatMap((p) => [p[0], p[1]]) : null);

export interface LiveResult { upright: boolean; marks: boolean; quad: number[] | null; distCm: number; edgePx: number; paperMean: number; clipFrac: number; contrast: number; ratioH: number; ratioV: number; lights: number[]; cue: number; allGreen: boolean }
export interface CaseResult {
  name: string; frame: number; xform: Xform;
  marksFound: boolean; orientGuess: boolean; marks: number[] | null; hq: number[] | null;
  status: number; inkCover: number; bgMean: number; blobs: number; usedInkThresh: number;
  start: number[]; flag: number[];
  crc: { straight: number; ink: number; cls: number; ground: number; spike: number; bouncy: number; scrb: number };
  rethresh: { params: Partial<VisionParams>; status: number; inkCover: number; blobs: number; ground: number; spike: number; bouncy: number; scrb: number }[];
  live: LiveResult[];
}

/** Run one case through vision.ts and collect everything the C++ must reproduce. `frames` are the decoded stored photos. */
export function runCase(frames: Uint16Array[], c: Case): CaseResult {
  const photo = applyXform(frames[c.frame], c.xform);
  const r = visionProcess(photo, defaultParams()), w = r.work, lv = r.level;
  const marks = w.marks ? flat(w.marks.pts) : null, hq = w.marks ? worldToVga(w.marks) : null;
  const res: CaseResult = {
    name: c.name, frame: c.frame, xform: c.xform,
    marksFound: r.report.marksFound, orientGuess: r.report.orientGuess, marks, hq,
    status: r.report.status, inkCover: r.report.inkCover, bgMean: r.report.bgMean, blobs: r.report.blobs, usedInkThresh: r.params.inkThresh,
    start: [lv.startX, lv.startY], flag: [lv.flagX, lv.flagY],
    crc: { straight: crc32(bytes(w.straight565)), ink: crc32(w.ink), cls: crc32(w.cls), ground: crc32(lv.ground), spike: crc32(lv.spike), bouncy: crc32(lv.bouncy), scrb: scrbCrc(lv) },
    rethresh: [], live: [],
  };
  for (const extra of c.rethresh ?? []) {                                       // Review's thinner / thicker buttons etc.
    const rr = visionRethreshold(w, { ...defaultParams(), ...extra });
    res.rethresh.push({ params: extra, status: rr.report.status, inkCover: rr.report.inkCover, blobs: rr.report.blobs, ground: crc32(rr.level.ground), spike: crc32(rr.level.spike), bouncy: crc32(rr.level.bouncy), scrb: scrbCrc(rr.level) });
  }
  const gray = grayQvga(photo);
  for (const upright of c.name === 'clean' ? [true, false] : [true]) {
    const lc = liveCheck(gray, upright);
    res.live.push({ upright, marks: lc.marks, quad: flat(lc.quad as [number, number][] | null), distCm: lc.distCm, edgePx: lc.edgePx, paperMean: lc.paperMean, clipFrac: lc.clipFrac, contrast: lc.contrast, ratioH: lc.ratioH, ratioV: lc.ratioV, lights: lc.lights, cue: CUE[lc.cue], allGreen: lc.allGreen });
  }
  return res;
}
