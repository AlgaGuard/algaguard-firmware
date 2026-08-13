#include <unity.h>

#include "algaguard/nutrient_index.hpp"

namespace {
void assertNear(double expected, double actual) {
  TEST_ASSERT_FLOAT_WITHIN(0.0001F, static_cast<float>(expected),
                           static_cast<float>(actual));
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_triangular_suitability_inside_band_is_one() {
  assertNear(1.0, algaguard::triangular_suitability(7.0, 6.5, 8.5, 1.5));
  assertNear(1.0, algaguard::triangular_suitability(6.5, 6.5, 8.5, 1.5));
  assertNear(1.0, algaguard::triangular_suitability(8.5, 6.5, 8.5, 1.5));
}

void test_triangular_suitability_decays_linearly_outside_band() {
  // Halfway through the tolerance band beyond the high edge -> 0.5.
  assertNear(0.5, algaguard::triangular_suitability(8.5 + 0.75, 6.5, 8.5, 1.5));
  assertNear(0.5, algaguard::triangular_suitability(6.5 - 0.75, 6.5, 8.5, 1.5));
}

void test_triangular_suitability_clamps_at_zero_beyond_tolerance() {
  assertNear(0.0, algaguard::triangular_suitability(20.0, 6.5, 8.5, 1.5));
  assertNear(0.0, algaguard::triangular_suitability(-5.0, 6.5, 8.5, 1.5));
}

void test_nutrient_percent_known_midpoint() {
  // TDS at exactly half the reference concentration, pH and temperature
  // exactly centered in their optimal bands -> both suitability factors are
  // 1.0, so the result is exactly the TDS normalization: 50%.
  const double result = algaguard::nutrient_percent(
      algaguard::kNutrientReferencePpm / 2.0, 7.5, 25.0);
  assertNear(50.0, result);
}

void test_nutrient_percent_never_escapes_zero_to_hundred() {
  TEST_ASSERT_TRUE(algaguard::nutrient_percent(1.0e9, 7.0, 25.0) <= 100.0);
  TEST_ASSERT_TRUE(algaguard::nutrient_percent(0.0, 0.0, 0.0) >= 0.0);
  TEST_ASSERT_TRUE(algaguard::nutrient_percent(-500.0, -10.0, -50.0) >= 0.0);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_triangular_suitability_inside_band_is_one);
  RUN_TEST(test_triangular_suitability_decays_linearly_outside_band);
  RUN_TEST(test_triangular_suitability_clamps_at_zero_beyond_tolerance);
  RUN_TEST(test_nutrient_percent_known_midpoint);
  RUN_TEST(test_nutrient_percent_never_escapes_zero_to_hundred);
  return UNITY_END();
}
