// editing.h - the pure rules of the Draw / Edit screen: stroke interpolation, the undo ring, pin dragging, the ghost finger.
// Header-only and free of hardware, so test/test_draw can run it on the PC.   Port of the Draw part of sim/src/modes.ts.
//
//   Stroke   : press stamps a disc, every move paints a line (paintLine stamps every STAMP_SPACING px) from the last point:
//              no gaps however fast the finger moves. `extend()` says when the "pen click" is due (every 24 px of travel).
//   UndoRing : UNDO_LEVELS snapshots of the three layers + the two pins (23 KB each). The caller supplies the memory
//              (PSRAM on the device), so this header never allocates.
//   Pin      : grab within PIN_GRAB_RADIUS of the Start ball or the Flag centre (the nearer wins); the drop clears a hole.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include "config.h"
#include "level.h"

namespace scribble {

enum class Tool : uint8_t { Ground, Spike, Bouncy, Eraser };

/** Material of a tool: MAT_GROUND / MAT_SPIKE / MAT_BOUNCY, or MAT_ERASE (-1). */
inline int toolMaterial(Tool t) {
  return t == Tool::Ground ? MAT_GROUND : t == Tool::Spike ? MAT_SPIKE : t == Tool::Bouncy ? MAT_BOUNCY : MAT_ERASE;
}
inline double toolRadius(Tool t) { return t == Tool::Eraser ? double(ERASER_RADIUS) : double(BRUSH_RADIUS); }

// ---- snapshots and the undo ring ------------------------------------------------------------------------------------------
/** What undo restores: the pixels and the two pins (not the name, rev or extras). */
struct LevelSnap {
  uint8_t ground[LAYER_BYTES], spike[LAYER_BYTES], bouncy[LAYER_BYTES];
  int32_t startX, startY, flagX, flagY;
};
inline void snapTake(const Level& l, LevelSnap& s) {
  std::memcpy(s.ground, l.ground, LAYER_BYTES); std::memcpy(s.spike, l.spike, LAYER_BYTES); std::memcpy(s.bouncy, l.bouncy, LAYER_BYTES);
  s.startX = l.startX; s.startY = l.startY; s.flagX = l.flagX; s.flagY = l.flagY;
}
inline void snapRestore(Level& l, const LevelSnap& s) {
  std::memcpy(l.ground, s.ground, LAYER_BYTES); std::memcpy(l.spike, s.spike, LAYER_BYTES); std::memcpy(l.bouncy, s.bouncy, LAYER_BYTES);
  l.startX = s.startX; l.startY = s.startY; l.flagX = s.flagX; l.flagY = s.flagY;
}
inline bool snapSame(const LevelSnap& a, const LevelSnap& b) { return std::memcmp(&a, &b, sizeof(LevelSnap)) == 0; }   // no padding: arrays + four int32
inline bool snapEquals(const Level& l, const LevelSnap& s) {
  return !std::memcmp(s.ground, l.ground, LAYER_BYTES) && !std::memcmp(s.spike, l.spike, LAYER_BYTES) && !std::memcmp(s.bouncy, l.bouncy, LAYER_BYTES) &&
         s.startX == l.startX && s.startY == l.startY && s.flagX == l.flagX && s.flagY == l.flagY;
}

class UndoRing {
 public:
  /** `mem` = room for `cap` snapshots (owned by the caller). */
  void attach(LevelSnap* mem, int cap) { mem_ = mem; cap_ = cap; head_ = 0; n_ = 0; }
  bool ready() const { return mem_ != nullptr; }
  void clear() { head_ = 0; n_ = 0; }
  int size() const { return n_; }
  /** Remember the level as it is now; the oldest snapshot falls out when the ring is full. */
  void push(const Level& l) {
    if (!mem_) return;
    snapTake(l, mem_[head_]);
    head_ = (head_ + 1) % cap_;
    if (n_ < cap_) ++n_;
  }
  void pushSnap(const LevelSnap& s) {
    if (!mem_) return;
    mem_[head_] = s;
    head_ = (head_ + 1) % cap_;
    if (n_ < cap_) ++n_;
  }
  /** Restore the newest snapshot into `l` and drop it. False when there is none. */
  bool pop(Level& l) {
    if (!mem_ || n_ == 0) return false;
    head_ = (head_ + cap_ - 1) % cap_;
    --n_;
    snapRestore(l, mem_[head_]);
    return true;
  }
  /** The newest snapshot (nullptr when empty). */
  const LevelSnap* top() const { return (!mem_ || n_ == 0) ? nullptr : &mem_[(head_ + cap_ - 1) % cap_]; }
 private:
  LevelSnap* mem_ = nullptr; int cap_ = 0, head_ = 0, n_ = 0;
};

/** A CRC-32 over the three layers (one running sum, no big buffer) plus the pins' positions: the emulator prints it when Draw saves, the
 *  PC tests compute the expected value from the same strokes (emu/run_draw_tests.sh compares them). */
inline uint32_t levelDigest(const Level& l) {
  uint32_t c = 0xffffffffu;
  auto feed = [&c](const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1; }
  };
  feed(l.ground, LAYER_BYTES); feed(l.spike, LAYER_BYTES); feed(l.bouncy, LAYER_BYTES);
  const int32_t pins[4] = {l.startX, l.startY, l.flagX, l.flagY};
  for (int32_t v : pins) { const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; feed(b, 4); }
  return c ^ 0xffffffffu;
}

