// vision.ts - camera frame of the printed template -> Level.   (portable core: no DOM, no I/O)
// Mirrors firmware vision.h/.cpp. Pipeline: docs/system-design.md section 5.
//
//   0 capture (VGA RGB565, done by camera.ts)  1 find the four corner marks  2 straighten (homography + warp)
//   3 convert  4 paper estimate  5 ink strength  6 classify  7 clean up  8 quality  9 place pins  10 pack
//
// Everything after stage 2 is integer maths. Only the homography solve is floating point; its result is
// rounded to 16.16 fixed point (here: to 1/65536) before the per-pixel work.
import * as C from './config';
import { Level, newLevel, setBit, getBit, clearAround } from './level';

const W = C.WORLD_W, H = C.WORLD_H, N = W * H;
const VW = C.CAM_W, VH = C.CAM_H;
const QW = 320, QH = 240; // the "QVGA" gray image used to find the marks

// ---- result types ---------------------------------------------------------------------------
export const Q_OK = 0, Q_MARKS_MISSING = 1, Q_ORIENT_GUESS = 2, Q_TOO_DARK = 3, Q_EMPTY = 4, Q_TOO_MUCH_INK = 5, Q_NOISY = 6;
export type Pt = [number, number];

export interface VisionParams {
  inkThresh: number; satMin: number; colorVMin: number; minBlob: number;
  closeR: number; thickenR: number; edgeMargin: number; bgMin: number; markRatio: number;
}
export function defaultParams(): VisionParams {
  return {
    inkThresh: C.INK_THRESH, satMin: C.SAT_MIN, colorVMin: C.COLOR_V_MIN, minBlob: C.MIN_BLOB,
    closeR: C.CLOSE_R, thickenR: C.THICKEN_R, edgeMargin: C.EDGE_MARGIN, bgMin: C.BG_MIN, markRatio: C.MARK_RATIO,
  };
}

export interface VisionReport {
  status: number; marksFound: boolean; orientGuess: boolean;
  inkCover: number; bgMean: number; blobs: number;
}
/** Everything kept between stages so the Review screen can re-threshold without the camera. */
export interface VisionWork {
  straight565: Uint16Array;   // the straightened page, 320 x 192 RGB565 (also drawn dimmed behind the Review screen)
  gray: Uint8Array;           // stage 3
  bg: Uint8Array;             // stage 4: per-pixel paper brightness
  ink: Uint8Array;            // stage 5: 0..255
  cls: Uint8Array;            // stage 6: 0 dark/other (ground), 1 red, 2 blue
  labels: Uint8Array;         // final: 0 none, 1 ground, 2 spike, 3 bouncy (after clean-up)
  marks: Marks | null;
  bgMean: number;
}
export interface VisionResult { level: Level; report: VisionReport; work: VisionWork }

// ---- small helpers ------------------------------------------------------------------------------
const R5 = new Uint8Array(32), G6 = new Uint8Array(64);
for (let i = 0; i < 32; i++) R5[i] = (i << 3) | (i >> 2);
for (let i = 0; i < 64; i++) G6[i] = (i << 2) | (i >> 4);
const rOf = (p: number) => R5[p >> 11], gOf = (p: number) => G6[(p >> 5) & 63], bOf = (p: number) => R5[p & 31];
export const pack565 = (r: number, g: number, b: number) => ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
const grayOf = (r: number, g: number, b: number) => (77 * r + 150 * g + 29 * b) >> 8;
const clamp = (v: number, lo: number, hi: number) => (v < lo ? lo : v > hi ? hi : v);

/** 640x480 RGB565 -> 320x240 gray by 2 x 2 averaging (the image the mark finder works on). */
export function grayQvga(vga: Uint16Array): Uint8Array {
  const out = new Uint8Array(QW * QH);
  for (let y = 0; y < QH; y++) for (let x = 0; x < QW; x++) {
    const i = (2 * y) * VW + 2 * x;
    let s = 0;
    for (const o of [0, 1, VW, VW + 1]) { const p = vga[i + o]; s += grayOf(rOf(p), gOf(p), bOf(p)); }
    out[y * QW + x] = s >> 2;
  }
  return out;
}

// =================================================================================================
// Stage 1: find the four corner marks
// =================================================================================================
export interface Marks {
  /** Mark centres in VGA pixels, in PAGE order: top-left (the ring), top-right, bottom-right, bottom-left. */
  pts: [Pt, Pt, Pt, Pt];
  orientGuess: boolean;
}
interface Blob { area: number; cx: number; cy: number; bw: number; bh: number; fill: number }

