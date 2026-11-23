// vision.cpp - see vision.h. Every expression mirrors sim/src/vision.ts in the same order; the golden tests
// (test/test_vision, data from sim/scripts/make-vision-golden.ts) check that the bytes come out identical.
//
// Float discipline: doubles appear only where there are a handful of values, and only with + - * / sqrt (exactly rounded
// in IEEE 754, so the same bits as JavaScript). No hypot / acos / atan2 / cos / sin / round() from libm; instead len(),
// atan2DegUp(), the COS16 / SIN16 tables and jsRound() (== Math.round). Fused multiply-add would change the bits: the
// ESP32-S3 has none for double, and on a PC build with -mfma or -march=native add -ffp-contract=off (the native tests also
// pass with -march=native -O3, but that is the compiler's choice, not a promise).
#include "vision.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace scribble {

// ---- small helpers ------------------------------------------------------------------------------
static inline int rOf(unsigned p) { return int(((p >> 11) << 3) | (p >> 13)); }
static inline int gOf(unsigned p) { return int((((p >> 5) & 63) << 2) | ((p >> 9) & 3)); }
static inline int bOf(unsigned p) { return int(((p & 31) << 3) | ((p >> 2) & 7)); }
static inline int grayOf(int r, int g, int b) { return (77 * r + 150 * g + 29 * b) >> 8; }
static inline int iclamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int64_t lclamp(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline double dclamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline double dmax(double a, double b) { return a > b ? a : b; }
static inline double len(double x, double y) { return std::sqrt(x * x + y * y); }
/** == Math.round: halves go up (toward +infinity). */
static inline int64_t jsRound(double v) { const double f = std::floor(v); return int64_t(f) + ((v - f) >= 0.5 ? 1 : 0); }
/** floor(a / b) for b > 0 (the / operator truncates toward zero). */
static inline int fdiv(int a, int b) { int q = a / b; if ((a % b) < 0) q--; return q; }

static const double PI_ = 3.141592653589793;   // == Math.PI
// cos / sin of k * 22.5 degrees as literals (identical in vision.ts and physics.cpp); the 8-step circles use the even entries.
static const double COS16[16] = { 1.0, 0.9238795325112867, 0.7071067811865476, 0.38268343236508984, 6.123233995736766e-17, -0.3826834323650897, -0.7071067811865475, -0.9238795325112867, -1.0, -0.9238795325112868, -0.7071067811865477, -0.38268343236509034, -1.8369701987210297e-16, 0.38268343236509, 0.7071067811865474, 0.9238795325112865 };
static const double SIN16[16] = { 0.0, 0.3826834323650898, 0.7071067811865475, 0.9238795325112867, 1.0, 0.9238795325112867, 0.7071067811865476, 0.3826834323650899, 1.2246467991473532e-16, -0.38268343236508967, -0.7071067811865475, -0.9238795325112865, -1.0, -0.9238795325112866, -0.7071067811865477, -0.3826834323650904 };

/** atan(t) in degrees for 0 <= t <= 1 (Abramowitz-Stegun 4.4.49), + - * only. */
static double atanDeg(double t) {
  const double t2 = t * t;
  return t * (0.9998660 + t2 * (-0.3302995 + t2 * (0.1801410 + t2 * (-0.0851330 + t2 * 0.0208351)))) * 57.29577951308232;
}
/** atan2(y, x) in degrees for y >= 0, result 0..180. */
static double atan2DegUp(double y, double x) {
  const double ax = x < 0 ? -x : x;
  double a;
  if (y == 0 && ax == 0) a = 0; else if (y >= ax) a = 90 - atanDeg(ax / y); else a = atanDeg(y / ax);
  return x < 0 ? 180 - a : a;
}

VisionParams defaultParams() {
  return { INK_THRESH, SAT_MIN, COLOR_V_MIN, MIN_BLOB, CLOSE_R, THICKEN_R, EDGE_MARGIN, BG_MIN, MARK_RATIO };
}

void grayQvga(const uint16_t* vga, uint8_t* out) {
  for (int y = 0; y < QVGA_H; y++) for (int x = 0; x < QVGA_W; x++) {
    const int i = (2 * y) * VGA_W + 2 * x;
    const int o[4] = { 0, 1, VGA_W, VGA_W + 1 };
    int s = 0;
    for (int k = 0; k < 4; k++) { const unsigned p = vga[i + o[k]]; s += grayOf(rOf(p), gOf(p), bOf(p)); }
    out[y * QVGA_W + x] = uint8_t(s >> 2);
  }
}

// =================================================================================================
// Stage 1: find the four corner marks
// =================================================================================================
namespace {

struct Blob { int area; double cx, cy; int bw, bh; double fill; };
struct Quad { int idx[4]; Pt pts[4]; double score; };

/** Keep the MARK_MAX_CANDIDATES most disc-like blobs, stable for equal keys. */
void addCandidate(Blob* list, int& n, const Blob& b) {
  const double key = std::fabs(b.fill - 0.76);
  int i = n;
  while (i > 0 && std::fabs(list[i - 1].fill - 0.76) > key) i--;
  if (i >= MARK_MAX_CANDIDATES) return;
  const int last = n < MARK_MAX_CANDIDATES ? n : MARK_MAX_CANDIDATES - 1;
  for (int j = last; j > i; j--) list[j] = list[j - 1];
  list[i] = b;
  if (n < MARK_MAX_CANDIDATES) n++;
}

/** Adaptive threshold (integer test) + connected components on the QVGA gray image; returns the candidate count. */
int findBlobs(const uint8_t* gray, double markRatio, uint8_t* mask, uint32_t* integ, Blob* blobs) {
  const int iw = QVGA_W + 1;
  for (int x = 0; x < iw; x++) integ[x] = 0;
  for (int y = 0; y < QVGA_H; y++) {
    integ[(y + 1) * iw] = 0;
    uint32_t row = 0;
    for (int x = 0; x < QVGA_W; x++) { row += gray[y * QVGA_W + x]; integ[(y + 1) * iw + x + 1] = integ[y * iw + x + 1] + row; }
  }
  const int half = (MARK_WINDOW - 1) >> 1;
  const int ratioQ = int(jsRound(markRatio * 1024));              // MARK_RATIO in 1/1024
  for (int y = 0; y < QVGA_H; y++) {
    const int y0 = imax(0, y - half), y1 = imin(QVGA_H, y + half + 1);
    for (int x = 0; x < QVGA_W; x++) {
      const int x0 = imax(0, x - half), x1 = imin(QVGA_W, x + half + 1);
      const int sum = int(integ[y1 * iw + x1] - integ[y0 * iw + x1] - integ[y1 * iw + x0] + integ[y0 * iw + x0]);
      const int area = (x1 - x0) * (y1 - y0), ga = gray[y * QVGA_W + x] * area;
      mask[y * QVGA_W + x] = (ga * 1024 < ratioQ * sum && sum - ga >= MARK_MIN_CONTRAST * area) ? 1 : 0;
    }
  }
  // 4-connected components; the integral image is no longer needed, so its memory is the stack
  uint32_t* stack = integ;
  int nb = 0;
  for (int s = 0; s < QVGA_W * QVGA_H; s++) {
    if (mask[s] != 1) continue;
    int sp = 0; stack[sp++] = uint32_t(s); mask[s] = 2;
    int area = 0, sx = 0, sy = 0, minx = QVGA_W, maxx = 0, miny = QVGA_H, maxy = 0;
    while (sp) {
      const int p = int(stack[--sp]), px = p % QVGA_W, py = p / QVGA_W;
      area++; sx += px; sy += py;
      minx = imin(minx, px); maxx = imax(maxx, px); miny = imin(miny, py); maxy = imax(maxy, py);
      if (px > 0 && mask[p - 1] == 1) { mask[p - 1] = 2; stack[sp++] = uint32_t(p - 1); }
      if (px < QVGA_W - 1 && mask[p + 1] == 1) { mask[p + 1] = 2; stack[sp++] = uint32_t(p + 1); }
      if (py > 0 && mask[p - QVGA_W] == 1) { mask[p - QVGA_W] = 2; stack[sp++] = uint32_t(p - QVGA_W); }
      if (py < QVGA_H - 1 && mask[p + QVGA_W] == 1) { mask[p + QVGA_W] = 2; stack[sp++] = uint32_t(p + QVGA_W); }
    }
    if (area < MARK_AREA_MIN || area > MARK_AREA_MAX) continue;
    const int bw = maxx - minx + 1, bh = maxy - miny + 1;
    const double asp = double(bw) / bh, fill = double(area) / (bw * bh);
    if (asp < MARK_ASPECT_MIN || asp > MARK_ASPECT_MAX || fill < MARK_FILL_MIN || fill > MARK_FILL_MAX) continue;
    addCandidate(blobs, nb, { area, double(sx) / area, double(sy) / area, bw, bh, fill });
  }
  return nb;
}

/** Order four points clockwise (image y points down): ascending atan2 angle around the centroid, without atan2. */
void orderClockwise(const Pt in[4], Pt o[4]) {
  const double cx = (in[0].x + in[1].x + in[2].x + in[3].x) / 4, cy = (in[0].y + in[1].y + in[2].y + in[3].y) / 4;
  auto half = [](double dx, double dy) { return dy < 0 ? 0 : dy > 0 ? 2 : dx > 0 ? 1 : 3; };
  auto cmp = [&](const Pt& a, const Pt& b) {
    const double ax = a.x - cx, ay = a.y - cy, bx = b.x - cx, by = b.y - cy;
    const int ha = half(ax, ay), hb = half(bx, by);
    if (ha != hb) return ha - hb;
    const double cr = ax * by - ay * bx; return cr > 0 ? -1 : cr < 0 ? 1 : 0;
  };
  for (int i = 0; i < 4; i++) o[i] = in[i];
  for (int i = 1; i < 4; i++) for (int j = i; j > 0 && cmp(o[j - 1], o[j]) > 0; j--) { const Pt t = o[j]; o[j] = o[j - 1]; o[j - 1] = t; }
}

/** Choose the best four blobs: rectangular, similar size, 5:3 (or 3:5) overall. */
bool bestQuad(const Blob* blobs, int n, Quad& best) {
  bool have = false;
  const double target = double(TPL_MARK_RECT_W_MM) / TPL_MARK_RECT_H_MM;
  for (int a = 0; a < n - 3; a++) for (int b = a + 1; b < n - 2; b++) for (int c = b + 1; c < n - 1; c++) for (int d = c + 1; d < n; d++) {
    const int ix[4] = { a, b, c, d };
    const Blob* q[4] = { &blobs[a], &blobs[b], &blobs[c], &blobs[d] };
    const int amax = imax(imax(q[0]->area, q[1]->area), imax(q[2]->area, q[3]->area)), amin = imin(imin(q[0]->area, q[1]->area), imin(q[2]->area, q[3]->area));
    if (double(amax) / amin > QUAD_AREA_RATIO_MAX) continue;
    const Pt raw[4] = { { q[0]->cx, q[0]->cy }, { q[1]->cx, q[1]->cy }, { q[2]->cx, q[2]->cy }, { q[3]->cx, q[3]->cy } };
    Pt pts[4]; orderClockwise(raw, pts);
    double s[4];
    for (int i = 0; i < 4; i++) s[i] = len(pts[(i + 1) % 4].x - pts[i].x, pts[(i + 1) % 4].y - pts[i].y);
    if (std::fmin(std::fmin(s[0], s[1]), std::fmin(s[2], s[3])) < 20) continue;
    double angDev = 0; bool ok = true;
    for (int i = 0; i < 4; i++) {
      const Pt& p0 = pts[(i + 3) % 4]; const Pt& p1 = pts[i]; const Pt& p2 = pts[(i + 1) % 4];
      const double v1x = p0.x - p1.x, v1y = p0.y - p1.y, v2x = p2.x - p1.x, v2y = p2.y - p1.y;
      const double cross = v1x * v2y - v1y * v2x, dot = v1x * v2x + v1y * v2y;
      const double dev = std::fabs(atan2DegUp(std::fabs(cross), dot) - 90); angDev += dev; if (dev > QUAD_ANGLE_TOL) ok = false;
    }
    if (!ok) continue;
    const double asym0 = std::fabs(s[0] - s[2]) / dmax(s[0], s[2]), asym1 = std::fabs(s[1] - s[3]) / dmax(s[1], s[3]);
    if (asym0 > QUAD_SIDE_TOL || asym1 > QUAD_SIDE_TOL) continue;
    const double l1 = (s[0] + s[2]) / 2, l2 = (s[1] + s[3]) / 2;
    const double aspect = dmax(l1, l2) / std::fmin(l1, l2);
    const double aspErr = std::fabs(aspect - target) / target;
    if (aspErr > QUAD_ASPECT_TOL) continue;
    double area = 0; for (int i = 0; i < 4; i++) area += pts[i].x * pts[(i + 1) % 4].y - pts[(i + 1) % 4].x * pts[i].y;
    area = std::fabs(area) / 2;
    const double score = angDev / 4 + aspErr * 60 + (asym0 + asym1) * 30 - (area / (QVGA_W * QVGA_H)) * 60;
    if (!have || score < best.score) { have = true; for (int i = 0; i < 4; i++) { best.idx[i] = ix[i]; best.pts[i] = pts[i]; } best.score = score; }
  }
  return have;
}

/** Coarse search on a QVGA gray image: the ordered quad. */
bool findQuadCoarse(const uint8_t* gray, double markRatio, uint8_t* mask, uint32_t* integ, Blob* blobs, Quad& q) {
  const int n = findBlobs(gray, markRatio, mask, integ, blobs);
  return bestQuad(blobs, n, q);
}

/** Bilinear gray sample from the VGA frame at (x,y) in VGA pixels (double; used for the 21 ring-test samples only). */
double vgaGray(const uint16_t* vga, double x, double y) {
  x = dclamp(x, 0, VGA_W - 1.001); y = dclamp(y, 0, VGA_H - 1.001);
  const int x0 = int(x), y0 = int(y);
  const double fx = x - x0, fy = y - y0;
  auto g = [&](int xx, int yy) { const unsigned p = vga[yy * VGA_W + xx]; return grayOf(rOf(p), gOf(p), bOf(p)); };
  return (g(x0, y0) * (1 - fx) + g(x0 + 1, y0) * fx) * (1 - fy) + (g(x0, y0 + 1) * (1 - fx) + g(x0 + 1, y0 + 1) * fx) * fy;
}
/** Gray of the VGA pixel at integer (x,y), coordinates clamped to the frame. */
int vgaGrayInt(const uint16_t* vga, int x, int y) {
  const unsigned p = vga[iclamp(y, 0, VGA_H - 1) * VGA_W + iclamp(x, 0, VGA_W - 1)];
  return grayOf(rOf(p), gOf(p), bOf(p));
}

}  // namespace

bool findMarks(const uint16_t* vga, const VisionParams& p, VisionWork& work, Marks& out) {
  MarkScratch& sc = work.scratch.mark;
  grayQvga(vga, sc.gray);
  Blob blobs[MARK_MAX_CANDIDATES]; Quad q;
  if (!findQuadCoarse(sc.gray, p.markRatio, sc.mask, sc.integ, blobs, q)) return false;
  // map the coarse blobs to their ordered points so we keep each blob's size
  double mcx[4], mcy[4], mR[4];
  for (int k = 0; k < 4; k++) {
    const Pt pt = q.pts[k];
    const Blob* bl = &blobs[q.idx[0]];
    for (int j = 1; j < 4; j++) { const Blob* b = &blobs[q.idx[j]]; bl = (len(bl->cx - pt.x, bl->cy - pt.y) < len(b->cx - pt.x, b->cy - pt.y)) ? bl : b; }
    mcx[k] = pt.x * 2 + 1; mcy[k] = pt.y * 2 + 1; mR[k] = std::sqrt(bl->area / PI_) * 2;   // VGA px
  }
  // refine each centre: intensity-weighted centroid of the dark pixels in a 40 x 40 VGA window (integer weights)
  Pt refined[4];
  for (int k = 0; k < 4; k++) {
    int lo = 255, hi = 0; const int x0 = int(jsRound(mcx[k])) - 20, y0 = int(jsRound(mcy[k])) - 20;
    uint8_t win[1600];
    for (int y = 0; y < 40; y++) for (int x = 0; x < 40; x++) { const int g = vgaGrayInt(vga, x0 + x, y0 + y); win[y * 40 + x] = uint8_t(g); if (g < lo) lo = g; if (g > hi) hi = g; }
    const int thr2 = lo + hi; int64_t sw = 0, sx = 0, sy = 0;
    for (int y = 0; y < 40; y++) for (int x = 0; x < 40; x++) { const int w = thr2 - 2 * win[y * 40 + x]; if (w > 0) { sw += w; sx += int64_t(w) * (x0 + x); sy += int64_t(w) * (y0 + y); } }
    if (sw > 0) refined[k] = { double(sx) / double(sw) + 0.5, double(sy) / double(sw) + 0.5 };
    else refined[k] = { mcx[k], mcy[k] };
  }
  // ring test: a bright hole in the middle of the dark disc
  int rings = 0, ringAt = -1;
  for (int i = 0; i < 4; i++) {
    const double cx = refined[i].x, cy = refined[i].y, R = mR[i];
    const double centre = (vgaGray(vga, cx, cy) + vgaGray(vga, cx - 1, cy) + vgaGray(vga, cx + 1, cy) + vgaGray(vga, cx, cy - 1) + vgaGray(vga, cx, cy + 1)) / 5;
    double body = 0, paper = 0;
    for (int k = 0; k < 8; k++) { const double c = COS16[2 * k], s = SIN16[2 * k]; body += vgaGray(vga, cx + 0.72 * R * c, cy + 0.72 * R * s); paper += vgaGray(vga, cx + 2.1 * R * c, cy + 2.1 * R * s); }
    body /= 8; paper /= 8;
    const double score = (centre - body) / dmax(1, paper - body);
    if (score > 0.45) { rings++; if (ringAt < 0) ringAt = i; }
  }
  int tl; bool guess = false;
  if (rings == 1) tl = ringAt;
  else {
    guess = true; tl = 0;
    for (int i = 0; i < 4; i++) if (refined[i].x + refined[i].y < refined[tl].x + refined[tl].y) tl = i;
  }
  for (int k = 0; k < 4; k++) out.pts[k] = refined[(tl + k) % 4];   // TL, TR, BR, BL in page order
  out.orientGuess = guess;
  return true;
}

// =================================================================================================
// Stage 2: straighten (homography + warp)
// =================================================================================================
void solveHomography(const Pt src[4], const Pt dst[4], double h[9]) {
  double A[8][9];
  for (int i = 0; i < 4; i++) {
    const double x = src[i].x, y = src[i].y, u = dst[i].x, v = dst[i].y;
    const double r0[9] = { x, y, 1, 0, 0, 0, -u * x, -u * y, u };
    const double r1[9] = { 0, 0, 0, x, y, 1, -v * x, -v * y, v };
    std::memcpy(A[2 * i], r0, sizeof r0); std::memcpy(A[2 * i + 1], r1, sizeof r1);
  }
  for (int c = 0; c < 8; c++) {
    int piv = c; for (int r = c + 1; r < 8; r++) if (std::fabs(A[r][c]) > std::fabs(A[piv][c])) piv = r;
    if (piv != c) { double t[9]; std::memcpy(t, A[c], sizeof t); std::memcpy(A[c], A[piv], sizeof t); std::memcpy(A[piv], t, sizeof t); }
    for (int r = 0; r < 8; r++) if (r != c) { const double f = A[r][c] / A[c][c]; for (int k = c; k < 9; k++) A[r][k] -= f * A[c][k]; }
  }
  for (int i = 0; i < 8; i++) h[i] = A[i][8] / A[i][i];
  h[8] = 1;
}

bool worldToVga(const Marks& m, int64_t hq[8]) {
  const Pt wp[4] = { { MARK_WORLD_X0, MARK_WORLD_Y0 }, { MARK_WORLD_X1, MARK_WORLD_Y0 }, { MARK_WORLD_X1, MARK_WORLD_Y1 }, { MARK_WORLD_X0, MARK_WORLD_Y1 } };
  double h[9]; solveHomography(wp, m.pts, h);
  for (int i = 0; i < 8; i++) {
    const double v = h[i] * (i < 6 ? 65536.0 : 1073741824.0);
    if (!(std::fabs(v) < 1e12)) return false;
    hq[i] = jsRound(v);
  }
  return true;
}

void centreCropMap(int64_t hq[8]) {
  const int64_t v[8] = { 2 * 65536, 0, 0, 0, 2 * 65536, 48 * 65536, 0, 0 };
  for (int i = 0; i < 8; i++) hq[i] = v[i];
}

void warp(const uint16_t* vga, const int64_t hq[8], uint16_t* out) {
  const int64_t h0 = hq[0], h1 = hq[1], h2 = hq[2], h3 = hq[3], h4 = hq[4], h5 = hq[5], h6 = hq[6], h7 = hq[7];
  const int64_t NUMAX = 8589934592LL;                                  // 2^33: keeps nu * inv below 2^52, like the TypeScript
  const int64_t UMAX8 = int64_t(VGA_W - 2) * 256 + 255, VMAX8 = int64_t(VGA_H - 2) * 256 + 255;
  for (int y = 0; y < WORLD_H; y++) for (int x = 0; x < WORLD_W; x++) {
    int sr = 0, sg = 0, sb = 0;
    for (int oy4 = 1; oy4 <= 3; oy4 += 2) for (int ox4 = 1; ox4 <= 3; ox4 += 2) {
      const int X4 = 4 * x + ox4, Y4 = 4 * y + oy4;
      int64_t dq = (h6 * X4 + h7 * Y4 + 4294967296LL) >> 16;           // d in 16.16, kept in 1/16 .. 32768
      dq = dq < 4096 ? 4096 : dq > 2147483647 ? 2147483647 : dq;
      const int32_t inv = int32_t(2147483647) / int32_t(dq);
      const int64_t nu = lclamp(h0 * X4 + h1 * Y4 + 4 * h2, -NUMAX, NUMAX), nv = lclamp(h3 * X4 + h4 * Y4 + 4 * h5, -NUMAX, NUMAX);
      const int64_t u8 = lclamp(((nu * inv) >> 25) - 128, 0, UMAX8), v8 = lclamp(((nv * inv) >> 25) - 128, 0, VMAX8);   // 1/256 px
      const int x0 = int(u8 >> 8), y0 = int(v8 >> 8), fx = int(u8 & 255), fy = int(v8 & 255);
      const unsigned p00 = vga[y0 * VGA_W + x0], p10 = vga[y0 * VGA_W + x0 + 1], p01 = vga[(y0 + 1) * VGA_W + x0], p11 = vga[(y0 + 1) * VGA_W + x0 + 1];
      const int w00 = (256 - fx) * (256 - fy), w10 = fx * (256 - fy), w01 = (256 - fx) * fy, w11 = fx * fy;   // sum 65536
      sr += rOf(p00) * w00 + rOf(p10) * w10 + rOf(p01) * w01 + rOf(p11) * w11;
      sg += gOf(p00) * w00 + gOf(p10) * w10 + gOf(p01) * w01 + gOf(p11) * w11;
      sb += bOf(p00) * w00 + bOf(p10) * w10 + bOf(p01) * w01 + bOf(p11) * w11;
    }
    out[y * WORLD_W + x] = pack565((sr + 131072) >> 18, (sg + 131072) >> 18, (sb + 131072) >> 18);
  }
}

// =================================================================================================
// Stages 3-5: convert, paper estimate, ink strength, and the colour class
// =================================================================================================
static void analyse(VisionWork& work, const VisionParams& p) {
  const uint16_t* straight = work.straight565;
  uint8_t* gray = work.scratch.analyse.gray; uint8_t* bgp = work.scratch.analyse.bg;
  const int W = WORLD_W, H = WORLD_H;
  for (int i = 0; i < PAGE_N; i++) {
    const unsigned px = straight[i]; const int r = rOf(px), g = gOf(px), b = bOf(px);
    gray[i] = uint8_t(grayOf(r, g, b));
    const int mx = imax(r, imax(g, b)), mn = imin(r, imin(g, b)), d = mx - mn;
    const int sat = mx < 16 ? 0 : fdiv(d * 255, mx);
    int c = 0;
    if (mx >= p.colorVMin && sat >= p.satMin) {
      int h = 0;
      if (d > 0) {
        if (mx == r) h = fdiv(60 * (g - b), d); else if (mx == g) h = 120 + fdiv(60 * (b - r), d); else h = 240 + fdiv(60 * (r - g), d);
        if (h < 0) h += 360;
      }
      if (h >= HUE_RED_LO || h <= HUE_RED_HI) c = 1; else if (h >= HUE_BLUE_LO && h <= HUE_BLUE_HI) c = 2;
    }
    work.cls[i] = uint8_t(c);
  }
  // stage 4: paper estimate = max gray per 16 x 16 block, 3 x 3 blurred (kept x 256), bilinearly upsampled
  constexpr int gw = WORLD_W / BG_BLOCK, gh = WORLD_H / BG_BLOCK;   // 20 x 12
  int grid[gw * gh], blur[gw * gh];
  for (int by = 0; by < gh; by++) for (int bx = 0; bx < gw; bx++) {
    int m = 0; for (int y = 0; y < BG_BLOCK; y++) for (int x = 0; x < BG_BLOCK; x++) m = imax(m, gray[(by * BG_BLOCK + y) * W + bx * BG_BLOCK + x]);
    grid[by * gw + bx] = m;
  }
  int sum = 0;
  for (int by = 0; by < gh; by++) for (int bx = 0; bx < gw; bx++) {
    int s = 0, n = 0;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { const int X = bx + dx, Y = by + dy; if (X >= 0 && Y >= 0 && X < gw && Y < gh) { s += grid[Y * gw + X]; n++; } }
    const int v = fdiv(s * 256 + (n >> 1), n);                        // mean x 256, rounded
    blur[by * gw + bx] = v; sum += v;
  }
  work.bgMean = double(sum) / (gw * gh * 256);
  for (int y = 0; y < H; y++) {
    const int fy = iclamp(2 * y - 15, 0, (gh - 1) * 32 - 1), y0 = fy >> 5, ty = fy & 31;   // (y + 0.5) / 16 - 0.5 in 1/32 grid cells
    for (int x = 0; x < W; x++) {
      const int fx = iclamp(2 * x - 15, 0, (gw - 1) * 32 - 1), x0 = fx >> 5, tx = fx & 31;
      const int v = (blur[y0 * gw + x0] * (32 - tx) + blur[y0 * gw + x0 + 1] * tx) * (32 - ty) + (blur[(y0 + 1) * gw + x0] * (32 - tx) + blur[(y0 + 1) * gw + x0 + 1] * tx) * ty;
      bgp[y * W + x] = uint8_t((v + 131072) >> 18);
    }
  }
  // stage 5: ink strength (independent of the threshold)
  for (int i = 0; i < PAGE_N; i++) {
    const int bg = bgp[i], g = gray[i];
    work.ink[i] = (bg >= p.bgMin && g < bg) ? uint8_t(imin(255, fdiv(255 * (bg - g), bg))) : 0;
  }
}

// =================================================================================================
// Stage 7: clean up (despeckle, close, thicken, border, priority)
// =================================================================================================
/** Flood fill (4-connected) over pixels equal to `from`, setting them to `to`; returns the component size. */
static int flood(uint8_t* mask, int s, uint8_t from, uint8_t to, uint16_t* stack) {
  const int W = WORLD_W, H = WORLD_H;
  int sp = 0, n = 0; stack[sp++] = uint16_t(s); mask[s] = to;
  while (sp) {
    const int q = stack[--sp]; n++; const int x = q % W, y = q / W;
    if (x > 0 && mask[q - 1] == from) { mask[q - 1] = to; stack[sp++] = uint16_t(q - 1); }
    if (x < W - 1 && mask[q + 1] == from) { mask[q + 1] = to; stack[sp++] = uint16_t(q + 1); }
    if (y > 0 && mask[q - W] == from) { mask[q - W] = to; stack[sp++] = uint16_t(q - W); }
    if (y < H - 1 && mask[q + W] == from) { mask[q + W] = to; stack[sp++] = uint16_t(q + W); }
  }
  return n;
}
/** Remove 4-connected components smaller than `min` pixels from a 0/1 mask. Returns the number kept. */
static int despeckle(uint8_t* mask, int min, uint16_t* stack) {
  int kept = 0;
  for (int s = 0; s < PAGE_N; s++) {
    if (mask[s] != 1) continue;
    const int n = flood(mask, s, 1, 2, stack);                        // 2 = visited
    if (n < min) flood(mask, s, 2, 0, stack); else kept++;            // too small: erase it; else leave it marked 2
  }
  for (int i = 0; i < PAGE_N; i++) if (mask[i] == 2) mask[i] = 1;
  return kept;
}
/** In place: separable square dilate (outside = 0) or erode (outside = 1) of radius r, with a sliding window count. */
static void morph(uint8_t* buf, uint8_t* tmp, int r, bool dilate) {
  const int W = WORLD_W, H = WORLD_H;
  if (r <= 0) return;
  const uint8_t on = dilate ? 1 : 0;                                  // the value that "counts" (erode counts zeros)
  for (int y = 0; y < H; y++) {                                       // rows: buf -> tmp
    const int row = y * W; int cnt = 0;
    for (int x = 0; x < r && x < W; x++) if (buf[row + x] == on) cnt++;
    for (int x = 0; x < W; x++) {
      if (x + r < W && buf[row + x + r] == on) cnt++;
      if (x - r - 1 >= 0 && buf[row + x - r - 1] == on) cnt--;
      tmp[row + x] = dilate ? (cnt > 0 ? 1 : 0) : (cnt > 0 ? 0 : 1);
    }
  }
  uint8_t col[WORLD_W]; std::memset(col, 0, sizeof col);              // columns: tmp -> buf, one counter per column
  for (int y = 0; y < r && y < H; y++) for (int x = 0; x < W; x++) if (tmp[y * W + x] == on) col[x]++;
  for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
    if (y + r < H && tmp[(y + r) * W + x] == on) col[x]++;
    if (y - r - 1 >= 0 && tmp[(y - r - 1) * W + x] == on) col[x]--;
    buf[y * W + x] = dilate ? (col[x] > 0 ? 1 : 0) : (col[x] > 0 ? 0 : 1);
  }
}

