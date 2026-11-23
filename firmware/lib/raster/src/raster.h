// raster.h - tiny anti-aliased shape rasteriser (portable: no M5GFX, no Arduino, header only).
//
// Every scan function walks the pixels a shape touches and calls  put(x, y, coverage)  with coverage 0..255 (255 = the
// pixel centre is fully inside). The Renderer blends those pixels into the frame sprite; the icon cache writes them into
// a Mask (an 8-bit alpha picture). Shapes are given in pixel units, (0,0) = top-left corner of pixel (0,0), so a pixel's
// centre is at (x + 0.5, y + 0.5). Angles follow the HTML canvas: 0 = +x, increasing clockwise on screen (y points down).
// Native unit tests: test/test_raster.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace scribble {
namespace raster {

constexpr float PI_F = 3.14159265358979f;
constexpr float TWO_PI_F = 6.28318530717959f;

inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline int cov255(float c) { return int(clamp01(c) * 255.0f + 0.5f); }
inline int ifloor(float v) { int i = int(v); return (float(i) > v) ? i - 1 : i; }
inline int iceil(float v) { int i = int(v); return (float(i) < v) ? i + 1 : i; }
inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }

/** A clip rectangle [x0, x1) x [y0, y1). */
struct Clip { int x0, y0, x1, y1; };

/** Distance from point p to the segment a-b. */
inline float segDist(float px, float py, float ax, float ay, float bx, float by) {
  const float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
  float t = l2 > 1e-9f ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0.0f;
  t = t < 0 ? 0 : (t > 1 ? 1 : t);
  const float qx = ax + t * dx - px, qy = ay + t * dy - py;
  return std::sqrt(qx * qx + qy * qy);
}

/** A round-capped line of the given width. */
template <class F> void scanCapsule(const Clip& c, float x0, float y0, float x1, float y1, float width, F&& put) {
  const float hw = width * 0.5f, m = hw + 1.0f;
  const float ylo = (y0 < y1 ? y0 : y1) - m, yhi = (y0 < y1 ? y1 : y0) + m;
  const int ya = imax(c.y0, ifloor(ylo)), yb = imin(c.y1, iceil(yhi));
  const float dy = y1 - y0;
  for (int y = ya; y < yb; y++) {
    const float yc = y + 0.5f;
    float t0 = 0, t1 = 1;
    if (dy > 1e-6f || dy < -1e-6f) {
      float u = (yc - m - y0) / dy, v = (yc + m - y0) / dy;
      if (u > v) { const float s = u; u = v; v = s; }
      t0 = u < 0 ? 0 : u; t1 = v > 1 ? 1 : v;
      if (t0 > t1) continue;
    } else if (yc < y0 - m || yc > y0 + m) continue;
    const float xa = x0 + (x1 - x0) * t0, xb = x0 + (x1 - x0) * t1;
    const int xs = imax(c.x0, ifloor((xa < xb ? xa : xb) - m)), xe = imin(c.x1, iceil((xa < xb ? xb : xa) + m));
    for (int x = xs; x < xe; x++) {
      const int a = cov255(hw + 0.5f - segDist(x + 0.5f, yc, x0, y0, x1, y1));
      if (a > 0) put(x, y, a);
    }
  }
}

/** A filled circle. */
template <class F> void scanDisc(const Clip& c, float cx, float cy, float r, F&& put) {
  const int ya = imax(c.y0, ifloor(cy - r - 1)), yb = imin(c.y1, iceil(cy + r + 1));
  for (int y = ya; y < yb; y++) {
    const float dy = y + 0.5f - cy, rr = r + 1.0f, h2 = rr * rr - dy * dy;
    if (h2 <= 0) continue;
    const float h = std::sqrt(h2);
    const int xs = imax(c.x0, ifloor(cx - h)), xe = imin(c.x1, iceil(cx + h));
    for (int x = xs; x < xe; x++) {
      const float dx = x + 0.5f - cx;
      const int a = cov255(r + 0.5f - std::sqrt(dx * dx + dy * dy));
      if (a > 0) put(x, y, a);
    }
  }
}

