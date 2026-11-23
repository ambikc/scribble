// level.cpp - see level.h. Mirrors sim/src/level.ts; the golden tests check the bytes are identical.
#include "level.h"
#include <cmath>
#include <cstring>

namespace scribble {

void levelInit(Level& l) {
  std::memset(&l, 0, sizeof(l));
  l.startX = STARTER_START_X; l.startY = STARTER_START_Y; l.flagX = STARTER_FLAG_X; l.flagY = STARTER_FLAG_Y;
}
uint8_t* layerOf(Level& l, int m) { return m == MAT_GROUND ? l.ground : m == MAT_SPIKE ? l.spike : l.bouncy; }
const uint8_t* layerOf(const Level& l, int m) { return m == MAT_GROUND ? l.ground : m == MAT_SPIKE ? l.spike : l.bouncy; }

int materialAt(const Level& l, int x, int y) {
  if (!inWorld(x, y)) return 0;
  if (getBit(l.ground, x, y)) return 1 + MAT_GROUND;
  if (getBit(l.spike, x, y)) return 1 + MAT_SPIKE;
  if (getBit(l.bouncy, x, y)) return 1 + MAT_BOUNCY;
  return 0;
}

static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int imin(int a, int b) { return a < b ? a : b; }

void paintDisc(Level& l, int m, double cx, double cy, double r) {
  const int x0 = imax(0, int(std::floor(cx - r))), x1 = imin(WORLD_W - 1, int(std::ceil(cx + r)));
  const int y0 = imax(0, int(std::floor(cy - r))), y1 = imin(WORLD_H - 1, int(std::ceil(cy + r)));
  uint8_t* own = layerOf(l, m);
  for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
    const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
    if (dx * dx + dy * dy <= r * r) {
      setBit(own, x, y);
      for (int k = 0; k < 3; k++) if (k != m) clearBit(layerOf(l, k), x, y);
    }
  }
}

void eraseDisc(Level& l, double cx, double cy, double r) {
  const int x0 = imax(0, int(std::floor(cx - r))), x1 = imin(WORLD_W - 1, int(std::ceil(cx + r)));
  const int y0 = imax(0, int(std::floor(cy - r))), y1 = imin(WORLD_H - 1, int(std::ceil(cy + r)));
  for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
    const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
    if (dx * dx + dy * dy <= r * r) { clearBit(l.ground, x, y); clearBit(l.spike, x, y); clearBit(l.bouncy, x, y); }
  }
}

void paintLine(Level& l, int m, double x0, double y0, double x1, double y1, double r) {
  const double dist = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
  const int n = imax(1, int(std::ceil(dist / STAMP_SPACING)));
  for (int i = 0; i <= n; i++) {
    const double t = double(i) / n, x = x0 + (x1 - x0) * t, y = y0 + (y1 - y0) * t;
    if (m == MAT_ERASE) eraseDisc(l, x, y, r); else paintDisc(l, m, x, y, r);
  }
}

void fillRect(Level& l, int m, int x0, int y0, int x1, int y1) {
  uint8_t* own = layerOf(l, m);
  for (int y = imax(0, y0); y < imin(WORLD_H, y1); y++) for (int x = imax(0, x0); x < imin(WORLD_W, x1); x++) {
    setBit(own, x, y);
    for (int k = 0; k < 3; k++) if (k != m) clearBit(layerOf(l, k), x, y);
  }
}

bool isEmpty(const Level& l) {
  for (int i = 0; i < LAYER_BYTES; i++) if (l.ground[i] | l.spike[i] | l.bouncy[i]) return false;
  return true;
}

// ---- CRC-32 (IEEE) ------------------------------------------------------------------------------------
uint32_t crc32(const uint8_t* bytes, size_t start, size_t end) {
  static uint32_t table[256]; static bool ready = false;
  if (!ready) { for (uint32_t n = 0; n < 256; n++) { uint32_t c = n; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1; table[n] = c; } ready = true; }
  uint32_t c = 0xffffffffu;
  for (size_t i = start; i < end; i++) c = table[(c ^ bytes[i]) & 0xff] ^ (c >> 8);
  return c ^ 0xffffffffu;
}

