// game_core.cpp - the shared helpers of the state machine and the tick/draw dispatch.   Port of the helpers in sim/src/modes.ts.
// OWNER: Core agent. The screens themselves live in game_<screen>.cpp (see the table in game.h).
#include "game.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace scribble {

namespace {
constexpr float DT = 1.0f / PHYSICS_HZ;
constexpr int BAR_Y = SCREEN_H - BAR_H;   // 192: the bottom bar

uint32_t rngState = 0x9E3779B9u;
float frand() {                           // cheap xorshift in [0, 1): particles are cosmetic, so no need for random()
  rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
  return (rngState >> 8) * (1.0f / 16777216.0f);
}
}  // namespace

// =====================================================================================================================
// start-up and screen changes
// =====================================================================================================================
void Game::begin() {
  audio.volume = store.settings.sound;
  blankLevel(level);
  newWorld(play.world, level);
  slot = 0; dirtySave = false; levelIsStarter = false; nParts = 0; toast.t = 0; t = 0; nowMs = millis();
  go(Screen::Title);
#ifdef EMU
  // Emulator only: EMU_START=<screen> jumps straight to a screen so scripts can test one screen in isolation;
  // EMU_SLOT=<0-9> is the level slot for draw / play / win / menu (default 0).
  if (const char* st = std::getenv("EMU_START")) {
    const int sl = std::getenv("EMU_SLOT") ? std::atoi(std::getenv("EMU_SLOT")) : 0;
    struct { const char* n; Screen s; } tbl[] = {{"title", Screen::Title}, {"picker", Screen::Picker}, {"chooser", Screen::Chooser}, {"standup", Screen::Standup},
      {"check", Screen::Check}, {"countdown", Screen::Countdown}, {"processing", Screen::Processing}, {"review", Screen::Review}};
    if (!std::strcmp(st, "play")) openLevel(sl, true);
    else if (!std::strcmp(st, "draw")) openLevel(sl, false);
    else if (!std::strcmp(st, "win")) { openLevel(sl, true); go(Screen::Win); }
    else if (!std::strcmp(st, "menu")) { openLevel(sl, true); menuFrom = Screen::Play; go(Screen::Menu); }
    else for (auto& e : tbl) if (!std::strcmp(st, e.n)) go(e.s);
  }
#endif
}

void Game::go(Screen s) {
  screen = s; screenT = 0; transition = 1;
#ifdef EMU
  static const char* const names[] = {"title", "picker", "chooser", "standup", "check", "countdown", "processing", "review", "draw", "play", "win", "menu"};
  std::printf("[screen %s]\n", names[int(s)]);   // emulator only: scripts grep this to check the screen flow
#endif
}

void Game::say(const char* text) {
  std::strncpy(toast.text, text, sizeof toast.text - 1);
  toast.text[sizeof toast.text - 1] = 0;
  toast.t = 2.2f;
}

void Game::flash(uint16_t color, float seconds) {
  play.fxFlash = seconds; play.fxFlashColor = color;
}

// =====================================================================================================================
// buttons that every screen needs
// =====================================================================================================================
/** A 40 x 40 corner chip at (x, 4) with a 48 px hit area. `extra` may carry fill/active/holdMs/longId/longMs/iconSize; id, position,
 *  size and (when given) the icon are always set here. */
void Game::chip(const char* id, Icon icon, int x, const Btn* extra) {
  Btn b = extra ? *extra : Btn{};
  b.id = id; b.x = x; b.y = 4; b.w = 40; b.h = 40; b.kind = BtnKind::Chip;
  if (icon != Icon::None) b.icon = icon;
  if (!b.hit) b.hit = 4;
  ui.add(b);
}
void Game::chipBack(const Btn* extra, Icon icon) { chip("back", icon, 4, extra); }
void Game::chipMenu() { chip("menu", Icon::Menu, 276); }

void Game::bar(const Btn* items, const int* widths, int n) {
  int x = 0;
  for (int i = 0; i < n; i++) {
    Btn b = items[i];
    b.x = x; b.y = BAR_Y; b.w = widths[i]; b.h = BAR_H; b.kind = BtnKind::Tab;
    ui.add(b);
    x += widths[i];
  }
}

namespace {
void drawTopBarBg(Renderer& r, const Btn& b, float, void*) {
  r.g().fillRect(0, 0, 320, 48, PAL_WHITE);
  r.g().fillRect(0, 46, 320, 2, PAL_INK);
  if (b.icon != Icon::None) drawIcon(r, b.icon, 160, 24, 28);
}
}  // namespace