/** A circle outline of line width `width` (centred on radius r), optionally only the arc a0 .. a0+sweep, with round caps. */
template <class F> void scanArc(const Clip& c, float cx, float cy, float r, float width, float a0, float sweep, bool roundCaps, F&& put) {
  const float hw = width * 0.5f, outer = r + hw + 1.0f;
  const bool full = sweep >= TWO_PI_F - 1e-4f;
  if (!full && sweep <= 0) return;
  const float ex0 = cx + r * std::cos(a0), ey0 = cy + r * std::sin(a0);
  const float ex1 = cx + r * std::cos(a0 + sweep), ey1 = cy + r * std::sin(a0 + sweep);
  const int ya = imax(c.y0, ifloor(cy - outer)), yb = imin(c.y1, iceil(cy + outer));
  for (int y = ya; y < yb; y++) {
    const float dy = y + 0.5f - cy, h2 = outer * outer - dy * dy;
    if (h2 <= 0) continue;
    const float h = std::sqrt(h2);
    const int xs = imax(c.x0, ifloor(cx - h)), xe = imin(c.x1, iceil(cx + h));
    for (int x = xs; x < xe; x++) {
      const float dx = x + 0.5f - cx, d = std::sqrt(dx * dx + dy * dy);
      float a = 0;
      const float band = hw + 0.5f - std::fabs(d - r);
      if (band > 0) {
        bool inside = full;
        if (!full) {
          float rel = std::atan2(dy, dx) - a0;
          rel = rel - TWO_PI_F * std::floor(rel / TWO_PI_F);
          inside = rel <= sweep;
        }
        if (inside) a = clamp01(band);
      }
      if (!full && roundCaps) {
        const float c0 = hw + 0.5f - std::sqrt((x + 0.5f - ex0) * (x + 0.5f - ex0) + (y + 0.5f - ey0) * (y + 0.5f - ey0));
        const float c1 = hw + 0.5f - std::sqrt((x + 0.5f - ex1) * (x + 0.5f - ex1) + (y + 0.5f - ey1) * (y + 0.5f - ey1));
        if (c0 > a) a = clamp01(c0);
        if (c1 > a) a = clamp01(c1);
      }
      const int ai = cov255(a);
      if (ai > 0) put(x, y, ai);
    }
  }
}

/** A filled polygon (even-odd rule), xy = x0,y0,x1,y1,...; 4 sub-scanlines per pixel row, exact horizontal coverage. */
template <class F> void scanPolygon(const Clip& c, const float* xy, int n, F&& put) {
  if (n < 3) return;
  float ymin = xy[1], ymax = xy[1];
  for (int i = 1; i < n; i++) { const float y = xy[2 * i + 1]; if (y < ymin) ymin = y; if (y > ymax) ymax = y; }
  const int ya = imax(c.y0, ifloor(ymin)), yb = imin(c.y1, iceil(ymax));
  constexpr int SUB = 4;
  const int W = c.x1 - c.x0;
  if (W <= 0 || W > 512) return;
  uint16_t acc[512];
  for (int y = ya; y < yb; y++) {
    std::memset(acc, 0, sizeof(uint16_t) * W);
    bool any = false;
    for (int s = 0; s < SUB; s++) {
      const float yy = y + (s + 0.5f) / SUB;
      float xs[32]; int k = 0;
      for (int i = 0, j = n - 1; i < n; j = i++) {
        const float y0 = xy[2 * j + 1], y1 = xy[2 * i + 1];
        if ((y0 <= yy && yy < y1) || (y1 <= yy && yy < y0)) {
          if (k < 32) xs[k++] = xy[2 * j] + (yy - y0) * (xy[2 * i] - xy[2 * j]) / (y1 - y0);
        }
      }
      for (int a = 1; a < k; a++) { const float v = xs[a]; int b = a - 1; while (b >= 0 && xs[b] > v) { xs[b + 1] = xs[b]; b--; } xs[b + 1] = v; }
      for (int a = 0; a + 1 < k; a += 2) {
        float xa = xs[a], xb = xs[a + 1];
        if (xb <= c.x0 || xa >= c.x1) continue;
        if (xa < c.x0) xa = float(c.x0);
        if (xb > c.x1) xb = float(c.x1);
        if (xb <= xa) continue;
        const int ia = ifloor(xa), ib = ifloor(xb);
        const float K = 256.0f / SUB;
        if (ia == ib) acc[ia - c.x0] += uint16_t((xb - xa) * K);
        else {
          acc[ia - c.x0] += uint16_t((ia + 1 - xa) * K);
          for (int i = ia + 1; i < ib; i++) acc[i - c.x0] += uint16_t(K);
          if (ib < c.x1) acc[ib - c.x0] += uint16_t((xb - ib) * K);
        }
        any = true;
      }
    }
    if (!any) continue;
    for (int i = 0; i < W; i++) if (acc[i]) put(c.x0 + i, y, acc[i] > 255 ? 255 : int(acc[i]));
  }
}