/** Adaptive threshold + connected components on the QVGA gray image; returns blob-shaped candidates. */
export function findBlobs(gray: Uint8Array, markRatio: number): Blob[] {
  const iw = QW + 1;
  const integ = new Uint32Array(iw * (QH + 1));
  for (let y = 0; y < QH; y++) {
    let row = 0;
    for (let x = 0; x < QW; x++) { row += gray[y * QW + x]; integ[(y + 1) * iw + x + 1] = integ[y * iw + x + 1] + row; }
  }
  const half = (C.MARK_WINDOW - 1) >> 1;
  const mask = new Uint8Array(QW * QH);
  for (let y = 0; y < QH; y++) {
    const y0 = Math.max(0, y - half), y1 = Math.min(QH, y + half + 1);
    for (let x = 0; x < QW; x++) {
      const x0 = Math.max(0, x - half), x1 = Math.min(QW, x + half + 1);
      const sum = integ[y1 * iw + x1] - integ[y0 * iw + x1] - integ[y1 * iw + x0] + integ[y0 * iw + x0];
      const mean = sum / ((x1 - x0) * (y1 - y0));
      const g = gray[y * QW + x];
      if (g < markRatio * mean && mean - g >= C.MARK_MIN_CONTRAST) mask[y * QW + x] = 1;
    }
  }
  // 4-connected components with an explicit stack (no recursion, fixed memory on the device)
  const stack = new Int32Array(QW * QH);
  const blobs: Blob[] = [];
  for (let s = 0; s < mask.length; s++) {
    if (mask[s] !== 1) continue;
    let sp = 0; stack[sp++] = s; mask[s] = 2;
    let area = 0, sx = 0, sy = 0, minx = QW, maxx = 0, miny = QH, maxy = 0;
    while (sp) {
      const p = stack[--sp]; const px = p % QW, py = (p / QW) | 0;
      area++; sx += px; sy += py;
      if (px < minx) minx = px; if (px > maxx) maxx = px; if (py < miny) miny = py; if (py > maxy) maxy = py;
      if (px > 0 && mask[p - 1] === 1) { mask[p - 1] = 2; stack[sp++] = p - 1; }
      if (px < QW - 1 && mask[p + 1] === 1) { mask[p + 1] = 2; stack[sp++] = p + 1; }
      if (py > 0 && mask[p - QW] === 1) { mask[p - QW] = 2; stack[sp++] = p - QW; }
      if (py < QH - 1 && mask[p + QW] === 1) { mask[p + QW] = 2; stack[sp++] = p + QW; }
    }
    if (area < C.MARK_AREA_MIN || area > C.MARK_AREA_MAX) continue;
    const bw = maxx - minx + 1, bh = maxy - miny + 1, asp = bw / bh, fill = area / (bw * bh);
    if (asp < C.MARK_ASPECT_MIN || asp > C.MARK_ASPECT_MAX || fill < C.MARK_FILL_MIN || fill > C.MARK_FILL_MAX) continue;
    blobs.push({ area, cx: sx / area, cy: sy / area, bw, bh, fill });
  }
  // keep the most disc-like ones (a disc fills pi/4 = 0.785 of its box)
  blobs.sort((a, b) => Math.abs(a.fill - 0.76) - Math.abs(b.fill - 0.76));
  return blobs.slice(0, C.MARK_MAX_CANDIDATES);
}

/** Order four points clockwise (image y points down) around their centroid. */
function orderClockwise(p: Pt[]): Pt[] {
  const cx = p.reduce((s, q) => s + q[0], 0) / 4, cy = p.reduce((s, q) => s + q[1], 0) / 4;
  return [...p].sort((a, b) => Math.atan2(a[1] - cy, a[0] - cx) - Math.atan2(b[1] - cy, b[0] - cx));
}

interface Quad { blobs: Blob[]; pts: Pt[]; score: number }

/** Choose the best four blobs: rectangular, similar size, 5:3 (or 3:5) overall. */
function bestQuad(blobs: Blob[]): Quad | null {
  const n = blobs.length; let best: Quad | null = null;
  const target = C.TPL_MARK_RECT_W_MM / C.TPL_MARK_RECT_H_MM;
  for (let a = 0; a < n - 3; a++) for (let b = a + 1; b < n - 2; b++) for (let c = b + 1; c < n - 1; c++) for (let d = c + 1; d < n; d++) {
    const q = [blobs[a], blobs[b], blobs[c], blobs[d]];
    const areas = q.map((k) => k.area);
    if (Math.max(...areas) / Math.min(...areas) > C.QUAD_AREA_RATIO_MAX) continue;
    const pts = orderClockwise(q.map((k): Pt => [k.cx, k.cy]));
    const s: number[] = [];
    for (let i = 0; i < 4; i++) s.push(Math.hypot(pts[(i + 1) % 4][0] - pts[i][0], pts[(i + 1) % 4][1] - pts[i][1]));
    if (Math.min(...s) < 20) continue;
    let angDev = 0, ok = true;
    for (let i = 0; i < 4; i++) {
      const p0 = pts[(i + 3) % 4], p1 = pts[i], p2 = pts[(i + 1) % 4];
      const v1x = p0[0] - p1[0], v1y = p0[1] - p1[1], v2x = p2[0] - p1[0], v2y = p2[1] - p1[1];
      const ang = (Math.acos(clamp((v1x * v2x + v1y * v2y) / (Math.hypot(v1x, v1y) * Math.hypot(v2x, v2y)), -1, 1)) * 180) / Math.PI;
      const dev = Math.abs(ang - 90); angDev += dev; if (dev > C.QUAD_ANGLE_TOL) ok = false;
    }
    if (!ok) continue;
    const asym0 = Math.abs(s[0] - s[2]) / Math.max(s[0], s[2]), asym1 = Math.abs(s[1] - s[3]) / Math.max(s[1], s[3]);
    if (asym0 > C.QUAD_SIDE_TOL || asym1 > C.QUAD_SIDE_TOL) continue;
    const l1 = (s[0] + s[2]) / 2, l2 = (s[1] + s[3]) / 2;
    const aspect = Math.max(l1, l2) / Math.min(l1, l2);
    const aspErr = Math.abs(aspect - target) / target;
    if (aspErr > C.QUAD_ASPECT_TOL) continue;
    // shoelace area: prefer the big quad (the page corners) over small ones inside the drawing
    let area = 0; for (let i = 0; i < 4; i++) area += pts[i][0] * pts[(i + 1) % 4][1] - pts[(i + 1) % 4][0] * pts[i][1];
    area = Math.abs(area) / 2;
    const score = angDev / 4 + aspErr * 60 + (asym0 + asym1) * 30 - (area / (QW * QH)) * 60;
    if (!best || score < best.score) best = { blobs: q, pts, score };
  }
  return best;
}