/** The 48 px top bar of the list screens: white band, a back tab at the left, `titleIcon` in the middle and optionally one more tab at the right. */
void Game::topBar(Icon titleIcon, const char* rightId, Icon rightIcon, bool rightActive) {
  Btn bg; bg.id = "topbar"; bg.x = 0; bg.y = 0; bg.w = 320; bg.h = 48; bg.kind = BtnKind::Custom; bg.disabled = true; bg.icon = titleIcon; bg.draw = drawTopBarBg;
  ui.add(bg);
  Btn back; back.id = "back"; back.x = 0; back.y = 0; back.w = 48; back.h = 48; back.kind = BtnKind::Tab; back.icon = Icon::Back; back.iconSize = 26; back.fill = PAL_WHITE; back.edge = Edge::Bottom;
  ui.add(back);
  if (rightId) {
    Btn rb; rb.id = rightId; rb.x = 272; rb.y = 0; rb.w = 48; rb.h = 48; rb.kind = BtnKind::Tab; rb.icon = rightIcon; rb.iconSize = 26;
    rb.fill = rightActive ? PAL_RED : PAL_WHITE; rb.active = rightActive; rb.edge = Edge::Bottom;
    ui.add(rb);
  }
}

Icon Game::soundIcon() const {
  const int v = audio.volume < 0 ? 0 : audio.volume > 2 ? 2 : audio.volume;
  return v == 0 ? Icon::Sound0 : v == 1 ? Icon::Sound1 : Icon::Sound2;
}
void Game::cycleSound() { store.settings.sound = audio.cycleVolume(); store.saveSettings(); }

// =====================================================================================================================
// particles, toast, transition
// =====================================================================================================================
void Game::spawn(int n, float x, float y, const uint16_t* colors, int nColors, float speed, float life, int size, int kind) {
  if (n <= 0 || nColors <= 0) return;
  if (n > MAX_PARTICLES) n = MAX_PARTICLES;
  const int over = nParts + n - MAX_PARTICLES;                        // the oldest particles make room
  if (over > 0) { std::memmove(parts, parts + over, sizeof(Particle) * (nParts - over)); nParts -= over; }
  for (int i = 0; i < n; i++) {
    const float a = frand() * 6.2831853f, s = speed * (0.4f + frand() * 0.8f);
    Particle& p = parts[nParts++];
    p.x = x; p.y = y; p.vx = std::cos(a) * s; p.vy = std::sin(a) * s - (kind == 3 ? speed * 0.6f : 0.0f);
    p.life = life; p.max = life; p.color = colors[int(frand() * nColors) % nColors]; p.size = uint8_t(size); p.kind = uint8_t(kind);
  }
}

__attribute__((weak)) void Game::playFxStep(float dt) {
  auto dec = [dt](float& v) { v = v - dt > 0 ? v - dt : 0; };
  dec(play.fxStretch); dec(play.fxSquash); dec(play.fxShake); dec(play.fxRed); dec(play.fxFlash); dec(play.spawnRing);
}

void Game::updateFx(float dt) {
  int k = 0;
  for (int i = 0; i < nParts; i++) {
    Particle p = parts[i];
    p.x += p.vx * dt; p.y += p.vy * dt;
    p.vy += (p.kind == 3 ? 180.0f : p.kind == 2 ? 0.0f : 300.0f) * dt;
    p.life -= dt;
    if (p.life > 0) parts[k++] = p;
  }
  nParts = k;
  playFxStep(dt);
  toast.t = toast.t - dt > 0 ? toast.t - dt : 0;
  transition = transition - dt / 0.15f > 0 ? transition - dt / 0.15f : 0;
}

void Game::drawParts() {
  partsDrawn = true;
  for (int i = 0; i < nParts; i++) {
    const Particle& p = parts[i];
    const float k = p.life / p.max > 0 ? p.life / p.max : 0;
    const float al = k * 1.5f > 1 ? 1.0f : k * 1.5f;
    const uint8_t a = uint8_t(al * 255);
    if (p.kind == 2) r.ring(p.x, p.y, (1 - k) * 22 + 3, 2, p.color, 0, 6.2831853f, a);
    else if (p.kind == 1) r.disc(p.x, p.y, float(p.size), p.color, a);
    else if (p.kind == 3) {
      const float an = p.life * 9, c = std::cos(an), s = std::sin(an), w = float(p.size), h = p.size * 0.5f;
      const float q[8] = {p.x + (-w) * c - (-h) * s, p.y + (-w) * s + (-h) * c, p.x + w * c - (-h) * s, p.y + w * s + (-h) * c,
                          p.x + w * c - h * s, p.y + w * s + h * c, p.x + (-w) * c - h * s, p.y + (-w) * s + h * c};
      r.polygon(q, 4, p.color, a);
    } else {
      const float h = p.size * 0.5f;
      const float q[8] = {p.x - h, p.y - h, p.x + h, p.y - h, p.x + h, p.y + h, p.x - h, p.y + h};
      r.polygon(q, 4, p.color, a);
    }
  }
}

