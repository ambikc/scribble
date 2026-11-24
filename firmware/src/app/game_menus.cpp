// game_menus.cpp - the level Picker, the New-level Chooser and the pause Menu overlay.   OWNER: Menus agent.
// Port of tickPicker/drawCell, tickChooser and tickMenu/drawMenu in sim/src/modes.ts.
#include "game.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace scribble {

/** The Draw screen's "Clear" is undoable in the simulator. Draw owns the undo ring, so it may define a strong version of this
 *  (`void scribble::menusPushUndo(Game&)`: push the current `level` on its undo stack); until then Clear cannot be undone. */
__attribute__((weak)) void menusPushUndo(Game&) {}

namespace {

constexpr uint16_t PAL_DIM_INK = 0x5AA8;                  // #5B5646, the plus and the number of an empty slot
constexpr float PI_F = 3.14159265f;

const char* const SLOT_IDS[SLOTS] = {"slot0", "slot1", "slot2", "slot3", "slot4", "slot5", "slot6", "slot7", "slot8", "slot9"};

/** A 64 x 96 slot cell: the thumbnail, number, tick, camera mark, delete badge and hold ring of a filled slot, or the dashed plus of an empty one. */
void drawCell(Renderer& r, const Btn& b, float hold, void* user) {
  Game& g = *static_cast<Game*>(user);
  const int i = b.id[4] - '0';
  const Level* lv = g.store.slots[i];
  const uint8_t* th = g.store.thumbs[i];
  if (lv && th) {
    const bool sel = i == g.store.settings.lastSlot;
    r.roundRect(b.x, b.y, 64, 96, 0, sel ? PAL_YELLOW : PAL_WHITE, PAL_INK, sel ? 3 : 2);
    r.thumb(b.x + 2, b.y + 4, th, 1);
    char num[12]; std::snprintf(num, sizeof num, "%d", i + 1);
    r.text(num, b.x + 32, b.y + (g.menus.deleteMode ? 55 : 58), 22);
    if (g.store.isSolved(i)) { r.disc(float(b.x + 54), float(b.y + 33), 7.5f, PAL_INK); r.disc(float(b.x + 54), float(b.y + 33), 5.5f, PAL_GREEN); }
    if (lv->fromPaper) { r.disc(float(b.x + 12), float(b.y + 12), 9, PAL_WHITE, 200); drawIcon(r, Icon::Camera, b.x + 12, b.y + 12, 14, PAL_INK); }
    if (g.menus.deleteMode) { r.roundRect(b.x + 18, b.y + 69, 28, 23, 6, PAL_RED, PAL_INK, 2); drawIcon(r, Icon::Trash, b.x + 32, b.y + 80, 16); }
    if (hold > 0) r.ring(float(b.x + 32), float(b.y + 48), 26, 4, PAL_GREEN, -PI_F * 0.5f, hold * 2 * PI_F);
  } else {
    r.fillRectA(b.x + 1, b.y + 1, 62, 94, PAL_PAPER, 255);
    // the dashed frame: 4 px dashes, 3 px gaps, 2 px thick, along a 60 x 92 rectangle
    const int x0 = b.x + 2, y0 = b.y + 2, w = 60, h = 92;
    for (int d = 0; d < w; d += 7) { const int l = d + 4 > w ? w - d : 4; r.fillRectA(x0 + d, y0, l, 2, PAL_GREY, 255); r.fillRectA(x0 + d, y0 + h - 2, l, 2, PAL_GREY, 255); }
    for (int d = 0; d < h; d += 7) { const int l = d + 4 > h ? h - d : 4; r.fillRectA(x0, y0 + d, 2, l, PAL_GREY, 255); r.fillRectA(x0 + w - 2, y0 + d, 2, l, PAL_GREY, 255); }
    drawIcon(r, Icon::Plus, b.x + 32, b.y + 26, 30, PAL_DIM_INK);
    char num[12]; std::snprintf(num, sizeof num, "%d", i + 1);
    r.text(num, b.x + 32, b.y + 58, i == 9 ? 20 : 22, PAL_DIM_INK);
    if (g.store.broken[i]) r.disc(float(b.x + 54), float(b.y + 10), 4, PAL_RED);
  }
}

/** One cell of the pause menu: a white (or coloured) tile with an ink frame, the icon, and the hold ring of Clear. */
void drawMenuCell(Renderer& r, const Btn& b, float hold, void*) {
  r.roundRect(b.x, b.y, b.w, b.h, 0, b.fill >= 0 ? uint16_t(b.fill) : PAL_WHITE, PAL_INK, 2);
  drawIcon(r, b.icon, b.x + b.w / 2, b.y + b.h / 2, 44);
  if (hold > 0) r.ring(float(b.x + b.w / 2), float(b.y + b.h / 2), 34, 4, PAL_GREEN, -PI_F * 0.5f, hold * 2 * PI_F);
}

}  // namespace

