// game_draw.cpp - the Draw / Edit screen.   Port of tickDraw / drawDraw / drawDrawScene / drawTool in sim/src/modes.ts.
// OWNER: Draw agent.  The rules (stroke interpolation, undo ring, pin dragging, ghost finger) are in lib/core/src/editing.h.
//
//   top      : corner chips  <- (save, back to the level picker)   [menu] (pause overlay)
//   field    : 320 x 192, the level; one finger paints with the selected tool, a drag that starts within PIN_GRAB_RADIUS of the
//              Start ball or the Flag moves that pin instead (and never also paints)
//   bottom   : Ground | Spike | Bouncy | Eraser | Undo | Play   (52 56 52 52 56 52 px wide, 48 px high)
//
// Speed: the whole frame is redrawn and pushed every tick anyway (the SPI push of the 153 KB frame is the long pole), and
// drawLayers() skips empty bytes, so a private cache of the level layers would cost about what it saves: not done.
#include "game.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(ARDUINO) && defined(BOARD_HAS_PSRAM)
#include <esp_heap_caps.h>
#endif

namespace scribble {

namespace {
constexpr int BAR_Y = SCREEN_H - BAR_H;                 // 192: top of the toolbar, bottom of the playfield
constexpr uint16_t GHOST_INK = 0x5AA8;                  // #5B5646

/** Big buffers go to PSRAM on the device, plain malloc elsewhere (same rule as level_store.cpp). */
void* bigAlloc(size_t n) {
#if defined(ARDUINO) && defined(BOARD_HAS_PSRAM)
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) return p;
#endif
  return std::malloc(n);
}

/** Save if there is anything to save; in the emulator also print levelDigest() of the saved level (scripts compare it with what
 *  test/test_draw computes for the same strokes, see emu/run_draw_tests.sh). */
void saveLevel(Game& g) {
  const bool was = g.dirtySave;
  g.saveCurrent();
#ifdef EMU
  if (was) { std::printf("[level crc %08X]\n", unsigned(levelDigest(g.level))); std::fflush(stdout); }
#else
  (void)was;
#endif
}

/** One toolbar button, drawn like sim's drawTool: white (yellow when selected, green for Play, dotted paper when disabled), an ink line on top and
 *  at the right, a 3 px frame when selected, the Spike icon in red and the Bouncy icon in blue. */
void drawToolBtn(Renderer& r, const Btn& b, float, void* user) {
  Game* g = static_cast<Game*>(user);
  const bool play = std::strcmp(b.id, "play") == 0;
  const uint16_t fill = play ? PAL_GREEN : b.active ? PAL_YELLOW : b.disabled ? PAL_PAPER_DOT : PAL_WHITE;
  M5Canvas& c = r.g();
  c.fillRect(b.x, b.y, b.w, b.h, fill);
  c.fillRect(b.x, b.y, b.w, 2, PAL_INK);
  c.fillRect(b.x + b.w - 2, b.y, 2, b.h, PAL_INK);
  if (b.active) {
    c.fillRect(b.x, b.y, b.w, 3, PAL_INK); c.fillRect(b.x, b.y + b.h - 3, b.w, 3, PAL_INK);
    c.fillRect(b.x, b.y, 3, b.h, PAL_INK); c.fillRect(b.x + b.w - 3, b.y, 3, b.h, PAL_INK);
  }
  const uint16_t col = !std::strcmp(b.id, "spike") ? PAL_RED : !std::strcmp(b.id, "bouncy") ? PAL_BLUE : b.disabled ? PAL_GREY : PAL_INK;
  drawIcon(r, b.icon, b.x + b.w / 2, b.y + 25, b.iconSize ? b.iconSize : 30, col);
  const char* pid = g->ui.pressedId();
  if (pid[0] && !std::strcmp(pid, b.id)) r.fillRectA(b.x, b.y, b.w, b.h, 0x0000, 31);
}
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
bool DrawState::ensureUndo() {
  if (undo.ready()) return true;
  LevelSnap* mem = static_cast<LevelSnap*>(bigAlloc(sizeof(LevelSnap) * UNDO_LEVELS));
  if (!mem) return false;                                    // out of memory: no undo, but drawing still works
  undo.attach(mem, UNDO_LEVELS);
  return true;
}
void DrawState::pushUndo(const Level& l) { if (ensureUndo()) undo.push(l); }

// ---------------------------------------------------------------------------------------------------------------------
void Game::tickDraw(const InputState& in) {
  DrawState& d = drawS;
  const TouchState& t = in.touch;

  // a different level was loaded (openLevel): the old undo history and the ghost belong to the old one
  if (d.serial != levelSerial) { d.serial = levelSerial; d.undo.clear(); d.ghostDone = false; d.menuSnapValid = false; }
  if (screenT < 0.02f) {                                     // first tick after go(Draw)
    d.stroke.end(); d.pinDrag = Pin::None; d.idleSince = nowMs;
    if (d.menuSnapValid) {                                   // back from the menu overlay: did it change the level (Clear)? then make it undoable
      d.menuSnapValid = false;
      if (d.menuSnap && !snapEquals(level, *d.menuSnap)) {
        const LevelSnap* top = d.undo.top();
        if (d.ensureUndo() && (!top || !snapSame(*top, *d.menuSnap))) d.undo.pushSnap(*d.menuSnap);
        markEdited();
      }
    }
  }

  chipBack(); chipMenu();
  struct Item { const char* id; int w; Icon icon; Tool tool; };
  static const Item items[] = {
    {"ground", 52, Icon::Ground, Tool::Ground}, {"spike", 56, Icon::Spike, Tool::Spike}, {"bouncy", 52, Icon::Bouncy, Tool::Bouncy},
    {"eraser", 52, Icon::Eraser, Tool::Eraser}, {"undo", 56, Icon::Undo, Tool::Ground}, {"play", 52, Icon::Play, Tool::Ground},
  };
  int x = 0;
  for (const Item& it : items) {
    Btn b; b.id = it.id; b.x = x; b.y = BAR_Y; b.w = it.w; b.h = BAR_H; b.kind = BtnKind::Custom; b.icon = it.icon;
    b.draw = drawToolBtn; b.user = this;
    if (!std::strcmp(it.id, "undo")) b.disabled = !d.canUndo();
    else if (!std::strcmp(it.id, "play")) b.iconSize = 32;
    else b.active = d.tool == it.tool;
    ui.add(b); x += it.w;
  }

  const char* hit = ui.update(in);
  if (t.pressed) { d.ghostDone = true; d.idleSince = nowMs; }
  if (hit) {
    if (!std::strcmp(hit, "back")) { saveLevel(*this); goBack(); return; }
    if (!std::strcmp(hit, "menu")) {
      if (!d.menuSnap) d.menuSnap = static_cast<LevelSnap*>(bigAlloc(sizeof(LevelSnap)));
      if (d.menuSnap) { snapTake(level, *d.menuSnap); d.menuSnapValid = true; }
      menuFrom = Screen::Draw; go(Screen::Menu); return;
    }
    if (!std::strcmp(hit, "play")) {
      saveLevel(*this);
      store.settings.firstRunDone = true; store.saveSettings();
      startPlay(); return;
    }
    if (!std::strcmp(hit, "undo")) { if (d.undo.pop(level)) { markEdited(); audio.play(Sfx::Tick); } }
    else if (!std::strcmp(hit, "ground")) { d.tool = Tool::Ground; audio.play(Sfx::Tick); }
    else if (!std::strcmp(hit, "spike")) { d.tool = Tool::Spike; audio.play(Sfx::Tick); }
    else if (!std::strcmp(hit, "bouncy")) { d.tool = Tool::Bouncy; audio.play(Sfx::Tick); }
    else if (!std::strcmp(hit, "eraser")) { d.tool = Tool::Eraser; audio.play(Sfx::Tick); }
  }

  // pins first: grabbing a pin beats drawing, and a drag that began on a pin never paints
  if (t.pressed && !ui.touchOnButton && t.y < BAR_Y) {
    const Pin p = pinGrab(level, t.x, t.y);
    if (p != Pin::None) { d.pinDrag = p; d.pushUndo(level); }
  }
  if (d.pinDrag != Pin::None && t.down) pinMove(level, d.pinDrag, t.x, t.y);
  if (d.pinDrag != Pin::None && t.released) {                // drop: never bury a pin
    pinDrop(level, d.pinDrag);
    d.pinDrag = Pin::None; markEdited(); audio.play(Sfx::Tick);
  }

  if (d.pinDrag == Pin::None) {
    const int mat = toolMaterial(d.tool); const double rad = toolRadius(d.tool);
    if (t.pressed && !ui.touchOnButton && t.y < BAR_Y) {
      d.pushUndo(level);
      d.stroke.begin(level, mat, t.x, t.y, rad);
      markEdited();
    } else if (d.stroke.active && t.down) {
      const bool click = d.stroke.extend(level, mat, t.x, t.y, rad);
      markEdited();
      if (click) audio.play(d.tool == Tool::Ground ? Sfx::PenGround : d.tool == Tool::Spike ? Sfx::PenSpike : d.tool == Tool::Bouncy ? Sfx::PenBouncy : Sfx::Tick);
    }
    if (t.released && d.stroke.active) d.stroke.end();
  }

  // autosave after a few idle seconds
  if (dirtySave && !d.stroke.active && d.pinDrag == Pin::None && (nowMs - lastEditAt) / 1000.0 > AUTOSAVE_IDLE_S) saveLevel(*this);
}

// ---------------------------------------------------------------------------------------------------------------------
/** The playfield without any buttons (the menu overlay draws this under its dimmer). */
void drawDrawScene(Game& g) {
  Renderer& r = g.r; DrawState& d = g.drawS; const Level& lv = g.level;
  r.clearPaper();
  r.drawLayers(lv);
  r.startPin(float(lv.startX), float(lv.startY), d.pinDrag == Pin::Start);
  r.flagPin(float(lv.flagX), float(lv.flagY), d.pinDrag == Pin::Flag);
  if (g.screen == Screen::Draw) {
    GhostPose gp;
    if (!d.ghostDone && g.levelIsStarter && !g.dirtySave && !d.canUndo() && ghostPose((g.nowMs - d.idleSince) / 1000.0, gp)) {
      const float dx = float(gp.x - 72), dy = float(gp.y - 118), len = std::sqrt(dx * dx + dy * dy);
      for (float s = 0; s <= len; s += 11.0f) { const float k = len > 0 ? s / len : 0; r.disc(72 + dx * k, 118 + dy * k, 3, GHOST_INK, 204); }   // the dotted trail
      r.disc(float(gp.x), float(gp.y), 13, PAL_INK, 64);
      r.ring(float(gp.x), float(gp.y), 13, 2, GHOST_INK);
    }
    if (d.tool == Tool::Eraser && d.stroke.active) r.ring(float(d.stroke.lx), float(d.stroke.ly), float(ERASER_RADIUS), 1.5f, PAL_GREY);
  }
}

void Game::drawDraw() {
  drawDrawScene(*this);
  if (screen == Screen::Draw) ui.draw(r, nowMs);             // (the menu overlay calls drawDraw() under its own buttons: no double draw)
}

}  // namespace scribble
