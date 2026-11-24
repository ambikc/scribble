// ui.cpp - icons, buttons, hit-testing and hold-to-activate.   Port of sim/src/ui.ts.
//
// Icons are the SVG path data of ui.ts ICONS (24 x 24 grid), parsed and flattened into line segments and polygons, rasterised
// anti-aliased into small 8-bit masks once, cached per (icon, size) and then blitted in any colour.
#include "ui.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "raster.h"

namespace scribble {

// =====================================================================================================================
// the icon data: ui.ts ICONS, verbatim
// =====================================================================================================================
namespace {

struct IconDef { const char* fill[2]; const char* stroke[2]; float w; };

const IconDef ICONS[] = {
  /* Back    */ {{nullptr, nullptr}, {"M10 6l-6 6 6 6M4.5 12H20", nullptr}, 2.6f},
  /* Menu    */ {{nullptr, nullptr}, {"M4.5 7h15M4.5 12h15M4.5 17h15", nullptr}, 2.8f},
  /* Play    */ {{"M7 4.5v15l12-7.5z", nullptr}, {nullptr, nullptr}, 2.4f},
  /* Grid    */ {{nullptr, nullptr}, {"M5.5 4h4a1.5 1.5 0 011.5 1.5v4A1.5 1.5 0 019.5 11h-4A1.5 1.5 0 014 9.5v-4A1.5 1.5 0 015.5 4zM14.5 4h4A1.5 1.5 0 0120 5.5v4a1.5 1.5 0 01-1.5 1.5h-4A1.5 1.5 0 0113 9.5v-4A1.5 1.5 0 0114.5 4zM5.5 13h4a1.5 1.5 0 011.5 1.5v4A1.5 1.5 0 019.5 20h-4A1.5 1.5 0 014 18.5v-4A1.5 1.5 0 015.5 13zM14.5 13h4a1.5 1.5 0 011.5 1.5v4a1.5 1.5 0 01-1.5 1.5h-4a1.5 1.5 0 01-1.5-1.5v-4a1.5 1.5 0 011.5-1.5z", nullptr}, 2.2f},
  /* Camera  */ {{nullptr, nullptr}, {"M3.5 8.5h3.5l1.5-2.5h7l1.5 2.5h3.5v11h-17z", "M15.6 13.5a3.6 3.6 0 11-7.2 0 3.6 3.6 0 017.2 0z"}, 2.2f},
  /* Pencil  */ {{nullptr, nullptr}, {"M4.5 19.5l1-4.5L16 4.5l3.5 3.5L9 18.5zM14 6.5l3.5 3.5", nullptr}, 2.4f},
  /* Undo    */ {{nullptr, nullptr}, {"M8.5 6L4 10.5 8.5 15M4.5 10.5H14a5.5 5.5 0 010 11h-3", nullptr}, 2.4f},
  /* Restart */ {{nullptr, nullptr}, {"M19.5 12a7.5 7.5 0 11-2.5-5.6M19.5 4.5v4.5H15", nullptr}, 2.4f},
  /* Home    */ {{nullptr, nullptr}, {"M3.5 11.5L12 4l8.5 7.5M6 10v9.5h12V10", nullptr}, 2.4f},
  /* Plus    */ {{nullptr, nullptr}, {"M12 5v14M5 12h14", nullptr}, 3.0f},
  /* Trash   */ {{nullptr, nullptr}, {"M5 7h14M9.5 7V4.5h5V7M7 7l.8 12.5h8.4L17 7M10.5 11v5M13.5 11v5", nullptr}, 2.2f},
  /* Sound0  */ {{"M4 9.5h4l5-4v13l-5-4H4z", nullptr}, {"M16 9.5l5 5M21 9.5l-5 5", nullptr}, 2.0f},
  /* Sound1  */ {{"M4 9.5h4l5-4v13l-5-4H4z", nullptr}, {"M16 9a4 4 0 010 6", nullptr}, 2.2f},
  /* Sound2  */ {{"M4 9.5h4l5-4v13l-5-4H4z", nullptr}, {"M16 9a4 4 0 010 6M18.6 6.4a8 8 0 010 11.2", nullptr}, 2.2f},
  /* Up      */ {{nullptr, nullptr}, {"M12 19.5v-14M6 11.5l6-6 6 6", nullptr}, 3.0f},
  /* Check   */ {{nullptr, nullptr}, {"M5 12.5l4.5 4.5L19 7", nullptr}, 3.2f},
  /* InkLess */ {{nullptr, nullptr}, {"M5 9h14", "M6 16h12"}, 2.0f},
  /* InkMore */ {{nullptr, nullptr}, {"M5 8h14", "M6 17h12"}, 4.4f},
  /* Help    */ {{nullptr, nullptr}, {"M9 9a3 3 0 116 0c0 2-3 2.2-3 4.5M12 17.5v.5", nullptr}, 2.8f},
  /* Stop    */ {{"M6 6h12v12H6z", nullptr}, {nullptr, nullptr}, 2.4f},
  /* Eraser  */ {{nullptr, nullptr}, {"M6.5 17.5l-2-2 9-9 5 5-6 6H7.5zM11 20h8", nullptr}, 2.2f},
  /* Ground  */ {{nullptr, nullptr}, {"M3 12h18", nullptr}, 5.0f},
  /* Spike   */ {{nullptr, nullptr}, {"M2.5 18L7 6l4.5 12L16 6l4.5 12", nullptr}, 3.0f},
  /* Bouncy  */ {{nullptr, nullptr}, {"M3 17h4l3-10 4 10 3-10 3 10h1", nullptr}, 2.8f},
  /* Ruler   */ {{nullptr, nullptr}, {"M3 12h18M3 12l4-3.5M3 12l4 3.5M21 12l-4-3.5M21 12l-4 3.5", nullptr}, 2.2f},
  /* Focus   */ {{"M15.5 12a3.5 3.5 0 11-7 0 3.5 3.5 0 017 0z", nullptr}, {"M5 9V5h4M19 9V5h-4M5 15v4h4M19 15v4h-4", nullptr}, 2.4f},
  /* Bulb    */ {{nullptr, nullptr}, {"M9 18h6M10 21h4M8.5 14.5a6 6 0 1 1 7 0c-.8.7-1.5 1.5-1.5 2.5h-4c0-1-.7-1.8-1.5-2.5z", nullptr}, 2.0f},
  /* Level   */ {{nullptr, nullptr}, {"M5.5 7h13a2.5 2.5 0 012.5 2.5v0a2.5 2.5 0 01-2.5 2.5h-13A2.5 2.5 0 013 9.5v0A2.5 2.5 0 015.5 7z", "M12 12v5"}, 2.2f},
};
constexpr int N_ICONS = int(sizeof ICONS / sizeof ICONS[0]);

// ---- SVG path -> polylines ------------------------------------------------------------------------------------------
constexpr int MAX_PTS = 160, MAX_SUB = 8;
struct Flat { float xy[MAX_PTS * 2]; int n = 0; int start[MAX_SUB + 1]; bool closed[MAX_SUB]; int nSub = 0; };

struct Parse {
  const char* s;
  void skip() { while (*s == ' ' || *s == ',' || *s == '\t' || *s == '\n') ++s; }
  bool more() { skip(); return *s && ((*s >= '0' && *s <= '9') || *s == '-' || *s == '.' || *s == '+'); }
  float num() {
    skip(); const char* p = s; bool neg = false;
    if (*p == '-') { neg = true; ++p; } else if (*p == '+') ++p;
    float v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    if (*p == '.') { ++p; float k = 0.1f; while (*p >= '0' && *p <= '9') { v += (*p++ - '0') * k; k *= 0.1f; } }
    s = p; return neg ? -v : v;
  }
  int flag() { skip(); const int f = *s == '1'; if (*s == '0' || *s == '1') ++s; return f; }
};

void addPt(Flat& f, float x, float y) { if (f.n < MAX_PTS) { f.xy[2 * f.n] = x; f.xy[2 * f.n + 1] = y; ++f.n; } }

/** A circular SVG arc from (x0,y0) to (x,y) (rx = ry = r, no rotation), appended as short segments (without the start point). */
void addArc(Flat& f, float x0, float y0, float r, int large, int sweep, float x, float y) {
  const float x1p = (x0 - x) * 0.5f, y1p = (y0 - y) * 0.5f;
  const float d2 = x1p * x1p + y1p * y1p;
  if (d2 < 1e-9f) return;
  if (d2 > r * r) r = std::sqrt(d2);
  float k = (r * r - d2) / d2; if (k < 0) k = 0;
  const float coef = (large == sweep ? -1.0f : 1.0f) * std::sqrt(k);
  const float cxp = coef * y1p, cyp = -coef * x1p;
  const float cx = cxp + (x0 + x) * 0.5f, cy = cyp + (y0 + y) * 0.5f;
  const float t1 = std::atan2((y1p - cyp), (x1p - cxp)), t2 = std::atan2((-y1p - cyp), (-x1p - cxp));
  float dt = t2 - t1;
  if (!sweep && dt > 0) dt -= raster::TWO_PI_F;
  if (sweep && dt < 0) dt += raster::TWO_PI_F;
  int steps = int(std::ceil(std::fabs(dt) / (raster::PI_F / 14)));
  if (steps < 2) steps = 2;
  for (int i = 1; i <= steps; i++) { const float a = t1 + dt * i / steps; addPt(f, cx + r * std::cos(a), cy + r * std::sin(a)); }
}

void addCubic(Flat& f, float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3) {
  for (int i = 1; i <= 6; i++) {
    const float t = i / 6.0f, u = 1 - t;
    addPt(f, u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3, u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3);
  }
}

/** Parse the SVG path subset the icons use: M m L l H h V v C c A a Z z (arcs circular). */
void flatten(const char* d, Flat& f) {
  Parse p{d}; float cx = 0, cy = 0, sx = 0, sy = 0; char cmd = 0;
  f.n = 0; f.nSub = 0;
  auto beginSub = [&](float x, float y) {
    if (f.nSub >= MAX_SUB) return;
    f.start[f.nSub] = f.n; f.closed[f.nSub] = false; ++f.nSub; addPt(f, x, y);
  };
  for (;;) {
    p.skip();
    if (!*p.s) break;
    if (!p.more()) cmd = *p.s++;
    else if (cmd == 'M') cmd = 'L'; else if (cmd == 'm') cmd = 'l';
    const bool rel = cmd >= 'a';
    switch (cmd) {
      case 'M': case 'm': { float x = p.num(), y = p.num(); if (rel) { x += cx; y += cy; } cx = sx = x; cy = sy = y; beginSub(cx, cy); break; }
      case 'L': case 'l': { float x = p.num(), y = p.num(); if (rel) { x += cx; y += cy; } cx = x; cy = y; addPt(f, cx, cy); break; }
      case 'H': case 'h': { float x = p.num(); if (rel) x += cx; cx = x; addPt(f, cx, cy); break; }
      case 'V': case 'v': { float y = p.num(); if (rel) y += cy; cy = y; addPt(f, cx, cy); break; }
      case 'C': case 'c': {
        float x1 = p.num(), y1 = p.num(), x2 = p.num(), y2 = p.num(), x = p.num(), y = p.num();
        if (rel) { x1 += cx; y1 += cy; x2 += cx; y2 += cy; x += cx; y += cy; }
        addCubic(f, cx, cy, x1, y1, x2, y2, x, y); cx = x; cy = y; break;
      }
      case 'A': case 'a': {
        const float rx = p.num(); p.num(); p.num(); const int large = p.flag(), sweep = p.flag(); float x = p.num(), y = p.num();
        if (rel) { x += cx; y += cy; }
        addArc(f, cx, cy, rx, large, sweep, x, y); cx = x; cy = y; break;
      }
      case 'Z': case 'z': { if (f.nSub) f.closed[f.nSub - 1] = true; cx = sx; cy = sy; break; }
      default: return;
    }
  }
  f.start[f.nSub] = f.n;
}

// ---- the icon cache ---------------------------------------------------------------------------------------------------
constexpr int PAD = 2;                                   // mask margin so strokes at the edge of the grid are not cut
constexpr int CACHE = 20;
struct Entry { uint8_t icon = 0, size = 0; uint32_t used = 0; uint8_t* a = nullptr; int w = 0, h = 0; };
Entry cache[CACHE];
uint32_t useClock = 0;
Flat flat;                                               // scratch (static: not on the small task stack)

void renderIcon(const IconDef& d, int size, raster::Mask& m, float ox, float oy) {
  const float sc = size / 24.0f;
  for (int k = 0; k < 2; k++) if (d.fill[k]) {
    flatten(d.fill[k], flat);
    for (int s = 0; s < flat.nSub; s++) {
      const int n = flat.start[s + 1] - flat.start[s];
      float pts[MAX_PTS * 2];
      for (int i = 0; i < n; i++) { pts[2 * i] = ox + flat.xy[2 * (flat.start[s] + i)] * sc; pts[2 * i + 1] = oy + flat.xy[2 * (flat.start[s] + i) + 1] * sc; }
      m.polygon(pts, n);
    }
  }
  for (int k = 0; k < 2; k++) if (d.stroke[k]) {
    flatten(d.stroke[k], flat);
    for (int s = 0; s < flat.nSub; s++) {
      const int n = flat.start[s + 1] - flat.start[s];
      float pts[MAX_PTS * 2];
      for (int i = 0; i < n; i++) { pts[2 * i] = ox + flat.xy[2 * (flat.start[s] + i)] * sc; pts[2 * i + 1] = oy + flat.xy[2 * (flat.start[s] + i) + 1] * sc; }
      m.polyline(pts, n, d.w * sc, flat.closed[s]);
    }
  }
}

const Entry* lookup(Icon icon, int size) {
  const int idx = int(icon) - 1;
  if (idx < 0 || idx >= N_ICONS || size < 4 || size > 120) return nullptr;
  Entry* victim = &cache[0];
  for (Entry& e : cache) {
    if (e.a && e.icon == uint8_t(icon) && e.size == uint8_t(size)) { e.used = ++useClock; return &e; }
    if (!e.a) victim = &e; else if (victim->a && e.used < victim->used) victim = &e;
  }
  const int dim = size + 2 * PAD;
  if (victim->a && (victim->w != dim)) { std::free(victim->a); victim->a = nullptr; }
  if (!victim->a) victim->a = static_cast<uint8_t*>(std::malloc(size_t(dim) * dim));
  if (!victim->a) return nullptr;
  victim->icon = uint8_t(icon); victim->size = uint8_t(size); victim->w = victim->h = dim; victim->used = ++useClock;
  raster::Mask m; m.a = victim->a; m.w = m.h = dim; m.clear();
  // size / 2 may be x.5: the 24-grid origin sits at PAD + (a half pixel when the size is odd), applied again when blitting
  renderIcon(ICONS[idx], size, m, float(PAD) + (size & 1 ? 0.5f : 0.0f), float(PAD) + (size & 1 ? 0.5f : 0.0f));
  return victim;
}

}  // namespace

void drawIcon(Renderer& r, Icon icon, int cx, int cy, int size, uint16_t color) {
  const Entry* e = lookup(icon, size);
  if (!e) return;
  // the grid origin is (cx - size/2, cy - size/2); floor of that, minus the mask margin, is where the mask's pixel (0,0) lands
  r.blendMask(e->a, cx - (size + 1) / 2 - PAD, cy - (size + 1) / 2 - PAD, e->w, e->h, color);
}

// =====================================================================================================================
// buttons
// =====================================================================================================================
bool UI::add(const Btn& b) {
  if (count >= MAX_BTNS) return false;
  btns[count++] = b;
  return true;
}

/** The button under (x, y); within HIT_FORGIVE of several, the nearest wins. Disabled buttons are skipped. */
const Btn* UI::find(int x, int y) const {
  const Btn* best = nullptr; float bd = 1e9f;
  for (int i = 0; i < count; i++) {
    const Btn& b = btns[i];
    if (b.disabled) continue;
    const int pad = b.hit;
    const float dx = float(std::fmax(std::fmax(b.x - pad - x, 0), x - (b.x + b.w + pad)));
    const float dy = float(std::fmax(std::fmax(b.y - pad - y, 0), y - (b.y + b.h + pad)));
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d == 0) return &b;                                // inside: done
    if (d <= HIT_FORGIVE && d < bd) { best = &b; bd = d; }  // near: the nearest wins
  }
  return best;
}

