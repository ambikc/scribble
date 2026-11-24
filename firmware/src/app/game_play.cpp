// game_play.cpp - the Play screen and the Win screen.   Port of tickPlay/drawPlayScene/drawPlay/tickWin/drawWin of sim/src/modes.ts.
// OWNER: Play/Win agent. State: game_play.h (PlayState). Screens and effects: docs/screens.md sections 7 and 9, docs/game-design.md section 7.
//
//   tickPlay   buttons (Edit hold, menu, Restart hold, Jump pad), the jump edge, ONE physicsStep per tick, events -> sound + effects
//   drawPlay   cached static scene + puffs + flag + start ring + ball + particles + tilt hint + red flash, then the buttons
//   tickWin / drawWin   the three bottom buttons, confetti, "YOU WIN!", the bouncing star, the happy ball on its line
//
// Speed: the level never changes while playing, so paper + the three material layers are rendered ONCE per level into a full-frame
// copy (`PlayState::scene`, 150 KB in PSRAM) and each frame starts with a single memcpy of it. That is what clearPaper() already costs,
// so drawLayers() is paid once instead of 60 times a second. The scene is rebuilt whenever Play is entered with different layers.
#include "game.h"
#include <cmath>
#ifdef EMU
#include <chrono>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(ARDUINO) && defined(BOARD_HAS_PSRAM)
#include <esp_heap_caps.h>
#endif

