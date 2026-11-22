// synth.ts - render a Level onto the printed A4 template as a camera would see it.
// Used for (a) golden / round-trip tests of vision.ts and (b) the simulator's "Demo photo" button, so paper
// mode can be tried with no paper and no camera. Camera model: pinhole, focal length from the 46 degree FOV.
import * as C from './config';
import { Level, getBit } from './level';
import { pack565 } from './vision';

export interface ShotOptions {
  distanceCm?: number;      // camera to sheet centre (default 40)
  yawDeg?: number;          // sheet turned about its vertical axis
  pitchDeg?: number;        // sheet tipped about its horizontal axis
  rollDeg?: number;         // sheet rotated in its own plane (180 = held upside down)
  offsetXmm?: number; offsetYmm?: number;
  brightness?: number;      // 0..1, paper white level (default 0.92)
  gradient?: number;        // 0..1 darkening from left to right (uneven light)
  shadow?: boolean;         // a hard diagonal shadow over the left of the sheet
  noise?: number;           // sensor noise, standard deviation in 8-bit levels
  blurPx?: number;          // box blur radius in VGA pixels (defocus)
  seed?: number;
  drawMarks?: boolean;      // false = a plain hand-drawn sheet (tests the fallback)
}

const SHEET_W = C.TPL_SHEET_W_MM, SHEET_H = C.TPL_SHEET_H_MM;
const CX = C.TPL_MARK_X0_MM + C.TPL_MARK_RECT_W_MM / 2, CY = C.TPL_MARK_Y0_MM + C.TPL_MARK_RECT_H_MM / 2;
const AREA_W = C.TPL_MARK_RECT_W_MM * C.AREA_SCALE, AREA_H = C.TPL_MARK_RECT_H_MM * C.AREA_SCALE;
const AREA_X0 = CX - AREA_W / 2, AREA_Y0 = CY - AREA_H / 2;

