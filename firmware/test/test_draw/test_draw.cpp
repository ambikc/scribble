// Native unit tests of the Draw / Edit rules (lib/core/src/editing.h):  pio test -e native -f test_draw
//
// The reference for "what a stroke does" is level.cpp's paintDisc / paintLine / eraseDisc, called directly below. The tests prove that
// editing.h (the code the Draw screen runs) gives the same bytes, that undo restores exactly, that materials stay exclusive, that a
// fast scribble leaves no gaps, and the pin rules. test_scenario_script prints the expected digests of emu/scripts/draw_strokes.txt
// as `DRAW_EXPECT n XXXXXXXX` lines; emu/run_draw_tests.sh compares them with the `[level crc XXXXXXXX]` lines the emulator prints.
#include <unity.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "editing.h"

using namespace scribble;

void setUp() {}
void tearDown() {}

struct P { int x, y; };

static void blank(Level& l) {            // = blankLevel() of level_store.cpp (that file is not part of the native build)
  levelInit(l);
  l.name[0] = 0;
  fillRect(l, MAT_GROUND, 0, 168, WORLD_W, 176);
  l.startX = 40; l.startY = 160; l.flagX = 280; l.flagY = 168;
}
static bool sameLayers(const Level& a, const Level& b) {
  return !std::memcmp(a.ground, b.ground, LAYER_BYTES) && !std::memcmp(a.spike, b.spike, LAYER_BYTES) && !std::memcmp(a.bouncy, b.bouncy, LAYER_BYTES);
}
// The reference: press = one disc, every further point = paintLine from the previous one (what modes.ts does).
static void refStroke(Level& l, int mat, const P* p, int n, double rad) {
  if (mat == MAT_ERASE) eraseDisc(l, p[0].x, p[0].y, rad); else paintDisc(l, mat, p[0].x, p[0].y, rad);
  for (int i = 1; i < n; i++) paintLine(l, mat, p[i - 1].x, p[i - 1].y, p[i].x, p[i].y, rad);
}
// The same through editing.h's Stroke (what the Draw screen runs).
static void editStroke(Level& l, Tool tool, const P* p, int n) {
  Stroke s; const int mat = toolMaterial(tool); const double rad = toolRadius(tool);
  s.begin(l, mat, p[0].x, p[0].y, rad);
  for (int i = 1; i < n; i++) s.extend(l, mat, p[i].x, p[i].y, rad);
  s.end();
}
static bool exclusive(const Level& l) {
  for (int i = 0; i < LAYER_BYTES; i++) if ((l.ground[i] & l.spike[i]) | (l.ground[i] & l.bouncy[i]) | (l.spike[i] & l.bouncy[i])) return false;
  return true;
}
static int countBits(const uint8_t* layer) { int n = 0; for (int i = 0; i < LAYER_BYTES; i++) n += __builtin_popcount(layer[i]); return n; }

// ---------------------------------------------------------------------------------------------------------------------
void test_stroke_equals_paintLine_polyline() {
  static const P a[] = {{60, 100}, {100, 60}, {150, 100}, {200, 60}, {250, 100}};
  Level ref, ed; blank(ref); blank(ed);
  refStroke(ref, MAT_GROUND, a, 5, BRUSH_RADIUS);
  editStroke(ed, Tool::Ground, a, 5);
  TEST_ASSERT_TRUE(sameLayers(ref, ed));
  TEST_ASSERT_GREATER_THAN(1000, countBits(ed.ground));
  TEST_ASSERT_EQUAL_UINT32(levelDigest(ref), levelDigest(ed));
}

void test_single_tap_stamps_a_disc() {
  Level ref, ed; blank(ref); blank(ed);
  paintDisc(ref, MAT_SPIKE, 120, 80, BRUSH_RADIUS);
  const P a[] = {{120, 80}};
  editStroke(ed, Tool::Spike, a, 1);
  TEST_ASSERT_TRUE(sameLayers(ref, ed));
  TEST_ASSERT_TRUE(getBit(ed.spike, 120, 80));
}