/** Coarse search on a QVGA gray image (used by the live Camera check as well): the ordered quad or null. */
export function findQuadCoarse(gray: Uint8Array, markRatio = C.MARK_RATIO): { pts: Pt[]; blobs: Blob[] } | null {
  const q = bestQuad(findBlobs(gray, markRatio));
  return q ? { pts: q.pts, blobs: q.blobs } : null;
}

/** Bilinear gray sample from the VGA frame at (x,y) in VGA pixels. */
function vgaGray(vga: Uint16Array, x: number, y: number): number {
  x = clamp(x, 0, VW - 1.001); y = clamp(y, 0, VH - 1.001);
  const x0 = x | 0, y0 = y | 0, fx = x - x0, fy = y - y0;
  const g = (xx: number, yy: number) => { const p = vga[yy * VW + xx]; return grayOf(rOf(p), gOf(p), bOf(p)); };
  return (g(x0, y0) * (1 - fx) + g(x0 + 1, y0) * fx) * (1 - fy) + (g(x0, y0 + 1) * (1 - fx) + g(x0 + 1, y0 + 1) * fx) * fy;
}

/** Stage 1 (full): coarse quad, ring detection at full VGA resolution (which mark is top-left), sub-pixel centres. */
export function findMarks(vga: Uint16Array, p: VisionParams): Marks | null {
  const gray = grayQvga(vga);
  const q = findQuadCoarse(gray, p.markRatio);
  if (!q) return null;
  // map the coarse blobs to their ordered points so we keep each blob's size
  const info = q.pts.map((pt) => {
    const bl = q.blobs.reduce((a, b) => (Math.hypot(a.cx - pt[0], a.cy - pt[1]) < Math.hypot(b.cx - pt[0], b.cy - pt[1]) ? a : b));
    return { cx: pt[0] * 2 + 1, cy: pt[1] * 2 + 1, R: Math.sqrt(bl.area / Math.PI) * 2 }; // VGA px
  });
  // refine each centre: intensity-weighted centroid of the dark pixels in a 40 x 40 VGA window
  const refined: Pt[] = info.map((m) => {
    let lo = 255, hi = 0; const x0 = Math.round(m.cx) - 20, y0 = Math.round(m.cy) - 20;
    for (let y = y0; y < y0 + 40; y++) for (let x = x0; x < x0 + 40; x++) { const g = vgaGray(vga, x, y); lo = Math.min(lo, g); hi = Math.max(hi, g); }
    const thr = (lo + hi) / 2; let sw = 0, sx = 0, sy = 0;
    for (let y = y0; y < y0 + 40; y++) for (let x = x0; x < x0 + 40; x++) { const w = Math.max(0, thr - vgaGray(vga, x, y)); sw += w; sx += w * x; sy += w * y; }
    return sw > 0 ? [sx / sw + 0.5, sy / sw + 0.5] : [m.cx, m.cy];
  });
  // ring test: a bright hole in the middle of the dark disc
  const ringScore = info.map((m, i) => {
    const [cx, cy] = refined[i];
    const centre = (vgaGray(vga, cx, cy) + vgaGray(vga, cx - 1, cy) + vgaGray(vga, cx + 1, cy) + vgaGray(vga, cx, cy - 1) + vgaGray(vga, cx, cy + 1)) / 5;
    let body = 0, paper = 0;
    for (let k = 0; k < 8; k++) { const a = (k * Math.PI) / 4; body += vgaGray(vga, cx + 0.72 * m.R * Math.cos(a), cy + 0.72 * m.R * Math.sin(a)); paper += vgaGray(vga, cx + 2.1 * m.R * Math.cos(a), cy + 2.1 * m.R * Math.sin(a)); }
    body /= 8; paper /= 8;
    return (centre - body) / Math.max(1, paper - body);
  });
  const rings = ringScore.map((s, i) => (s > 0.45 ? i : -1)).filter((i) => i >= 0);
  let tl: number, guess = false;
  if (rings.length === 1) tl = rings[0];
  else { guess = true; tl = refined.reduce((bi, pt, i) => (pt[0] + pt[1] < refined[bi][0] + refined[bi][1] ? i : bi), 0); }
  const pts = [0, 1, 2, 3].map((k) => refined[(tl + k) % 4]) as [Pt, Pt, Pt, Pt]; // TL, TR, BR, BL in page order
  return { pts, orientGuess: guess };
}