/** A polyline of round-capped segments (a closed outline when `closed`). */
template <class F> void scanPolyline(const Clip& c, const float* xy, int n, float width, bool closed, F&& put) {
  for (int i = 0; i + 1 < n; i++) scanCapsule(c, xy[2 * i], xy[2 * i + 1], xy[2 * i + 2], xy[2 * i + 3], width, put);
  if (closed && n > 2) scanCapsule(c, xy[2 * n - 2], xy[2 * n - 1], xy[0], xy[1], width, put);
}

/** An 8-bit alpha picture. Shapes are combined with "max" so overlapping strokes do not double up at the edges. */
struct Mask {
  uint8_t* a = nullptr; int w = 0, h = 0;
  Clip clip() const { return Clip{0, 0, w, h}; }
  void clear() { if (a) std::memset(a, 0, size_t(w) * h); }
  void put(int x, int y, int v) { uint8_t& p = a[y * w + x]; if (v > p) p = uint8_t(v); }
  void capsule(float x0, float y0, float x1, float y1, float width) { scanCapsule(clip(), x0, y0, x1, y1, width, [this](int x, int y, int v) { put(x, y, v); }); }
  void disc(float cx, float cy, float r) { scanDisc(clip(), cx, cy, r, [this](int x, int y, int v) { put(x, y, v); }); }
  void arc(float cx, float cy, float r, float width, float a0, float sweep, bool caps = true) { scanArc(clip(), cx, cy, r, width, a0, sweep, caps, [this](int x, int y, int v) { put(x, y, v); }); }
  void polygon(const float* xy, int n) { scanPolygon(clip(), xy, n, [this](int x, int y, int v) { put(x, y, v); }); }
  void polyline(const float* xy, int n, float width, bool closed) { scanPolyline(clip(), xy, n, width, closed, [this](int x, int y, int v) { put(x, y, v); }); }
  long sum() const { long s = 0; for (int i = 0; i < w * h; i++) s += a[i]; return s; }
};

/** Signed distance from point (px,py) to the outline of a rounded rectangle (negative inside). */
inline float roundRectSd(float px, float py, float x, float y, float w, float h, float r) {
  const float cx = x + w * 0.5f, cy = y + h * 0.5f, hx = w * 0.5f - r, hy = h * 0.5f - r;
  const float qx = std::fabs(px - cx) - hx, qy = std::fabs(py - cy) - hy;
  const float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
  const float inner = (qx > qy ? qx : qy);
  return std::sqrt(ox * ox + oy * oy) + (inner < 0 ? inner : 0) - r;
}

}  // namespace raster
}  // namespace scribble
