// Scribble Run - day-one hardware check (MIT).   docs/build-plan.md Steps 1-3.
//
// Flash this the day the CoreS3 arrives. Tap the right half of the bottom bar for the next page, the left half for
// the previous one. Every page also prints its numbers to the serial monitor (pio device monitor) so they can be
// copied into docs/hardware-notes.md.
//
//   0 INFO    palette, memory, flash                     (Step 1: colours look right?)
//   1 TOUCH   6 x 4 grid of 48 px targets, hit rate       (Step 1: can he hit 48 px?)
//   2 SPEED   display push timings                       (Step 1: is dirty-rect rendering needed?)
//   3 TILT    raw accelerometer, axis/sign picker, bar    (Step 2: which axis rolls the ball?)
//   4 BALL    the real physics on a built-in level        (Step 3: does it feel right?)
//   5 SOUND   tones                                      (Step 8 preview)
//
// The game itself is NOT here yet; this is a measuring tool built on the same portable core (lib/core).
#include <M5Unified.h>
#include "config.h"
#include "level.h"
#include "physics.h"

using namespace scribble;

static M5Canvas frame(&M5.Display);       // full-screen sprite in PSRAM (the "double-buffered sprite" of the design)
static M5Canvas scene(&M5.Display);       // the level, drawn once
static int page = 0;
static const int NPAGES = 6;
static const char* const NAMES[NPAGES] = { "INFO", "TOUCH", "SPEED", "TILT", "BALL", "SOUND" };

// ---- tilt: which accelerometer axis is "left-right", and its sign (picked on the TILT page) -----------------
static int tiltAxis = 0;    // 0 = x, 1 = y, 2 = z
static int tiltSign = 1;
static double tiltSmoothed = 0;
static float acc[3] = { 0, 0, 0 };

static void readImu() {
  if (M5.Imu.update()) { auto d = M5.Imu.getImuData(); acc[0] = d.accel.x; acc[1] = d.accel.y; acc[2] = d.accel.z; }
}
/** Dead-zone, linear to TILT_FULL, low-pass: the same maths as sim/src/input.ts. Call once per 60 Hz tick. */
static double tiltTick() {
  const double ax = tiltSign * acc[tiltAxis];
  double mag = (std::fabs(ax) - TILT_DEADZONE) / (TILT_FULL - TILT_DEADZONE);
  mag = mag < 0 ? 0 : mag > 1 ? 1 : mag;
  const double target = (ax > 0 ? 1 : ax < 0 ? -1 : 0) * mag;
  tiltSmoothed += (target - tiltSmoothed) * TILT_SMOOTH;
  if (std::fabs(tiltSmoothed) < 0.002) tiltSmoothed = 0;
  return tiltSmoothed;
}

// ---- small drawing helpers ------------------------------------------------------------------------------------
static void bar(const char* left) {
  frame.fillRect(0, 192, 320, 48, PAL_WHITE); frame.fillRect(0, 192, 320, 2, PAL_INK); frame.fillRect(159, 192, 2, 48, PAL_INK);
  frame.setTextColor(PAL_INK); frame.setFont(&fonts::FreeSansBold9pt7b); frame.setTextDatum(middle_center);
  frame.drawString(left, 80, 216); frame.drawString("next >", 240, 216);
  frame.setTextDatum(top_left);
}
static void title(const char* t) { frame.setFont(&fonts::FreeSansBold12pt7b); frame.setTextColor(PAL_INK); frame.setCursor(8, 6); frame.print(t); frame.setFont(&fonts::Font2); }

// ---- page 0: INFO ---------------------------------------------------------------------------------------------
static void pageInfo() {
  frame.fillScreen(PAL_PAPER); title("Scribble Run: hardware check");
  const uint16_t pal[9] = { PAL_PAPER, PAL_PAPER_DOT, PAL_INK, PAL_RED, PAL_BLUE, PAL_GREEN, PAL_YELLOW, PAL_WHITE, PAL_GREY };
  for (int i = 0; i < 9; i++) { frame.fillRect(8 + i * 34, 40, 30, 30, pal[i]); frame.drawRect(8 + i * 34, 40, 30, 30, PAL_INK); }
  frame.setCursor(8, 82);
  frame.printf("PSRAM %u KB (free %u KB)\nheap free %u KB\nflash %u MB\ndisplay %d x %d\n", unsigned(ESP.getPsramSize() / 1024), unsigned(ESP.getFreePsram() / 1024),
               unsigned(ESP.getFreeHeap() / 1024), unsigned(ESP.getFlashChipSize() >> 20), int(M5.Display.width()), int(M5.Display.height()));
  frame.print("Paper should look cream, not white;\nink nearly black. Tap 'next >' to continue.");
  bar("(first page)");
}

