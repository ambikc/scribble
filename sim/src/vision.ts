// vision.ts - camera frame of the printed template -> Level.   (portable core: no DOM, no I/O)
// Mirrors firmware vision.h/.cpp expression for expression. Pipeline: docs/system-design.md section 5.
//
//   0 capture (VGA RGB565, done by camera.ts)  1 find the four corner marks  2 straighten (homography + warp)
//   3 convert  4 paper estimate  5 ink strength  6 classify  7 clean up  8 quality  9 place pins  10 pack
//
// Float discipline (docs/system-design.md 9.2). Everything per pixel is INTEGER maths, so the C++ gives the same bytes:
// the QVGA mark threshold, the warp, the paper estimate, ink, classes and all clean-up stages. Floating point (double) is
// only used where the number of operations is tiny and every operation is + - * / sqrt (exactly rounded everywhere):
// the blob statistics, the quad search, the sub-pixel mark centres, the 8 x 8 homography solve, the live check.
// No Math.hypot / acos / atan2 / cos / sin (they differ between libraries): see len(), atan2DegUp(), COS16 / SIN16.
// The homography is rounded to 16.16 fixed point before the warp, and the warp uses integers only.
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
  gray: Uint8Array;           // stage 3 (scratch afterwards)
  bg: Uint8Array;             // stage 4: per-pixel paper brightness (scratch afterwards)
  ink: Uint8Array;            // stage 5: 0..255
  cls: Uint8Array;            // stage 6: 0 dark/other (ground), 1 red, 2 blue
  labels: Uint8Array;         // final: 0 none, 1 ground, 2 spike, 3 bouncy (after clean-up)
  marks: Marks | null;
  bgMean: number;
}
export interface VisionResult { level: Level; report: VisionReport; work: VisionWork }

// ---- small helpers ------------------------------------------------------------------------------
const rOf = (p: number) => ((p >> 11) << 3) | (p >> 13);
const gOf = (p: number) => (((p >> 5) & 63) << 2) | ((p >> 9) & 3);
const bOf = (p: number) => ((p & 31) << 3) | ((p >> 2) & 7);
export const pack565 = (r: number, g: number, b: number) => ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
const grayOf = (r: number, g: number, b: number) => (77 * r + 150 * g + 29 * b) >> 8;
const clamp = (v: number, lo: number, hi: number) => (v < lo ? lo : v > hi ? hi : v);
/** sqrt(x*x + y*y): sqrt is exactly rounded everywhere, Math.hypot is not. */
const len = (x: number, y: number) => Math.sqrt(x * x + y * y);
/** floor(a / b) for integers |a| < 2^52 and b > 0. In C++ this is a floor-division helper (the / operator truncates). */
const fdiv = (a: number, b: number) => Math.floor(a / b);

// cos / sin of k * 22.5 degrees as literals (the same tables as physics.ts); the 8-step circles use the even entries.
const COS16 = [1.0, 0.9238795325112867, 0.7071067811865476, 0.38268343236508984, 6.123233995736766e-17, -0.3826834323650897, -0.7071067811865475, -0.9238795325112867, -1.0, -0.9238795325112868, -0.7071067811865477, -0.38268343236509034, -1.8369701987210297e-16, 0.38268343236509, 0.7071067811865474, 0.9238795325112865];
const SIN16 = [0.0, 0.3826834323650898, 0.7071067811865475, 0.9238795325112867, 1.0, 0.9238795325112867, 0.7071067811865476, 0.3826834323650899, 1.2246467991473532e-16, -0.38268343236508967, -0.7071067811865475, -0.9238795325112865, -1.0, -0.9238795325112866, -0.7071067811865477, -0.3826834323650904];

/** atan(t) in degrees for 0 <= t <= 1 (Abramowitz-Stegun 4.4.49, error < 0.0006 degrees), + - * only. */
function atanDeg(t: number): number {
  const t2 = t * t;
  return t * (0.9998660 + t2 * (-0.3302995 + t2 * (0.1801410 + t2 * (-0.0851330 + t2 * 0.0208351)))) * 57.29577951308232;
}
/** atan2(y, x) in degrees for y >= 0, result 0..180, from + - * / only (so the C++ port gives the same bits). */
function atan2DegUp(y: number, x: number): number {
  const ax = x < 0 ? -x : x;
  let a: number;
  if (y === 0 && ax === 0) a = 0; else if (y >= ax) a = 90 - atanDeg(ax / y); else a = atanDeg(y / ax);
  return x < 0 ? 180 - a : a;
}