void test_fast_scribble_has_no_gaps() {
  // finger jumps 280 px between two samples: every point on the straight segment must still be painted (and a disc around it)
  static const P a[] = {{20, 20}, {300, 25}, {20, 45}, {300, 50}, {160, 150}, {10, 10}};
  Level l; blank(l);
  editStroke(l, Tool::Spike, a, 6);
  for (int i = 1; i < 6; i++) {
    const double len = std::hypot(double(a[i].x - a[i - 1].x), double(a[i].y - a[i - 1].y));
    for (int s = 0; s <= int(len); s++) {
      const double t = s / len, x = a[i - 1].x + (a[i].x - a[i - 1].x) * t, y = a[i - 1].y + (a[i].y - a[i - 1].y) * t;
      char msg[64]; std::snprintf(msg, sizeof msg, "gap at segment %d step %d", i, s);
      TEST_ASSERT_TRUE_MESSAGE(getBit(l.spike, int(x), int(y)), msg);
    }
  }
}

void test_eraser_removes_every_material() {
  Level l; blank(l);
  const P g[] = {{60, 40}, {200, 40}}, s[] = {{60, 60}, {200, 60}}, b[] = {{60, 80}, {200, 80}};
  editStroke(l, Tool::Ground, g, 2); editStroke(l, Tool::Spike, s, 2); editStroke(l, Tool::Bouncy, b, 2);
  TEST_ASSERT_TRUE(getBit(l.ground, 100, 40) && getBit(l.spike, 100, 60) && getBit(l.bouncy, 100, 80));
  const P e[] = {{100, 20}, {100, 100}};                       // a vertical eraser sweep through all three lines
  editStroke(l, Tool::Eraser, e, 2);
  for (int y = 20; y <= 100; y++) TEST_ASSERT_FALSE(getBit(l.ground, 100, y) | getBit(l.spike, 100, y) | getBit(l.bouncy, 100, y));
  TEST_ASSERT_TRUE(getBit(l.ground, 140, 40));                 // outside the eraser (radius 10) nothing changed
  Level ref; blank(ref);
  refStroke(ref, MAT_GROUND, g, 2, BRUSH_RADIUS); refStroke(ref, MAT_SPIKE, s, 2, BRUSH_RADIUS); refStroke(ref, MAT_BOUNCY, b, 2, BRUSH_RADIUS); refStroke(ref, MAT_ERASE, e, 2, ERASER_RADIUS);
  TEST_ASSERT_TRUE(sameLayers(ref, l));
}

void test_materials_are_exclusive() {
  Level l; blank(l);
  const P v[] = {{100, 30}, {100, 150}}, h1[] = {{40, 60}, {260, 70}}, h2[] = {{40, 100}, {260, 90}};
  editStroke(l, Tool::Ground, h1, 2); editStroke(l, Tool::Spike, v, 2); editStroke(l, Tool::Bouncy, h2, 2); editStroke(l, Tool::Spike, h1, 2);
  TEST_ASSERT_TRUE(exclusive(l));
  TEST_ASSERT_TRUE(getBit(l.spike, 100, 62));                  // the last paint wins where strokes cross
  TEST_ASSERT_FALSE(getBit(l.ground, 100, 62));
}

void test_pen_click_every_24px() {
  Level l; blank(l);
  Stroke s; s.begin(l, MAT_GROUND, 20, 100, 4);
  int clicks = 0;
  for (int x = 24; x <= 224; x += 4) if (s.extend(l, MAT_GROUND, x, 100, 4)) clicks++;   // 200 px in 4 px steps
  TEST_ASSERT_INT_WITHIN(1, 200 / 25, clicks);                 // one click per > 24 px of travel: 7 or 8
  TEST_ASSERT_LESS_THAN(10, clicks);
}

void test_undo_ring_restores_exactly_and_keeps_8() {
  static LevelSnap mem[UNDO_LEVELS];
  UndoRing u; u.attach(mem, UNDO_LEVELS);
  Level l; blank(l);
  static Level hist[12];
  for (int i = 0; i < 12; i++) {                               // 12 edits: only the last 8 can be undone
    hist[i] = l; u.push(l);
    const P p[] = {{20 + i * 20, 20}, {20 + i * 20, 120 + i}};
    editStroke(l, Tool(i % 3), p, 2);
  }
  TEST_ASSERT_EQUAL_INT(UNDO_LEVELS, u.size());
  for (int i = 11; i >= 4; i--) {
    TEST_ASSERT_TRUE(u.pop(l));
    TEST_ASSERT_TRUE_MESSAGE(sameLayers(l, hist[i]), "undo did not restore exactly");
    TEST_ASSERT_EQUAL_UINT32(levelDigest(hist[i]), levelDigest(l));
  }
  TEST_ASSERT_FALSE(u.pop(l));
  TEST_ASSERT_EQUAL_INT(0, u.size());
}