// =====================================================================================================================
// level picker
// =====================================================================================================================
void Game::tickPicker(const InputState& in) {
  topBar(Icon::Grid, "bin", Icon::Trash, menus.deleteMode);
  for (int i = 0; i < SLOTS; i++) {
    const bool filled = store.slots[i] != nullptr;
    Btn c; c.id = SLOT_IDS[i]; c.x = (i % 5) * 64; c.y = 48 + (i / 5) * 96; c.w = 64; c.h = 96;
    c.kind = BtnKind::Custom; c.holdMs = menus.deleteMode && filled ? uint16_t(DELETE_HOLD_MS) : 0; c.draw = drawCell; c.user = this;
    ui.add(c);
  }
  const char* hit = ui.update(in);
  if (!hit) return;
  if (!std::strcmp(hit, "back")) { menus.deleteMode = false; go(Screen::Title); }
  else if (!std::strcmp(hit, "bin")) menus.deleteMode = !menus.deleteMode;
  else if (!std::strncmp(hit, "slot", 4)) {
    const int i = hit[4] - '0';
    const bool filled = store.slots[i] != nullptr;
    if (menus.deleteMode && filled) { store.remove(i); say("Deleted"); }       // fires only after the DELETE_HOLD_MS hold
    else if (filled) openLevel(i, true);
    else { menus.pendingSlot = i; go(Screen::Chooser); }
  }
}

void Game::drawPicker() {
  r.clear(PAL_PAPER);
  ui.draw(r, nowMs);
}

// =====================================================================================================================
// new level chooser
// =====================================================================================================================
void Game::tickChooser(const InputState& in) {
  topBar(Icon::Plus);
  Btn paperB; paperB.id = "paper"; paperB.x = 16; paperB.y = 72; paperB.w = 136; paperB.h = 136;
  paperB.kind = BtnKind::Big; paperB.fill = PAL_YELLOW; paperB.icon = Icon::Camera; paperB.iconSize = 72;
  ui.add(paperB);
  Btn finger = paperB; finger.id = "finger"; finger.x = 168; finger.fill = PAL_WHITE; finger.icon = Icon::Pencil;
  ui.add(finger);

  const char* hit = ui.update(in);
  if (!hit) return;
  bool any = false;
  for (int i = 0; i < SLOTS; i++) if (store.slots[i]) any = true;
  if (!std::strcmp(hit, "back")) go(any ? Screen::Picker : Screen::Title);
  else if (!std::strcmp(hit, "paper")) {
    if (store.firstEmpty() < 0) { say("All 10 slots are full"); menus.deleteMode = false; go(Screen::Picker); return; }   // refuse early (the paper flow must never replace a level)
    cameFrom = Screen::Chooser; go(store.settings.standupSeen ? Screen::Check : Screen::Standup);
  }
  else if (!std::strcmp(hit, "finger")) {
    const int p = menus.pendingSlot;
    const int s = (p >= 0 && p < SLOTS && !store.slots[p]) ? p : store.firstEmpty();
    menus.pendingSlot = -1;
    if (s < 0) { say("All 10 slots are full"); menus.deleteMode = false; go(Screen::Picker); } else newInSlot(s);
  }
}