// =================================================================================================
// Stage 2: straighten (homography + warp)
// =================================================================================================
/** Solve the 3x3 projective map (h8 = 1) taking each src point to its dst point. Gaussian elimination. */
export function solveHomography(src: Pt[], dst: Pt[]): number[] {
  const A: number[][] = [];
  for (let i = 0; i < 4; i++) {
    const [x, y] = src[i], [u, v] = dst[i];
    A.push([x, y, 1, 0, 0, 0, -u * x, -u * y, u]);
    A.push([0, 0, 0, x, y, 1, -v * x, -v * y, v]);
  }
  for (let c = 0; c < 8; c++) {
    let piv = c; for (let r = c + 1; r < 8; r++) if (Math.abs(A[r][c]) > Math.abs(A[piv][c])) piv = r;
    [A[c], A[piv]] = [A[piv], A[c]];
    for (let r = 0; r < 8; r++) if (r !== c) { const f = A[r][c] / A[c][c]; for (let k = c; k < 9; k++) A[r][k] -= f * A[c][k]; }
  }
  const h = A.map((row, i) => row[8] / row[i]);
  return [...h, 1];
}

const q16 = (v: number) => Math.round(v * 65536) / 65536; // "16.16 fixed point"

/** World (level px) -> VGA map from the four marks, as quantised coefficients. */
export function worldToVga(m: Marks): number[] {
  const wp: Pt[] = [[C.MARK_WORLD_X0, C.MARK_WORLD_Y0], [C.MARK_WORLD_X1, C.MARK_WORLD_Y0], [C.MARK_WORLD_X1, C.MARK_WORLD_Y1], [C.MARK_WORLD_X0, C.MARK_WORLD_Y1]];
  return solveHomography(wp, m.pts).map(q16);
}
/** Fallback: no marks, so take the centre 640 x 384 of the frame (5:3) and shrink it to 320 x 192. */
export function centreCropMap(): number[] { return [2, 0, 0, 0, 2, 48, 0, 0, 1]; }

/** Warp the VGA frame onto the 320 x 192 level grid. Each output pixel averages 4 bilinear taps. */
export function warp(vga: Uint16Array, h: number[]): Uint16Array {
  const out = new Uint16Array(N);
  const [h0, h1, h2, h3, h4, h5, h6, h7] = h;
  const off = [0.25, 0.75];
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    let sr = 0, sg = 0, sb = 0;
    for (const oy of off) for (const ox of off) {
      const X = x + ox, Y = y + oy, d = h6 * X + h7 * Y + 1;
      const u = clamp((h0 * X + h1 * Y + h2) / d - 0.5, 0, VW - 1.001), v = clamp((h3 * X + h4 * Y + h5) / d - 0.5, 0, VH - 1.001);
      const x0 = u | 0, y0 = v | 0, fx = u - x0, fy = v - y0;
      const p00 = vga[y0 * VW + x0], p10 = vga[y0 * VW + x0 + 1], p01 = vga[(y0 + 1) * VW + x0], p11 = vga[(y0 + 1) * VW + x0 + 1];
      const w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy), w01 = (1 - fx) * fy, w11 = fx * fy;
      sr += rOf(p00) * w00 + rOf(p10) * w10 + rOf(p01) * w01 + rOf(p11) * w11;
      sg += gOf(p00) * w00 + gOf(p10) * w10 + gOf(p01) * w01 + gOf(p11) * w11;
      sb += bOf(p00) * w00 + bOf(p10) * w10 + bOf(p01) * w01 + bOf(p11) * w11;
    }
    out[y * W + x] = pack565(Math.round(sr / 4), Math.round(sg / 4), Math.round(sb / 4));
  }
  return out;
}