void test_undo_restores_pins_but_not_name_or_rev() {
  static LevelSnap mem[UNDO_LEVELS];
  UndoRing u; u.attach(mem, UNDO_LEVELS);
  Level l; blank(l); std::strcpy(l.name, "Mine"); l.rev = 7;
  u.push(l);
  Pin p = pinGrab(l, 42, 158); TEST_ASSERT_TRUE(p == Pin::Start);
  pinMove(l, p, 150, 100); l.rev = 9; std::strcpy(l.name, "Other");
  TEST_ASSERT_TRUE(u.pop(l));
  TEST_ASSERT_EQUAL_INT(40, l.startX); TEST_ASSERT_EQUAL_INT(160, l.startY);
  TEST_ASSERT_EQUAL_UINT32(9, l.rev); TEST_ASSERT_EQUAL_STRING("Other", l.name);
}

void test_pin_grab_radius_and_nearest() {
  Level l; blank(l);                                           // start (40,160); flag base (280,168), grab centre (280,156)
  TEST_ASSERT_TRUE(pinGrab(l, 40, 160) == Pin::Start);
  TEST_ASSERT_TRUE(pinGrab(l, 40 + PIN_GRAB_RADIUS, 160) == Pin::Start);           // exactly on the radius: grabbed
  TEST_ASSERT_TRUE(pinGrab(l, 40 + PIN_GRAB_RADIUS + 1, 160) == Pin::None);        // one pixel further: paints instead
  TEST_ASSERT_TRUE(pinGrab(l, 280, 156) == Pin::Flag);
  TEST_ASSERT_TRUE(pinGrab(l, 280, 156 - PIN_GRAB_RADIUS) == Pin::Flag);
  TEST_ASSERT_TRUE(pinGrab(l, 280, 156 - PIN_GRAB_RADIUS - 1) == Pin::None);
  TEST_ASSERT_TRUE(pinGrab(l, 280, 168 + 10) == Pin::Flag);    // measured from the flag centre, not the pole base: 22 px below the centre
  l.startX = 100; l.startY = 100; l.flagX = 120; l.flagY = 112;                    // overlapping grab circles: the nearer pin wins
  TEST_ASSERT_TRUE(pinGrab(l, 104, 100) == Pin::Start);
  TEST_ASSERT_TRUE(pinGrab(l, 118, 100) == Pin::Flag);
}

void test_pin_move_clamps_and_drop_clears_a_hole() {
  Level l; blank(l);
  const P g[] = {{100, 100}, {200, 100}};
  editStroke(l, Tool::Ground, g, 2);
  pinMove(l, Pin::Start, 150, 100);
  TEST_ASSERT_EQUAL_INT(150, l.startX); TEST_ASSERT_EQUAL_INT(100, l.startY);
  TEST_ASSERT_TRUE(getBit(l.ground, 150, 100));                                    // dragging alone does not cut
  pinDrop(l, Pin::Start);
  for (int dy = -START_SAFE_RADIUS; dy <= START_SAFE_RADIUS; dy++) for (int dx = -START_SAFE_RADIUS; dx <= START_SAFE_RADIUS; dx++)
    if (dx * dx + dy * dy <= (START_SAFE_RADIUS - 1) * (START_SAFE_RADIUS - 1)) TEST_ASSERT_FALSE(getBit(l.ground, 150 + dx, 100 + dy));
  TEST_ASSERT_TRUE(getBit(l.ground, 150 + START_SAFE_RADIUS + 3, 100));            // the rest of the stroke stays
  pinMove(l, Pin::Flag, 160, 100);
  TEST_ASSERT_EQUAL_INT(160, l.flagX); TEST_ASSERT_EQUAL_INT(100 + FLAG_CENTER_DY, l.flagY);
  pinDrop(l, Pin::Flag);
  TEST_ASSERT_FALSE(getBit(l.ground, 160, 100 + FLAG_SAFE_RADIUS - 2));
  pinMove(l, Pin::Start, -50, 999);                                                // dragged off the field: stays on it
  TEST_ASSERT_EQUAL_INT(8, l.startX); TEST_ASSERT_EQUAL_INT(WORLD_H - 2, l.startY);
  pinMove(l, Pin::Flag, 999, -50);
  TEST_ASSERT_EQUAL_INT(WORLD_W - 8, l.flagX); TEST_ASSERT_EQUAL_INT(8 + FLAG_CENTER_DY, l.flagY);
}