export function renderTemplateShot(level: Level, o: ShotOptions = {}): Uint16Array {
  const dist = (o.distanceCm ?? 40) * 10, bright = o.brightness ?? 0.92;
  const yaw = ((o.yawDeg ?? 0) * Math.PI) / 180, pitch = ((o.pitchDeg ?? 0) * Math.PI) / 180, roll = ((o.rollDeg ?? 0) * Math.PI) / 180;
  const f = C.CAM_FOCAL_PX_320 * 2; // pixels, 640 wide
  // rotation R = Rz(roll) * Ry(yaw) * Rx(pitch)
  const cy_ = Math.cos(yaw), sy_ = Math.sin(yaw), cp = Math.cos(pitch), sp = Math.sin(pitch), cr = Math.cos(roll), sr = Math.sin(roll);
  const Rx = [[1, 0, 0], [0, cp, -sp], [0, sp, cp]], Ry = [[cy_, 0, sy_], [0, 1, 0], [-sy_, 0, cy_]], Rz = [[cr, -sr, 0], [sr, cr, 0], [0, 0, 1]];
  const mul = (A: number[][], B: number[][]) => A.map((r) => [0, 1, 2].map((j) => r[0] * B[0][j] + r[1] * B[1][j] + r[2] * B[2][j]));
  const R = mul(Rz, mul(Ry, Rx));
  const Cc = [o.offsetXmm ?? 0, o.offsetYmm ?? 0, dist];
  const n = [R[0][2], R[1][2], R[2][2]];
  const nC = n[0] * Cc[0] + n[1] * Cc[1] + n[2] * Cc[2];

  let seed = (o.seed ?? 1) >>> 0;
  const rnd = () => ((seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0) / 4294967296);
  const R8 = new Float32Array(C.CAM_W * C.CAM_H), G8 = new Float32Array(C.CAM_W * C.CAM_H), B8 = new Float32Array(C.CAM_W * C.CAM_H);
  const markCentres = [[C.TPL_MARK_X0_MM, C.TPL_MARK_Y0_MM], [C.TPL_MARK_X0_MM + C.TPL_MARK_RECT_W_MM, C.TPL_MARK_Y0_MM], [C.TPL_MARK_X0_MM + C.TPL_MARK_RECT_W_MM, C.TPL_MARK_Y0_MM + C.TPL_MARK_RECT_H_MM], [C.TPL_MARK_X0_MM, C.TPL_MARK_Y0_MM + C.TPL_MARK_RECT_H_MM]];
  const offs = [0.25, 0.75];

  for (let py = 0; py < C.CAM_H; py++) for (let px = 0; px < C.CAM_W; px++) {
    let sr_ = 0, sg_ = 0, sb_ = 0;
    for (const oy of offs) for (const ox of offs) {
      const dx = (px + ox - C.CAM_W / 2) / f, dy = (py + oy - C.CAM_H / 2) / f;      // ray (dx, dy, 1)
      const nd = n[0] * dx + n[1] * dy + n[2];
      let r = 0, g = 0, b = 0, onSheet = false;
      if (Math.abs(nd) > 1e-6) {
        const t = nC / nd;
        if (t > 0) {
          const hx = dx * t - Cc[0], hy = dy * t - Cc[1], hz = t - Cc[2];
          const u = R[0][0] * hx + R[1][0] * hy + R[2][0] * hz + SHEET_W / 2;        // sheet mm from the top-left
          const v = R[0][1] * hx + R[1][1] * hy + R[2][1] * hz + SHEET_H / 2;
          if (u >= 0 && v >= 0 && u < SHEET_W && v < SHEET_H) {
            onSheet = true; r = g = b = 240;                                          // white paper
            // drawing: level pixel under this point
            const lx = Math.floor(((u - AREA_X0) / AREA_W) * C.WORLD_W), ly = Math.floor(((v - AREA_Y0) / AREA_H) * C.WORLD_H);
            if (lx >= 0 && ly >= 0 && lx < C.WORLD_W && ly < C.WORLD_H) {
              if (getBit(level.ground, lx, ly)) { r = 28; g = 26; b = 30; }
              else if (getBit(level.spike, lx, ly)) { r = 205; g = 50; b = 42; }
              else if (getBit(level.bouncy, lx, ly)) { r = 45; g = 95; b = 205; }
            }
            // dashed drawing-area box, very light grey
            const bx = Math.min(Math.abs(u - AREA_X0), Math.abs(u - (AREA_X0 + AREA_W))), by = Math.min(Math.abs(v - AREA_Y0), Math.abs(v - (AREA_Y0 + AREA_H)));
            if ((bx < 0.4 && v > AREA_Y0 && v < AREA_Y0 + AREA_H) || (by < 0.4 && u > AREA_X0 && u < AREA_X0 + AREA_W)) { r = g = b = 208; }
            // corner marks: three discs and one ring (top-left)
            if (o.drawMarks !== false) markCentres.forEach(([mx, my], i) => {
              const d = Math.hypot(u - mx, v - my);
              if (d <= C.TPL_MARK_DIAMETER_MM / 2 && !(i === 0 && d <= C.TPL_RING_HOLE_MM / 2)) { r = 22; g = 22; b = 26; }
            });
          }
        }
      }
      if (!onSheet) { r = 78; g = 66; b = 56; }                                       // the table
      let lum = bright * (1 - (o.gradient ?? 0) * (px / C.CAM_W));
      if (o.shadow && px < 150 + 0.35 * py) lum *= 0.62;
      sr_ += r * lum; sg_ += g * lum; sb_ += b * lum;
    }
    const i = py * C.CAM_W + px; R8[i] = sr_ / 4; G8[i] = sg_ / 4; B8[i] = sb_ / 4;
  }

  const blur = o.blurPx ?? 0;
  const chans = blur > 0 ? [R8, G8, B8].map((ch) => boxBlur(ch, blur)) : [R8, G8, B8];
  const out = new Uint16Array(C.CAM_W * C.CAM_H), sigma = o.noise ?? 0;
  for (let i = 0; i < out.length; i++) {
    const nz = sigma ? (rnd() + rnd() + rnd() - 1.5) * 2 * sigma : 0;
    out[i] = pack565(clamp8(chans[0][i] + nz), clamp8(chans[1][i] + nz * 0.8), clamp8(chans[2][i] + nz * 0.9));
  }
  return out;
}

const clamp8 = (v: number) => (v < 0 ? 0 : v > 255 ? 255 : Math.round(v));

function boxBlur(src: Float32Array, r: number): Float32Array {
  const w = C.CAM_W, h = C.CAM_H, tmp = new Float32Array(src.length), out = new Float32Array(src.length), k = 2 * r + 1;
  for (let y = 0; y < h; y++) { let s = 0; for (let x = -r; x <= r; x++) s += src[y * w + Math.min(w - 1, Math.max(0, x))];
    for (let x = 0; x < w; x++) { tmp[y * w + x] = s / k; s += src[y * w + Math.min(w - 1, x + r + 1)] - src[y * w + Math.max(0, x - r)]; } }
  for (let x = 0; x < w; x++) { let s = 0; for (let y = -r; y <= r; y++) s += tmp[Math.min(h - 1, Math.max(0, y)) * w + x];
    for (let y = 0; y < h; y++) { out[y * w + x] = s / k; s += tmp[Math.min(h - 1, y + r + 1) * w + x] - tmp[Math.max(0, y - r) * w + x]; } }
  return out;
}