/** Stages 6 (labels from ink + class) and 7. Fills work.labels. */
static void labelAndClean(VisionWork& work, const VisionParams& p, double& cover, int& blobsOut) {
  const int W = WORLD_W, H = WORLD_H;
  MorphScratch& m = work.scratch.morph;
  uint8_t* A = m.a; uint8_t* B = m.b; uint8_t* T = m.t; uint16_t* stack = m.stack;
  uint8_t* bits = work.labels; std::memset(bits, 0, PAGE_N);          // bit k = class k survived (0 ground, 1 spike, 2 bouncy)
  int blobs = 0, covered = 0;
  for (int k = 0; k < 3; k++) {
    for (int i = 0; i < PAGE_N; i++) A[i] = (work.ink[i] >= p.inkThresh && work.cls[i] == k) ? 1 : 0;
    blobs += despeckle(A, p.minBlob, stack);
    morph(A, T, p.closeR, true); morph(A, T, p.closeR, false);        // close: joins streaky wax gaps
    for (int i = 0; i < PAGE_N; i++) covered += A[i];
    // thicken THIN strokes only (see vision.ts)
    std::memcpy(B, A, PAGE_N);
    morph(B, T, THIN_OPEN_R, false); morph(B, T, THIN_OPEN_R, true);  // B = opened
    for (int i = 0; i < PAGE_N; i++) B[i] = uint8_t(A[i] & (B[i] ^ 1));   // B = thin = closed and not opened
    morph(B, T, p.thickenR, true);                                    // B = grown
    for (int i = 0; i < PAGE_N; i++) if (A[i] | B[i]) bits[i] |= uint8_t(1 << k);
  }
  for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
    const int i = y * W + x; const bool edge = x < p.edgeMargin || y < p.edgeMargin || x >= W - p.edgeMargin || y >= H - p.edgeMargin;
    const int b = bits[i];
    bits[i] = uint8_t(edge ? 0 : b & 2 ? 2 : b & 4 ? 3 : b & 1 ? 1 : 0);   // priority spike > bouncy > ground
  }
  cover = double(covered) / PAGE_N; blobsOut = blobs;
}

