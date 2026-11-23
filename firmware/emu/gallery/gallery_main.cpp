// gallery_main.cpp - a dev app that shows every Renderer and UI building block on one screen at a time.   MIT licensed.
//
//   pio run -e emu_gallery && EMU_SCRIPT=emu/scripts/gallery.txt SDL_VIDEODRIVER=dummy .pio/build/emu_gallery/program
//   python3 emu/make_sheet.py /tmp/gallery/*.ppm /tmp/gallery/sheet.png          (one contact sheet of the screenshots)
//
// Tap the top-right corner (the dots) for the next page, the top-left corner for the previous one. It does not need Input,
// Audio, LevelStore or Game: only renderer.cpp and ui.cpp, so it is the place to check how a new primitive looks.
#include <M5Unified.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "config.h"
#include "level.h"
#include "renderer.h"
#include "ui.h"

using namespace scribble;

static Renderer R;
static UI U;
static Level LV, LV2;
static uint8_t thumbs[3][THUMB_BYTES];
static uint16_t* camFrame = nullptr;    // 640 x 480 test picture
static uint16_t* pageFrame = nullptr;   // 320 x 192 test picture
static int page = 0;
static uint32_t now = 0, downAt = 0;
static float tsec = 0;
static const char* lastFired = "";
static uint32_t firedAt = 0;

enum Page { P_ICONS_SMALL, P_ICONS_BIG1, P_ICONS_BIG2, P_BUTTONS, P_TEXT, P_TEXT2, P_FONTS, P_LEVEL, P_LEVEL_FX, P_THUMBS, P_FX, P_MENU, P_PICTURES, P_COUNT };

// ---------------------------------------------------------------------------------------------------------------------
// a level with all three materials, drawn with the real brush
static void buildLevels() {
  levelInit(LV); std::strcpy(LV.name, "Gallery");
  LV.startX = 36; LV.startY = 88; LV.flagX = 288; LV.flagY = 150;
  paintLine(LV, MAT_GROUND, 16, 110, 90, 112, 4); paintLine(LV, MAT_GROUND, 90, 112, 130, 140, 4); paintLine(LV, MAT_GROUND, 130, 140, 190, 140, 4);
  paintLine(LV, MAT_GROUND, 190, 140, 230, 120, 4); paintLine(LV, MAT_GROUND, 230, 120, 304, 160, 4);
  for (int i = 0; i < 6; i++) paintLine(LV, MAT_SPIKE, 140 + i * 8, 134, 144 + i * 8, 124, 2.5);          // a spiky comb
  paintLine(LV, MAT_BOUNCY, 40, 168, 120, 168, 5); paintLine(LV, MAT_BOUNCY, 120, 168, 180, 180, 5);        // a bouncy pad
  paintLine(LV, MAT_SPIKE, 200, 60, 260, 70, 4);
  paintDisc(LV, MAT_GROUND, 250, 40, 14); paintDisc(LV, MAT_BOUNCY, 60, 40, 14); paintDisc(LV, MAT_SPIKE, 156, 40, 14);
  lessonBouncy(LV2);
  starterLevel(LV2); makeThumbnail(LV2, thumbs[0]);
  lessonSpike(LV2); makeThumbnail(LV2, thumbs[1]);
  makeThumbnail(LV, thumbs[2]);
  lessonBouncy(LV2);
}