// ---- thumbnails ------------------------------------------------------------------------------------------
void makeThumbnail(const Level& l, uint8_t out[THUMB_BYTES]) {
  static uint8_t idx[THUMB_W * THUMB_H];
  const double sx = double(WORLD_W) / THUMB_W, sy = double(WORLD_H) / THUMB_H;
  enum { PI_PAPER = 0, PI_INK = 2, PI_RED = 3, PI_BLUE = 4, PI_GREEN = 5 };
  for (int ty = 0; ty < THUMB_H; ty++) for (int tx = 0; tx < THUMB_W; tx++) {
    const int x0 = int(std::floor(tx * sx)), x1 = imax(x0 + 1, int(std::floor((tx + 1) * sx)));
    const int y0 = int(std::floor(ty * sy)), y1 = imax(y0 + 1, int(std::floor((ty + 1) * sy)));
    int g = 0, s = 0, b = 0, n = 0;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) { n++; g += getBit(l.ground, x, y); s += getBit(l.spike, x, y); b += getBit(l.bouncy, x, y); }
    idx[ty * THUMB_W + tx] = s * 4 >= n ? PI_RED : b * 4 >= n ? PI_BLUE : g * 4 >= n ? PI_INK : PI_PAPER;
  }
  const double pins[2][2] = { { double(l.startX), double(l.startY) }, { double(l.flagX), double(l.flagY - FLAG_CENTER_DY) } };
  for (const auto& p : pins) {
    const int tx = int(std::floor(p[0] / sx)), ty = int(std::floor(p[1] / sy));
    for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) {
      const int X = tx + dx, Y = ty + dy;
      if (X >= 0 && Y >= 0 && X < THUMB_W && Y < THUMB_H) idx[Y * THUMB_W + X] = PI_GREEN;
    }
  }
  for (int i = 0; i < THUMB_W * THUMB_H; i += 2) out[i >> 1] = uint8_t((idx[i] << 4) | idx[i + 1]);
}

// ---- .scrb encode / decode ----------------------------------------------------------------------------------
static constexpr int HEADER = 64;
static constexpr uint8_t FLAG_EXTRAS = 1, FLAG_THUMB = 2, FLAG_PAPER = 4;
static inline void put16(uint8_t* p, unsigned v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
static inline void put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }
static inline unsigned get16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static inline uint32_t get32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

size_t encodeLevel(const Level& l, bool withThumbnail, uint8_t* out, size_t cap) {
  const int nEx = l.nExtras > MAX_EXTRAS ? MAX_EXTRAS : l.nExtras;
  const size_t total = HEADER + (withThumbnail ? THUMB_BYTES : 0) + 3 * LAYER_BYTES + (nEx ? 1 + 5 * nEx : 0);
  if (cap < total) return 0;
  std::memset(out, 0, total);
  out[0] = 'S'; out[1] = 'C'; out[2] = 'R'; out[3] = 'B';
  out[4] = 1; out[5] = 0;                                         // version 1, raw layers
  put16(out + 6, WORLD_W); put16(out + 8, WORLD_H);
  put16(out + 10, unsigned(l.startX)); put16(out + 12, unsigned(l.startY)); put16(out + 14, unsigned(l.flagX)); put16(out + 16, unsigned(l.flagY));
  out[18] = 3;
  out[19] = uint8_t((nEx ? FLAG_EXTRAS : 0) | (withThumbnail ? FLAG_THUMB : 0) | (l.fromPaper ? FLAG_PAPER : 0));
  size_t len = strnlen(l.name, sizeof(l.name)), nb = len < 23 ? len : 23;
  while (nb > 0 && nb < len && (uint8_t(l.name[nb]) & 0xc0) == 0x80) nb--;   // never cut a UTF-8 character
  std::memcpy(out + 20, l.name, nb);
  put32(out + 48, l.rev);
  size_t p = HEADER;
  if (withThumbnail) { makeThumbnail(l, out + p); p += THUMB_BYTES; }
  std::memcpy(out + p, l.ground, LAYER_BYTES); p += LAYER_BYTES;
  std::memcpy(out + p, l.spike, LAYER_BYTES); p += LAYER_BYTES;
  std::memcpy(out + p, l.bouncy, LAYER_BYTES); p += LAYER_BYTES;
  if (nEx) { out[p++] = uint8_t(nEx); for (int i = 0; i < nEx; i++) { out[p++] = l.extras[i].type; put16(out + p, l.extras[i].x); put16(out + p + 2, l.extras[i].y); p += 4; } }
  put32(out + 44, crc32(out, HEADER, total));
  return total;
}