// =================================================================================================
// Stages 8-10: quality, pins, pack
// =================================================================================================
static int topSolid(const uint8_t* labels, int x) {
  for (int y = 0; y < WORLD_H; y++) { const int l = labels[y * WORLD_W + x]; if (l == 1 || l == 3) return y; }
  return -1;
}
static bool supported(const uint8_t* labels, int x, int yTop) {
  for (int dx = -6; dx <= 6; dx += 12) { const int t = topSolid(labels, iclamp(x + dx, 0, WORLD_W - 1)); if (!(t >= 0 && std::abs(t - yTop) <= 8)) return false; }
  return true;
}
static bool clearAbove(const uint8_t* labels, int x, int yTop) {
  for (int y = imax(0, yTop - 24); y < yTop; y++) if (labels[y * WORLD_W + x] == 2) return false;
  return yTop >= 24;
}
static void placePins(const uint8_t* labels, int& sx, int& sy, int& fx, int& fy) {
  sx = 32; sy = 32; fx = 288; fy = 100;
  for (int x = 16; x <= 120; x++) { const int t = topSolid(labels, x); if (t >= 0 && clearAbove(labels, x, t) && supported(labels, x, t)) { sx = x; sy = t - BALL_RADIUS - 1; break; } }
  for (int x = 304; x >= 200; x--) { const int t = topSolid(labels, x); if (t >= 0 && clearAbove(labels, x, t) && supported(labels, x, t)) { fx = x; fy = t; break; } }
  const int dx = fx - sx, dy = fy - sy;
  if (dx * dx + dy * dy < 100 * 100) fx = 304;
}

