// Native unit tests of the portable core (level + physics), run on the PC:  pio test -e native
// The golden data (shared/generated/golden.h) comes from the browser simulator, so these tests prove that the
// C++ port gives the SAME bytes and the SAME ball positions as the TypeScript the simulator runs.
#include <unity.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "level.h"
#include "physics.h"
#include "golden.h"

using namespace scribble;

static Level L, M;
static uint8_t buf[MAX_FILE_BYTES];

void setUp() {}
void tearDown() {}

static void expectBytes(const uint8_t* want, size_t wantN, const Level& l) {
  const size_t n = encodeLevel(l, true, buf, sizeof buf);
  TEST_ASSERT_EQUAL_UINT32(wantN, n);
  for (size_t i = 0; i < n; i++) if (buf[i] != want[i]) { char msg[64]; snprintf(msg, sizeof msg, "first difference at byte %u", unsigned(i)); TEST_FAIL_MESSAGE(msg); }
}

void test_file_size_matches_docs() {
  starterLevel(L);
  TEST_ASSERT_EQUAL_UINT32(64 + 1080 + 3 * 7680, encodeLevel(L, true, buf, sizeof buf));
  TEST_ASSERT_EQUAL_UINT32(64 + 3 * 7680, encodeLevel(L, false, buf, sizeof buf));
}

void test_lessons_are_byte_identical_to_the_simulator() {
  starterLevel(L); expectBytes(golden::level_lesson1_bridge, sizeof golden::level_lesson1_bridge, L);
  lessonSpike(L); expectBytes(golden::level_lesson2_spikes, sizeof golden::level_lesson2_spikes, L);
  lessonBouncy(L); expectBytes(golden::level_lesson3_bounce, sizeof golden::level_lesson3_bounce, L);
}

void test_brush_strokes_are_byte_identical_to_the_simulator() {
  starterLevel(L); std::strcpy(L.name, "Bridged");
  paintLine(L, MAT_GROUND, 72, 116, 120, 130, 4); paintLine(L, MAT_GROUND, 120, 130, 200, 124, 4); paintLine(L, MAT_GROUND, 200, 124, 248, 116, 4);
  expectBytes(golden::level_bridged, sizeof golden::level_bridged, L);       // includes the thumbnail
}

void test_decode_then_encode_round_trips() {
  const uint8_t* files[] = { golden::level_lesson1_bridge, golden::level_lesson2_spikes, golden::level_lesson3_bounce, golden::level_bridged };
  const size_t sizes[] = { sizeof golden::level_lesson1_bridge, sizeof golden::level_lesson2_spikes, sizeof golden::level_lesson3_bounce, sizeof golden::level_bridged };
  for (int k = 0; k < 4; k++) { TEST_ASSERT_EQUAL_INT(LEVEL_OK, decodeLevel(files[k], sizes[k], L, nullptr)); expectBytes(files[k], sizes[k], L); }
}

void test_bad_files_are_rejected() {
  const size_t n = sizeof golden::level_bridged;
  std::memcpy(buf, golden::level_bridged, n); buf[0] = 'X'; TEST_ASSERT_EQUAL_INT(LEVEL_ERR_MAGIC, decodeLevel(buf, n, M, nullptr));
  std::memcpy(buf, golden::level_bridged, n); buf[4] = 9;   TEST_ASSERT_EQUAL_INT(LEVEL_ERR_VERSION, decodeLevel(buf, n, M, nullptr));
  std::memcpy(buf, golden::level_bridged, n); buf[7000] ^= 1; TEST_ASSERT_EQUAL_INT(LEVEL_ERR_CRC, decodeLevel(buf, n, M, nullptr));
  std::memcpy(buf, golden::level_bridged, n); buf[6] = 0;   TEST_ASSERT_EQUAL_INT(LEVEL_ERR_DIMS, decodeLevel(buf, n, M, nullptr));
  TEST_ASSERT_EQUAL_INT(LEVEL_ERR_SIZE, decodeLevel(golden::level_bridged, 5000, M, nullptr));
}

void test_physics_matches_the_simulator_golden_vectors() {
  double worst = 0;
  for (int s = 0; s < golden::nScripts; s++) {
    const golden::Script& sc = *golden::scripts[s];
    TEST_ASSERT_EQUAL_INT(LEVEL_OK, decodeLevel(sc.level, sc.levelSize, L, nullptr));
    World w; newWorld(w, L);
    int k = 0;
    for (int i = 0; i < sc.ticks; i++) {
      double tilt = 0; for (int j = 0; j < sc.nTilt; j++) if (i >= sc.tilt[j].tick) tilt = sc.tilt[j].tilt;
      bool jump = false; for (int j = 0; j < sc.nJumps; j++) if (sc.jumps[j] == i) jump = true;
      physicsStep(w, L, { tilt, jump });
      if ((i + 1) % 30 == 0) {
        const golden::Checkpoint& cp = sc.cps[k++];
        const double d = std::fmax(std::fabs(w.ball.x - cp.x), std::fabs(w.ball.y - cp.y));
        worst = std::fmax(worst, d);
        char msg[96]; snprintf(msg, sizeof msg, "%s tick %d: (%.4f, %.4f) vs (%.4f, %.4f)", sc.name, cp.tick, w.ball.x, w.ball.y, cp.x, cp.y);
        TEST_ASSERT_TRUE_MESSAGE(d <= golden::tolerancePx, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(cp.state, w.state, msg);
      }
    }
  }
  printf("physics golden vectors: worst difference from the simulator = %.9f px\n", worst);
}

void test_jump_apex_is_50px() {
  levelInit(L); fillRect(L, MAT_GROUND, 0, 150, 320, 158); L.startX = 100; L.startY = 142; L.flagX = 300; L.flagY = 20;
  World w; newWorld(w, L);
  for (int i = 0; i < 60; i++) physicsStep(w, L, { 0, false });
  const double rest = w.ball.y; double top = rest;
  for (int i = 0; i < 60; i++) { physicsStep(w, L, { 0, i == 0 }); top = std::fmin(top, w.ball.y); }
  TEST_ASSERT_DOUBLE_WITHIN(2.0, 50.0, rest - top);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_file_size_matches_docs);
  RUN_TEST(test_lessons_are_byte_identical_to_the_simulator);
  RUN_TEST(test_brush_strokes_are_byte_identical_to_the_simulator);
  RUN_TEST(test_decode_then_encode_round_trips);
  RUN_TEST(test_bad_files_are_rejected);
  RUN_TEST(test_physics_matches_the_simulator_golden_vectors);
  RUN_TEST(test_jump_apex_is_50px);
  return UNITY_END();
}