LevelError decodeLevel(const uint8_t* b, size_t n, Level& l, uint8_t* thumbOut, bool* hasThumbOut) {
  if (n < size_t(HEADER)) return LEVEL_ERR_SIZE;
  if (b[0] != 'S' || b[1] != 'C' || b[2] != 'R' || b[3] != 'B') return LEVEL_ERR_MAGIC;
  if (b[4] > 1 || b[5] != 0) return LEVEL_ERR_VERSION;
  if (get16(b + 6) != unsigned(WORLD_W) || get16(b + 8) != unsigned(WORLD_H) || b[18] != 3) return LEVEL_ERR_DIMS;
  const uint8_t flags = b[19];
  const bool hasThumb = flags & FLAG_THUMB, hasExtras = flags & FLAG_EXTRAS;
  size_t need = HEADER + (hasThumb ? THUMB_BYTES : 0) + 3 * LAYER_BYTES;
  if (n < need + (hasExtras ? 1 : 0)) return LEVEL_ERR_SIZE;
  int nEx = 0;
  if (hasExtras) { nEx = b[need]; need += 1 + 5 * size_t(nEx); if (nEx > MAX_EXTRAS || n < need) return LEVEL_ERR_SIZE; }
  if (n != need) return LEVEL_ERR_SIZE;
  if (crc32(b, HEADER, n) != get32(b + 44)) return LEVEL_ERR_CRC;
  levelInit(l);
  l.startX = int(get16(b + 10)); l.startY = int(get16(b + 12)); l.flagX = int(get16(b + 14)); l.flagY = int(get16(b + 16));
  if (l.startX >= WORLD_W || l.startY >= WORLD_H || l.flagX >= WORLD_W || l.flagY >= WORLD_H) return LEVEL_ERR_RANGE;
  size_t e = 20; while (e < 44 && b[e]) e++;
  std::memcpy(l.name, b + 20, (e - 20) < 23 ? (e - 20) : 23);
  l.rev = get32(b + 48); l.fromPaper = flags & FLAG_PAPER;
  size_t p = HEADER;
  if (hasThumb) { if (thumbOut) std::memcpy(thumbOut, b + p, THUMB_BYTES); p += THUMB_BYTES; }
  if (hasThumbOut) *hasThumbOut = hasThumb;
  std::memcpy(l.ground, b + p, LAYER_BYTES); p += LAYER_BYTES;
  std::memcpy(l.spike, b + p, LAYER_BYTES); p += LAYER_BYTES;
  std::memcpy(l.bouncy, b + p, LAYER_BYTES); p += LAYER_BYTES;
  if (hasExtras) { p++; for (int i = 0; i < nEx; i++) { l.extras[i] = { b[p], uint16_t(get16(b + p + 1)), uint16_t(get16(b + p + 3)) }; p += 5; } l.nExtras = nEx; }
  return LEVEL_OK;
}

// ---- built-in levels (same as level.ts) ------------------------------------------------------------------------
void starterLevel(Level& l) {
  levelInit(l); std::strcpy(l.name, "Bridge");
  fillRect(l, MAT_GROUND, 16, 112, 72, 120);
  fillRect(l, MAT_GROUND, 248, 112, 304, 120);
}
void lessonSpike(Level& l) {
  levelInit(l); std::strcpy(l.name, "Spikes");
  fillRect(l, MAT_GROUND, 16, 112, 72, 120);
  fillRect(l, MAT_GROUND, 248, 112, 304, 120);
  fillRect(l, MAT_GROUND, 120, 104, 200, 160);
  fillRect(l, MAT_SPIKE, 120, 94, 200, 104);
}
void lessonBouncy(Level& l) {
  levelInit(l); std::strcpy(l.name, "Bounce");
  l.startX = 40; l.startY = 142;
  fillRect(l, MAT_GROUND, 16, 150, 72, 158);
  fillRect(l, MAT_GROUND, 72, 166, 100, 174);
  fillRect(l, MAT_GROUND, 164, 166, 196, 174);
  fillRect(l, MAT_BOUNCY, 100, 166, 164, 174);
  fillRect(l, MAT_GROUND, 232, 70, 304, 78);
  l.flagX = 280; l.flagY = 70;
}

}  // namespace scribble
