// Native tests of lib/core/src/tilt.h: the tilt filter must give the same numbers as the TypeScript in sim/src/input.ts.
#include <unity.h>
#include <cmath>
#include "tilt.h"

using namespace scribble;

void setUp() {}
void tearDown() {}

// the reference: input.ts poll(), line by line, in plain C++
static double refStep(double& smoothed, double rawAx) {
  const double mag = std::fmax(0, std::fabs(rawAx) - TILT_DEADZONE) / (TILT_FULL - TILT_DEADZONE);
  const double sign = rawAx > 0 ? 1 : rawAx < 0 ? -1 : 0;
  const double target = sign * std::fmin(1, mag);
  smoothed += (target - smoothed) * TILT_SMOOTH;
  if (std::fabs(smoothed) < 0.002) smoothed = 0;
  return smoothed;
}

void test_dead_zone_gives_exactly_zero() {
  TEST_ASSERT_EQUAL_DOUBLE(0, tiltTarget(0));
  TEST_ASSERT_EQUAL_DOUBLE(0, tiltTarget(TILT_DEADZONE));
  TEST_ASSERT_EQUAL_DOUBLE(0, tiltTarget(-TILT_DEADZONE));
  TEST_ASSERT_EQUAL_DOUBLE(0, tiltTarget(0.05));
  TiltFilter f; for (int i = 0; i < 100; i++) TEST_ASSERT_EQUAL_DOUBLE(0, f.step(0.04));    // sensor noise at rest never moves the ball
}

void test_linear_between_dead_zone_and_full() {
  const double mid = (TILT_DEADZONE + TILT_FULL) / 2;
  TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.5, tiltTarget(mid));
  TEST_ASSERT_DOUBLE_WITHIN(1e-12, -0.5, tiltTarget(-mid));
  TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.0, tiltTarget(TILT_FULL));
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.25, tiltTarget(TILT_DEADZONE + 0.25 * (TILT_FULL - TILT_DEADZONE)));
}

void test_saturates_at_plus_minus_one() {
  TEST_ASSERT_EQUAL_DOUBLE(1, tiltTarget(0.9));
  TEST_ASSERT_EQUAL_DOUBLE(-1, tiltTarget(-0.9));
  TEST_ASSERT_EQUAL_DOUBLE(1, tiltTarget(1e9));
  TiltFilter f; double v = 0; for (int i = 0; i < 200; i++) v = f.step(1.0);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1.0, v);
  TEST_ASSERT_TRUE(v <= 1.0);
}

void test_sign_is_preserved() {
  TiltFilter a, b; double up = 0, dn = 0;
  for (int i = 0; i < 20; i++) { up = a.step(0.3); dn = b.step(-0.3); }
  TEST_ASSERT_TRUE(up > 0.5); TEST_ASSERT_TRUE(dn < -0.5);
  TEST_ASSERT_EQUAL_DOUBLE(up, -dn);
}

void test_smoothing_is_a_first_order_low_pass() {
  TiltFilter f;
  TEST_ASSERT_DOUBLE_WITHIN(1e-12, TILT_SMOOTH, f.step(TILT_FULL));                                  // first tick: SMOOTH * target
  TEST_ASSERT_DOUBLE_WITHIN(1e-12, TILT_SMOOTH + (1 - TILT_SMOOTH) * TILT_SMOOTH, f.step(TILT_FULL));
  double v = 0; for (int i = 0; i < 30; i++) v = f.step(TILT_FULL);
  TEST_ASSERT_DOUBLE_WITHIN(1e-3, 1.0, v);                                                           // ~ settled after half a second
  // releasing decays back to exactly zero (the 0.002 snap), never lingering
  int n = 0; while (f.smoothed != 0 && n < 200) { f.step(0); n++; }
  TEST_ASSERT_TRUE(n < 60);
  TEST_ASSERT_EQUAL_DOUBLE(0, f.smoothed);
}

void test_matches_the_typescript_reference_on_a_wobbly_sequence() {
  TiltFilter f; double ref = 0;
  for (int i = 0; i < 600; i++) {
    const double raw = 0.45 * std::sin(i * 0.05) + 0.1 * std::sin(i * 0.9) + (i > 400 ? 0.2 : 0);
    const double a = f.step(raw), b = refStep(ref, raw);
    TEST_ASSERT_DOUBLE_WITHIN(1e-15, b, a);
    TEST_ASSERT_TRUE(a >= -1.0 && a <= 1.0);
  }
}

void test_reset() { TiltFilter f; f.step(1); f.reset(); TEST_ASSERT_EQUAL_DOUBLE(0, f.smoothed); }

void test_upright_standing_on_the_base() {
  TEST_ASSERT_TRUE(uprightFromAccel(0.0, -0.98, 0.05, false));     // the emulator's "upright" reading
  TEST_ASSERT_TRUE(uprightFromAccel(0.0, +0.98, 0.05, false));     // either sign of the vertical axis
  TEST_ASSERT_TRUE(uprightFromAccel(0.1, -0.85, 0.5, false));      // leaning back a little
}

void test_upright_flat_on_the_table_is_not_upright() {
  TEST_ASSERT_FALSE(uprightFromAccel(0.0, 0.0, 0.97, false));      // the emulator's default
  TEST_ASSERT_FALSE(uprightFromAccel(0.0, 0.0, 0.97, true));
  TEST_ASSERT_FALSE(uprightFromAccel(0.0, 0.5, 0.85, false));      // 30 degrees up from flat
}

void test_upright_rolled_sideways_is_not_upright() {
  TEST_ASSERT_FALSE(uprightFromAccel(0.8, 0.5, 0.3, false));
  TEST_ASSERT_FALSE(uprightFromAccel(-0.4, -0.9, 0.1, false));     // too much roll to ENTER...
  TEST_ASSERT_TRUE(uprightFromAccel(-0.4, -0.9, 0.1, true));       // ...but fine while already upright (hysteresis)
}

void test_upright_hysteresis_does_not_flicker() {
  // lean back slowly: |along| 0.9 -> 0.5 and forth; enters at 0.77, leaves below 0.60
  bool up = true; const double path[] = { 0.9, 0.7, 0.65, 0.62, 0.58, 0.65, 0.7, 0.75, 0.78 };
  const bool want[] = { true, true, true, true, false, false, false, false, true };
  for (int i = 0; i < 9; i++) { up = uprightFromAccel(0, -path[i], 0.3, up); TEST_ASSERT_EQUAL_INT_MESSAGE(want[i], up, "step"); }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_dead_zone_gives_exactly_zero);
  RUN_TEST(test_linear_between_dead_zone_and_full);
  RUN_TEST(test_saturates_at_plus_minus_one);
  RUN_TEST(test_sign_is_preserved);
  RUN_TEST(test_smoothing_is_a_first_order_low_pass);
  RUN_TEST(test_matches_the_typescript_reference_on_a_wobbly_sequence);
  RUN_TEST(test_reset);
  RUN_TEST(test_upright_standing_on_the_base);
  RUN_TEST(test_upright_flat_on_the_table_is_not_upright);
  RUN_TEST(test_upright_rolled_sideways_is_not_upright);
  RUN_TEST(test_upright_hysteresis_does_not_flicker);
  return UNITY_END();
}