// =================================================================================================
// Stages 3-5: convert, paper estimate, ink strength
// =================================================================================================
function analyse(straight: Uint16Array, work: VisionWork, p: VisionParams): void {
  const hue = new Uint16Array(N), sat = new Uint8Array(N), val = new Uint8Array(N);
  for (let i = 0; i < N; i++) {
    const px = straight[i], r = rOf(px), g = gOf(px), b = bOf(px);
    work.gray[i] = grayOf(r, g, b);
    const mx = Math.max(r, g, b), mn = Math.min(r, g, b), d = mx - mn;
    val[i] = mx; sat[i] = mx < 16 ? 0 : Math.floor((d * 255) / mx);
    let h = 0;
    if (d > 0) {
      if (mx === r) h = Math.floor((60 * (g - b)) / d); else if (mx === g) h = 120 + Math.floor((60 * (b - r)) / d); else h = 240 + Math.floor((60 * (r - g)) / d);
      if (h < 0) h += 360;
    }
    hue[i] = h;
  }
  // stage 4: paper estimate = max gray per 16 x 16 block, 3 x 3 blurred, bilinearly upsampled
  const gw = W / C.BG_BLOCK, gh = H / C.BG_BLOCK; // 20 x 12
  const grid = new Float32Array(gw * gh);
  for (let by = 0; by < gh; by++) for (let bx = 0; bx < gw; bx++) {
    let m = 0; for (let y = 0; y < C.BG_BLOCK; y++) for (let x = 0; x < C.BG_BLOCK; x++) m = Math.max(m, work.gray[(by * C.BG_BLOCK + y) * W + bx * C.BG_BLOCK + x]);
    grid[by * gw + bx] = m;
  }
  const blur = new Float32Array(gw * gh);
  for (let by = 0; by < gh; by++) for (let bx = 0; bx < gw; bx++) {
    let s = 0, n = 0;
    for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) { const X = bx + dx, Y = by + dy; if (X >= 0 && Y >= 0 && X < gw && Y < gh) { s += grid[Y * gw + X]; n++; } }
    blur[by * gw + bx] = s / n;
  }
  let sum = 0; for (const v of blur) sum += v; work.bgMean = sum / blur.length;
  for (let y = 0; y < H; y++) {
    const fy = clamp((y + 0.5) / C.BG_BLOCK - 0.5, 0, gh - 1.001), y0 = fy | 0, ty = fy - y0;
    for (let x = 0; x < W; x++) {
      const fx = clamp((x + 0.5) / C.BG_BLOCK - 0.5, 0, gw - 1.001), x0 = fx | 0, tx = fx - x0;
      const v = (blur[y0 * gw + x0] * (1 - tx) + blur[y0 * gw + x0 + 1] * tx) * (1 - ty) + (blur[(y0 + 1) * gw + x0] * (1 - tx) + blur[(y0 + 1) * gw + x0 + 1] * tx) * ty;
      work.bg[y * W + x] = Math.round(v);
    }
  }
  // stage 5 + 6: ink strength (independent of the threshold) and colour class (independent of ink)
  for (let i = 0; i < N; i++) {
    const bg = work.bg[i];
    work.ink[i] = bg >= p.bgMin ? clamp(Math.floor((255 * (bg - work.gray[i])) / bg), 0, 255) : 0;
    let c = 0;
    if (val[i] >= p.colorVMin && sat[i] >= p.satMin) {
      const h = hue[i];
      if (h >= C.HUE_RED_LO || h <= C.HUE_RED_HI) c = 1; else if (h >= C.HUE_BLUE_LO && h <= C.HUE_BLUE_HI) c = 2;
    }
    work.cls[i] = c;
  }
}

// =================================================================================================
// Stage 7: clean up (despeckle, close, thicken, border, priority)
// =================================================================================================
/** Remove 4-connected components smaller than `min` pixels. Returns the number kept. */
function despeckle(mask: Uint8Array, min: number): number {
  const seen = new Uint8Array(N), stack = new Int32Array(N), members = new Int32Array(N);
  let kept = 0;
  for (let s = 0; s < N; s++) {
    if (!mask[s] || seen[s]) continue;
    let sp = 0, n = 0; stack[sp++] = s; seen[s] = 1;
    while (sp) {
      const q = stack[--sp]; members[n++] = q; const x = q % W, y = (q / W) | 0;
      if (x > 0 && mask[q - 1] && !seen[q - 1]) { seen[q - 1] = 1; stack[sp++] = q - 1; }
      if (x < W - 1 && mask[q + 1] && !seen[q + 1]) { seen[q + 1] = 1; stack[sp++] = q + 1; }
      if (y > 0 && mask[q - W] && !seen[q - W]) { seen[q - W] = 1; stack[sp++] = q - W; }
      if (y < H - 1 && mask[q + W] && !seen[q + W]) { seen[q + W] = 1; stack[sp++] = q + W; }
    }
    if (n < min) for (let i = 0; i < n; i++) mask[members[i]] = 0; else kept++;
  }
  return kept;
}