// ---- page 1: TOUCH --------------------------------------------------------------------------------------------
static int target = 0, hits = 0, misses = 0; static int lastX = -1, lastY = -1; static uint32_t samples = 0, rateT0 = 0, rate = 0;
static void pageTouch(const m5::touch_detail_t& t) {
  if (t.isPressed() && t.y < 192) { lastX = t.x; lastY = t.y; samples++; }
  if (millis() - rateT0 >= 1000) { rate = samples; samples = 0; rateT0 = millis(); }
  if (t.wasPressed() && t.y < 192) {
    const int col = (t.x - 16) / 48, row = t.y / 48;
    if (t.x >= 16 && col < 6 && row < 4 && row * 6 + col == target) { hits++; target = (target + 7) % 24; M5.Speaker.tone(1200, 30); }
    else { misses++; M5.Speaker.tone(300, 60); }
    Serial.printf("touch: hits %d misses %d rate %u Hz\n", hits, misses, unsigned(rate));
  }
  frame.fillScreen(PAL_PAPER);
  for (int i = 0; i < 24; i++) { const int x = 16 + (i % 6) * 48, y = (i / 6) * 48; frame.fillRect(x + 2, y + 2, 44, 44, i == target ? PAL_GREEN : PAL_WHITE); frame.drawRect(x + 2, y + 2, 44, 44, PAL_INK); }
  if (lastX >= 0) frame.fillCircle(lastX, lastY, 4, PAL_RED);
  frame.setTextColor(PAL_INK); frame.setCursor(4, 176);
  frame.printf("hit the GREEN square   hits %d  misses %d  (%d%%)  %u Hz", hits, misses, hits + misses ? hits * 100 / (hits + misses) : 0, unsigned(rate));
  bar("< back");
}

// ---- page 2: SPEED --------------------------------------------------------------------------------------------
static char speedText[256] = "Tap the screen to measure.";
static void runSpeed() {
  M5Canvas small(&M5.Display); small.setPsram(false); small.createSprite(40, 40); small.fillScreen(PAL_RED);
  const int N = 10; uint32_t t0;
  t0 = micros(); for (int i = 0; i < N; i++) { frame.pushSprite(0, 0); M5.Display.waitDMA(); } const float full = (micros() - t0) / 1000.0f / N;
  t0 = micros(); for (int i = 0; i < N * 10; i++) { small.pushSprite(140, 100); M5.Display.waitDMA(); } const float rect = (micros() - t0) / 1000.0f / (N * 10);
  t0 = micros(); for (int i = 0; i < N * 10; i++) { M5.Display.setClipRect(140, 100, 40, 40); frame.pushSprite(0, 0); M5.Display.clearClipRect(); M5.Display.waitDMA(); }
  const float clip = (micros() - t0) / 1000.0f / (N * 10);
  snprintf(speedText, sizeof speedText, "full 320x240 push from PSRAM: %.1f ms (%.0f fps)\n40x40 push from SRAM: %.2f ms\n40x40 clip of the PSRAM sprite: %.2f ms\n\nDesign assumed ~31 ms full, <1.5 ms for 40x40.", full, 1000.0f / full, rect, clip);
  Serial.printf("speed: full %.2f ms, 40x40 sram %.3f ms, 40x40 clip %.3f ms\n", full, rect, clip);
  small.deleteSprite();
}
static void pageSpeed(const m5::touch_detail_t& t) {
  if (t.wasPressed() && t.y < 192) { frame.fillScreen(PAL_YELLOW); runSpeed(); }
  frame.fillScreen(PAL_PAPER); title("Display speed"); frame.setCursor(8, 40); frame.print(speedText); bar("< back");
}