void Game::drawChooser() {
  r.clearPaper();
  ui.draw(r, nowMs);
}

// =====================================================================================================================
// pause / menu overlay
// =====================================================================================================================
void Game::tickMenu(const InputState& in) {
#ifdef EMU
  // emulator only: EMU_START=menu opens the menu from Play; with EMU_MENU_FROM=draw it behaves as if opened from Draw (no Draw screen needed to test it)
  if (screenT < 0.02f && std::getenv("EMU_MENU_FROM") && !std::strcmp(std::getenv("EMU_MENU_FROM"), "draw")) menuFrom = Screen::Draw;
#endif
  chipBack();
  const bool fromPlay = menuFrom == Screen::Play;
  struct Cell { const char* id; Icon icon; int32_t fill; uint16_t hold; };
  Cell cells[6]; int n = 0;
  if (fromPlay) {
    cells[n++] = {"resume", Icon::Play, PAL_GREEN, 0}; cells[n++] = {"edit", Icon::Pencil, -1, 0}; cells[n++] = {"levels", Icon::Grid, -1, 0};
    cells[n++] = {"restart", Icon::Restart, -1, 0}; cells[n++] = {"sound", soundIcon(), -1, 0}; cells[n++] = {"home", Icon::Home, -1, 0};
  } else {
    cells[n++] = {"resume", Icon::Play, PAL_GREEN, 0}; cells[n++] = {"clear", Icon::Trash, -1, uint16_t(CLEAR_HOLD_MS)}; cells[n++] = {"levels", Icon::Grid, -1, 0};
    cells[n++] = {"sound", soundIcon(), -1, 0}; cells[n++] = {"home", Icon::Home, -1, 0};
  }
  for (int i = 0; i < n; i++) {
    Btn b; b.id = cells[i].id; b.x = 16 + (i % 3) * 96; b.y = 56 + (i / 3) * 80; b.w = 96; b.h = 80;
    b.kind = BtnKind::Custom; b.icon = cells[i].icon; b.fill = cells[i].fill; b.holdMs = cells[i].hold; b.draw = drawMenuCell;
    ui.add(b);
  }
  const char* hit = ui.update(in);

  // a tap outside the panel (and not on the back chip) closes the menu
  const TouchState& t = in.touch;
  auto inPanel = [](int x, int y) { return (x > 16 && x < 304 && y > 56 && y < 216) || (x < 48 && y < 48); };
  const bool outside = t.released && !inPanel(t.x, t.y) && !inPanel(t.startX, t.startY);

  if (hit && (!std::strcmp(hit, "back") || !std::strcmp(hit, "resume"))) go(menuFrom);
  else if (outside && !hit) go(menuFrom);
  else if (!hit) return;
  else if (!std::strcmp(hit, "edit")) go(Screen::Draw);
  else if (!std::strcmp(hit, "levels")) { saveCurrent(); menus.deleteMode = false; go(Screen::Picker); }
  else if (!std::strcmp(hit, "restart")) startPlay();
  else if (!std::strcmp(hit, "sound")) cycleSound();
  else if (!std::strcmp(hit, "home")) { saveCurrent(); go(Screen::Title); }
  else if (!std::strcmp(hit, "clear")) {
    menusPushUndo(*this);
    std::memset(level.ground, 0, sizeof level.ground); std::memset(level.spike, 0, sizeof level.spike); std::memset(level.bouncy, 0, sizeof level.bouncy);
    markEdited(); say("Cleared"); go(Screen::Draw);
  }
}

void Game::drawMenu() {
  // the scene behind, without its buttons (the menu's buttons are the ones in `ui` now), dimmed and frozen
  const int keep = ui.count; ui.count = 0;
  if (menuFrom == Screen::Play) drawPlay(); else drawDraw();
  ui.count = keep;
  r.dim(0.62f);
  r.roundRect(13, 53, 294, 166, 12, PAL_WHITE, PAL_INK, 3);
  ui.draw(r, nowMs);
}

}  // namespace scribble
