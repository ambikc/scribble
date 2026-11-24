// unit_main.cpp - unit tests of the app layer that need M5GFX / the emulated M5Unified (env:emu_unit).   MIT licensed.
//
//   pio run -e emu_unit && SDL_VIDEODRIVER=dummy .pio/build/emu_unit/program        (exit code 0 = every check passed)
//
// The real src/app/ui.cpp, input.cpp and renderer.cpp, compiled with the emulator (emu/run_all.sh runs this). Covered:
//   * UI: a press and a release that arrive in the SAME tick on a chip fire the chip and still count as a button touch
//     (touchOnButton / touchStartedOnButton), so a screen does not also treat the tap as a stroke or a jump;
//   * Input: a second finger ends the touch with a release edge and the screen is ignored until every finger is up; when the first
//     finger lifts, getDetail(0) becomes the second finger (another id), which must not continue the touch;
//   * Renderer::flash/dim: the fast paths give exactly the pixels of the plain per-pixel blend.
#include <M5Unified.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unistd.h>
#include "config.h"
#include "input.h"
#include "renderer.h"
#include "ui.h"

using namespace scribble;

static int fails = 0, checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++fails; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static Renderer R;

// ---------------------------------------------------------------------------------------------------------------------
static InputState touchState(bool down, bool pressed, bool released, int x, int y) {
  InputState s; s.touch.down = down; s.touch.pressed = pressed; s.touch.released = released; s.touch.x = x; s.touch.y = y;
  s.touch.startX = x; s.touch.startY = y; return s;
}
static void addBackChip(UI& ui) {
  Btn b; b.id = "back"; b.x = 4; b.y = 4; b.w = 40; b.h = 40; b.kind = BtnKind::Chip; b.icon = Icon::Back; b.hit = 4;
  ui.add(b);
}
/** What game_draw / game_play / game_paper do with a touch: a field touch only if it began on no button. */
static bool fieldTouch(const UI& ui, const InputState& in) { return in.touch.pressed && !ui.touchOnButton && in.touch.y < SCREEN_H - BAR_H; }

static void testUiSameTickTap() {
  UI ui;
  // a whole tap on the chip inside one tick: pressed and released, not down
  ui.begin(); addBackChip(ui);
  InputState in = touchState(false, true, true, 20, 20);
  const char* hit = ui.update(in);
  CHECK(hit && !std::strcmp(hit, "back"));
  CHECK(ui.touchOnButton);
  CHECK(ui.touchStartedOnButton);
  CHECK(!fieldTouch(ui, in));                           // the bug: this tap also counted as a stroke / jump
  // the next tick, no touch: the old meaning of touchOnButton ("while the current touch began on a button") is back to false
  ui.begin(); addBackChip(ui);
  in = touchState(false, false, false, 20, 20);
  CHECK(ui.update(in) == nullptr);
  CHECK(!ui.touchOnButton);
  CHECK(ui.touchStartedOnButton);                       // describes the last touch until the next press
  // a same-tick tap on the field is a field touch and fires nothing
  ui.begin(); addBackChip(ui);
  in = touchState(false, true, true, 160, 120);
  CHECK(ui.update(in) == nullptr);
  CHECK(!ui.touchOnButton && !ui.touchStartedOnButton);
  CHECK(fieldTouch(ui, in));
}

static void testUiNormalTap() {
  UI ui;
  ui.begin(); addBackChip(ui);
  InputState in = touchState(true, true, false, 20, 20);
  CHECK(ui.update(in) == nullptr);                      // fires on release
  CHECK(ui.touchOnButton && !fieldTouch(ui, in));
  ui.begin(); addBackChip(ui);
  in = touchState(false, false, true, 20, 20);
  const char* hit = ui.update(in);
  CHECK(hit && !std::strcmp(hit, "back"));
  CHECK(ui.touchOnButton);                              // still valid in the release tick
  ui.begin(); addBackChip(ui);
  in = touchState(true, true, false, 100, 100);         // next touch on the field
  CHECK(ui.update(in) == nullptr);
  CHECK(!ui.touchOnButton && !ui.touchStartedOnButton && fieldTouch(ui, in));
}