const char* UI::update(const InputState& in) {
  const TouchState& t = in.touch;
  const char* out = nullptr;
  if (t.pressed) {
    const Btn* b = find(t.x, t.y);
    havePressed_ = b != nullptr; fired_ = false; holdFrac_ = 0; touchStartedOnButton = touchOnButton = b != nullptr;
    if (b) {
      pressedCopy_ = *b;
      std::strncpy(pressedId_, b->id, sizeof pressedId_ - 1); pressedId_[sizeof pressedId_ - 1] = 0;
      if (b->onPress) { out = pressedCopy_.id; fired_ = true; }
    } else pressedId_[0] = 0;
  }
  if (t.down && havePressed_ && !fired_) {
    const Btn& b = pressedCopy_;
    const Btn* o = find(t.x, t.y);
    const bool over = o && std::strcmp(o->id, b.id) == 0;
    if (b.holdMs) {
      holdFrac_ = over ? std::fmin(1.0f, float(t.heldMs) / float(b.holdMs)) : 0.0f;
      if (over && t.heldMs >= b.holdMs) { out = b.id; fired_ = true; }
    } else if (b.longMs && b.longId && over && t.heldMs >= b.longMs) { out = b.longId; fired_ = true; }
  }
  if (t.released) {
    if (havePressed_ && !fired_ && !pressedCopy_.holdMs) {
      const Btn* o = find(t.x, t.y);
      if (o && std::strcmp(o->id, pressedCopy_.id) == 0) out = pressedCopy_.id;
    }
    havePressed_ = false; pressedId_[0] = 0; holdFrac_ = 0;   // touchOnButton stays valid for the rest of this tick (the screen reads it next)
  }
  if (!t.down && !t.released && !t.pressed) { havePressed_ = false; pressedId_[0] = 0; holdFrac_ = 0; touchOnButton = false; }
  return out;
}

