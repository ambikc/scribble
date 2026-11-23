// renderer.h - everything that puts pixels on the 320 x 240 screen.   Port of sim/src/renderer.ts.   (platform adapter: M5GFX)
//
// All drawing goes into one 320x240 RGB565 sprite (`g()`, in PSRAM) and present() pushes it to the display
// (docs/system-design.md 6.2: a double-buffered sprite; dirty-rect pushes are an optimisation to add AFTER measuring on the device).
// Primitives only, so each one maps onto M5GFX calls. Colours are the PAL_* constants of config.h (RGB565).
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
  M5Canvas& g() { return frame_; }                 // raw access for the rare custom drawing
  void present();                                  // push the frame to the display

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
  /** `sizePx` is a nominal pixel height (9..40); map to the nearest built-in font. `stroke` < 0: no outline. */
  void text(const char* s, int x, int y, int sizePx, uint16_t color = PAL_INK, Align a = Align::Center, int32_t stroke = -1);
  void roundRect(int x, int y, int w, int h, int r, uint16_t fill, int32_t stroke = PAL_INK, int lineWidth = 2);   // stroke < 0: none

  /** A 60 x 36, 4 bits-per-pixel palette thumbnail (as stored in the level file), drawn at integer `scale`. */
  void thumb(int x, int y, const uint8_t* bytes, int scale = 1);

  // ---- camera pictures ----
  /** The 640 x 480 RGB565 frame sampled down to dw x dh at (dx, dy); `dim` 0..255 darkens (255 = as is). */
  void frame565(const uint16_t* f, int dx, int dy, int dw, int dh, uint8_t dim = 255);
  /** The straightened 320 x 192 page, mixed toward paper colour (Review shows it at ~35 %). `strength` 0..255. */
  void straight565(const uint16_t* f, uint8_t strength = 90);

  // ---- full-screen effects ----
  void flash(uint16_t color, float alpha);         // alpha 0..1
  void dim(float alpha);

 private:
  M5Canvas frame_{&M5.Display};
  M5Canvas paper_{&M5.Display};                    // pre-rendered paper
  M5Canvas layers_{&M5.Display};                   // 320 x 192 with a transparent key colour
};

}  // namespace scribble