/** 640x480 RGB565 -> 320x240 gray by 2 x 2 averaging (the image the mark finder works on). */
export function grayQvga(vga: Uint16Array, out: Uint8Array = new Uint8Array(QW * QH)): Uint8Array {
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

/** Keep the MARK_MAX_CANDIDATES most disc-like blobs (a disc fills pi/4 = 0.785 of its box), stable for equal keys. */
function addCandidate(list: Blob[], b: Blob): void {
  const key = Math.abs(b.fill - 0.76);
  let i = list.length;
  while (i > 0 && Math.abs(list[i - 1].fill - 0.76) > key) i--;
  if (i >= C.MARK_MAX_CANDIDATES) return;
  list.splice(i, 0, b);
  if (list.length > C.MARK_MAX_CANDIDATES) list.pop();
}

/** Adaptive threshold (integer test) + connected components on the QVGA gray image; returns blob-shaped candidates. */
export function findBlobs(gray: Uint8Array, markRatio: number): Blob[] {
  const iw = QW + 1;
  const integ = new Uint32Array(iw * (QH + 1));
  for (let y = 0; y < QH; y++) {
    let row = 0;
    for (let x = 0; x < QW; x++) { row += gray[y * QW + x]; integ[(y + 1) * iw + x + 1] = integ[y * iw + x + 1] + row; }
  }
  const half = (C.MARK_WINDOW - 1) >> 1;
  const ratioQ = Math.round(markRatio * 1024);                 // MARK_RATIO in 1/1024
  const mask = new Uint8Array(QW * QH);
  for (let y = 0; y < QH; y++) {
    const y0 = Math.max(0, y - half), y1 = Math.min(QH, y + half + 1);
    for (let x = 0; x < QW; x++) {
      const x0 = Math.max(0, x - half), x1 = Math.min(QW, x + half + 1);
      const sum = integ[y1 * iw + x1] - integ[y0 * iw + x1] - integ[y1 * iw + x0] + integ[y0 * iw + x0];
      const area = (x1 - x0) * (y1 - y0), ga = gray[y * QW + x] * area;   // mean = sum / area; g * area is g in the same scale
      // dark if  g < ratio * mean  and  mean - g >= MARK_MIN_CONTRAST   (both multiplied by the window area)
      if (ga * 1024 < ratioQ * sum && sum - ga >= C.MARK_MIN_CONTRAST * area) mask[y * QW + x] = 1;
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
    addCandidate(blobs, { area, cx: sx / area, cy: sy / area, bw, bh, fill });
  }
  return blobs;
}

/** Order four points clockwise (image y points down) around their centroid: ascending atan2 angle, without atan2. */
function orderClockwise(p: Pt[]): Pt[] {
  const cx = (p[0][0] + p[1][0] + p[2][0] + p[3][0]) / 4, cy = (p[0][1] + p[1][1] + p[2][1] + p[3][1]) / 4;
  // quadrant key: angle in (-pi,0) = 0, exactly 0 = 1, (0,pi) = 2, exactly pi = 3; inside 0 and 2 the cross product orders
  const half = (dx: number, dy: number) => (dy < 0 ? 0 : dy > 0 ? 2 : dx > 0 ? 1 : 3);
  const cmp = (a: Pt, b: Pt) => {
    const ax = a[0] - cx, ay = a[1] - cy, bx = b[0] - cx, by = b[1] - cy, ha = half(ax, ay), hb = half(bx, by);
    if (ha !== hb) return ha - hb;
    const cr = ax * by - ay * bx; return cr > 0 ? -1 : cr < 0 ? 1 : 0;
  };
  const o: Pt[] = [p[0], p[1], p[2], p[3]];
  for (let i = 1; i < 4; i++) for (let j = i; j > 0 && cmp(o[j - 1], o[j]) > 0; j--) { const t = o[j]; o[j] = o[j - 1]; o[j - 1] = t; }
  return o;
}

interface Quad { blobs: Blob[]; pts: Pt[]; score: number }

/** Choose the best four blobs: rectangular, similar size, 5:3 (or 3:5) overall. */
function bestQuad(blobs: Blob[]): Quad | null {
  const n = blobs.length; let best: Quad | null = null;
  const target = C.TPL_MARK_RECT_W_MM / C.TPL_MARK_RECT_H_MM;
  for (let a = 0; a < n - 3; a++) for (let b = a + 1; b < n - 2; b++) for (let c = b + 1; c < n - 1; c++) for (let d = c + 1; d < n; d++) {
    const q = [blobs[a], blobs[b], blobs[c], blobs[d]];
    const amax = Math.max(q[0].area, q[1].area, q[2].area, q[3].area), amin = Math.min(q[0].area, q[1].area, q[2].area, q[3].area);
    if (amax / amin > C.QUAD_AREA_RATIO_MAX) continue;
    const pts = orderClockwise(q.map((k): Pt => [k.cx, k.cy]));
    const s: number[] = [];
    for (let i = 0; i < 4; i++) s.push(len(pts[(i + 1) % 4][0] - pts[i][0], pts[(i + 1) % 4][1] - pts[i][1]));
    if (Math.min(s[0], s[1], s[2], s[3]) < 20) continue;
    let angDev = 0, ok = true;
    for (let i = 0; i < 4; i++) {
      const p0 = pts[(i + 3) % 4], p1 = pts[i], p2 = pts[(i + 1) % 4];
      const v1x = p0[0] - p1[0], v1y = p0[1] - p1[1], v2x = p2[0] - p1[0], v2y = p2[1] - p1[1];
      const cross = v1x * v2y - v1y * v2x, dot = v1x * v2x + v1y * v2y;
      const dev = Math.abs(atan2DegUp(Math.abs(cross), dot) - 90); angDev += dev; if (dev > C.QUAD_ANGLE_TOL) ok = false;
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

/** Bilinear gray sample from the VGA frame at (x,y) in VGA pixels (double; used for the 21 ring-test samples only). */
function vgaGray(vga: Uint16Array, x: number, y: number): number {
  x = clamp(x, 0, VW - 1.001); y = clamp(y, 0, VH - 1.001);
  const x0 = x | 0, y0 = y | 0, fx = x - x0, fy = y - y0;
  const g = (xx: number, yy: number) => { const p = vga[yy * VW + xx]; return grayOf(rOf(p), gOf(p), bOf(p)); };
  return (g(x0, y0) * (1 - fx) + g(x0 + 1, y0) * fx) * (1 - fy) + (g(x0, y0 + 1) * (1 - fx) + g(x0 + 1, y0 + 1) * fx) * fy;
}
/** Gray of the VGA pixel at integer (x,y), coordinates clamped to the frame. */
function vgaGrayInt(vga: Uint16Array, x: number, y: number): number {
  const p = vga[clamp(y, 0, VH - 1) * VW + clamp(x, 0, VW - 1)];
  return grayOf(rOf(p), gOf(p), bOf(p));
}

/** Stage 1 (full): coarse quad, ring detection at full VGA resolution (which mark is top-left), sub-pixel centres. */
export function findMarks(vga: Uint16Array, p: VisionParams): Marks | null {
  const gray = grayQvga(vga);
  const q = findQuadCoarse(gray, p.markRatio);
  if (!q) return null;
  // map the coarse blobs to their ordered points so we keep each blob's size
  const info = q.pts.map((pt) => {
    const bl = q.blobs.reduce((a, b) => (len(a.cx - pt[0], a.cy - pt[1]) < len(b.cx - pt[0], b.cy - pt[1]) ? a : b));
    return { cx: pt[0] * 2 + 1, cy: pt[1] * 2 + 1, R: Math.sqrt(bl.area / Math.PI) * 2 }; // VGA px
  });
  // refine each centre: intensity-weighted centroid of the dark pixels in a 40 x 40 VGA window (integer weights)
  const refined: Pt[] = info.map((m) => {
    let lo = 255, hi = 0; const x0 = Math.round(m.cx) - 20, y0 = Math.round(m.cy) - 20;
    const win = new Uint8Array(1600);
    for (let y = 0; y < 40; y++) for (let x = 0; x < 40; x++) { const g = vgaGrayInt(vga, x0 + x, y0 + y); win[y * 40 + x] = g; if (g < lo) lo = g; if (g > hi) hi = g; }
    const thr2 = lo + hi; let sw = 0, sx = 0, sy = 0;           // weight = (thr - g) * 2, thr = (lo + hi) / 2
    for (let y = 0; y < 40; y++) for (let x = 0; x < 40; x++) { const w = thr2 - 2 * win[y * 40 + x]; if (w > 0) { sw += w; sx += w * (x0 + x); sy += w * (y0 + y); } }
    return sw > 0 ? [sx / sw + 0.5, sy / sw + 0.5] : [m.cx, m.cy];
  });
  // ring test: a bright hole in the middle of the dark disc
  const ringScore = info.map((m, i) => {
    const [cx, cy] = refined[i];
    const centre = (vgaGray(vga, cx, cy) + vgaGray(vga, cx - 1, cy) + vgaGray(vga, cx + 1, cy) + vgaGray(vga, cx, cy - 1) + vgaGray(vga, cx, cy + 1)) / 5;
    let body = 0, paper = 0;
    for (let k = 0; k < 8; k++) { const c = COS16[2 * k], s = SIN16[2 * k]; body += vgaGray(vga, cx + 0.72 * m.R * c, cy + 0.72 * m.R * s); paper += vgaGray(vga, cx + 2.1 * m.R * c, cy + 2.1 * m.R * s); }
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
/** Solve the 3x3 projective map (h8 = 1) taking each src point to its dst point. Gaussian elimination (double). */
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

/**
 * World (level px) -> VGA map from the four marks, as integers, or null if degenerate: h0..h5 in 16.16 fixed point
 * (round(h * 2^16)); h6 and h7 are ~1e-4, so 16.16 would keep 5 bits of them (1 % error at 20 degrees of tilt):
 * they get 2.30 (round(h * 2^30)) instead.
 */
export function worldToVga(m: Marks): number[] | null {
  const wp: Pt[] = [[C.MARK_WORLD_X0, C.MARK_WORLD_Y0], [C.MARK_WORLD_X1, C.MARK_WORLD_Y0], [C.MARK_WORLD_X1, C.MARK_WORLD_Y1], [C.MARK_WORLD_X0, C.MARK_WORLD_Y1]];
  const h = solveHomography(wp, m.pts), hq: number[] = [];
  for (let i = 0; i < 8; i++) { const v = h[i] * (i < 6 ? 65536 : 1073741824); if (!(Math.abs(v) < 1e12)) return null; hq.push(Math.round(v)); }
  return hq;
}
/** Fallback: no marks, so take the centre 640 x 384 of the frame (5:3) and shrink it to 320 x 192 (x2, x2, +48). */
export function centreCropMap(): number[] { return [2 * 65536, 0, 0, 0, 2 * 65536, 48 * 65536, 0, 0]; }

const UMAX8 = (VW - 2) * 256 + 255, VMAX8 = (VH - 2) * 256 + 255;   // largest sample position, 1/256 px, so x0 + 1 stays in the frame

/**
 * Warp the VGA frame onto the 320 x 192 level grid, integers only. Each output pixel averages 4 bilinear taps
 * (at +1/4 and +3/4 of the pixel). With X4 = 4 * X (quarter pixels), h0..h5 in 16.16 and h6, h7 in 2.30:
 *   D  = h6*X4 + h7*Y4 + 4*2^30      = 4 * 2^30 * d,   d = h6 X + h7 Y + 1;   dq = D >> 16 = d in 16.16
 *   NU = h0*X4 + h1*Y4 + 4*h2        = 4 * 65536 * (h0 X + h1 Y + h2),   NV likewise with h3, h4, h5
 *   u + 0.5 = NU / (4 * dq)
 * One 32-bit division per tap gives inv = 2^31 / dq; u and v follow by multiplication, in 1/256 pixel.
 */
export function warp(vga: Uint16Array, hq: number[], out: Uint16Array = new Uint16Array(N)): Uint16Array {
  const [h0, h1, h2, h3, h4, h5, h6, h7] = hq;
  const NUMAX = 8589934592;                                    // 2^33: keeps NU * inv below 2^52 (exact in JS and C++)
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    let sr = 0, sg = 0, sb = 0;
    for (let oy4 = 1; oy4 <= 3; oy4 += 2) for (let ox4 = 1; ox4 <= 3; ox4 += 2) {
      const X4 = 4 * x + ox4, Y4 = 4 * y + oy4;
      let dq = Math.floor((h6 * X4 + h7 * Y4 + 4294967296) / 65536); dq = dq < 4096 ? 4096 : dq > 2147483647 ? 2147483647 : dq;   // d in 16.16, kept in 1/16 .. 32768
      const inv = fdiv(2147483647, dq);
      const nu = clamp(h0 * X4 + h1 * Y4 + 4 * h2, -NUMAX, NUMAX), nv = clamp(h3 * X4 + h4 * Y4 + 4 * h5, -NUMAX, NUMAX);
      const u8 = clamp(fdiv(nu * inv, 33554432) - 128, 0, UMAX8), v8 = clamp(fdiv(nv * inv, 33554432) - 128, 0, VMAX8);   // 1/256 px
      const x0 = u8 >> 8, y0 = v8 >> 8, fx = u8 & 255, fy = v8 & 255;
      const p00 = vga[y0 * VW + x0], p10 = vga[y0 * VW + x0 + 1], p01 = vga[(y0 + 1) * VW + x0], p11 = vga[(y0 + 1) * VW + x0 + 1];
      const w00 = (256 - fx) * (256 - fy), w10 = fx * (256 - fy), w01 = (256 - fx) * fy, w11 = fx * fy;   // sum 65536
      sr += rOf(p00) * w00 + rOf(p10) * w10 + rOf(p01) * w01 + rOf(p11) * w11;
      sg += gOf(p00) * w00 + gOf(p10) * w10 + gOf(p01) * w01 + gOf(p11) * w11;
      sb += bOf(p00) * w00 + bOf(p10) * w10 + bOf(p01) * w01 + bOf(p11) * w11;
    }
    out[y * W + x] = pack565((sr + 131072) >> 18, (sg + 131072) >> 18, (sb + 131072) >> 18);   // / (4 taps * 65536), rounded
  }
  return out;
}

// =================================================================================================
// Stages 3-5: convert, paper estimate, ink strength, and the colour class
// =================================================================================================
function analyse(straight: Uint16Array, work: VisionWork, p: VisionParams): void {
  const gray = work.gray;
  for (let i = 0; i < N; i++) {
    const px = straight[i], r = rOf(px), g = gOf(px), b = bOf(px);
    gray[i] = grayOf(r, g, b);
    const mx = Math.max(r, g, b), mn = Math.min(r, g, b), d = mx - mn;
    const sat = mx < 16 ? 0 : fdiv(d * 255, mx);
    let c = 0;
    if (mx >= p.colorVMin && sat >= p.satMin) {
      let h = 0;
      if (d > 0) {
        if (mx === r) h = fdiv(60 * (g - b), d); else if (mx === g) h = 120 + fdiv(60 * (b - r), d); else h = 240 + fdiv(60 * (r - g), d);
        if (h < 0) h += 360;
      }
      if (h >= C.HUE_RED_LO || h <= C.HUE_RED_HI) c = 1; else if (h >= C.HUE_BLUE_LO && h <= C.HUE_BLUE_HI) c = 2;
    }
    work.cls[i] = c;
  }
  // stage 4: paper estimate = max gray per 16 x 16 block, 3 x 3 blurred (kept x 256), bilinearly upsampled
  const gw = W / C.BG_BLOCK, gh = H / C.BG_BLOCK; // 20 x 12
  const grid = new Int32Array(gw * gh);
  for (let by = 0; by < gh; by++) for (let bx = 0; bx < gw; bx++) {
    let m = 0; for (let y = 0; y < C.BG_BLOCK; y++) for (let x = 0; x < C.BG_BLOCK; x++) m = Math.max(m, gray[(by * C.BG_BLOCK + y) * W + bx * C.BG_BLOCK + x]);
    grid[by * gw + bx] = m;
  }
  const blur = new Int32Array(gw * gh);
  let sum = 0;
  for (let by = 0; by < gh; by++) for (let bx = 0; bx < gw; bx++) {
    let s = 0, n = 0;
    for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) { const X = bx + dx, Y = by + dy; if (X >= 0 && Y >= 0 && X < gw && Y < gh) { s += grid[Y * gw + X]; n++; } }
    const v = fdiv(s * 256 + (n >> 1), n);                      // mean x 256, rounded
    blur[by * gw + bx] = v; sum += v;
  }
  work.bgMean = sum / (gw * gh * 256);
  for (let y = 0; y < H; y++) {
    const fy = clamp(2 * y - 15, 0, (gh - 1) * 32 - 1), y0 = fy >> 5, ty = fy & 31;   // (y + 0.5) / 16 - 0.5 in 1/32 grid cells
    for (let x = 0; x < W; x++) {
      const fx = clamp(2 * x - 15, 0, (gw - 1) * 32 - 1), x0 = fx >> 5, tx = fx & 31;
      const v = (blur[y0 * gw + x0] * (32 - tx) + blur[y0 * gw + x0 + 1] * tx) * (32 - ty) + (blur[(y0 + 1) * gw + x0] * (32 - tx) + blur[(y0 + 1) * gw + x0 + 1] * tx) * ty;
      work.bg[y * W + x] = (v + 131072) >> 18;                  // x256 x1024
    }
  }
  // stage 5: ink strength (independent of the threshold)
  for (let i = 0; i < N; i++) {
    const bg = work.bg[i], g = gray[i];
    work.ink[i] = bg >= p.bgMin && g < bg ? Math.min(255, fdiv(255 * (bg - g), bg)) : 0;
  }
}

// =================================================================================================
// Stage 7: clean up (despeckle, close, thicken, border, priority)
// =================================================================================================
/** Flood fill (4-connected) over pixels equal to `from`, setting them to `to`; returns the component size. */
function flood(mask: Uint8Array, s: number, from: number, to: number, stack: Uint16Array): number {
  let sp = 0, n = 0; stack[sp++] = s; mask[s] = to;
  while (sp) {
    const q = stack[--sp]; n++; const x = q % W, y = (q / W) | 0;
    if (x > 0 && mask[q - 1] === from) { mask[q - 1] = to; stack[sp++] = q - 1; }
    if (x < W - 1 && mask[q + 1] === from) { mask[q + 1] = to; stack[sp++] = q + 1; }
    if (y > 0 && mask[q - W] === from) { mask[q - W] = to; stack[sp++] = q - W; }
    if (y < H - 1 && mask[q + W] === from) { mask[q + W] = to; stack[sp++] = q + W; }
  }
  return n;
}
/** Remove 4-connected components smaller than `min` pixels from a 0/1 mask. Returns the number kept. */
function despeckle(mask: Uint8Array, min: number, stack: Uint16Array): number {
  let kept = 0;
  for (let s = 0; s < N; s++) {
    if (mask[s] !== 1) continue;
    const n = flood(mask, s, 1, 2, stack);                     // 2 = visited
    if (n < min) flood(mask, s, 2, 0, stack); else kept++;     // too small: erase it; else leave it marked 2
  }
  for (let i = 0; i < N; i++) if (mask[i] === 2) mask[i] = 1;
  return kept;
}

/** In place: separable square dilate (outside = 0) or erode (outside = 1) of radius r, with a sliding window count. */
function morph(buf: Uint8Array, tmp: Uint8Array, r: number, dilate: boolean): void {
  if (r <= 0) return;
  const on = dilate ? 1 : 0;                                   // the value that "counts" (erode counts zeros)
  for (let y = 0; y < H; y++) {                                // rows: buf -> tmp
    const row = y * W; let cnt = 0;
    for (let x = 0; x < r && x < W; x++) if (buf[row + x] === on) cnt++;
    for (let x = 0; x < W; x++) {
      if (x + r < W && buf[row + x + r] === on) cnt++;
      if (x - r - 1 >= 0 && buf[row + x - r - 1] === on) cnt--;
      tmp[row + x] = dilate ? (cnt > 0 ? 1 : 0) : (cnt > 0 ? 0 : 1);
    }
  }
  const col = new Uint8Array(W);                               // columns: tmp -> buf, one counter per column
  for (let y = 0; y < r && y < H; y++) for (let x = 0; x < W; x++) if (tmp[y * W + x] === on) col[x]++;
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    if (y + r < H && tmp[(y + r) * W + x] === on) col[x]++;
    if (y - r - 1 >= 0 && tmp[(y - r - 1) * W + x] === on) col[x]--;
    buf[y * W + x] = dilate ? (col[x] > 0 ? 1 : 0) : (col[x] > 0 ? 0 : 1);
  }
}

/** Stages 6 (labels from ink + class) and 7. Fills work.labels and returns coverage and blob count. */
function labelAndClean(work: VisionWork, p: VisionParams): { cover: number; blobs: number } {
  const A = work.gray, B = work.bg, T = new Uint8Array(N), stack = new Uint16Array(N);   // scratch: work.gray / work.bg are free after analyse
  const bits = work.labels; bits.fill(0);                       // bit k = class k survived (0 ground, 1 spike, 2 bouncy)
  let blobs = 0, covered = 0;
  for (let k = 0; k < 3; k++) {
    for (let i = 0; i < N; i++) A[i] = work.ink[i] >= p.inkThresh && work.cls[i] === k ? 1 : 0;
    blobs += despeckle(A, p.minBlob, stack);
    morph(A, T, p.closeR, true); morph(A, T, p.closeR, false);   // close: joins streaky wax gaps
    for (let i = 0; i < N; i++) covered += A[i];
    // thicken THIN strokes only: anything that vanishes under a (2*THIN_OPEN_R+1)-square opening is thin,
    // and only those pixels are dilated, so strokes that are already fat keep their width
    B.set(A);
    morph(B, T, C.THIN_OPEN_R, false); morph(B, T, C.THIN_OPEN_R, true);   // B = opened
    for (let i = 0; i < N; i++) B[i] = A[i] & (B[i] ^ 1);        // B = thin = closed and not opened
    morph(B, T, p.thickenR, true);                               // B = grown
    for (let i = 0; i < N; i++) if (A[i] | B[i]) bits[i] |= 1 << k;
  }
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const i = y * W + x, edge = x < p.edgeMargin || y < p.edgeMargin || x >= W - p.edgeMargin || y >= H - p.edgeMargin, b = bits[i];
    bits[i] = edge ? 0 : b & 2 ? 2 : b & 4 ? 3 : b & 1 ? 1 : 0; // priority spike > bouncy > ground
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
  const fx = flag[0] - start[0], fy = flag[1] - start[1];
  if (fx * fx + fy * fy < 100 * 100) flag = [304, flag[1]];
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
  level.startX = pins.start[0]; level.startY = pins.start[1];
  level.flagX = pins.flag[0]; level.flagY = pins.flag[1];
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
  let marks = findMarks(vga, p);
  let hq = marks ? worldToVga(marks) : null;
  if (!hq) { marks = null; hq = centreCropMap(); }              // marks missing (or a degenerate quad): centre-crop
  work.marks = marks;
  warp(vga, hq, work.straight565);
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
  for (let i = 0; i < 4; i++) s.push(len(pts[(i + 1) % 4][0] - pts[i][0], pts[(i + 1) % 4][1] - pts[i][1]));
  const l1 = (s[0] + s[2]) / 2, l2 = (s[1] + s[3]) / 2;
  const longPair = l1 >= l2 ? [s[0], s[2]] : [s[1], s[3]], shortPair = l1 >= l2 ? [s[1], s[3]] : [s[0], s[2]];
  const wPx = Math.max(l1, l2);
  const distCm = (C.TPL_MARK_RECT_W_MM * C.CAM_FOCAL_PX_320) / wPx / 10;
  const ratioH = Math.max(...longPair) / Math.min(...longPair), ratioV = Math.max(...shortPair) / Math.min(...shortPair);

  // sharpness: 10-90 % edge width across each mark's boundary, 16 radial profiles each
  let edgeSum = 0, edgeN = 0, markDark = 0;
  q.blobs.forEach((bl) => {
    const R = Math.sqrt(bl.area / Math.PI);
    let dark = 0; for (let k = 0; k < 8; k++) dark += bil(gray, bl.cx + 0.7 * R * COS16[2 * k], bl.cy + 0.7 * R * SIN16[2 * k]); markDark += dark / 8;
    for (let k = 0; k < 16; k++) {
      const c = COS16[k], sn = SIN16[k];
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
