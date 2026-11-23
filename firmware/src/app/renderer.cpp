// renderer.cpp - the 320 x 240 frame sprite and everything drawn into it.   Port of sim/src/renderer.ts.
#include "renderer.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include "raster.h"

namespace scribble {

namespace {

constexpr int SW = SCREEN_W, SH = SCREEN_H;
constexpr raster::Clip SCREEN_CLIP{0, 0, SW, SH};

constexpr uint16_t rgb565(int r, int g, int b) { return uint16_t(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }
constexpr uint16_t sw16(uint16_t c) { return uint16_t((c << 8) | (c >> 8)); }

/** Blend src over dst (both RGB565, host order); a = 0..255. 32 alpha levels, the classic two-lane trick. */
inline uint16_t mix565(uint16_t dst, uint16_t src, int a) {
  const uint32_t k = uint32_t(a + 1) >> 3;                       // 0..32
  const uint32_t S = (src | (uint32_t(src) << 16)) & 0x07E0F81Fu;
  const uint32_t D = (dst | (uint32_t(dst) << 16)) & 0x07E0F81Fu;
  uint32_t R = (S * k + D * (32 - k)) >> 5;
  R &= 0x07E0F81Fu;
  return uint16_t(R | (R >> 16));
}

constexpr uint16_t C_LRED = rgb565(255, 190, 184);   // the light ticks on spikes

}  // namespace

// =====================================================================================================================
// setup, background, present
// =====================================================================================================================
bool Renderer::begin() {
  frame_.setColorDepth(16); frame_.setPsram(true);
  paper_.setColorDepth(16); paper_.setPsram(true);
  if (!frame_.createSprite(SW, SH) || !paper_.createSprite(SW, SH)) {
    std::printf("renderer: out of memory for the frame sprites\n");
    return false;
  }
  fb_ = static_cast<uint16_t*>(frame_.getBuffer());
  paperFb_ = static_cast<uint16_t*>(paper_.getBuffer());
  // the pre-rendered paper: cream with a faint dot every 16 px
  paper_.fillSprite(PAL_PAPER);
  for (int y = 8; y < SH; y += 16) for (int x = 8; x < SW; x += 16) paper_.fillRect(x - 1, y - 1, 2, 2, PAL_PAPER_DOT);
  // M5GFX keeps 16-bit sprites byte-swapped (high byte first); make sure, because drawLayers() and friends write the buffer directly
  frame_.drawPixel(0, 0, 0x1234);
  if (fb_[0] != 0x3412) std::printf("renderer: WARNING frame buffer is not byte-swapped (0x%04X); direct pixel writes will have wrong colours\n", fb_[0]);
  frame_.fillSprite(PAL_PAPER);
  return true;
}

void Renderer::clearPaper() { std::memcpy(fb_, paperFb_, size_t(SW) * SH * 2); }
void Renderer::clear(uint16_t color) { frame_.fillSprite(color); }

void Renderer::present() {
  const int ox = int(std::lround(shakeX)), oy = int(std::lround(shakeY));
  if (ox == 0 && oy == 0) { frame_.pushSprite(&M5.Display, 0, 0); return; }
  M5.Display.startWrite();
  // the strips the shifted frame leaves uncovered
  if (ox > 0) M5.Display.fillRect(0, 0, ox, SH, PAL_PAPER); else if (ox < 0) M5.Display.fillRect(SW + ox, 0, -ox, SH, PAL_PAPER);
  if (oy > 0) M5.Display.fillRect(0, 0, SW, oy, PAL_PAPER); else if (oy < 0) M5.Display.fillRect(0, SH + oy, SW, -oy, PAL_PAPER);
  frame_.pushSprite(&M5.Display, ox, oy);
  M5.Display.endWrite();
}

// =====================================================================================================================
// pixel helpers
// =====================================================================================================================
void Renderer::blendPx(int x, int y, uint16_t color, uint8_t alpha) {
  if (x < 0 || y < 0 || x >= SW || y >= SH || alpha == 0) return;
  uint16_t& p = fb_[y * SW + x];
  p = alpha == 255 ? sw16(color) : sw16(mix565(sw16(p), color, alpha));
}

void Renderer::spanBlend(int y, int xa, int xb, uint16_t color, int a) {   // [xa, xb) on row y
  if (y < 0 || y >= SH || a <= 0) return;
  if (xa < 0) xa = 0;
  if (xb > SW) xb = SW;
  uint16_t* p = fb_ + y * SW;
  if (a >= 255) { const uint16_t s = sw16(color); for (int x = xa; x < xb; x++) p[x] = s; }
  else for (int x = xa; x < xb; x++) p[x] = sw16(mix565(sw16(p[x]), color, a));
}

void Renderer::fillRectA(int x, int y, int w, int h, uint16_t color, uint8_t alpha) {
  if (alpha == 0) return;
  const int x1 = x + w, y1 = y + h;
  for (int yy = y < 0 ? 0 : y; yy < y1 && yy < SH; yy++) spanBlend(yy, x, x1, color, alpha);
}

void Renderer::blendMask(const uint8_t* a, int x, int y, int w, int h, uint16_t color, uint8_t alpha) {
  const uint16_t s = sw16(color);
  for (int j = 0; j < h; j++) {
    const int yy = y + j;
    if (yy < 0 || yy >= SH) continue;
    const uint8_t* row = a + j * w;
    uint16_t* p = fb_ + yy * SW;
    for (int i = 0; i < w; i++) {
      int c = row[i];
      if (!c) continue;
      const int xx = x + i;
      if (xx < 0 || xx >= SW) continue;
      if (alpha != 255) c = (c * alpha + 127) / 255;
      p[xx] = c >= 255 ? s : sw16(mix565(sw16(p[xx]), color, c));
    }
  }
}

// ---- anti-aliased shapes ---------------------------------------------------------------------------------------------
void Renderer::disc(float cx, float cy, float r, uint16_t color, uint8_t alpha) {
  raster::scanDisc(SCREEN_CLIP, cx, cy, r, [&](int x, int y, int c) { blendPx(x, y, color, alpha == 255 ? c : (c * alpha) / 255); });
}
void Renderer::ring(float cx, float cy, float r, float width, uint16_t color, float a0, float sweep, uint8_t alpha) {
  raster::scanArc(SCREEN_CLIP, cx, cy, r, width, a0, sweep, true, [&](int x, int y, int c) { blendPx(x, y, color, alpha == 255 ? c : (c * alpha) / 255); });
}
void Renderer::dashedRing(float cx, float cy, float r, float width, uint16_t color, float dashOn, float dashOff) {
  const float period = dashOn + dashOff, circ = raster::TWO_PI_F * r;
  const int n = int(circ / period + 0.5f);
  if (n < 1) return;
  const float step = raster::TWO_PI_F / n, on = step * dashOn / period;
  for (int i = 0; i < n; i++) {
    const float a = i * step, b = a + on;
    line(cx + r * std::cos(a), cy + r * std::sin(a), cx + r * std::cos(b), cy + r * std::sin(b), width, color);
  }
}
void Renderer::line(float x0, float y0, float x1, float y1, float width, uint16_t color, uint8_t alpha) {
  raster::scanCapsule(SCREEN_CLIP, x0, y0, x1, y1, width, [&](int x, int y, int c) { blendPx(x, y, color, alpha == 255 ? c : (c * alpha) / 255); });
}
void Renderer::polygon(const float* xy, int n, uint16_t color, uint8_t alpha) {
  raster::scanPolygon(SCREEN_CLIP, xy, n, [&](int x, int y, int c) { blendPx(x, y, color, alpha == 255 ? c : (c * alpha) / 255); });
}
void Renderer::polyline(const float* xy, int n, float width, uint16_t color, bool closed, uint8_t alpha) {
  // union of the segments via a coverage row buffer would be exact; the double-blended joints are invisible at these widths
  raster::scanPolyline(SCREEN_CLIP, xy, n, width, closed, [&](int x, int y, int c) { blendPx(x, y, color, alpha == 255 ? c : (c * alpha) / 255); });
}

// ---- rounded rectangles -----------------------------------------------------------------------------------------------
// Signed-distance rendering: coverage of the fill from the distance to the path, of the stroke from |distance| (a stroke is
// centred on the path, like canvas). Straight runs are filled as spans; only the corner columns are evaluated per pixel.
void Renderer::rrect(int x, int y, int w, int h, int r, uint16_t fill, uint8_t fillA, int32_t stroke, int lw) {
  if (w <= 0 || h <= 0) return;
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  const float hw = stroke >= 0 ? lw * 0.5f : 0.0f;
  const int b = int(std::ceil(hw)) + 1;                         // how far the stroke can reach outside the path
  const int midA = x + r, midB = x + w - r;                     // columns [midA, midB): the straight run, distance depends on the row only
  for (int yy = y - b; yy < y + h + b; yy++) {
    if (yy < 0 || yy >= SH) continue;
    const float py = yy + 0.5f;
    {
      const float sdm = std::fmax(y - py, py - (y + h));         // signed distance to the top/bottom edge
      const int fa = (raster::cov255(0.5f - sdm) * fillA + 127) / 255;
      if (fa > 0) spanBlend(yy, midA, midB, fill, fa);
      if (stroke >= 0) {
        const int sa = raster::cov255(hw + 0.5f - std::fabs(sdm));
        if (sa > 0) spanBlend(yy, midA, midB, uint16_t(stroke), sa);
      }
    }
    for (int side = 0; side < 2; side++) {
      const int xa = side == 0 ? x - b : midB, xb = side == 0 ? midA : x + w + b;
      for (int xx = xa < 0 ? 0 : xa; xx < xb && xx < SW; xx++) {
        const float sd = raster::roundRectSd(xx + 0.5f, py, float(x), float(y), float(w), float(h), float(r));
        const int fa = (raster::cov255(0.5f - sd) * fillA + 127) / 255;
        if (fa > 0) blendPx(xx, yy, fill, uint8_t(fa));
        if (stroke >= 0) {
          const int sa = raster::cov255(hw + 0.5f - std::fabs(sd));
          if (sa > 0) blendPx(xx, yy, uint16_t(stroke), uint8_t(sa));
        }
      }
    }
  }
}
void Renderer::roundRect(int x, int y, int w, int h, int r, uint16_t fill, int32_t stroke, int lineWidth) { rrect(x, y, w, h, r, fill, 255, stroke, lineWidth); }
void Renderer::roundRectA(int x, int y, int w, int h, int r, uint16_t fill, uint8_t fillAlpha, int32_t stroke, int lineWidth) { rrect(x, y, w, h, r, fill, fillAlpha, stroke, lineWidth); }

// =====================================================================================================================
// the level
// =====================================================================================================================
void Renderer::drawLayers(const Level& level, LayerMask mask, int ox, int oy, uint8_t alpha) {
  if (alpha == 0) return;
  const uint16_t cInk = sw16(PAL_INK), cRed = sw16(PAL_RED), cLred = sw16(C_LRED), cBlue = sw16(PAL_BLUE), cWhite = sw16(PAL_WHITE);
  for (int py = 0; py < WORLD_H; py++) {
    const int sy = py + oy;
    if (sy < 0 || sy >= SH) continue;
    const uint8_t* g = level.ground + py * ROW_BYTES;
    const uint8_t* s = level.spike + py * ROW_BYTES;
    const uint8_t* bo = level.bouncy + py * ROW_BYTES;
    uint16_t* row = fb_ + sy * SW;
    for (int bx = 0; bx < ROW_BYTES; bx++) {
      const unsigned gb = mask.ground ? g[bx] : 0, sb = mask.spike ? (s[bx] & ~gb) : 0, bb = mask.bouncy ? (bo[bx] & ~gb & ~s[bx]) : 0;   // ground wins over spike over bouncy
      if (!(gb | sb | bb)) continue;
      for (int bit = 0; bit < 8; bit++) {
        const unsigned m = 0x80u >> bit;
        uint16_t c;
        if (gb & m) c = cInk;
        else if (sb & m) { const int px = bx * 8 + bit; c = ((px + py) & 3) == 0 ? cLred : cRed; }
        else if (bb & m) { const int px = bx * 8 + bit; c = ((px & 3) == 0 && (py & 3) == 0) ? cWhite : cBlue; }
        else continue;
        const int sx = bx * 8 + bit + ox;
        if (sx < 0 || sx >= SW) continue;
        row[sx] = alpha == 255 ? c : sw16(mix565(sw16(row[sx]), sw16(c), alpha));
      }
    }
  }
}

// =====================================================================================================================
// sprites
// =====================================================================================================================
/** The ball: yellow, ink outline (2 px stroke centred on radius 7), two eyes that turn as it rolls, optional happy mouth.
 *  Rendered per pixel with 2 x 2 samples so the squash/stretch stays smooth at any sub-pixel position. */
void Renderer::ball(float x, float y, float angle, float sx, float sy, Face face) {
  const float R = float(BALL_RADIUS), outer = R;                  // stroke band: 6 .. 8
  const float ca = std::cos(angle * 0.5f), sa = std::sin(angle * 0.5f);
  const int x0 = raster::ifloor(x - outer * sx - 1), x1 = raster::iceil(x + outer * sx + 1);
  const int y0 = raster::ifloor(y - outer * sy - 1), y1 = raster::iceil(y + outer * sy + 1);
  const int yc0 = raster::imax(0, y0), yc1 = raster::imin(SH, y1), xc0 = raster::imax(0, x0), xc1 = raster::imin(SW, x1);
  const float isx = 1.0f / sx, isy = 1.0f / sy;
  const uint8_t Y5 = (PAL_YELLOW >> 11) & 31, Y6 = (PAL_YELLOW >> 5) & 63, Y5b = PAL_YELLOW & 31;
  const uint8_t K5 = (PAL_INK >> 11) & 31, K6 = (PAL_INK >> 5) & 63, K5b = PAL_INK & 31;
  const float offs[2] = {0.25f, 0.75f};
  for (int py = yc0; py < yc1; py++) {
    for (int px = xc0; px < xc1; px++) {
      int n = 0, r5 = 0, g6 = 0, b5 = 0;
      for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
        const float lx = (px + offs[i] - x) * isx, ly = (py + offs[j] - y) * isy;
        const float d2 = lx * lx + ly * ly;
        if (d2 > outer * outer) continue;
        const float d = std::sqrt(d2);
        bool ink = d > R - 2.0f;
        if (!ink) {
          const float fx = lx * ca + ly * sa, fy = -lx * sa + ly * ca;     // into the rotated frame the eyes and mouth live in
          const float e0x = fx + 2.6f, e1x = fx - 2.6f, ey = fy + 1.8f;
          if (e0x * e0x + ey * ey <= 2.25f || e1x * e1x + ey * ey <= 2.25f) ink = true;
          else if (face == Face::Happy) {
            const float my = fy - 0.5f, rr = std::sqrt(fx * fx + my * my);
            if (rr >= 2.7f && rr <= 4.1f) { const float an = std::atan2(my, fx); if (an >= 0.15f * raster::PI_F && an <= 0.85f * raster::PI_F) ink = true; }
          }
        }
        ++n;
        if (ink) { r5 += K5; g6 += K6; b5 += K5b; } else { r5 += Y5; g6 += Y6; b5 += Y5b; }
      }
      if (!n) continue;
      uint16_t& p = fb_[py * SW + px];
      const uint16_t d = sw16(p);
      const int dr = (d >> 11) & 31, dg = (d >> 5) & 63, db = d & 31, rest = 4 - n;
      const int R5 = (r5 + dr * rest + 2) >> 2, G6 = (g6 + dg * rest + 2) >> 2, B5 = (b5 + db * rest + 2) >> 2;
      p = sw16(uint16_t((R5 << 11) | (G6 << 5) | B5));
    }
  }
}

void Renderer::flag(float x, float y, float wave) {
  line(x, y, x, y - 24, 3, PAL_INK);
  const float w = std::sin(wave * 30.0f) * 2.0f * wave;
  const float tri[6] = {x + 1, y - 24, x + 21, y - 17 + w, x + 1, y - 10};
  polygon(tri, 3, PAL_GREEN);
  polyline(tri, 3, 2, PAL_INK, true);
}

void Renderer::startPin(float x, float y, bool active) {
  const float chev[6] = {x - 6, y - 24, x, y - 17, x + 6, y - 24};
  polyline(chev, 3, 3, PAL_GREEN);
  disc(x, y, 13, PAL_WHITE, 178);
  ring(x, y, 13, 3, PAL_GREEN);
  ball(x, y, 0);
  if (active) dashedRing(x, y, float(PIN_GRAB_RADIUS), 1.5f, PAL_GREEN);
}

void Renderer::flagPin(float x, float y, bool active) {
  dashedRing(x, y - float(FLAG_CENTER_DY), active ? float(PIN_GRAB_RADIUS) : 16.0f, 2.0f, PAL_GREEN);
  flag(x, y);
}

// =====================================================================================================================
// text
// =====================================================================================================================
namespace {
// cap = height of a capital letter in px (measured in the emulator, see emu/gallery); bold = draw twice, 1 px apart (the small bitmap font has no bold)
struct FontSel { const lgfx::IFont* f; float scale; int cap; bool bold; int baselineAdj; };

/** Map the TypeScript's nominal font size (px, Fredoka, capitals ~0.7 em) to a built-in font of about the same capital height.
 *  Fractional scaling of the outline fonts looks bad (uneven glyph spacing), so only whole steps are used. */
FontSel pickFont(int sizePx) {
  if (sizePx <= 10) return FontSel{&fonts::Font0, 1.0f, 7, false, 1};
  if (sizePx <= 16) return FontSel{&fonts::Font2, 1.0f, 10, true, 1};
  if (sizePx <= 21) return FontSel{&fonts::FreeSansBold9pt7b, 1.0f, 13, false, 0};
  if (sizePx <= 31) return FontSel{&fonts::FreeSansBold12pt7b, 1.0f, 18, false, 0};
  if (sizePx <= 43) return FontSel{&fonts::FreeSansBold18pt7b, 1.0f, 26, false, 0};
  if (sizePx < 68) return FontSel{&fonts::FreeSansBold24pt7b, 1.0f, 34, false, 0};
  return FontSel{&fonts::FreeSansBold24pt7b, 2.0f, 68, false, 0};
}
}  // namespace

int Renderer::textWidth(const char* s, int sizePx) {
  const FontSel fs = pickFont(sizePx);
  frame_.setFont(fs.f); frame_.setTextSize(fs.scale);
  const int w = int(frame_.textWidth(s)) + (fs.bold ? 1 : 0);
  frame_.setTextSize(1);
  return w;
}

void Renderer::text(const char* s, int x, int y, int sizePx, uint16_t color, Align a, int32_t stroke) {
  const FontSel fs = pickFont(sizePx);
  frame_.setFont(fs.f); frame_.setTextSize(fs.scale);
  frame_.setTextDatum(a == Align::Left ? textdatum_t::baseline_left : a == Align::Right ? textdatum_t::baseline_right : textdatum_t::baseline_center);
  const int by = y + (fs.cap + 1) / 2 + fs.baselineAdj;           // baseline so that the capitals are centred on y
  if (stroke >= 0) {
    frame_.setTextColor(uint16_t(stroke));
    static const int8_t off[12][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}, {0, -1}, {0, 1}, {-1, 0}, {1, 0}, {-2, 0}, {2, 0}, {0, -2}, {0, 2}};
    for (const auto& o : off) frame_.drawString(s, x + o[0], by + o[1]);
  }
  frame_.setTextColor(color);
  frame_.drawString(s, x, by);
  if (fs.bold) frame_.drawString(s, x + 1, by);
  frame_.setTextSize(1);
}