static void finish(VisionWork& work, const VisionParams& p, bool marksFound, bool orientGuess, Level& level, VisionReport& report) {
  double cover; int blobs;
  labelAndClean(work, p, cover, blobs);
  int status = VQ_OK;
  if (work.bgMean < Q_TOO_DARK) status = VQ_TOO_DARK;
  else if (cover < Q_EMPTY_COVER) status = VQ_EMPTY;
  else if (cover > Q_MUCH_COVER) status = VQ_TOO_MUCH_INK;
  else if (blobs > Q_NOISY_BLOBS) status = VQ_NOISY;
  else if (!marksFound) status = VQ_MARKS_MISSING;
  else if (orientGuess) status = VQ_ORIENT_GUESS;
  levelInit(level);
  level.fromPaper = true;
  for (int y = 0; y < WORLD_H; y++) for (int x = 0; x < WORLD_W; x++) {
    const int l = work.labels[y * WORLD_W + x];
    if (l == 1) setBit(level.ground, x, y); else if (l == 2) setBit(level.spike, x, y); else if (l == 3) setBit(level.bouncy, x, y);
  }
  placePins(work.labels, level.startX, level.startY, level.flagX, level.flagY);
  clearAround(level, level.startX, level.startY, START_SAFE_RADIUS);
  clearAround(level, level.flagX, level.flagY - FLAG_CENTER_DY, FLAG_SAFE_RADIUS);
  report.status = status; report.marksFound = marksFound; report.orientGuess = orientGuess;
  report.inkCover = cover; report.bgMean = work.bgMean; report.blobs = blobs;
}