float UI::holdFor(const char* id) const {
  return havePressed_ && pressedCopy_.holdMs && std::strcmp(pressedCopy_.id, id) == 0 ? holdFrac_ : 0.0f;
}

namespace {
/** The icon, with a ring around it that fills green while a hold button is held. */
void glyph(Renderer& r, const Btn& b, int bx, int by, float hold, int size) {
  const int cx = bx + b.w / 2, cy = by + b.h / 2;
  if (b.icon != Icon::None) drawIcon(r, b.icon, cx, cy, size, b.disabled ? PAL_GREY : PAL_INK);
  if (b.holdMs) {
    const float R = std::fmin(b.w, b.h) * 0.5f - 3.0f;
    r.ring(float(cx), float(cy), R, 3, PAL_PAPER_DOT);
    if (hold > 0) r.ring(float(cx), float(cy), R, 3, PAL_GREEN, -raster::PI_F * 0.5f, hold * raster::TWO_PI_F);
  }
}
}  // namespace

void UI::draw(Renderer& r, uint32_t nowMs) {
  for (int i = 0; i < count; i++) {
    const Btn& b = btns[i];
    const float hold = holdFor(b.id);
    if (b.kind == BtnKind::Custom) { if (b.draw) b.draw(r, b, hold, b.user); continue; }
    const bool down = havePressed_ && std::strcmp(pressedCopy_.id, b.id) == 0;
    if (b.kind == BtnKind::Tab) {
      const uint16_t fill = b.disabled ? PAL_PAPER_DOT : (b.fill >= 0 ? uint16_t(b.fill) : PAL_WHITE);
      r.g().fillRect(b.x, b.y, b.w, b.h, fill);
      r.g().fillRect(b.x, b.edge == Edge::Bottom ? b.y + b.h - 2 : b.y, b.w, 2, PAL_INK);
      r.g().fillRect(b.x + b.w - 2, b.y, 2, b.h, PAL_INK);
      if (b.active) {   // a 3 px ink frame just inside the tab
        r.g().fillRect(b.x, b.y, b.w, 3, PAL_INK); r.g().fillRect(b.x, b.y + b.h - 3, b.w, 3, PAL_INK);
        r.g().fillRect(b.x, b.y, 3, b.h, PAL_INK); r.g().fillRect(b.x + b.w - 3, b.y, 3, b.h, PAL_INK);
      }
      if (down) r.fillRectA(b.x, b.y, b.w, b.h, 0x0000, 31);
      // (nowMs % 1005): one period of sin(ms / 160) is 1005 ms; a float of the raw ms count gets steppy after hours
      if (b.pulse) r.fillRectA(b.x, b.y, b.w, b.h, PAL_WHITE, uint8_t(255 * (0.15f + 0.15f * std::sin(float(nowMs % 1005u) / 160.0f))));
      glyph(r, b, b.x, b.y, hold, b.iconSize ? b.iconSize : 30);
    } else if (b.kind == BtnKind::Chip) {
      const int y = b.y + (down ? 1 : 0);
      if (b.fill >= 0) r.roundRect(b.x, y, b.w, b.h, 12, uint16_t(b.fill), PAL_INK, 2);
      else r.roundRectA(b.x, y, b.w, b.h, 12, PAL_PAPER, 224, PAL_INK, 2);
      glyph(r, b, b.x, y, hold, b.iconSize ? b.iconSize : 22);
    } else {   // Big: a card with a hard shadow that it sinks into when pressed
      const int off = down ? 1 : 4;
      r.roundRect(b.x + off, b.y + off, b.w, b.h, 14, PAL_INK, -1);
      r.roundRect(b.x + (4 - off), b.y + (4 - off), b.w, b.h, 14, b.fill >= 0 ? uint16_t(b.fill) : PAL_WHITE, PAL_INK, 3);
      glyph(r, b, b.x + (4 - off), b.y + (4 - off), hold, b.iconSize ? b.iconSize : 56);
    }
  }
}

}  // namespace scribble