// ---- page 3: TILT ---------------------------------------------------------------------------------------------
static void pageTilt(const m5::touch_detail_t& t) {
  if (t.wasPressed() && t.y < 192) { if (t.x < 160) tiltAxis = (tiltAxis + 1) % 3; else tiltSign = -tiltSign; }
  const double tilt = tiltTick();
  frame.fillScreen(PAL_PAPER); title("Tilt sensor (BMI270)");
  frame.setCursor(8, 36); frame.printf("ax %+.2f g   ay %+.2f g   az %+.2f g", acc[0], acc[1], acc[2]);
  frame.setCursor(8, 54); frame.printf("rolling axis: %c%c   (tap left: axis, right: sign)", tiltSign > 0 ? '+' : '-', "xyz"[tiltAxis]);
  frame.drawRect(20, 90, 280, 30, PAL_INK); frame.fillRect(159, 86, 2, 38, PAL_INK);
  const int w = int(tilt * 138); frame.fillRect(w >= 0 ? 160 : 160 + w, 92, std::abs(w), 26, std::fabs(tilt) >= 0.999 ? PAL_GREEN : PAL_BLUE);
  frame.setCursor(8, 132); frame.printf("tilt %+.2f   Tilt the RIGHT edge DOWN: the bar must grow right.", tilt);
  frame.setCursor(8, 150); frame.printf("upright check: gravity mostly on %c", std::fabs(acc[0]) > std::fabs(acc[1]) ? (std::fabs(acc[0]) > std::fabs(acc[2]) ? 'x' : 'z') : (std::fabs(acc[1]) > std::fabs(acc[2]) ? 'y' : 'z'));
  static uint32_t lastLog = 0; if (millis() - lastLog > 500) { lastLog = millis(); Serial.printf("imu: ax %+.3f ay %+.3f az %+.3f  tilt %+.3f\n", acc[0], acc[1], acc[2], tilt); }
  bar("< back");
}

// ---- page 4: BALL (the real physics) ---------------------------------------------------------------------------
static Level level; static World world; static bool ballReady = false; static uint32_t lastUs = 0; static double accT = 0;
static uint32_t physUsSum = 0, physTicks = 0, frames = 0, fpsT0 = 0; static float fps = 0, physUs = 0;
static void drawLevelInto(M5Canvas& c) {
  c.fillScreen(PAL_PAPER);
  for (int y = 8; y < WORLD_H; y += 16) for (int x = 8; x < WORLD_W; x += 16) c.drawPixel(x, y, PAL_PAPER_DOT);
  for (int y = 0; y < WORLD_H; y++) for (int x = 0; x < WORLD_W; x++) {
    if (getBit(level.ground, x, y)) c.drawPixel(x, y, PAL_INK);
    else if (getBit(level.spike, x, y)) c.drawPixel(x, y, ((x + y) & 3) == 0 ? PAL_WHITE : PAL_RED);
    else if (getBit(level.bouncy, x, y)) c.drawPixel(x, y, ((x & 3) == 0 && (y & 3) == 0) ? PAL_WHITE : PAL_BLUE);
  }
  c.drawLine(level.flagX, level.flagY, level.flagX, level.flagY - 24, PAL_INK);
  c.fillTriangle(level.flagX + 1, level.flagY - 24, level.flagX + 21, level.flagY - 17, level.flagX + 1, level.flagY - 10, PAL_GREEN);
}
static void pageBall(const m5::touch_detail_t& t) {
  if (!ballReady) {
    starterLevel(level); paintLine(level, MAT_GROUND, 72, 116, 120, 130, 4); paintLine(level, MAT_GROUND, 120, 130, 200, 124, 4); paintLine(level, MAT_GROUND, 200, 124, 248, 116, 4);
    fillRect(level, MAT_SPIKE, 110, 160, 160, 170); fillRect(level, MAT_BOUNCY, 170, 160, 215, 170);
    drawLevelInto(scene); newWorld(world, level); ballReady = true; lastUs = micros();
  }
  // fixed 60 Hz physics with an accumulator (docs/system-design.md 6.1)
  const uint32_t now = micros(); accT += (now - lastUs) / 1e6; lastUs = now; if (accT > 0.1) accT = 0.1;
  bool jump = t.wasPressed() && t.y < 192 && t.x >= JUMP_EDGE_MARGIN && t.x <= SCREEN_W - JUMP_EDGE_MARGIN;
  while (accT >= 1.0 / PHYSICS_HZ) {
    const double tilt = tiltTick();
    const uint32_t p0 = micros();
    const uint32_t ev = physicsStep(world, level, { tilt, jump });
    physUsSum += micros() - p0; physTicks++; jump = false;
    if (ev & EV_JUMP) M5.Speaker.tone(550, 60);
    if (ev & EV_BOUNCE) M5.Speaker.tone(700, 120);
    if (ev & EV_POP) M5.Speaker.tone(200, 160);
    if (ev & EV_WIN) { M5.Speaker.tone(784, 90); }
    if (world.state == ST_CELEBRATING && world.stateTime > 1.5) newWorld(world, level);
    accT -= 1.0 / PHYSICS_HZ;
  }
  scene.pushSprite(&frame, 0, 0);
  frame.fillRect(0, 192, 320, 48, PAL_PAPER);
  if (world.state != ST_DYING) { frame.fillCircle(int(world.ball.x), int(world.ball.y), BALL_RADIUS - 1, PAL_YELLOW); frame.drawCircle(int(world.ball.x), int(world.ball.y), BALL_RADIUS - 1, PAL_INK); }
  frames++; if (millis() - fpsT0 >= 1000) { fps = frames * 1000.0f / (millis() - fpsT0); frames = 0; fpsT0 = millis(); physUs = physTicks ? float(physUsSum) / physTicks : 0; physUsSum = physTicks = 0;
    Serial.printf("ball: %.1f fps (full-screen pushes), physics %.0f us/tick\n", fps, physUs); }
  frame.setTextColor(PAL_INK); frame.setFont(&fonts::Font2); frame.setCursor(4, 2); frame.printf("tilt to roll, tap to jump   %.0f fps   physics %.0f us/tick", fps, physUs);
  bar("< back");
}