void test_ghost_finger_timing() {
  GhostPose g;
  TEST_ASSERT_FALSE(ghostPose(0, g));
  TEST_ASSERT_FALSE(ghostPose(GHOST_IDLE_S - 0.01, g));
  TEST_ASSERT_TRUE(ghostPose(GHOST_IDLE_S, g));
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 72, g.x); TEST_ASSERT_DOUBLE_WITHIN(0.01, 118, g.y);
  TEST_ASSERT_TRUE(ghostPose(GHOST_IDLE_S + 1.25, g));                             // half way through the 2.5 s draw
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 72 + 88, g.x); TEST_ASSERT_DOUBLE_WITHIN(0.01, 124, g.y);
  TEST_ASSERT_TRUE(ghostPose(GHOST_IDLE_S + 2.5, g));
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 248, g.x);
  TEST_ASSERT_TRUE(ghostPose(GHOST_IDLE_S + 3.0, g));                              // rests at the end for 0.625 s
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 248, g.x);
  TEST_ASSERT_FALSE(ghostPose(GHOST_IDLE_S + 3.2, g));                             // then vanishes ...
  TEST_ASSERT_TRUE(ghostPose(GHOST_IDLE_S + 4.5 + 0.5, g));                        // ... and starts over every 4.5 s
}