// =====================================================================================================================
// saving and opening
// =====================================================================================================================
void Game::saveCurrent() {
  if (dirtySave) { store.save(slot, level); dirtySave = false; }
}
void Game::markEdited() { dirtySave = true; lastEditAt = nowMs; r.dirty(); }

void Game::openLevel(int slotIndex, bool playIt) {
  if (slotIndex < 0 || slotIndex >= SLOTS) slotIndex = 0;
  const Level* src = store.slots[slotIndex];
  slot = slotIndex;
  if (src) level = *src; else blankLevel(level);
  levelIsStarter = std::strcmp(level.name, "Bridge") == 0;
  dirtySave = false; ++levelSerial;
  store.settings.lastSlot = slotIndex; store.saveSettings();
  r.dirty();
  if (playIt) startPlay(); else go(Screen::Draw);
}

void Game::newInSlot(int slotIndex) {
  blankLevel(level);
  store.save(slotIndex, level);
  openLevel(slotIndex, false);
  say("New level");
}

void Game::startPlay() {
  newWorld(play.world, level);
  nParts = 0;
  play.winT = 0; play.spawnRing = 0.2f; play.tiltHint = play.everPlayed ? 0.0f : 3.0f; play.everPlayed = true;
  go(Screen::Play);
}

void Game::leaveCamera() { go(cameFrom == Screen::Chooser ? Screen::Chooser : Screen::Title); }

void Game::goBack() {
  switch (screen) {
    case Screen::Picker: go(Screen::Title); break;
    case Screen::Chooser: go(Screen::Picker); break;
    case Screen::Standup: case Screen::Check: leaveCamera(); break;
    case Screen::Countdown: case Screen::Review: go(Screen::Check); break;
    case Screen::Draw: saveCurrent(); go(Screen::Picker); break;
    case Screen::Menu: go(menuFrom); break;
    default: break;
  }
}

// =====================================================================================================================
// the 60 Hz tick and the frame
// =====================================================================================================================
void Game::tick(const InputState& in) {
  nowMs = millis(); t += DT; screenT += DT;
  ui.begin();
  switch (screen) {
    case Screen::Title: tickTitle(in); break;
    case Screen::Picker: tickPicker(in); break;
    case Screen::Chooser: tickChooser(in); break;
    case Screen::Standup: tickStandup(in); break;
    case Screen::Check: tickCheck(in); break;
    case Screen::Countdown: tickCountdown(in); break;
    case Screen::Processing: tickProcessing(in); break;
    case Screen::Review: tickReview(in); break;
    case Screen::Draw: tickDraw(in); break;
    case Screen::Play: tickPlay(in); break;
    case Screen::Win: tickWin(in); break;
    case Screen::Menu: tickMenu(in); break;
  }
  if (in.keyBack && screen != Screen::Title && screen != Screen::Play) goBack();
  updateFx(DT);
}

void Game::draw() {
  partsDrawn = false;
  switch (screen) {
    case Screen::Title: drawTitle(); break;
    case Screen::Picker: drawPicker(); break;
    case Screen::Chooser: drawChooser(); break;
    case Screen::Standup: drawStandup(); break;
    case Screen::Check: drawCheck(); break;
    case Screen::Countdown: drawCountdown(); break;
    case Screen::Processing: drawProcessing(); break;
    case Screen::Review: drawReview(); break;
    case Screen::Draw: drawDraw(); break;
    case Screen::Play: drawPlay(); break;
    case Screen::Win: drawWin(); break;
    case Screen::Menu: drawMenu(); break;
  }
  if (!partsDrawn) drawParts();
  if (play.fxFlash > 0 && screen != Screen::Play) r.flash(play.fxFlashColor, play.fxFlash * 8);
  if (transition > 0) r.flash(PAL_PAPER, transition * 0.9f);
  if (toast.t > 0) {   // a dark pill near the bottom that fades out
    const float k = toast.t * 2 > 1 ? 1.0f : toast.t * 2;
    const int tw = r.textWidth(toast.text, 11), w = tw + 28 < 80 ? 80 : (tw + 28 > 300 ? 300 : tw + 28);
    r.roundRectA(160 - w / 2, 160, w, 22, 11, PAL_INK, uint8_t(k * 255), -1);
    if (k > 0.5f) r.text(toast.text, 160, 171, 11, PAL_WHITE);
  }
}

}  // namespace scribble
