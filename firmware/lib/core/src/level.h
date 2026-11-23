// level.h - the level data model and the .scrb file format.   (portable core: no Arduino, no I/O, no allocation)
// Line-by-line port of sim/src/level.ts. Format spec: docs/system-design.md section 4.
//
//   World is 320 x 192. A layer is 1 bit per pixel, row-major, MSB first, 40 bytes per row.
//   Three layers: ground, spike, bouncy. They are EXCLUSIVE (a pixel is in at most one layer).
#pragma once
#include <cstddef>
#include <cstdint>
#include "config.h"

namespace scribble {

constexpr int MAT_GROUND = 0, MAT_SPIKE = 1, MAT_BOUNCY = 2, MAT_ERASE = -1;
constexpr int THUMB_BYTES = THUMB_W * THUMB_H / 2;                 // 1080
constexpr int MAX_EXTRAS = 8;
constexpr size_t MAX_FILE_BYTES = 64 + THUMB_BYTES + 3 * LAYER_BYTES + 1 + 5 * MAX_EXTRAS;  // 24,225

struct Extra { uint8_t type; uint16_t x, y; };                    // type 1 = star (stretch goal)

struct Level {
  uint8_t ground[LAYER_BYTES], spike[LAYER_BYTES], bouncy[LAYER_BYTES];
  int startX, startY;        // ball centre at spawn
  int flagX, flagY;          // pole base
  char name[24];             // UTF-8, NUL-terminated, at most 23 bytes
  uint32_t rev;              // +1 on every save
  bool fromPaper;            // flags bit2
  Extra extras[MAX_EXTRAS]; int nExtras;
};

void levelInit(Level& l);                                           // empty level, starter pins
uint8_t* layerOf(Level& l, int m);
const uint8_t* layerOf(const Level& l, int m);

// bit access: bit = (layer[y*40 + (x>>3)] >> (7 - (x&7))) & 1   (callers check bounds)
inline int getBit(const uint8_t* layer, int x, int y) { return (layer[y * ROW_BYTES + (x >> 3)] >> (7 - (x & 7))) & 1; }
inline void setBit(uint8_t* layer, int x, int y) { layer[y * ROW_BYTES + (x >> 3)] |= uint8_t(0x80 >> (x & 7)); }
inline void clearBit(uint8_t* layer, int x, int y) { layer[y * ROW_BYTES + (x >> 3)] &= uint8_t(~(0x80 >> (x & 7))); }
inline bool inWorld(int x, int y) { return x >= 0 && y >= 0 && x < WORLD_W && y < WORLD_H; }
int materialAt(const Level& l, int x, int y);                       // 0 = nothing, 1 + material

// painting (finger mode, built-in levels, pin clearing)
void paintDisc(Level& l, int m, double cx, double cy, double r);    // clears the other two layers there
void eraseDisc(Level& l, double cx, double cy, double r);
void paintLine(Level& l, int m, double x0, double y0, double x1, double y1, double r);  // m = MAT_ERASE erases
void fillRect(Level& l, int m, int x0, int y0, int x1, int y1);
inline void clearAround(Level& l, double cx, double cy, double r) { eraseDisc(l, cx, cy, r); }
bool isEmpty(const Level& l);

uint32_t crc32(const uint8_t* bytes, size_t start, size_t end);
void makeThumbnail(const Level& l, uint8_t out[THUMB_BYTES]);       // 60 x 36, 4 bits/pixel palette index

enum LevelError { LEVEL_OK = 0, LEVEL_ERR_MAGIC, LEVEL_ERR_VERSION, LEVEL_ERR_SIZE, LEVEL_ERR_CRC, LEVEL_ERR_DIMS, LEVEL_ERR_RANGE };
size_t encodeLevel(const Level& l, bool withThumbnail, uint8_t* out, size_t cap);   // returns bytes written, 0 if cap too small
LevelError decodeLevel(const uint8_t* bytes, size_t n, Level& out, uint8_t* thumbOut /* may be null */, bool* hasThumb = nullptr);

// the three built-in "lessons"
void starterLevel(Level& l);
void lessonSpike(Level& l);
void lessonBouncy(Level& l);

}  // namespace scribble