// ---- page 5: SOUND ----------------------------------------------------------------------------------------------
static int volume = 64;
static void pageSound(const m5::touch_detail_t& t) {
  if (t.wasPressed() && t.y < 192) {
    if (t.y < 96) { const int f[4] = { 523, 659, 784, 1047 }; for (int i = 0; i < 4; i++) { M5.Speaker.tone(f[i], 90); delay(100); } }
    else { volume = volume >= 192 ? 32 : volume * 2; M5.Speaker.setVolume(volume); M5.Speaker.tone(1000, 80); }
  }
  frame.fillScreen(PAL_PAPER); title("Speaker");
  frame.fillRect(8, 40, 304, 50, PAL_GREEN); frame.setTextColor(PAL_INK); frame.setCursor(20, 58); frame.print("tap: the win jingle (C E G C)");
  frame.fillRect(8, 110, 304, 50, PAL_WHITE); frame.drawRect(8, 110, 304, 50, PAL_INK); frame.setCursor(20, 128); frame.printf("tap: volume %d / 255 (low default ~64?)", volume);
  bar("< back");
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  M5.Display.setRotation(1);
  M5.Speaker.setVolume(volume);
  frame.setPsram(true); frame.setColorDepth(16);
  if (!frame.createSprite(SCREEN_W, SCREEN_H)) Serial.println("could not allocate the frame sprite in PSRAM");
  scene.setPsram(true); scene.setColorDepth(16); scene.createSprite(WORLD_W, WORLD_H);
  rateT0 = fpsT0 = millis();
  Serial.println("Scribble Run hardware check. Pages: INFO TOUCH SPEED TILT BALL SOUND");
}

void loop() {
  M5.update(); readImu();
  const auto t = M5.Touch.getDetail();
  if (t.wasPressed() && t.y >= 192) {                              // bottom bar: left = back, right = next
    page = (page + (t.x >= 160 ? 1 : NPAGES - 1)) % NPAGES;
    Serial.printf("page %s\n", NAMES[page]); M5.Speaker.tone(1000, 15); ballReady = false;
    return;
  }
  switch (page) {
    case 0: pageInfo(); break; case 1: pageTouch(t); break; case 2: pageSpeed(t); break;
    case 3: pageTilt(t); break; case 4: pageBall(t); break; case 5: pageSound(t); break;
  }
  frame.pushSprite(0, 0);
  if (page != 4) delay(16);
}
