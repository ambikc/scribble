// ui.h - buttons, hit-testing and the icon set.   Port of sim/src/ui.ts.   (platform adapter: draws through Renderer)
//
// One rule keeps screens simple: every tick a screen (1) ui.begin() and add()s its buttons, (2) asks ui.update(input) which
// button fired, (3) reacts. draw() paints the same buttons. Buttons are REBUILT every tick, so identity is the id string
// (compare with strcmp, never the pointer). Ids are string literals with static storage.
// Every button is >= 48 px (or a 40 px chip with 4 px extra hit padding): docs/screens.md section 1.
#pragma once
#include <cstdint>
#include "config.h"
#include "input.h"
#include "renderer.h"

namespace scribble {

enum class Icon : uint8_t {
  None, Back, Menu, Play, Grid, Camera, Pencil, Undo, Restart, Home, Plus, Trash, Sound0, Sound1, Sound2, Up, Check,
  InkLess, InkMore, Help, Stop, Eraser, Ground, Spike, Bouncy, Ruler, Focus, Bulb, Level
};
/** Draw an icon centred at (cx, cy), `size` px square (the paths are the 24 x 24 grid of ui.ts ICONS, drawn as lines/polygons). */
void drawIcon(Renderer& r, Icon icon, int cx, int cy, int size, uint16_t color = PAL_INK);

enum class BtnKind : uint8_t { Tab, Chip, Big, Custom };
enum class Edge : uint8_t { Top, Bottom };

struct Btn;
using BtnDraw = void (*)(Renderer& r, const Btn& b, float hold, void* user);

struct Btn {
  const char* id = "";                     // static string
  int x = 0, y = 0, w = 0, h = 0;
  BtnKind kind = BtnKind::Tab;
  Icon icon = Icon::None; int iconSize = 0;   // 0 = the kind's default (Tab 30, Chip 22, Big 56)
  int32_t fill = -1;                       // RGB565 or -1 for the kind's default
  uint16_t holdMs = 0;                     // needs a hold of this long (a green ring fills while held)
  bool onPress = false;                    // fires on touch-down (the Jump pad)
  const char* longId = nullptr; uint16_t longMs = 0;   // a long press fires longId instead of id
  Edge edge = Edge::Top;                   // which edge of a Tab carries the ink line (the top bar uses Bottom)
  bool active = false;                     // pushed in, selected tool
  bool disabled = false;
  bool pulse = false;                      // gentle pulse (GO when all lights are green)
  uint8_t hit = 0;                         // extra hit padding (corner chips use 4)
  BtnDraw draw = nullptr; void* user = nullptr;   // Custom buttons
};

class UI {
 public:
  static constexpr int MAX_BTNS = 32;
  Btn btns[MAX_BTNS]; int count = 0;
  bool touchOnButton = false;              // the current touch began on a button: the screen must not also treat it as a stroke/jump

  void begin() { count = 0; }
  bool add(const Btn& b);                  // false if full
  /** Returns the id of the button activated this tick, or nullptr. */
  const char* update(const InputState& in);
  float holdFor(const char* id) const;     // 0..1 while a hold button is held
  const char* pressedId() const { return pressedId_; }
  void draw(Renderer& r, uint32_t nowMs);
 private:
  const Btn* find(int x, int y) const;
  char pressedId_[24] = ""; bool havePressed_ = false, fired_ = false; float holdFrac_ = 0;
  Btn pressedCopy_;
};

}  // namespace scribble