/** Separable square dilate (outside = 0) or erode (outside = 1) of radius r. */
function morph(src: Uint8Array, r: number, dilate: boolean): Uint8Array {
  if (r <= 0) return src.slice();
  const tmp = new Uint8Array(N), out = new Uint8Array(N), outside = dilate ? 0 : 1;
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    let v = dilate ? 0 : 1;
    for (let k = -r; k <= r; k++) { const X = x + k; const s = X < 0 || X >= W ? outside : src[y * W + X]; if (dilate ? s : !s) { v = dilate ? 1 : 0; break; } }
    tmp[y * W + x] = v;
  }
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    let v = dilate ? 0 : 1;
    for (let k = -r; k <= r; k++) { const Y = y + k; const s = Y < 0 || Y >= H ? outside : tmp[Y * W + x]; if (dilate ? s : !s) { v = dilate ? 1 : 0; break; } }
    out[y * W + x] = v;
  }
  return out;
}

/** Stages 6 (labels from ink + class) and 7. Fills work.labels and returns coverage and blob count. */
function labelAndClean(work: VisionWork, p: VisionParams): { cover: number; blobs: number } {
  const masks = [new Uint8Array(N), new Uint8Array(N), new Uint8Array(N)]; // ground, spike, bouncy
  for (let i = 0; i < N; i++) if (work.ink[i] >= p.inkThresh) masks[work.cls[i] === 1 ? 1 : work.cls[i] === 2 ? 2 : 0][i] = 1;
  let blobs = 0, covered = 0;
  const finals: Uint8Array[] = [];
  for (let k = 0; k < 3; k++) {
    blobs += despeckle(masks[k], p.minBlob);
    const closed = morph(morph(masks[k], p.closeR, true), p.closeR, false);        // close: joins streaky wax gaps
    for (let i = 0; i < N; i++) covered += closed[i];
    // thicken THIN strokes only: anything that vanishes under a (2*THIN_OPEN_R+1)-square opening is thin,
    // and only those pixels are dilated, so strokes that are already fat keep their width
    const opened = morph(morph(closed, C.THIN_OPEN_R, false), C.THIN_OPEN_R, true);
    const thin = new Uint8Array(N); for (let i = 0; i < N; i++) thin[i] = closed[i] & (opened[i] ^ 1);
    const grown = morph(thin, p.thickenR, true);
    const m = new Uint8Array(N); for (let i = 0; i < N; i++) m[i] = closed[i] | grown[i];
    finals.push(m);
  }
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const i = y * W + x, edge = x < p.edgeMargin || y < p.edgeMargin || x >= W - p.edgeMargin || y >= H - p.edgeMargin;
    work.labels[i] = edge ? 0 : finals[1][i] ? 2 : finals[2][i] ? 3 : finals[0][i] ? 1 : 0; // priority spike > bouncy > ground
  }
  return { cover: covered / N, blobs };
}

// =================================================================================================
// Stages 8-10: quality, pins, pack
// =================================================================================================
function topSolid(labels: Uint8Array, x: number): number {
  for (let y = 0; y < H; y++) { const l = labels[y * W + x]; if (l === 1 || l === 3) return y; }
  return -1;
}
function placePins(labels: Uint8Array): { start: Pt; flag: Pt } {
  const supported = (x: number, yTop: number) => [-6, 6].every((dx) => { const t = topSolid(labels, clamp(x + dx, 0, W - 1)); return t >= 0 && Math.abs(t - yTop) <= 8; });
  const clearAbove = (x: number, yTop: number) => { for (let y = Math.max(0, yTop - 24); y < yTop; y++) if (labels[y * W + x] === 2) return false; return yTop >= 24; };
  let start: Pt = [32, 32], flag: Pt = [288, 100];
  for (let x = 16; x <= 120; x++) { const t = topSolid(labels, x); if (t >= 0 && clearAbove(x, t) && supported(x, t)) { start = [x, t - C.BALL_RADIUS - 1]; break; } }
  for (let x = 304; x >= 200; x--) { const t = topSolid(labels, x); if (t >= 0 && clearAbove(x, t) && supported(x, t)) { flag = [x, t]; break; } }
  if (Math.hypot(flag[0] - start[0], flag[1] - start[1]) < 100) flag = [304, flag[1]];
  return { start, flag };
}

function finish(work: VisionWork, p: VisionParams, marksFound: boolean, orientGuess: boolean): VisionResult {
  const { cover, blobs } = labelAndClean(work, p);
  let status = Q_OK;
  if (work.bgMean < C.Q_TOO_DARK) status = Q_TOO_DARK;
  else if (cover < C.Q_EMPTY_COVER) status = Q_EMPTY;
  else if (cover > C.Q_MUCH_COVER) status = Q_TOO_MUCH_INK;
  else if (blobs > C.Q_NOISY_BLOBS) status = Q_NOISY;
  else if (!marksFound) status = Q_MARKS_MISSING;
  else if (orientGuess) status = Q_ORIENT_GUESS;
  const level = newLevel();
  level.fromPaper = true;
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const l = work.labels[y * W + x];
    if (l === 1) setBit(level.ground, x, y); else if (l === 2) setBit(level.spike, x, y); else if (l === 3) setBit(level.bouncy, x, y);
  }
  const pins = placePins(work.labels);
  level.startX = Math.round(pins.start[0]); level.startY = Math.round(pins.start[1]);
  level.flagX = Math.round(pins.flag[0]); level.flagY = Math.round(pins.flag[1]);
  clearAround(level, level.startX, level.startY, C.START_SAFE_RADIUS);
  clearAround(level, level.flagX, level.flagY - C.FLAG_CENTER_DY, C.FLAG_SAFE_RADIUS);
  return { level, report: { status, marksFound, orientGuess, inkCover: cover, bgMean: work.bgMean, blobs }, work };
}