namespace scribble {

namespace {
constexpr float DT = 1.0f / PHYSICS_HZ;
constexpr int BAR_Y = SCREEN_H - BAR_H;                 // 192
constexpr uint16_t C_WIN_RAYS = 0xDF9C;                 // #D9F3E4, the green-white of the win flash and the rays
constexpr uint16_t C_HINT = 0x5AA8;                     // #5B5646, the tilt-hint arrows
constexpr size_t SCENE_BYTES = size_t(SCREEN_W) * SCREEN_H * 2;
constexpr float PI_F = 3.14159265f;
constexpr float WIN_LOCK_S = 0.6f;                      // no button works for this long after the Win screen appears
constexpr float WIN_SAVE_DELAY_S = 0.7f;                // the deferred settings write

uint32_t rng = 0x2545F491u;
float frand() {                                         // effects only: a cheap xorshift in [0, 1)
  rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
  return (rng >> 8) * (1.0f / 16777216.0f);
}

uint16_t* allocScene() {
#if defined(ARDUINO) && defined(BOARD_HAS_PSRAM)
  return static_cast<uint16_t*>(heap_caps_malloc(SCENE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));   // never the scarce internal RAM
#else
  return static_cast<uint16_t*>(std::malloc(SCENE_BYTES));
#endif
}

uint32_t layersCrc(const Level& l) {
  return crc32(l.ground, 0, LAYER_BYTES) ^ (crc32(l.spike, 0, LAYER_BYTES) * 31u) ^ (crc32(l.bouncy, 0, LAYER_BYTES) * 131u);
}

#ifdef EMU
bool trace() { static const bool on = std::getenv("EMU_TRACE") != nullptr; return on; }
#endif
}  // namespace

// =====================================================================================================================
// effects shared with the core: the strong version of the weak Game::playFxStep (game_core.cpp)
// =====================================================================================================================
void Game::playFxStep(float dt) {
  auto dec = [dt](float& v) { v = v - dt > 0 ? v - dt : 0; };
  dec(play.fxStretch); dec(play.fxSquash); dec(play.fxShake); dec(play.fxRed); dec(play.fxFlash); dec(play.spawnRing);
  if (play.fxShake > 0) { r.shakeX = (frand() - 0.5f) * 4.0f; r.shakeY = (frand() - 0.5f) * 4.0f; }   // +-2 px for 0.15 s; present() applies it
  else { r.shakeX = 0; r.shakeY = 0; }                                                              // runs on every screen, so it can never stick
}

void flushSolved(Game& g) {
  if (!g.play.solvedPending) return;
  g.play.solvedPending = false;
  g.store.saveSettings();
#ifdef EMU
  std::printf("[win saved slot %d]\n", g.play.solvedSlot);
#endif
}

// =====================================================================================================================
// Play
// =====================================================================================================================
void Game::tickPlay(const InputState& in) {
  Btn back; back.holdMs = HOLD_MS;
  chipBack(&back, Icon::Pencil);                          // Edit: needs the hold
  chipMenu();
  Btn items[2];
  items[0].id = "restart"; items[0].icon = Icon::Restart; items[0].iconSize = 28; items[0].holdMs = HOLD_MS;
  items[1].id = "jump"; items[1].icon = Icon::Up; items[1].iconSize = 38; items[1].fill = PAL_GREEN; items[1].onPress = true;
  const int widths[2] = {120, 200};
  bar(items, widths, 2);

  if (screenT < 0.02f) {                                  // Play was just entered: is the cached scene still this level?
    const uint32_t crc = layersCrc(level);
    if (!play.haveCrc || crc != play.sceneCrc) { play.sceneOk = false; play.nPuffs = 0; play.sceneCrc = crc; play.haveCrc = true; }
  }

  const char* hit = ui.update(in);
  const TouchState& t = in.touch;
  if (hit && !std::strcmp(hit, "back")) {
#ifdef EMU
    std::printf("[play edit]\n");
#endif
    play.nPuffs = 0; flushSolved(*this); go(Screen::Draw); return;
  }
  if ((hit && !std::strcmp(hit, "menu")) || in.keyBack) {   // Esc: the emulator's stand-in for the chip
#ifdef EMU
    std::printf("[play menu]\n");
#endif
    menuFrom = Screen::Play; flushSolved(*this); go(Screen::Menu); return;
  }
  if (hit && !std::strcmp(hit, "restart")) {
    newWorld(play.world, level); play.spawnRing = 0.2f; audio.play(Sfx::Bloop);
#ifdef EMU
    std::printf("[play restart]\n");
#endif
  }

  // jump: a tap anywhere on the playfield (not on a button, not a thumb on the screen edge), the pad, or the key
  const bool gripSafe = t.startX >= JUMP_EDGE_MARGIN && t.startX <= SCREEN_W - JUMP_EDGE_MARGIN;
  const bool jump = (hit && !std::strcmp(hit, "jump")) || in.keyJump || (t.pressed && !ui.touchOnButton && t.y < BAR_Y && gripSafe);
  play.tiltHint = play.tiltHint - DT > 0 ? play.tiltHint - DT : 0;

  World& w = play.world;
  const uint32_t ev = physicsStep(w, level, StepInput{double(in.tilt), jump});
  const Ball& b = w.ball;
  const float bx = float(b.x), by = float(b.y);
#ifdef EMU
  if (trace() && int(screenT * 60 + 0.5f) % 6 == 0) std::printf("[trace %.2f x=%.1f y=%.1f vx=%.0f vy=%.0f st=%d]\n", screenT, bx, by, b.vx, b.vy, w.state);
#endif
  static const uint16_t cGrey[1] = {PAL_GREY}, cBlue[2] = {PAL_BLUE, PAL_WHITE}, cRed[2] = {PAL_RED, PAL_WHITE}, cGreen[1] = {PAL_GREEN};
  static const uint16_t cConf[4] = {PAL_RED, PAL_BLUE, PAL_YELLOW, PAL_GREEN};
  if (ev & EV_JUMP) { audio.play(Sfx::Jump); play.fxStretch = 0.1f; }
  if (ev & EV_LAND) { audio.play(Sfx::Land); play.fxSquash = 0.08f; spawn(3, bx, by + 8, cGrey, 1, 30, 0.3f, 2); }
  if (ev & EV_BOUNCE) { audio.play(Sfx::Bounce); play.fxStretch = 0.14f; spawn(4, bx, by, cBlue, 2, 90, 0.45f, 2, 1); }
  if (ev & EV_POP) {
    audio.play(Sfx::Pop); play.fxShake = 0.15f; play.fxRed = 0.1f; spawn(8, bx, by, cRed, 2, 120, 0.55f, 3);
    if (play.nPuffs == 5) { std::memmove(play.puffs, play.puffs + 1, sizeof(PlayState::Puff) * 4); play.nPuffs = 4; }
    play.puffs[play.nPuffs++] = {bx, by};
  }
  if (ev & EV_FELL) audio.play(Sfx::Whoosh);
  if (ev & EV_RESPAWN) { audio.play(Sfx::Bloop); play.spawnRing = 0.2f; spawn(1, float(level.startX), float(level.startY), cGreen, 1, 0, 0.35f, 0, 2); }
  if (ev & EV_WIN) {
    audio.play(Sfx::Win); play.fxFlash = 0.12f; play.fxFlashColor = C_WIN_RAYS; play.winT = 0;
    spawn(1, float(level.flagX), float(level.flagY - 12), cGreen, 1, 0, 0.5f, 0, 2);
    spawn(24, float(level.flagX), float(level.flagY - 12), cConf, 4, 150, 1.4f, 3, 3);
    if (slot >= 0 && slot < SLOTS) { store.settings.solvedMask |= 1u << slot; play.solvedPending = true; play.solvedSlot = slot; }   // written later (flushSolved)
#ifdef EMU
    std::printf("[win slot %d]\n", slot);
#endif
  }
  if (play.solvedPending && w.state != ST_CELEBRATING) flushSolved(*this);   // restarted during the celebration: no jingle to disturb any more
  if (w.state == ST_CELEBRATING) { play.winT += DT; if (play.winT >= WIN_SCREEN_DELAY) { play.winT = 0; go(Screen::Win); } }
}

/** paper + the three layers: from the cached copy when there is one, else drawn from scratch. */
static void drawStatic(Game& g) {
  PlayState& p = g.play;
#ifdef EMU
  static const bool noCache = std::getenv("EMU_NOCACHE") != nullptr;   // dev: compare the cost of the two paths (printed with EMU_TRACE)
  if (noCache) {
    const auto t0 = std::chrono::steady_clock::now();
    g.r.clearPaper(); g.r.drawLayers(g.level);
    if (trace()) { static double sum = 0; static int n = 0; sum += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
                   if (++n == 60) { std::printf("[perf uncached clearPaper+drawLayers %.0f us]\n", sum / n); sum = 0; n = 0; } }
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  struct Report { std::chrono::steady_clock::time_point t0; ~Report() {
    if (!trace()) return;
    static double sum = 0; static int n = 0; sum += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    if (++n == 60) { std::printf("[perf cached scene copy %.0f us]\n", sum / n); sum = 0; n = 0; } } } report{t0};
#endif
  if (p.scene && p.sceneOk) { std::memcpy(g.r.px(), p.scene, SCENE_BYTES); return; }
  g.r.clearPaper();
  g.r.drawLayers(g.level);
  if (!p.scene) p.scene = allocScene();
  if (p.scene) { std::memcpy(p.scene, g.r.px(), SCENE_BYTES); p.sceneOk = true; }
}

void drawPlayScene(Game& g) {
  Renderer& r = g.r; PlayState& p = g.play; const Level& lv = g.level; const World& w = p.world;
  drawStatic(g);
  for (int i = 0; i < p.nPuffs; i++) {                    // faint grey clouds where the ball popped
    const float x = p.puffs[i].x, y = p.puffs[i].y;
    r.disc(x - 5, y, 6, PAL_GREY, 140); r.disc(x + 4, y - 2, 7, PAL_GREY, 140); r.disc(x, y + 4, 5, PAL_GREY, 140);
  }
  const bool celebrating = w.state == ST_CELEBRATING;
  r.flag(float(lv.flagX), float(lv.flagY), celebrating ? 1.0f : 0.0f);
  if (p.spawnRing > 0) r.ring(float(lv.startX), float(lv.startY), 6 + (1 - p.spawnRing / 0.2f) * 12, 2, PAL_GREEN);
  if (w.state != ST_DYING) {
    const float hop = celebrating ? -std::fabs(std::sin(float(w.stateTime) * 9)) * 10 : 0;
    const float sx = p.fxSquash > 0 ? 1.25f : p.fxStretch > 0 ? 0.85f : 1, sy = p.fxSquash > 0 ? 0.75f : p.fxStretch > 0 ? 1.25f : 1;
    const float grow = p.spawnRing > 0 ? 1 - p.spawnRing / 0.2f : 1;
    r.ball(float(w.ball.x), float(w.ball.y) + hop, float(w.ball.angle), sx * (grow > 0.05f ? grow : 0.05f), sy * (grow > 0.05f ? grow : 0.05f),
           celebrating ? Face::Happy : Face::Normal);
  }
  g.drawParts();
  if (p.tiltHint > 0) {                                   // "<- tilt ->" arrows beside the ball on the very first play
    const float k = p.tiltHint > 0.5f ? 1.0f : p.tiltHint * 2, wob = std::sin(g.t * 6) * 4;
    for (int s = -1; s <= 1; s += 2) {
      const float x = float(w.ball.x) + s * (26 + wob), y = float(w.ball.y) - 26, fs = float(s);
      const float head[6] = {x - 6 * fs, y - 6, x, y, x - 6 * fs, y + 6};
      r.polyline(head, 3, 3, C_HINT, false, uint8_t(k * 255));
      r.line(x - 16 * fs, y, x, y, 3, C_HINT, uint8_t(k * 255));
    }
  }
  if (p.fxRed > 0) r.flash(PAL_RED, p.fxRed * 2.2f);
}

void Game::drawPlay() {
  drawPlayScene(*this);
  ui.draw(r, nowMs);
  if (play.fxFlash > 0) r.flash(play.fxFlashColor, play.fxFlash * 6);
}

// =====================================================================================================================
// Win
// =====================================================================================================================
void Game::tickWin(const InputState& in) {
  Btn items[3];
  items[0].id = "edit"; items[0].icon = Icon::Pencil; items[0].iconSize = 32;
  items[1].id = "again"; items[1].icon = Icon::Play; items[1].iconSize = 38; items[1].fill = PAL_GREEN;
  items[2].id = "new"; items[2].icon = Icon::Plus; items[2].iconSize = 34;
  const int widths[3] = {104, 112, 104};
  bar(items, widths, 3);
  const char* hit = ui.update(in);                        // always, so the UI keeps its press state; the result may be discarded below
  const TouchState& t = in.touch;
  // a touch that was already down when Win appeared, or that began within the first WIN_LOCK_S, never presses a button
  if (screenT < 0.02f) play.winStale = (t.down || t.released) && !t.pressed;
  if (t.pressed) play.winStale = screenT < WIN_LOCK_S;
  static const uint16_t cConf[4] = {PAL_RED, PAL_BLUE, PAL_YELLOW, PAL_GREEN};
  if (screenT < 4 && frand() < 0.35f) spawn(1, frand() * 320, -4, cConf, 4, 20, 3, 3, 3);   // confetti keeps falling for 4 s
  if (screenT >= WIN_SAVE_DELAY_S) flushSolved(*this);
  if (!hit || screenT < WIN_LOCK_S || play.winStale) return;
  flushSolved(*this);
#ifdef EMU
  std::printf("[win button %s]\n", hit);
#endif
  if (!std::strcmp(hit, "edit")) go(Screen::Draw);
  else if (!std::strcmp(hit, "again")) startPlay();
  else if (!std::strcmp(hit, "new")) {
    const int s = store.firstEmpty();                     // like modes.ts: the new level goes into the first free slot
    if (s < 0) { say("All 10 slots are full"); menus.deleteMode = false; go(Screen::Picker); }
    else { menus.pendingSlot = s; go(Screen::Chooser); }
  }
}

void Game::drawWin() {
  r.clear(PAL_PAPER);
  for (int i = 0; i < 18; i++) {                          // slowly turning rays, centred on the star
    const float a = (i * 20 + t * 6) * PI_F / 180, a2 = a + 10 * PI_F / 180, R = 400;
    const float q[6] = {160, 91, 160 + std::cos(a) * R, 91 + std::sin(a) * R, 160 + std::cos(a2) * R, 91 + std::sin(a2) * R};
    r.polygon(q, 3, C_WIN_RAYS);
  }
  const float k = screenT / 0.4f > 1 ? 1.0f : screenT / 0.4f;
  const float ease = k < 1 ? 1 - std::pow(1 - k, 3.0f) * std::cos(k * 9) : 1.0f;       // overshoots, then settles
  r.text("YOU WIN!", 160, 22, 28, PAL_INK, Align::Center, PAL_YELLOW);
  float star[20];
  for (int i = 0; i < 10; i++) {
    const float a = -PI_F / 2 + i * PI_F / 5, rr = (i % 2 ? 19.0f : 42.0f) * ease;
    star[i * 2] = 160 + std::cos(a) * rr; star[i * 2 + 1] = 76 + std::sin(a) * rr;
  }
  r.polygon(star, 10, PAL_YELLOW);
  r.polyline(star, 10, 4, PAL_INK, true);
  r.disc(160 - 9 * ease, 76 - 4 * ease, 3 * ease, PAL_INK); r.disc(160 + 9 * ease, 76 - 4 * ease, 3 * ease, PAL_INK);
  r.ring(160, 76 + 4 * ease, 9 * ease, 3, PAL_INK, 0.2f * PI_F, 0.6f * PI_F);
  r.line(84, 168, 252, 168, 8, PAL_INK);                  // the ground the ball stands on
  r.ball(150, 156 - std::fabs(std::sin(t * 4)) * 5, 0, 1, 1, Face::Happy);
  r.flag(226, 164, 1);
  drawParts();
  ui.draw(r, nowMs);
}

}  // namespace scribble