// ---- strokes ----------------------------------------------------------------------------------------------------------------
inline double clampPx(double v, double hi) { return v < 0 ? 0 : v > hi ? hi : v; }

struct Stroke {
  bool active = false;
  double lx = 0, ly = 0;        // last point
  double dist = 0;              // travel since the last pen click

  /** Touch down at (x, y): stamp one disc and start following the finger. */
  void begin(Level& l, int mat, double x, double y, double rad) {
    x = clampPx(x, WORLD_W - 1); y = clampPx(y, WORLD_H - 1);
    active = true; lx = x; ly = y; dist = 0;
    if (mat == MAT_ERASE) eraseDisc(l, x, y, rad); else paintDisc(l, mat, x, y, rad);
  }
  /** The finger is at (x, y): paint the segment from the last point. Returns true when a pen click is due (every 24 px of travel). */
  bool extend(Level& l, int mat, double x, double y, double rad) {
    x = clampPx(x, WORLD_W - 1); y = clampPx(y, WORLD_H - 1);
    paintLine(l, mat, lx, ly, x, y, rad);
    dist += std::sqrt((x - lx) * (x - lx) + (y - ly) * (y - ly));
    lx = x; ly = y;
    if (dist > 24) { dist = 0; return true; }
    return false;
  }
  void end() { active = false; }
};

// ---- pins -------------------------------------------------------------------------------------------------------------------
enum class Pin : uint8_t { None, Start, Flag };

/** Which pin does a touch at (x, y) grab? The Start ball is centred on startX/Y, the Flag's grab centre is FLAG_CENTER_DY above its base. */
inline Pin pinGrab(const Level& l, int x, int y) {
  const double ds = std::hypot(double(x - l.startX), double(y - l.startY));
  const double df = std::hypot(double(x - l.flagX), double(y - (l.flagY - FLAG_CENTER_DY)));
  if (std::fmin(ds, df) > PIN_GRAB_RADIUS) return Pin::None;
  return ds <= df ? Pin::Start : Pin::Flag;
}
/** The two corner chips (back, menu) win a touch within their 48 px box + HIT_FORGIVE 6 = 54 px of the top corners. A pin whose grab
 *  centre sat there could never be touched again, so pinMove keeps the centre at least this far (58) from those corners. */
constexpr int PIN_CHIP_CLEAR = 58;
/** True when a touch at (x, y) lands on a corner chip's hit area (box + forgiveness), where the chip beats a pin grab. */
inline bool inChipCorner(int x, int y) { return y <= 54 && (x <= 54 || x >= WORLD_W - 54); }
/** The finger is at (x, y) while dragging `p`: the pin follows, kept inside the playfield and out from under the corner chips. */
inline void pinMove(Level& l, Pin p, int x, int y) {
  const int cx = x < 8 ? 8 : x > WORLD_W - 8 ? WORLD_W - 8 : x;
  int cy = y < 8 ? 8 : y > WORLD_H - 2 ? WORLD_H - 2 : y;
  if ((cx < PIN_CHIP_CLEAR || cx > WORLD_W - PIN_CHIP_CLEAR) && cy < PIN_CHIP_CLEAR) cy = PIN_CHIP_CLEAR;   // never under a corner chip
  if (p == Pin::Start) { l.startX = cx; l.startY = cy; }
  else if (p == Pin::Flag) { l.flagX = cx; l.flagY = cy + FLAG_CENTER_DY; }
}
/** Dropped: clear a hole so a pin is never buried (START_SAFE_RADIUS / FLAG_SAFE_RADIUS). */
inline void pinDrop(Level& l, Pin p) {
  if (p == Pin::Start) clearAround(l, l.startX, l.startY, START_SAFE_RADIUS);
  else if (p == Pin::Flag) clearAround(l, l.flagX, l.flagY - FLAG_CENTER_DY, FLAG_SAFE_RADIUS);
}

// ---- the ghost finger of a brand-new starter level ----------------------------------------------------------------------------
struct GhostPose { double x, y, trail; };
/** After GHOST_IDLE_S seconds without a touch a translucent ball draws a bridge from (72, 118) to (248, 118) over 2.5 s, rests, repeats every 4.5 s. */
inline bool ghostPose(double idleSeconds, GhostPose& g) {
  if (idleSeconds < GHOST_IDLE_S) return false;
  const double cyc = std::fmod(idleSeconds - GHOST_IDLE_S, 4.5) / 2.5;
  if (cyc > 1.25) return false;
  const double u = cyc < 1 ? cyc : 1;
  g.x = 72 + u * 176; g.y = 118 + std::sin(u * 3.14159265358979323846) * 6; g.trail = u;
  return true;
}

}  // namespace scribble
