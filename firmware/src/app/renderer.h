// renderer.h - everything that puts pixels on the 320 x 240 screen.   Port of sim/src/renderer.ts.   (platform adapter: M5GFX)
//
// All drawing goes into one 320x240 RGB565 sprite (`g()`, in PSRAM) and present() pushes it to the display
// (docs/system-design.md 6.2: a double-buffered sprite; dirty-rect pushes are an optimisation to add AFTER measuring on the device).
// Primitives only, so each one maps onto M5GFX calls. Colours are the PAL_* constants of config.h (RGB565).
//
// Anti-aliasing: the shapes that matter for looks (discs, rings, arcs, lines, polygons, rounded rectangles, the ball, the icons)
// are rasterised by lib/raster with sub-pixel coverage and blended into the frame; text uses M5GFX's built-in fonts.
// Rules for screen authors: src/app/README.md.
#pragma once
#include <M5Unified.h>
#include <cstdint>
#include "config.h"
#include "level.h"

namespace scribble {

struct LayerMask { bool ground = true, spike = true, bouncy = true; };
enum class Face : uint8_t { Normal, Happy };
enum class Align : uint8_t { Left, Center, Right };

class Renderer {
 public:
  float shakeX = 0, shakeY = 0;                    // applied by present()

  bool begin();                                    // allocate the sprites (PSRAM), pre-render the paper
  M5Canvas& g() { return frame_; }                 // raw access for the rare custom drawing (RGB565; see px() for the byte order)
  void present();                                  // push the frame to the display
  void dirty() {}                                  // the simulator's "level pixels changed" hook; the whole frame is redrawn every tick, so nothing to do

  // ---- background ----
  void clearPaper();                               // cream with a faint dot every 16 px
  void clear(uint16_t color);

  // ---- the level, one texture per material (colour-blind safe: ink / diagonal ticks / dots) ----
  /** `alpha` 0..255 (255 = opaque; the Review screen fades layers in). `mask` lets Review reveal them one by one. */
  void drawLayers(const Level& level, LayerMask mask = {}, int x = 0, int y = 0, uint8_t alpha = 255);

  // ---- sprites ----
  void ball(float x, float y, float angle = 0, float sx = 1, float sy = 1, Face face = Face::Normal);
  void flag(float x, float y, float wave = 0);     // (x, y) is the pole base
  void startPin(float x, float y, bool active = false);
  void flagPin(float x, float y, bool active = false);

  // ---- text and shapes ----
  /** `sizePx` is a nominal pixel height (the TypeScript's font size, 8..48); it is mapped to the nearest built-in font.
   *  `y` is the vertical middle of the capital letters. `stroke` >= 0: an outline of that colour. */
  void text(const char* s, int x, int y, int sizePx, uint16_t color = PAL_INK, Align a = Align::Center, int32_t stroke = -1);
  int textWidth(const char* s, int sizePx);        // pixels the string would occupy
  /** Like canvas roundRect: the outline is centred on the rectangle's edge. `stroke` < 0: none. */
  void roundRect(int x, int y, int w, int h, int r, uint16_t fill, int32_t stroke = PAL_INK, int lineWidth = 2);
  /** The same with a translucent fill (fillAlpha 0..255), e.g. the corner chips. */
  void roundRectA(int x, int y, int w, int h, int r, uint16_t fill, uint8_t fillAlpha, int32_t stroke = PAL_INK, int lineWidth = 2);

  /** A 60 x 36, 4 bits-per-pixel palette thumbnail (as stored in the level file), drawn at integer `scale`. */
  void thumb(int x, int y, const uint8_t* bytes, int scale = 1);

  // ---- anti-aliased shapes (floating-point positions; alpha 0..255; angles as the canvas: 0 = +x, clockwise on screen) ----
  void disc(float cx, float cy, float r, uint16_t color, uint8_t alpha = 255);
  /** A circle outline; with `sweep` < 2 pi only the arc a0 .. a0 + sweep (round caps). */
  void ring(float cx, float cy, float r, float width, uint16_t color, float a0 = 0, float sweep = 6.2831853f, uint8_t alpha = 255);
  void dashedRing(float cx, float cy, float r, float width, uint16_t color, float dashOn = 3, float dashOff = 4);
  void line(float x0, float y0, float x1, float y1, float width, uint16_t color, uint8_t alpha = 255);   // round caps
  void polygon(const float* xy, int n, uint16_t color, uint8_t alpha = 255);                              // filled, xy = x0,y0,x1,y1...
  void polyline(const float* xy, int n, float width, uint16_t color, bool closed = false, uint8_t alpha = 255);
  void fillRectA(int x, int y, int w, int h, uint16_t color, uint8_t alpha);                              // translucent rectangle
  /** Paint `color` through an 8-bit coverage picture w x h at (x, y) (icons, glyphs). */
  void blendMask(const uint8_t* a, int x, int y, int w, int h, uint16_t color, uint8_t alpha = 255);

  // ---- camera pictures ----
  /** The 640 x 480 RGB565 frame sampled down to dw x dh at (dx, dy); `dim` 0..255 darkens (255 = as is). */
  void frame565(const uint16_t* f, int dx, int dy, int dw, int dh, uint8_t dim = 255);
  /** The straightened 320 x 192 page, mixed toward paper colour (Review shows it at ~35 %). `strength` 0..255. */
  void straight565(const uint16_t* f, uint8_t strength = 90);

  // ---- full-screen effects ----
  void flash(uint16_t color, float alpha);         // alpha 0..1; below ~1/32 (where the blend changes no pixel) it returns at once
  void dim(float alpha);                           // darken with ink, alpha 0..1

  // ---- raw pixels (rare) ----
  /** The frame as uint16_t[320*240]. M5GFX stores RGB565 byte-swapped: write `swap565(PAL_x)`. */
  uint16_t* px() { return fb_; }
  static constexpr uint16_t swap565(uint16_t c) { return uint16_t((c << 8) | (c >> 8)); }
  /** Blend one pixel (clipped). */
  void blendPx(int x, int y, uint16_t color, uint8_t alpha);

 private:
  M5Canvas frame_{&M5.Display};
  M5Canvas paper_{&M5.Display};                    // pre-rendered paper
  uint16_t* fb_ = nullptr;
  uint16_t* paperFb_ = nullptr;
  void rrect(int x, int y, int w, int h, int r, uint16_t fill, uint8_t fillA, int32_t stroke, int lw);
  void spanBlend(int y, int xa, int xb, uint16_t color, int a);
};

}  // namespace scribble