// ---------------------------------------------------------------------------------------------------------------------
struct Step { bool down, pressed, released; int x, y; };
static uint32_t nowMs = 0;
static Step stepInput(Input& in) {
  M5.update(); nowMs += 17;
  in.sample(nowMs);
  const InputState s = in.poll(nowMs);
  in.consumeEdges();
  return {s.touch.down, s.touch.pressed, s.touch.released, s.touch.x, s.touch.y};
}

static void testInputOneFinger() {
  Input in; in.begin();
  emu::setFinger(0, true, 50, 60);
  Step s = stepInput(in); CHECK(s.pressed && s.down && s.x == 50 && s.y == 60);
  emu::setFinger(0, true, 80, 60);
  s = stepInput(in); CHECK(!s.pressed && s.down && s.x == 80);
  emu::setFinger(0, false);
  s = stepInput(in); CHECK(s.released && !s.down);
  s = stepInput(in); CHECK(!s.pressed && !s.released && !s.down);
}

static void testInputSecondFingerEndsTheTouch() {
  Input in; in.begin();
  emu::setFinger(0, true, 50, 60);
  Step s = stepInput(in); CHECK(s.pressed && s.down);
  emu::setFinger(0, true, 90, 60);
  s = stepInput(in); CHECK(s.down && s.x == 90);
  CHECK(M5.Touch.getCount() == 1);
  emu::setFinger(1, true, 250, 180);                    // a second finger lands
  s = stepInput(in);
  CHECK(M5.Touch.getCount() == 2);
  CHECK(s.released && !s.down);                         // the touch ends here ...
  CHECK(s.x == 90 && s.y == 60);                        // ... at the first finger's last point
  emu::setFinger(0, false);                             // the first finger lifts: getDetail(0) is now the second finger
  M5.update(); nowMs += 17;
  CHECK(M5.Touch.getDetail().isPressed() && M5.Touch.getDetail().id == 1 && M5.Touch.getDetail().x == 250);
  in.sample(nowMs); s = Step{}; { const InputState st = in.poll(nowMs); in.consumeEdges(); s = {st.touch.down, st.touch.pressed, st.touch.released, st.touch.x, st.touch.y}; }
  CHECK(!s.pressed && !s.down && !s.released);          // the bug: the second finger silently continued the stroke
  emu::setFinger(1, true, 200, 150);
  s = stepInput(in); CHECK(!s.pressed && !s.down && s.x == 90);   // still ignored while any finger is down
  emu::setFinger(1, false);
  s = stepInput(in); CHECK(!s.pressed && !s.released && !s.down);
  s = stepInput(in); CHECK(!s.pressed && !s.down);
  emu::setFinger(0, true, 30, 40);                      // every finger was up: a new touch works again
  s = stepInput(in); CHECK(s.pressed && s.down && s.x == 30 && s.y == 40);
  emu::setFinger(0, false);
  s = stepInput(in); CHECK(s.released);
  stepInput(in);
}

static void testInputFirstLiftsAsSecondLands() {
  // in one update finger 0 lifts and finger 1 lands: getCount() is 2 (one down, one just released), getDetail(0) is finger 1
  Input in; in.begin();
  emu::setFinger(0, true, 50, 60);
  Step s = stepInput(in); CHECK(s.pressed);
  emu::setFinger(0, false); emu::setFinger(1, true, 200, 100);
  s = stepInput(in); CHECK(s.released && !s.down && s.x == 50);
  emu::setFinger(1, true, 210, 100);
  s = stepInput(in); CHECK(!s.pressed && !s.down);
  emu::setFinger(1, false);
  stepInput(in); stepInput(in);
  emu::setFinger(1, true, 120, 90);                     // later a lone finger with id 1 is an ordinary touch
  s = stepInput(in); CHECK(s.pressed && s.x == 120);
  emu::setFinger(1, false);
  s = stepInput(in); CHECK(s.released);
  stepInput(in);
}