// ---------------------------------------------------------------------------------------------------------------------
// The scenario of emu/scripts/draw_strokes.txt: the same touches, applied to a blank level with the plain reference functions.
// Each state the emulator saves (after every 5.5 s of idle) is one `DRAW_EXPECT` line.
void test_scenario_script() {
  static const P a[] = {{60, 100}, {100, 60}, {150, 100}, {200, 60}, {250, 100}};          // s1 ground zig-zag
  static const P sp[] = {{60, 130}, {260, 130}};                                            // s2 spike line
  static const P bo[] = {{100, 40}, {100, 150}};                                            // s3 bouncy, crosses both
  static const P er[] = {{150, 30}, {150, 140}};                                            // s4 eraser, crosses all
  static const P zz[] = {{20, 70}, {300, 75}, {20, 95}, {300, 100}};                       // fast scribble, 280 px per sample
  static const P pinPathS[] = {{45, 158}, {100, 150}, {150, 100}};                          // start pin dragged onto the zig-zag
  static const P pinPathF[] = {{282, 160}, {240, 100}, {200, 60}};                          // flag pin dragged onto the zig-zag
  static const P near[] = {{176, 100}, {176, 130}};                                         // 26 px from the start pin: paints, does not grab
  static const P sp2[] = {{230, 140}, {280, 140}};                                          // after the toolbar / chip drags: spike
  Level l; blank(l);
  uint32_t seq[16]; int n = 0;
  auto save = [&] { seq[n++] = levelDigest(l); };
  Level h1, h2, h3, h4;
  refStroke(l, MAT_GROUND, a, 5, BRUSH_RADIUS);  h1 = l; save();                            // 1 save
  refStroke(l, MAT_SPIKE, sp, 2, BRUSH_RADIUS);  h2 = l; save();                            // 2
  refStroke(l, MAT_BOUNCY, bo, 2, BRUSH_RADIUS); h3 = l; save();                            // 3
  refStroke(l, MAT_ERASE, er, 2, ERASER_RADIUS); h4 = l; save();                            // 4
  l = h3; save();                                                                           // 5 undo
  l = h2; save();                                                                           // 6 undo
  l = h1; save();                                                                           // 7 undo
  refStroke(l, MAT_GROUND, zz, 4, BRUSH_RADIUS); save();                                    // 8 fast scribble
  { l.startX = 150; l.startY = 100; clearAround(l, 150, 100, START_SAFE_RADIUS); } save();  // 9 start pin dropped on ground (no paint from the drag)
  { l.flagX = 200; l.flagY = 60 + FLAG_CENTER_DY; clearAround(l, 200, 60, FLAG_SAFE_RADIUS); } save();   // 10 flag pin likewise
  refStroke(l, MAT_GROUND, near, 2, BRUSH_RADIUS); save();                                  // 11 stroke next to a pin
  refStroke(l, MAT_SPIKE, sp2, 2, BRUSH_RADIUS); save();                                    // 12 after toolbar/chip drags that must not paint
  (void)pinPathS; (void)pinPathF;
  for (int i = 0; i < n; i++) std::printf("DRAW_EXPECT %d %08X\n", i + 1, unsigned(seq[i]));
  TEST_ASSERT_EQUAL_INT(12, n);

  // the same through the code the screen runs
  Level e; blank(e);
  static LevelSnap mem[UNDO_LEVELS]; UndoRing u; u.attach(mem, UNDO_LEVELS);
  u.push(e); editStroke(e, Tool::Ground, a, 5);  TEST_ASSERT_EQUAL_UINT32(seq[0], levelDigest(e));
  u.push(e); editStroke(e, Tool::Spike, sp, 2);  TEST_ASSERT_EQUAL_UINT32(seq[1], levelDigest(e));
  u.push(e); editStroke(e, Tool::Bouncy, bo, 2); TEST_ASSERT_EQUAL_UINT32(seq[2], levelDigest(e));
  u.push(e); editStroke(e, Tool::Eraser, er, 2); TEST_ASSERT_EQUAL_UINT32(seq[3], levelDigest(e));
  u.pop(e); TEST_ASSERT_EQUAL_UINT32(seq[4], levelDigest(e));
  u.pop(e); TEST_ASSERT_EQUAL_UINT32(seq[5], levelDigest(e));
  u.pop(e); TEST_ASSERT_EQUAL_UINT32(seq[6], levelDigest(e));
  u.push(e); editStroke(e, Tool::Ground, zz, 4); TEST_ASSERT_EQUAL_UINT32(seq[7], levelDigest(e));
  u.push(e); { const Pin p = pinGrab(e, pinPathS[0].x, pinPathS[0].y); TEST_ASSERT_TRUE(p == Pin::Start); for (const P& q : pinPathS) pinMove(e, p, q.x, q.y); pinDrop(e, p); }
  TEST_ASSERT_EQUAL_UINT32(seq[8], levelDigest(e));
  u.push(e); { const Pin p = pinGrab(e, pinPathF[0].x, pinPathF[0].y); TEST_ASSERT_TRUE(p == Pin::Flag); for (const P& q : pinPathF) pinMove(e, p, q.x, q.y); pinDrop(e, p); }
  TEST_ASSERT_EQUAL_UINT32(seq[9], levelDigest(e));
  TEST_ASSERT_TRUE(pinGrab(e, near[0].x, near[0].y) == Pin::None);
  u.push(e); editStroke(e, Tool::Ground, near, 2); TEST_ASSERT_EQUAL_UINT32(seq[10], levelDigest(e));
  u.push(e); editStroke(e, Tool::Spike, sp2, 2); TEST_ASSERT_EQUAL_UINT32(seq[11], levelDigest(e));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_stroke_equals_paintLine_polyline);
  RUN_TEST(test_single_tap_stamps_a_disc);
  RUN_TEST(test_fast_scribble_has_no_gaps);
  RUN_TEST(test_eraser_removes_every_material);
  RUN_TEST(test_materials_are_exclusive);
  RUN_TEST(test_pen_click_every_24px);
  RUN_TEST(test_undo_ring_restores_exactly_and_keeps_8);
  RUN_TEST(test_undo_restores_pins_but_not_name_or_rev);
  RUN_TEST(test_pin_grab_radius_and_nearest);
  RUN_TEST(test_pin_move_clamps_and_drop_clears_a_hole);
  RUN_TEST(test_ghost_finger_timing);
  RUN_TEST(test_scenario_script);
  return UNITY_END();
}
