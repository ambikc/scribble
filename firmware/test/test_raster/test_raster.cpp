// Native unit tests of the anti-aliased shape rasteriser (lib/raster):  pio test -e native
// The areas of the shapes must come out right (coverage sums / 255 = area in pixels), wherever the shape sits.
#include <unity.h>
#include <cmath>
#include "raster.h"

using namespace scribble::raster;

static uint8_t buf[100 * 100];
static Mask M;

void setUp() { M.a = buf; M.w = 100; M.h = 100; M.clear(); }
void tearDown() {}

static double area() { return double(M.sum()) / 255.0; }

void test_disc_area_matches_pi_r_squared() {
  const float rs[] = {3, 8.5f, 20}; const float offs[] = {0, 0.37f, 0.5f};
  for (float r : rs) for (float o : offs) {
    M.clear(); M.disc(50 + o, 50 + o * 0.7f, r);
    const double want = 3.14159265 * r * r;
    TEST_ASSERT_DOUBLE_WITHIN(want * 0.03 + 0.6, want, area());
  }
}

void test_polygon_area_is_exact_for_a_rectangle() {
  const float xy[] = {10.25f, 20.5f, 40.75f, 20.5f, 40.75f, 35.25f, 10.25f, 35.25f};
  M.polygon(xy, 4);
  TEST_ASSERT_DOUBLE_WITHIN(1.5, 30.5 * 14.75, area());
}

void test_triangle_area() {
  const float xy[] = {10, 10, 50, 10, 10, 50};
  M.polygon(xy, 3);
  TEST_ASSERT_DOUBLE_WITHIN(8.0, 800.0, area());
}

void test_capsule_area_is_length_times_width_plus_the_round_caps() {
  M.capsule(20, 50, 80, 50, 6);
  TEST_ASSERT_DOUBLE_WITHIN(5.0, 60.0 * 6 + 3.14159265 * 9, area());
  M.clear(); M.capsule(20, 20, 70, 60, 4);
  const double len = std::sqrt(50.0 * 50 + 40 * 40);
  TEST_ASSERT_DOUBLE_WITHIN(5.0, len * 4 + 3.14159265 * 4, area());
}

void test_arc_covers_the_requested_fraction_of_the_ring() {
  M.arc(50, 50, 30, 4, 0, scribble::raster::TWO_PI_F);                   // whole ring: 2 pi r w
  TEST_ASSERT_DOUBLE_WITHIN(12.0, 2 * 3.14159265 * 30 * 4, area());
  M.clear(); M.arc(50, 50, 30, 4, -1.5707963f, 3.14159265f, false);      // a half ring starting at 12 o'clock, clockwise: the right half
  TEST_ASSERT_DOUBLE_WITHIN(10.0, 3.14159265 * 30 * 4, area());
  TEST_ASSERT_TRUE(buf[50 * 100 + 80] > 200);                            // 3 o'clock is on the arc
  TEST_ASSERT_EQUAL_UINT8(0, buf[50 * 100 + 20]);                        // 9 o'clock is not
}

void test_shapes_are_clipped_to_the_mask() {
  M.disc(2, 2, 10); M.capsule(-50, 3, 200, 3, 4); static const float big[8] = {-20, -20, 150, -20, 150, 150, -20, 150}; M.polygon(big, 4);
  TEST_ASSERT_EQUAL_UINT8(255, buf[0]);
  TEST_ASSERT_EQUAL_UINT8(255, buf[99 * 100 + 99]);
}

void test_round_rect_distance_is_signed() {
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -10.0f, roundRectSd(50, 10, 0, 0, 100, 20, 4));      // centre of a 100x20 box: 10 inside
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, roundRectSd(50, 0, 0, 0, 100, 20, 4));          // on the top edge
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 3.0f, roundRectSd(103, 10, 0, 0, 100, 20, 4));        // 3 px outside the right edge
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 4.0f * (1.41421356f - 1.0f), roundRectSd(0, 0, 0, 0, 100, 20, 4));  // the sharp corner is cut away
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_disc_area_matches_pi_r_squared);
  RUN_TEST(test_polygon_area_is_exact_for_a_rectangle);
  RUN_TEST(test_triangle_area);
  RUN_TEST(test_capsule_area_is_length_times_width_plus_the_round_caps);
  RUN_TEST(test_arc_covers_the_requested_fraction_of_the_ring);
  RUN_TEST(test_shapes_are_clipped_to_the_mask);
  RUN_TEST(test_round_rect_distance_is_signed);
  return UNITY_END();
}
