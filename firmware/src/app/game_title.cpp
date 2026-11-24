// game_title.cpp - the Title screen.   Port of tickTitle / drawTitle in sim/src/modes.ts.   OWNER: Menus agent.
// The logo is a little stroke font (the Renderer has no rotation transform, so the wobbly hand-lettering is drawn with
// polylines); the hill is a thick polyline; the ball and the flag are the Renderer's.
#include "game.h"
#include <cmath>
#include <cstring>

namespace scribble {

namespace {

// ---- the stroke font: cell height 14, each glyph is a few polylines ----------------------------------------------------
struct Pt { float x, y; };
struct StrokeDef { const Pt* p; int n; };
struct Glyph { const StrokeDef* s; int n; float w; };       // w = width of the letter in cell units (the advance adds a gap)
#define STROKE(a) {a, int(sizeof(a) / sizeof((a)[0]))}
#define GLYPH(strokes, w) {strokes, int(sizeof(strokes) / sizeof((strokes)[0])), w}

const Pt pS[] = {{9, 3}, {8, 1}, {5, 0.3f}, {2, 1.2f}, {1, 3.5f}, {2.5f, 6}, {7, 8}, {9, 10.5f}, {8, 13}, {5, 13.8f}, {2, 13}, {1, 11}};
const Pt pC[] = {{9, 2.5f}, {7, 0.6f}, {4, 0.3f}, {1.8f, 2.5f}, {1, 7}, {1.8f, 11.5f}, {4, 13.7f}, {7, 13.5f}, {9, 11.5f}};
const Pt pR1[] = {{1, 14}, {1, 0.3f}, {6, 0.2f}, {8.8f, 2}, {8.8f, 5}, {6, 7}, {1, 7.2f}};
const Pt pR2[] = {{5, 7.2f}, {9.2f, 14}};
const Pt pI[] = {{1, 0.3f}, {1, 14}};
const Pt pB1[] = {{1, 14}, {1, 0.3f}, {6, 0.2f}, {8.5f, 2}, {8.3f, 5}, {5.5f, 7}, {1, 7.2f}};
const Pt pB2[] = {{5.5f, 7}, {8.8f, 9}, {9, 12}, {6, 13.9f}, {1, 14}};
const Pt pL[] = {{1, 0.3f}, {1, 14}, {8, 14}};
const Pt pE1[] = {{8.5f, 0.3f}, {1, 0.3f}, {1, 14}, {8.5f, 14}};
const Pt pE2[] = {{1, 7.2f}, {6.5f, 7.2f}};
const Pt pU[] = {{1, 0.3f}, {1, 10}, {2.5f, 13}, {5, 14}, {7.5f, 13}, {9, 10}, {9, 0.3f}};
const Pt pN[] = {{1, 14}, {1, 0.3f}, {9, 14}, {9, 0.3f}};
const Pt pBang1[] = {{1, 0.3f}, {1, 9}};
const Pt pBang2[] = {{1, 13.6f}, {1, 13.9f}};

const StrokeDef sS[] = {STROKE(pS)}, sC[] = {STROKE(pC)}, sR[] = {STROKE(pR1), STROKE(pR2)}, sI[] = {STROKE(pI)},
                sB[] = {STROKE(pB1), STROKE(pB2)}, sL[] = {STROKE(pL)}, sE[] = {STROKE(pE1), STROKE(pE2)}, sU[] = {STROKE(pU)},
                sN[] = {STROKE(pN)}, sBang[] = {STROKE(pBang1), STROKE(pBang2)};

const Glyph gS = GLYPH(sS, 10), gC = GLYPH(sC, 10), gR = GLYPH(sR, 10), gI = GLYPH(sI, 2), gB = GLYPH(sB, 10), gL = GLYPH(sL, 9),
            gE = GLYPH(sE, 9.5f), gU = GLYPH(sU, 10), gN = GLYPH(sN, 10), gBang = GLYPH(sBang, 2);

/** One word of the logo: letters centred on (cx, cy), `capPx` tall, each turned and nudged a little (the wobble). `wig` is 0..1: how
 *  much it is wiggling right now. */
void drawWord(Renderer& r, const Glyph* const* gl, int n, float cx, float cy, float capPx, float rowTilt, float t, float wig, uint16_t color) {
  const float s = capPx / 14.0f, gap = 3.0f;
  float total = 0;
  for (int i = 0; i < n; i++) total += gl[i]->w + (i + 1 < n ? gap : 0);
  const float ca = std::cos(rowTilt), sa = std::sin(rowTilt);
  float x = -total * 0.5f * s;
  const float lw = 2.6f * s;
  for (int i = 0; i < n; i++) {
    const Glyph& g = *gl[i];
    // a hand-drawn base wobble (different for every letter) plus the wiggle burst
    const float base = 0.06f * std::sin(i * 2.3f + 0.7f);
    const float ang = base + wig * 0.16f * std::sin(t * 16.0f + i * 1.9f);
    const float dy = 1.3f * std::sin(i * 1.7f + 2.0f) + wig * 2.4f * std::sin(t * 13.0f + i * 2.4f);
    const float gcx = g.w * 0.5f, gcy = 7.0f, c2 = std::cos(ang), s2 = std::sin(ang);
    const float lx = x + g.w * 0.5f * s;                       // the letter's centre along the row
    for (int k = 0; k < g.n; k++) {
      float pts[2 * 16]; const StrokeDef& st = g.s[k];
      const int m = st.n > 16 ? 16 : st.n;
      for (int j = 0; j < m; j++) {
        const float px = (st.p[j].x - gcx) * s, py = (st.p[j].y - gcy) * s;
        const float rx = px * c2 - py * s2, ry = px * s2 + py * c2;
        const float qx = lx + rx, qy = dy + ry;
        pts[2 * j] = cx + qx * ca - qy * sa;                   // the whole row is tilted too
        pts[2 * j + 1] = cy + qx * sa + qy * ca;
      }
      r.polyline(pts, m, lw, color);
    }
    x += (g.w + gap) * s;
  }
}

float hillY(float x) { const float d = (x - 140.0f) / 34.0f; return 176.0f - 26.0f * std::exp(-d * d); }

}  // namespace

void Game::tickTitle(const InputState& in) {
  chip("sound", soundIcon(), 276);
  Btn items[4];
  items[0].id = "levels"; items[0].icon = Icon::Grid;
  items[1].id = "paper"; items[1].icon = Icon::Camera; items[1].fill = PAL_YELLOW; items[1].iconSize = 34;
  items[2].id = "play"; items[2].icon = Icon::Play; items[2].fill = PAL_GREEN; items[2].iconSize = 36;
  items[3].id = "draw"; items[3].icon = Icon::Pencil;
  static const int widths[4] = {72, 80, 88, 80};
  bar(items, widths, 4);

  const char* hit = ui.update(in);
  if (!hit) return;
  if (!std::strcmp(hit, "sound")) cycleSound();
  else if (!std::strcmp(hit, "levels")) { menus.deleteMode = false; go(Screen::Picker); }
  else if (!std::strcmp(hit, "paper")) {
    if (store.firstEmpty() < 0) { say("All 10 slots are full"); menus.deleteMode = false; go(Screen::Picker); return; }   // refuse early: the paper flow must never replace a level
    cameFrom = Screen::Title; menus.pendingSlot = -1; go(store.settings.standupSeen ? Screen::Check : Screen::Standup);
  } else if (!std::strcmp(hit, "draw")) {
    const int s = store.firstEmpty();
    if (s < 0) { say("All 10 slots are full"); menus.deleteMode = false; go(Screen::Picker); } else newInSlot(s);
  } else if (!std::strcmp(hit, "play")) {
    // the very first time: the starter "Bridge" in Draw with the ghost finger (its gap cannot be crossed, Play would show an unwinnable level).
    // firstRunDone becomes true when he presses Play inside Draw.
    if (!store.settings.firstRunDone && store.slots[0]) { openLevel(0, false); return; }
    // the last level he played; if it is gone, the first one that exists; with an empty box, a fresh level to draw on
    int s = store.settings.lastSlot;
    if (s < 0 || s >= SLOTS || !store.slots[s]) { s = -1; for (int i = 0; i < SLOTS; i++) if (store.slots[i]) { s = i; break; } }
    if (s >= 0) openLevel(s, true);
    else { const int e = store.firstEmpty(); if (e >= 0) newInSlot(e); else go(Screen::Picker); }
  }
}

void Game::drawTitle() {
  r.clearPaper();
  // the logo: two rows of wobbly letters turned opposite ways, wiggling for 0.7 s every 2 s
  const float ph = std::fmod(t, 2.0f), wig = ph < 0.7f ? (1.0f - ph / 0.7f) * (1.0f - ph / 0.7f) : 0.0f;
  static const Glyph* const scribble[8] = {&gS, &gC, &gR, &gI, &gB, &gB, &gL, &gE};
  static const Glyph* const run[4] = {&gR, &gU, &gN, &gBang};
  drawWord(r, scribble, 8, 160, 34, 30, -0.035f, t, wig, PAL_INK);
  drawWord(r, run, 4, 160, 84, 38, 0.035f, t, wig, PAL_RED);

  // a hill, a ball that rolls and jumps, a flag
  float hill[88 * 2]; int n = 0;
  for (int x = -4; x <= 324; x += 4) { hill[2 * n] = float(x); hill[2 * n + 1] = hillY(float(x)); n++; }
  r.polyline(hill, n, 8, PAL_INK);
  const float u = std::fmod(t * 0.22f, 1.0f), bx = 24 + u * 244;
  const float jump = (u > 0.5f && u < 0.72f) ? std::sin((u - 0.5f) / 0.22f * 3.14159265f) * 28 : 0.0f;
  r.ball(bx, hillY(bx) - 12 - jump, bx / 8);
  r.flag(272, 172, u > 0.97f ? 1.0f : 0.0f);
  ui.draw(r, nowMs);
}

}  // namespace scribble