void visionProcessOnce(const uint16_t* vga, const VisionParams& p, VisionWork& work, Level& level, VisionReport& report) {
  Marks marks;
  bool found = findMarks(vga, p, work, marks);
  int64_t hq[8];
  if (!(found && worldToVga(marks, hq))) { found = false; centreCropMap(hq); }   // marks missing (or a degenerate quad): centre-crop
  work.marksFound = found;
  if (found) work.marks = marks; else std::memset(&work.marks, 0, sizeof work.marks);
  warp(vga, hq, work.straight565);
  analyse(work, p);
  finish(work, p, found, found && marks.orientGuess, level, report);
}

void visionRethreshold(VisionWork& work, const VisionParams& p, Level& level, VisionReport& report) {
  finish(work, p, work.marksFound, work.marksFound && work.marks.orientGuess, level, report);
}

void visionProcess(const uint16_t* vga, const VisionParams& p, VisionWork& work, Level& level, VisionReport& report, VisionParams* used) {
  VisionParams usedP = p;
  visionProcessOnce(vga, p, work, level, report);
  if (report.status == VQ_EMPTY || report.status == VQ_TOO_MUCH_INK) {
    const double factor = report.status == VQ_EMPTY ? 0.75 : 1.25;
    VisionParams p2 = p; p2.inkThresh = int(jsRound(p.inkThresh * factor));
    VisionReport r2;
    visionRethreshold(work, p2, level, r2);
    if (r2.status == VQ_OK || r2.status == VQ_MARKS_MISSING || r2.status == VQ_ORIENT_GUESS) { report = r2; usedP = p2; }
    else visionRethreshold(work, p, level, report);                    // keep the first result: same labels and level again
  }
  if (used) *used = usedP;
}