static void testInputTwoFingersAtOnce() {
  Input in; in.begin();
  emu::setFinger(0, true, 50, 60); emu::setFinger(1, true, 250, 60);
  Step s = stepInput(in); CHECK(!s.pressed && !s.down);
  emu::setFinger(1, false);
  s = stepInput(in); CHECK(!s.pressed && !s.down);    // finger 0 alone now, but it started as a two-finger touch: ignored
  emu::setFinger(0, false);
  stepInput(in); stepInput(in);
  emu::setFinger(0, true, 70, 80);
  s = stepInput(in); CHECK(s.pressed);
  emu::setFinger(0, false); stepInput(in); stepInput(in);
}

// ---------------------------------------------------------------------------------------------------------------------
/** The blend Renderer::flash used before the fast paths (32 alpha levels, two-lane trick), per pixel. */
static uint16_t refMix(uint16_t dst, uint16_t src, int a) {
  const uint32_t k = uint32_t(a + 1) >> 3;
  const uint32_t S = (src | (uint32_t(src) << 16)) & 0x07E0F81Fu, D = (dst | (uint32_t(dst) << 16)) & 0x07E0F81Fu;
  uint32_t r = ((S * k + D * (32 - k)) >> 5) & 0x07E0F81Fu;
  return uint16_t(r | (r >> 16));
}
static uint16_t sw(uint16_t c) { return uint16_t((c << 8) | (c >> 8)); }

static void testFlashMatchesThePlainBlend() {
  const int N = SCREEN_W * SCREEN_H;
  std::vector<uint16_t> start(N), want(N);
  uint32_t rng = 12345;
  for (int i = 0; i < N; i++) { rng = rng * 1103515245u + 12345u; start[i] = uint16_t(rng >> 8); }
  const float alphas[] = {0.0f, 0.001f, 0.02f, 0.026f, 0.0275f, 0.03f, 0.04f, 0.2f, 0.5f, 0.97f, 0.999f, 1.0f, 1.7f, -0.3f};
  for (float al : alphas) {
    for (int pass = 0; pass < 2; pass++) {
      const uint16_t color = pass ? PAL_INK : PAL_PAPER;
      std::memcpy(R.px(), start.data(), N * 2);
      float a01 = al > 1 ? 1 : al;
      const int a = int(a01 * 255 + 0.5f);
      for (int i = 0; i < N; i++) want[i] = al <= 0 ? start[i] : a >= 255 ? sw(color) : sw(refMix(sw(start[i]), color, a));
      if (pass) R.dim(al); else R.flash(color, al);
      const bool same = std::memcmp(R.px(), want.data(), N * 2) == 0;
      if (!same) std::printf("  flash/dim alpha %.4f pass %d differs\n", al, pass);
      CHECK(same);
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------
void setup() {
  static char scriptPath[] = "/tmp/scribble_emu_unit_XXXXXX";
  const int fd = ::mkstemp(scriptPath);                 // an empty script: virtual clock, scripted fingers (emu::setFinger)
  if (fd >= 0) ::close(fd);
  ::setenv("EMU_SCRIPT", scriptPath, 1);
  M5.begin();
  if (!R.begin()) { std::printf("FAIL renderer.begin\n"); std::exit(1); }
  testUiSameTickTap();
  testUiNormalTap();
  testInputOneFinger();
  testInputSecondFingerEndsTheTouch();
  testInputFirstLiftsAsSecondLands();
  testInputTwoFingersAtOnce();
  testFlashMatchesThePlainBlend();
  std::remove(scriptPath);
  std::printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
  std::fflush(stdout);
  std::_Exit(fails ? 1 : 0);
}
void loop() {}