static uint16_t c565(int r, int g, int b) { return uint16_t(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }
static void buildPictures() {
  camFrame = new uint16_t[CAM_W * CAM_H];
  for (int y = 0; y < CAM_H; y++) for (int x = 0; x < CAM_W; x++) {
    const bool paper = x > 120 && x < 520 && y > 90 && y < 400;
    int r = paper ? 235 : 70 + x / 12, g = paper ? 228 : 90 + y / 10, b = paper ? 205 : 120;
    if (paper && ((x / 40 + y / 40) & 1) == 0) { r -= 12; g -= 12; b -= 12; }
    if (paper && std::abs(y - 245) < 6 && x > 160 && x < 480) { r = 28; g = 28; b = 36; }
    camFrame[y * CAM_W + x] = c565(r, g, b);
  }
  pageFrame = new uint16_t[WORLD_W * WORLD_H];
  for (int y = 0; y < WORLD_H; y++) for (int x = 0; x < WORLD_W; x++) pageFrame[y * WORLD_W + x] = ((x / 16 + y / 16) & 1) ? c565(40, 40, 60) : c565(255, 255, 255);
}

// ---------------------------------------------------------------------------------------------------------------------
static const char* const ICON_NAMES[] = {"none", "back", "menu", "play", "grid", "camera", "pencil", "undo", "restart", "home", "plus", "trash", "sound0", "sound1", "sound2", "up", "check",
                                         "inkLess", "inkMore", "help", "stop", "eraser", "ground", "spike", "bouncy", "ruler", "focus", "bulb", "level"};

static void pageIconsSmall() {
  R.clearPaper();
  R.text("icons 22 and 30 px", 160, 12, 11, PAL_GREY);
  for (int i = 1; i <= 28; i++) {                       // 22 px: 14 per row
    const int k = i - 1, cx = 16 + (k % 14) * 22 + 0, cy = 36 + (k / 14) * 26;
    drawIcon(R, Icon(i), cx, cy, 22);
  }
  for (int i = 1; i <= 28; i++) {                       // 30 px: 10 per row
    const int k = i - 1, cx = 22 + (k % 10) * 30 + 0, cy = 108 + (k / 10) * 36;
    drawIcon(R, Icon(i), cx, cy, 30);
  }
  // the same at 24 / 26 / 28 in red, blue and green to see rounding at odd sizes
  for (int i = 0; i < 8; i++) drawIcon(R, Icon(2 + i), 20 + i * 38, 220, 23 + (i & 3), i & 1 ? PAL_RED : PAL_BLUE);
}
static void pageIconsBig(int part) {
  R.clearPaper();
  for (int k = 0; k < 14; k++) {
    const int i = 1 + part * 14 + k;
    if (i > 28) break;
    const int cx = 32 + (k % 5) * 64, cy = 32 + (k / 5) * 70;
    drawIcon(R, Icon(i), cx, cy, 56);
    R.text(ICON_NAMES[i], cx, cy + 34, 8, PAL_GREY);
  }
}

// ---------------------------------------------------------------------------------------------------------------------
static void drawCustom(Renderer& r, const Btn& b, float hold, void* user) {
  r.roundRect(b.x, b.y, b.w, b.h, 0, PAL_WHITE, PAL_INK, 2);
  r.text(user ? static_cast<const char*>(user) : "hold 1.5 s", b.x + b.w / 2, b.y + b.h / 2, 11);
  if (hold > 0) r.ring(b.x + 12.0f, b.y + b.h / 2.0f, 9, 3, PAL_GREEN, -1.5708f, hold * 6.2831853f);
}

static void tickButtons(const InputState& in) {
  U.begin();
  auto chip = [](const char* id, Icon ic, int x, int fill = -1, uint16_t hold = 0, bool dis = false) {
    Btn b; b.id = id; b.x = x; b.y = 4; b.w = 40; b.h = 40; b.kind = BtnKind::Chip; b.icon = ic; b.hit = 4; b.fill = fill; b.holdMs = hold; b.disabled = dis; U.add(b);
  };
  chip("c1", Icon::Back, 4); chip("c2", Icon::Sound1, 52, PAL_YELLOW); chip("c3", Icon::Restart, 100, -1, HOLD_MS); chip("c4", Icon::Trash, 148, -1, 0, true);
  chip("c5", Icon::Help, 196, -1); chip("c6", Icon::Menu, 276);
  Btn bg; bg.id = "g1"; bg.x = 16; bg.y = 58; bg.w = 136; bg.h = 100; bg.kind = BtnKind::Big; bg.fill = PAL_YELLOW; bg.icon = Icon::Camera; bg.iconSize = 64; U.add(bg);
  bg.id = "g2"; bg.x = 168; bg.fill = PAL_WHITE; bg.icon = Icon::Pencil; U.add(bg);
  Btn cu; cu.id = "cu"; cu.x = 104; cu.y = 164; cu.w = 80; cu.h = 24; cu.kind = BtnKind::Custom; cu.holdMs = 1500; cu.draw = drawCustom; U.add(cu);
  Btn lp; lp.id = "lp"; lp.longId = "lp_long"; lp.longMs = 700; lp.x = 8; lp.y = 164; lp.w = 90; lp.h = 24; lp.kind = BtnKind::Custom; lp.draw = drawCustom; lp.user = (void*)"tap | long"; U.add(lp);
  Btn jp; jp.id = "jump"; jp.onPress = true; jp.x = 214; jp.y = 164; jp.w = 96; jp.h = 24; jp.kind = BtnKind::Custom; jp.draw = drawCustom; jp.user = (void*)"jump (on press)"; U.add(jp);
  struct T { const char* id; Icon ic; int fill; bool active, dis, pulse; uint16_t hold; };
  const T tabs[6] = {{"t1", Icon::Grid, -1, false, false, false, 0}, {"t2", Icon::Pencil, -1, true, false, false, 0}, {"t3", Icon::Camera, -1, false, true, false, 0},
                     {"t4", Icon::Play, PAL_GREEN, false, false, true, 0}, {"t5", Icon::Stop, PAL_RED, true, false, false, 0}, {"t6", Icon::Trash, PAL_YELLOW, false, false, false, CLEAR_HOLD_MS}};
  const int w[6] = {53, 53, 54, 53, 53, 54}; int x = 0;
  for (int i = 0; i < 6; i++) {
    Btn b; b.id = tabs[i].id; b.x = x; b.y = 192; b.w = w[i]; b.h = 48; b.kind = BtnKind::Tab; b.icon = tabs[i].ic; b.fill = tabs[i].fill; b.active = tabs[i].active;
    b.disabled = tabs[i].dis; b.pulse = tabs[i].pulse; b.holdMs = tabs[i].hold; U.add(b); x += w[i];
  }
  const char* hit = U.update(in);
  if (hit) { lastFired = hit; firedAt = now; std::printf("fired %s\n", hit); }
}
static void pageButtons() {
  R.clearPaper();
  R.text("tap a button; hold the ones with a ring", 160, 51, 8, PAL_GREY);
  U.draw(R, now);
  if (now - firedAt < 1500 && *lastFired) R.text(lastFired, 250, 84, 22, PAL_RED, Align::Center, PAL_WHITE);
  R.text(U.pressedId(), 250, 120, 14, PAL_GREY);
}

// ---------------------------------------------------------------------------------------------------------------------
static void pageText() {
  R.clearPaper();
  static const int sizes[] = {8, 9, 11, 14, 18, 20, 22, 28, 30, 40, 42, 48};
  int y = 4;
  for (int s : sizes) {
    char buf[64]; std::snprintf(buf, sizeof buf, "%d Scribble Run 0123", s);
    const int w = R.textWidth(buf, s);
    R.text(buf, 6, y + (s * 7) / 10 / 1 + 0, s, PAL_INK, Align::Left);
    char wb[16]; std::snprintf(wb, sizeof wb, "%dpx", w);
    R.text(wb, 314, y + 6, 8, PAL_GREY, Align::Right);
    y += (s <= 14 ? 12 : s <= 22 ? s * 7 / 10 + 8 : s * 8 / 10 + 3);
    if (y > 232) break;
  }
}
static void pageFonts() {   // candidates for the small sizes (the renderer picks one of these in pickFont)
  R.clearPaper();
  struct C { const char* name; const lgfx::IFont* f; float sc; };
  const C cs[] = {{"Font0", &fonts::Font0, 1}, {"Font2", &fonts::Font2, 1}, {"DejaVu9", &fonts::DejaVu9, 1}, {"DejaVu12", &fonts::DejaVu12, 1},
                  {"FSB9 .6", &fonts::FreeSansBold9pt7b, 0.6f}, {"FSB9 .7", &fonts::FreeSansBold9pt7b, 0.7f}, {"FSB9 .8", &fonts::FreeSansBold9pt7b, 0.8f},
                  {"FSB9 1", &fonts::FreeSansBold9pt7b, 1}, {"FS9 .8", &fonts::FreeSans9pt7b, 0.8f}, {"TomThumb 2", &fonts::TomThumb, 2}, {"Font2 x1.5", &fonts::Font2, 1.5f}};
  int y = 4;
  for (const C& c : cs) {
    R.g().setFont(c.f); R.g().setTextSize(c.sc); R.g().setTextColor(PAL_INK); R.g().setTextDatum(textdatum_t::top_left);
    char b[64]; std::snprintf(b, sizeof b, "%s: Tap to start 0123 Paper", c.name);
    R.g().drawString(b, 6, y); y += int(R.g().fontHeight()) + 3;
  }
  R.g().setTextSize(1);
}
static void pageText2() {
  R.clearPaper();
  R.text("SCRIBBLE", 160, 34, 42, PAL_INK);
  R.text("RUN!", 160, 78, 42, PAL_RED);
  R.text("YOU WIN!", 160, 118, 28, PAL_INK, Align::Center, PAL_YELLOW);
  R.text("3", 234, 150, 30, PAL_INK); R.text("2", 263, 150, 30, PAL_RED); R.text("1", 290, 150, 30, PAL_BLUE);
  R.text("All 10 slots are full", 100, 190, 14, PAL_INK);
  R.text("Left", 8, 214, 14, PAL_INK, Align::Left); R.text("Center", 160, 214, 14, PAL_INK, Align::Center); R.text("Right", 312, 214, 14, PAL_INK, Align::Right);
  R.line(160, 200, 160, 232, 1, PAL_RED); R.line(8, 200, 8, 232, 1, PAL_RED); R.line(312, 200, 312, 232, 1, PAL_RED);
  R.text("40 cm", 222, 190, 14); R.text("Paper not found", 250, 230, 9, PAL_GREY);
}

// ---------------------------------------------------------------------------------------------------------------------
static void drawLevelScene(const Level& lv, float ang, uint8_t alpha) {
  R.clearPaper();
  R.drawLayers(lv, {}, 0, 0, alpha);
  R.startPin(float(lv.startX), float(lv.startY), false);
  R.flagPin(float(lv.flagX), float(lv.flagY), false);
}
static void pageLevel() {
  drawLevelScene(LV, 0, 255);
  // the ball at rest, rolling (angle), squashed, stretched, happy
  R.ball(30, 100, 0); R.ball(60, 100, 1.2f); R.ball(90, 100 - 3, 0, 1.3f, 0.7f); R.ball(110, 96, 0, 0.75f, 1.35f); R.ball(135, 100, 2.5f, 1, 1, Face::Happy);
  R.ball(160.5f, 100.5f, 0.3f); R.ball(200, 100, 0.0f, 1.5f, 1.5f, Face::Happy);
  R.flag(250, 100, 0); R.flag(270, 100, 0.5f); R.flag(290, 100, 1);
  R.startPin(220, 200, true); R.flagPin(260, 220, true);
  R.text("lesson: all three materials", 160, 184, 8, PAL_GREY);
}
static void pageLevelFx() {
  R.clearPaper();
  R.drawLayers(LV, {}, 0, 0, 90);                     // faded in
  LayerMask only; only.ground = true; only.spike = false; only.bouncy = false;
  R.drawLayers(LV, only, 0, 0, 255);                  // ground only
  R.drawLayers(LV2, {}, 160, 96, 255);                // the bouncy lesson, offset (clipped at the right edge)
  R.drawLayers(LV2, {}, -100, 120, 200);
  R.text("alpha 90 / mask / offset", 160, 12, 8, PAL_GREY);
}

static void pageThumbs() {   // a 60 x 36 thumbnail at scale 1, 2 and 3, and a picker cell around it
  R.clear(PAL_PAPER);
  R.thumb(8, 8, thumbs[0], 1); R.thumb(76, 8, thumbs[1], 1); R.thumb(8, 52, thumbs[2], 2); R.thumb(8, 130, thumbs[1], 3);
  R.text("x1", 160, 26, 11, PAL_GREY); R.text("x2", 160, 88, 11, PAL_GREY); R.text("x3", 160, 220, 11, PAL_GREY);
  const int cx = 188, cy = 8;                                   // the selected cell: yellow border, solved dot
  R.roundRect(cx, cy, 64, 96, 0, PAL_YELLOW, PAL_INK, 3); R.thumb(cx + 2, cy + 4, thumbs[1], 1); R.text("2", cx + 32, cy + 58, 22);
  R.disc(cx + 54, cy + 33, 6, PAL_GREEN); R.ring(cx + 54, cy + 33, 6, 2, PAL_INK);
  R.roundRect(cx + 64, cy, 64, 96, 0, PAL_WHITE, PAL_INK, 2); R.thumb(cx + 66, cy + 4, thumbs[0], 1); R.text("1", cx + 96, cy + 58, 22);
  R.roundRect(cx, cy + 104, 64, 96, 8, PAL_PAPER, PAL_GREY, 2);   // an empty slot
  drawIcon(R, Icon::Plus, cx + 32, cy + 130, 30, PAL_GREY); R.text("3", cx + 32, cy + 162, 22, PAL_GREY);
  R.roundRect(cx + 64, cy + 104, 64, 96, 0, PAL_WHITE, PAL_INK, 2); R.thumb(cx + 66, cy + 108, thumbs[2], 1); R.text("4", cx + 96, cy + 162, 22);
  drawIcon(R, Icon::Camera, cx + 64 + 12, cy + 104 + 12, 14);   // made from paper
  R.roundRect(cx + 78, cy + 192, 28, 24, 6, PAL_RED, PAL_INK, 2); drawIcon(R, Icon::Trash, cx + 92, cy + 204, 16);   // delete mode badge
}

// ---------------------------------------------------------------------------------------------------------------------
static void pageFx() {
  R.clearPaper();
  drawLevelScene(LV, 0, 255);
  // particles of the four kinds, frozen at different ages
  const uint16_t cols[4] = {PAL_YELLOW, PAL_RED, PAL_BLUE, PAL_GREEN};
  for (int k = 0; k < 4; k++) {
    for (int i = 0; i < 12; i++) {
      const float a = i * 0.52f + k, d = 14 + (i % 4) * 5, x = 50 + k * 72 + std::cos(a) * d, y = 60 + std::sin(a) * d;
      const uint16_t col = cols[(i + k) % 4];
      if (k == 0) { const float q[8] = {x - 2, y - 2, x + 2, y - 2, x + 2, y + 2, x - 2, y + 2}; R.polygon(q, 4, col, uint8_t(255 - i * 12)); }
      else if (k == 1) R.disc(x, y, 3, col, uint8_t(255 - i * 12));
      else if (k == 2) { if (i < 4) R.ring(50 + k * 72, 60, 3 + i * 5.5f, 2, col, 0, 6.2831853f, uint8_t(255 - i * 50)); }
      else { const float c = std::cos(a * 3), s = std::sin(a * 3); const float q[8] = {x - 4 * c + 2 * s, y - 4 * s - 2 * c, x + 4 * c + 2 * s, y + 4 * s - 2 * c, x + 4 * c - 2 * s, y + 4 * s + 2 * c, x - 4 * c - 2 * s, y - 4 * s + 2 * c}; R.polygon(q, 4, col); }
    }
  }
  // toast
  R.roundRectA(60, 160, 200, 22, 11, PAL_INK, 255, -1); R.text("Cleared (undo brings it back)", 160, 171, 11, PAL_WHITE);
  R.roundRectA(60, 186, 200, 22, 11, PAL_INK, 110, -1);
  // flash and dim in quarters
  R.fillRectA(0, 212, 320, 28, PAL_WHITE, 0);
  R.g().fillRect(0, 214, 320, 26, PAL_PAPER);
  R.text("flash 0.5 / dim 0.5 below", 160, 223, 8, PAL_GREY);
}
static void pageFx2() {   // the pause menu: dimmed level, a card, six cells, a hold ring
  drawLevelScene(LV, 0, 255);
  R.ball(150, 100, 0.5f, 1, 1, Face::Happy);
  // quarters: flash white, flash green, dim, ink 0.62 + card (the pause menu)
  static Level dummy; (void)dummy;
  R.flash(PAL_PAPER, 0.9f * (0.5f + 0.5f * std::sin(tsec * 0)));
  R.dim(0.62f);
  R.roundRect(13, 53, 294, 166, 12, PAL_WHITE, PAL_INK, 3);
  const Icon ics[6] = {Icon::Play, Icon::Trash, Icon::Grid, Icon::Sound2, Icon::Home, Icon::Pencil};
  for (int i = 0; i < 6; i++) {
    const int x = 16 + (i % 3) * 96, y = 56 + (i / 3) * 80;
    R.g().fillRect(x, y, 96, 80, i == 0 ? PAL_GREEN : PAL_WHITE); R.g().drawRect(x, y, 96, 80, PAL_INK);
    drawIcon(R, ics[i], x + 48, y + 40, 44);
  }
  R.ring(16 + 96 + 48, 56 + 40, 34, 4, PAL_GREEN, -1.5708f, 4.2f);
}

static void pagePictures() {
  R.straight565(pageFrame, 90);                         // a straightened page at ~35 %, full 320 x 192
  R.frame565(camFrame, 32, 0, 256, 192);                // the live preview, 256 x 192
  R.roundRectA(58, 35, 204, 122, 6, PAL_INK, 0, PAL_WHITE, 3);
  R.g().fillRect(0, 192, 320, 48, PAL_PAPER_DOT);
  R.frame565(camFrame, 4, 196, 64, 44, 255); R.frame565(camFrame, 74, 196, 64, 44, 120); R.frame565(camFrame, 144, 196, 64, 44, 60);   // dim 255 / 120 / 60
  R.fillRectA(214, 196, 102, 40, PAL_RED, 128);
  R.text("fillRectA", 265, 216, 14, PAL_WHITE);
}

// ---------------------------------------------------------------------------------------------------------------------
void setup() {
  M5.begin();
  R.begin();
  buildLevels(); buildPictures();
  std::printf("gallery ready: %d pages\n", int(P_COUNT));
}

static void drawPager() {
  for (int i = 0; i < P_COUNT; i++) R.g().fillRect(318 - (P_COUNT - i) * 5, 2, 3, 3, i == page ? PAL_RED : PAL_GREY);
}

void loop() {
  M5.update();
  now = millis(); tsec = now / 1000.0f;
  const auto t = M5.Touch.getDetail();
  InputState in;
  in.touch.down = t.down; in.touch.pressed = t.pressed; in.touch.released = t.released; in.touch.x = t.x; in.touch.y = t.y;
  if (t.pressed) { downAt = now; in.touch.startX = t.x; in.touch.startY = t.y; }
  in.touch.heldMs = t.down ? now - downAt : 0;
  static bool pagerTouch = false;
  if (t.pressed) pagerTouch = false;
  if (t.pressed && t.y < 22 && t.x > 296) { pagerTouch = true; page = (page + 1) % P_COUNT; std::printf("page %d\n", page); }
  else if (t.pressed && t.y < 22 && t.x < 24 && page != P_BUTTONS) { pagerTouch = true; page = (page + P_COUNT - 1) % P_COUNT; std::printf("page %d\n", page); }
  if (pagerTouch) in.touch = TouchState{};   // a page-turn tap is not a tap on the page
  if (page == P_BUTTONS) tickButtons(in);
  switch (page) {
    case P_ICONS_SMALL: pageIconsSmall(); break;
    case P_ICONS_BIG1: pageIconsBig(0); break;
    case P_ICONS_BIG2: pageIconsBig(1); break;
    case P_BUTTONS: pageButtons(); break;
    case P_TEXT: pageText(); break;
    case P_TEXT2: pageText2(); break;
    case P_FONTS: pageFonts(); break;
    case P_LEVEL: pageLevel(); break;
    case P_LEVEL_FX: pageLevelFx(); break;
    case P_THUMBS: pageThumbs(); break;
    case P_FX: pageFx(); break;
    case P_MENU: pageFx2(); break;
    case P_PICTURES: pagePictures(); break;
  }
  drawPager();
  R.present();
}