// =================================================================================================
// The live Camera check
// =================================================================================================
static double bil(const uint8_t* gray, double x, double y) {
  x = dclamp(x, 0, QVGA_W - 1.001); y = dclamp(y, 0, QVGA_H - 1.001);
  const int x0 = int(x), y0 = int(y);
  const double fx = x - x0, fy = y - y0;
  auto g = [&](int X, int Y) { return int(gray[Y * QVGA_W + X]); };
  return (g(x0, y0) * (1 - fx) + g(x0 + 1, y0) * fx) * (1 - fy) + (g(x0, y0 + 1) * (1 - fx) + g(x0 + 1, y0 + 1) * fx) * fy;
}

void visionLiveCheck(const uint8_t* gray, bool accelUpright, VisionWork& work, LiveCheck& out) {
  out = LiveCheck{};
  out.marks = false; out.edgePx = 99; out.ratioH = 1; out.ratioV = 1;
  MarkScratch& sc = work.scratch.mark;
  Blob blobs[MARK_MAX_CANDIDATES]; Quad q;
  if (!findQuadCoarse(gray, MARK_RATIO, sc.mask, sc.integ, blobs, q)) return;
  const Pt* pts = q.pts; double s[4];
  for (int i = 0; i < 4; i++) s[i] = len(pts[(i + 1) % 4].x - pts[i].x, pts[(i + 1) % 4].y - pts[i].y);
  const double l1 = (s[0] + s[2]) / 2, l2 = (s[1] + s[3]) / 2;
  const double longA = l1 >= l2 ? s[0] : s[1], longB = l1 >= l2 ? s[2] : s[3], shortA = l1 >= l2 ? s[1] : s[0], shortB = l1 >= l2 ? s[3] : s[2];
  const double wPx = dmax(l1, l2);
  const double distCm = (TPL_MARK_RECT_W_MM * CAM_FOCAL_PX_320) / wPx / 10;
  const double ratioH = dmax(longA, longB) / std::fmin(longA, longB), ratioV = dmax(shortA, shortB) / std::fmin(shortA, shortB);

  // sharpness: 10-90 % edge width across each mark's boundary, 16 radial profiles each
  double edgeSum = 0, markDark = 0; int edgeN = 0;
  for (int bi = 0; bi < 4; bi++) {
    const Blob& bl = blobs[q.idx[bi]];
    const double R = std::sqrt(bl.area / PI_);
    double dark = 0; for (int k = 0; k < 8; k++) dark += bil(gray, bl.cx + 0.7 * R * COS16[2 * k], bl.cy + 0.7 * R * SIN16[2 * k]); markDark += dark / 8;
    for (int k = 0; k < 16; k++) {
      const double c = COS16[k], sn = SIN16[k];
      double prof[160]; int np = 0;
      for (double t = 0; t <= 3 * R; t += 0.25) prof[np++] = bil(gray, bl.cx + t * c, bl.cy + t * sn);
      const int i07 = int(jsRound(0.7 * R / 0.25)), i06 = int(jsRound(0.6 * R / 0.25)), i2 = int(jsRound(2 * R / 0.25));
      const double lo = (prof[i07] + prof[i06]) / 2;
      double hi = 0; for (int i = i2; i < np; i++) hi = dmax(hi, prof[i]);
      if (hi - lo < 40) continue;
      const double t10 = lo + 0.1 * (hi - lo), t90 = lo + 0.9 * (hi - lo);
      double a10 = -1, a90 = -1;
      for (int i = i07; i < np; i++) { if (a10 < 0 && prof[i] >= t10) a10 = i * 0.25; if (a90 < 0 && prof[i] >= t90) { a90 = i * 0.25; break; } }
      if (a10 >= 0 && a90 >= a10) { edgeSum += a90 - a10; edgeN++; }
    }
  }
  const double edgePx = edgeN ? edgeSum / edgeN : 99;
  markDark /= 4;

  // light: the paper between the marks (80th percentile of a 12 x 8 grid inside the quad)
  const Pt p0 = pts[0], p1 = pts[1], p2 = pts[2], p3 = pts[3];
  double samples[96]; int ns = 0, clip = 0;
  for (int j = 0; j < 8; j++) for (int i = 0; i < 12; i++) {
    const double u = 0.08 + (0.84 * i) / 11, v = 0.08 + (0.84 * j) / 7;
    const double x = (p0.x * (1 - u) + p1.x * u) * (1 - v) + (p3.x * (1 - u) + p2.x * u) * v, y = (p0.y * (1 - u) + p1.y * u) * (1 - v) + (p3.y * (1 - u) + p2.y * u) * v;
    const double g = bil(gray, x, y); samples[ns++] = g; if (g >= 250) clip++;
  }
  for (int i = 1; i < ns; i++) for (int j = i; j > 0 && samples[j - 1] > samples[j]; j--) { const double t = samples[j]; samples[j] = samples[j - 1]; samples[j - 1] = t; }
  const double paperMean = samples[int(std::floor(ns * 0.8))], clipFrac = double(clip) / ns, contrast = paperMean - markDark;

  bool inFrame = true;
  for (int i = 0; i < 4; i++) if (!(pts[i].x >= 8 && pts[i].y >= 8 && pts[i].x <= QVGA_W - 8 && pts[i].y <= QVGA_H - 8)) inFrame = false;
  int dLight, cue = CUE_NONE;
  if (wPx >= DIST_GREEN_MIN_PX && wPx <= DIST_GREEN_MAX_PX && inFrame) dLight = LIGHT_GREEN;
  else {
    dLight = wPx >= DIST_GREEN_MIN_PX * 0.85 && wPx <= DIST_GREEN_MAX_PX * 1.12 ? LIGHT_YELLOW : LIGHT_RED;
    cue = wPx < DIST_GREEN_MIN_PX ? CUE_CLOSER : CUE_BACK;
  }
  const int sLight = edgePx <= SHARP_GREEN_MAX_PX ? LIGHT_GREEN : edgePx <= SHARP_YELLOW_MAX_PX ? LIGHT_YELLOW : LIGHT_RED;
  const int lLight = (paperMean >= LIGHT_MEAN_MIN && paperMean <= LIGHT_MEAN_MAX && clipFrac < LIGHT_CLIP_MAX && contrast >= LIGHT_CONTRAST_MIN) ? LIGHT_GREEN
    : (paperMean >= 100 && paperMean <= 252 && contrast >= 60) ? LIGHT_YELLOW : LIGHT_RED;
  const double worst = dmax(ratioH, ratioV);
  const int tLight = !accelUpright ? LIGHT_RED : worst <= LEVEL_RATIO_MAX ? LIGHT_GREEN : worst <= LEVEL_WARN_MAX ? LIGHT_YELLOW : LIGHT_RED;
  if (cue == CUE_NONE && tLight != LIGHT_GREEN) cue = CUE_TILT;

  out.marks = true;
  for (int i = 0; i < 4; i++) out.quad[i] = pts[i];
  out.distCm = distCm; out.edgePx = edgePx; out.paperMean = paperMean; out.clipFrac = clipFrac; out.contrast = contrast;
  out.ratioH = ratioH; out.ratioV = ratioV;
  out.lights[0] = dLight; out.lights[1] = sLight; out.lights[2] = lLight; out.lights[3] = tLight;
  out.cue = cue;
  out.allGreen = dLight == LIGHT_GREEN && sLight == LIGHT_GREEN && lLight == LIGHT_GREEN && tLight == LIGHT_GREEN;
}

double levelLayerIoU(const uint8_t* a, const uint8_t* b) {
  int inter = 0, uni = 0;
  for (int y = 0; y < WORLD_H; y++) for (int x = 0; x < WORLD_W; x++) { const int A = getBit(a, x, y), B = getBit(b, x, y); inter += A & B; uni += A | B; }
  return uni ? double(inter) / uni : 1;
}

}  // namespace scribble