// =====================================================================================================================
// pictures
// =====================================================================================================================
void Renderer::thumb(int x, int y, const uint8_t* bytes, int scale) {
  static const uint16_t pal[9] = {PAL_PAPER, PAL_PAPER_DOT, PAL_INK, PAL_RED, PAL_BLUE, PAL_GREEN, PAL_YELLOW, PAL_WHITE, PAL_GREY};
  if (!bytes || scale < 1) return;
  for (int j = 0; j < THUMB_H; j++) {
    for (int i = 0; i < THUMB_W; i++) {
      const int k = j * THUMB_W + i;
      int v = (k & 1) == 0 ? bytes[k >> 1] >> 4 : bytes[k >> 1] & 15;
      if (v > 8) v = 8;
      const uint16_t c = sw16(pal[v]);
      for (int dy = 0; dy < scale; dy++) {
        const int yy = y + j * scale + dy;
        if (yy < 0 || yy >= SH) continue;
        for (int dx = 0; dx < scale; dx++) { const int xx = x + i * scale + dx; if (xx >= 0 && xx < SW) fb_[yy * SW + xx] = c; }
      }
    }
  }
}

void Renderer::frame565(const uint16_t* f, int dx, int dy, int dw, int dh, uint8_t dim) {
  if (!f || dw <= 0 || dh <= 0) return;
  static int16_t sxTab[SW];
  const int tw = dw > SW ? SW : dw;
  for (int x = 0; x < tw; x++) { int s = int((x + 0.5f) * CAM_W / dw); sxTab[x] = int16_t(s >= CAM_W ? CAM_W - 1 : s); }
  for (int y = 0; y < dh; y++) {
    const int yy = dy + y;
    if (yy < 0 || yy >= SH) continue;
    int sy = int((y + 0.5f) * CAM_H / dh); if (sy >= CAM_H) sy = CAM_H - 1;
    const uint16_t* src = f + sy * CAM_W;
    uint16_t* dst = fb_ + yy * SW;
    for (int x = 0; x < tw; x++) {
      const int xx = dx + x;
      if (xx < 0 || xx >= SW) continue;
      uint16_t p = src[sxTab[x]];
      if (dim != 255) { const int r = (((p >> 11) & 31) * dim) / 255, g = (((p >> 5) & 63) * dim) / 255, b = ((p & 31) * dim) / 255; p = uint16_t((r << 11) | (g << 5) | b); }
      dst[xx] = sw16(p);
    }
  }
}

void Renderer::straight565(const uint16_t* f, uint8_t strength) {
  if (!f) return;
  for (int i = 0; i < WORLD_W * WORLD_H; i++) fb_[i] = sw16(mix565(PAL_PAPER, f[i], strength));
}

// =====================================================================================================================
// full-screen effects
// =====================================================================================================================
void Renderer::flash(uint16_t color, float alpha) {
  if (alpha <= 0) return;
  if (alpha > 1) alpha = 1;
  const int a = int(alpha * 255 + 0.5f);
  const uint16_t s = sw16(color);
  for (int i = 0; i < SW * SH; i++) fb_[i] = a >= 255 ? s : sw16(mix565(sw16(fb_[i]), color, a));
}
void Renderer::dim(float alpha) { flash(PAL_INK, alpha); }

}  // namespace scribble