function newWork(): VisionWork {
  return { straight565: new Uint16Array(N), gray: new Uint8Array(N), bg: new Uint8Array(N), ink: new Uint8Array(N), cls: new Uint8Array(N), labels: new Uint8Array(N), marks: null, bgMean: 0 };
}

/** The whole pipeline once, with the given parameters. */
export function visionProcessOnce(vga: Uint16Array, p: VisionParams): VisionResult {
  const work = newWork();
  const marks = findMarks(vga, p);
  work.marks = marks;
  work.straight565 = warp(vga, marks ? worldToVga(marks) : centreCropMap());
  analyse(work.straight565, work, p);
  return finish(work, p, marks !== null, marks?.orientGuess ?? false);
}

/** Review's thinner/thicker buttons: stages 6-10 again on the kept ink array. No camera, no page finder. */
export function visionRethreshold(work: VisionWork, p: VisionParams): VisionResult {
  return finish(work, p, work.marks !== null, work.marks?.orientGuess ?? false);
}

/** The full pipeline plus the silent single automatic retry (docs/system-design.md 5.8). */
export function visionProcess(vga: Uint16Array, p: VisionParams = defaultParams()): VisionResult & { params: VisionParams } {
  let r = visionProcessOnce(vga, p), used = p;
  if (r.report.status === Q_EMPTY || r.report.status === Q_TOO_MUCH_INK) {
    const factor = r.report.status === Q_EMPTY ? 0.75 : 1.25;
    const p2 = { ...p, inkThresh: Math.round(p.inkThresh * factor) };
    const r2 = visionRethreshold(r.work, p2);
    if (r2.report.status === Q_OK || r2.report.status === Q_MARKS_MISSING || r2.report.status === Q_ORIENT_GUESS) { r = r2; used = p2; }
  }
  return { ...r, params: used };
}

// =================================================================================================
// The live Camera check (calibration screen): four lights from the four marks. ~8 Hz on the preview.
// =================================================================================================
export const LIGHT_GREY = 0, LIGHT_RED = 1, LIGHT_YELLOW = 2, LIGHT_GREEN = 3;
export interface LiveCheck {
  marks: boolean;
  quad: Pt[] | null;               // QVGA pixels, clockwise (page corner order unknown without the ring test)
  distCm: number; edgePx: number; paperMean: number; clipFrac: number; contrast: number;
  ratioH: number; ratioV: number;  // folded >= 1: how different opposite sides are (perspective)
  /** distance, sharp, light, level */
  lights: [number, number, number, number];
  cue: '' | 'closer' | 'back' | 'tilt';
  allGreen: boolean;
}

function bil(gray: Uint8Array, x: number, y: number): number {
  x = clamp(x, 0, QW - 1.001); y = clamp(y, 0, QH - 1.001);
  const x0 = x | 0, y0 = y | 0, fx = x - x0, fy = y - y0, g = (X: number, Y: number) => gray[Y * QW + X];
  return (g(x0, y0) * (1 - fx) + g(x0 + 1, y0) * fx) * (1 - fy) + (g(x0, y0 + 1) * (1 - fx) + g(x0 + 1, y0 + 1) * fx) * fy;
}

export function liveCheck(gray: Uint8Array, accelUpright = true): LiveCheck {
  const none: LiveCheck = { marks: false, quad: null, distCm: 0, edgePx: 99, paperMean: 0, clipFrac: 0, contrast: 0, ratioH: 1, ratioV: 1, lights: [0, 0, 0, 0], cue: '', allGreen: false };
  const q = findQuadCoarse(gray);
  if (!q) return none;
  const pts = q.pts, s: number[] = [];
  for (let i = 0; i < 4; i++) s.push(Math.hypot(pts[(i + 1) % 4][0] - pts[i][0], pts[(i + 1) % 4][1] - pts[i][1]));
  const l1 = (s[0] + s[2]) / 2, l2 = (s[1] + s[3]) / 2;
  const longPair = l1 >= l2 ? [s[0], s[2]] : [s[1], s[3]], shortPair = l1 >= l2 ? [s[1], s[3]] : [s[0], s[2]];
  const wPx = Math.max(l1, l2);
  const distCm = (C.TPL_MARK_RECT_W_MM * C.CAM_FOCAL_PX_320) / wPx / 10;
  const ratioH = Math.max(...longPair) / Math.min(...longPair), ratioV = Math.max(...shortPair) / Math.min(...shortPair);

  // sharpness: 10-90 % edge width across each mark's boundary, 16 radial profiles each
  let edgeSum = 0, edgeN = 0, markDark = 0;
  q.blobs.forEach((bl) => {
    const R = Math.sqrt(bl.area / Math.PI);
    let dark = 0; for (let k = 0; k < 8; k++) { const a = (k * Math.PI) / 4; dark += bil(gray, bl.cx + 0.7 * R * Math.cos(a), bl.cy + 0.7 * R * Math.sin(a)); } markDark += dark / 8;
    for (let k = 0; k < 16; k++) {
      const a = (k * Math.PI) / 8, c = Math.cos(a), sn = Math.sin(a);
      const prof: number[] = []; for (let t = 0; t <= 3 * R; t += 0.25) prof.push(bil(gray, bl.cx + t * c, bl.cy + t * sn));
      const lo = (prof[Math.round(0.7 * R / 0.25)] + prof[Math.round(0.6 * R / 0.25)]) / 2, hi = prof.slice(Math.round(2 * R / 0.25)).reduce((m, v) => Math.max(m, v), 0);
      if (hi - lo < 40) continue;
      const t10 = lo + 0.1 * (hi - lo), t90 = lo + 0.9 * (hi - lo);
      let a10 = -1, a90 = -1;
      for (let i = Math.round(0.7 * R / 0.25); i < prof.length; i++) { if (a10 < 0 && prof[i] >= t10) a10 = i * 0.25; if (a90 < 0 && prof[i] >= t90) { a90 = i * 0.25; break; } }
      if (a10 >= 0 && a90 >= a10) { edgeSum += a90 - a10; edgeN++; }
    }
  });
  const edgePx = edgeN ? edgeSum / edgeN : 99;
  markDark /= q.blobs.length;

  // light: the paper between the marks (80th percentile of a 12 x 8 grid inside the quad)
  const [p0, p1, p2, p3] = pts, samples: number[] = []; let clip = 0;
  for (let j = 0; j < 8; j++) for (let i = 0; i < 12; i++) {
    const u = 0.08 + (0.84 * i) / 11, v = 0.08 + (0.84 * j) / 7;
    const x = (p0[0] * (1 - u) + p1[0] * u) * (1 - v) + (p3[0] * (1 - u) + p2[0] * u) * v, y = (p0[1] * (1 - u) + p1[1] * u) * (1 - v) + (p3[1] * (1 - u) + p2[1] * u) * v;
    const g = bil(gray, x, y); samples.push(g); if (g >= 250) clip++;
  }
  samples.sort((a, b) => a - b);
  const paperMean = samples[Math.floor(samples.length * 0.8)], clipFrac = clip / samples.length, contrast = paperMean - markDark;

  const inFrame = pts.every((p) => p[0] >= 8 && p[1] >= 8 && p[0] <= QW - 8 && p[1] <= QH - 8);
  let dLight: number, cue: LiveCheck['cue'] = '';
  if (wPx >= C.DIST_GREEN_MIN_PX && wPx <= C.DIST_GREEN_MAX_PX && inFrame) dLight = LIGHT_GREEN;
  else {
    dLight = wPx >= C.DIST_GREEN_MIN_PX * 0.85 && wPx <= C.DIST_GREEN_MAX_PX * 1.12 ? LIGHT_YELLOW : LIGHT_RED;
    cue = wPx < C.DIST_GREEN_MIN_PX ? 'closer' : 'back';
  }
  const sLight = edgePx <= C.SHARP_GREEN_MAX_PX ? LIGHT_GREEN : edgePx <= C.SHARP_YELLOW_MAX_PX ? LIGHT_YELLOW : LIGHT_RED;
  const lLight = paperMean >= C.LIGHT_MEAN_MIN && paperMean <= C.LIGHT_MEAN_MAX && clipFrac < C.LIGHT_CLIP_MAX && contrast >= C.LIGHT_CONTRAST_MIN ? LIGHT_GREEN
    : paperMean >= 100 && paperMean <= 252 && contrast >= 60 ? LIGHT_YELLOW : LIGHT_RED;
  const worst = Math.max(ratioH, ratioV);
  const tLight = !accelUpright ? LIGHT_RED : worst <= C.LEVEL_RATIO_MAX ? LIGHT_GREEN : worst <= C.LEVEL_WARN_MAX ? LIGHT_YELLOW : LIGHT_RED;
  if (!cue && tLight !== LIGHT_GREEN) cue = 'tilt';
  const lights: [number, number, number, number] = [dLight, sLight, lLight, tLight];
  return { marks: true, quad: pts, distCm, edgePx, paperMean, clipFrac, contrast, ratioH, ratioV, lights, cue, allGreen: lights.every((l) => l === LIGHT_GREEN) };
}

// ---- tiny utility for tests and the debug panel ----------------------------------------------------------------
export function levelLayerIoU(a: Uint8Array, b: Uint8Array): number {
  let inter = 0, uni = 0;
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) { const A = getBit(a, x, y), B = getBit(b, x, y); inter += A & B; uni += A | B; }
  return uni ? inter / uni : 1;
}
