#include <unity.h>

#include <cmath>

#include "algaguard/npk_estimator.hpp"
#include "golden_vectors.hpp"

void setUp() {}
void tearDown() {}

namespace {
// Unity's double-precision asserts are disabled in this build, and its float
// asserts would hide a mismatch smaller than ~1e-4 mg/L.
void assertWithin(double expected, double actual) {
  TEST_ASSERT_TRUE_MESSAGE(std::fabs(expected - actual) < 1e-9,
                           "C++ estimate differs from scikit-learn");
}
}  // namespace

void test_matches_scikit_learn_predictions() {
  for (const auto& v : kNpkGoldenVectors) {
    assertWithin(v.nitrateMgL, algaguard::estimate_nitrate_mg_l(v.ph));
    assertWithin(v.phosphateMgL, algaguard::estimate_phosphate_mg_l(v.ph));
    assertWithin(v.potassiumMgL,
                 algaguard::estimate_potassium_mg_l(v.temperatureC));
  }
}

void test_in_range_reading_produces_all_three_estimates() {
  const auto estimate = algaguard::estimate_npk(8.0, 25.0);
  TEST_ASSERT_TRUE(estimate.nitrateMgL.has_value());
  TEST_ASSERT_TRUE(estimate.phosphateMgL.has_value());
  TEST_ASSERT_TRUE(estimate.potassiumMgL.has_value());
  // Nitrate falls as pH rises in the training data.
  TEST_ASSERT_TRUE(*algaguard::estimate_npk(7.5, 25.0).nitrateMgL >
                   *algaguard::estimate_npk(8.8, 25.0).nitrateMgL);
}

void test_out_of_range_inputs_are_not_extrapolated() {
  const auto lowPh = algaguard::estimate_npk(6.0, 25.0);
  TEST_ASSERT_FALSE(lowPh.nitrateMgL.has_value());
  TEST_ASSERT_FALSE(lowPh.phosphateMgL.has_value());
  TEST_ASSERT_TRUE(lowPh.potassiumMgL.has_value());

  const auto hotWater = algaguard::estimate_npk(8.0, 40.0);
  TEST_ASSERT_TRUE(hotWater.nitrateMgL.has_value());
  TEST_ASSERT_FALSE(hotWater.potassiumMgL.has_value());

  const auto notANumber = algaguard::estimate_npk(NAN, NAN);
  TEST_ASSERT_FALSE(notANumber.nitrateMgL.has_value());
  TEST_ASSERT_FALSE(notANumber.potassiumMgL.has_value());
}

void test_estimates_are_never_negative() {
  for (double ph = algaguard::npk_weights::kPhMin;
       ph <= algaguard::npk_weights::kPhMax; ph += 0.01) {
    const auto estimate = algaguard::estimate_npk(ph, 25.0);
    TEST_ASSERT_TRUE(*estimate.nitrateMgL >= 0.0);
    TEST_ASSERT_TRUE(*estimate.phosphateMgL >= 0.0);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_matches_scikit_learn_predictions);
  RUN_TEST(test_in_range_reading_produces_all_three_estimates);
  RUN_TEST(test_out_of_range_inputs_are_not_extrapolated);
  RUN_TEST(test_estimates_are_never_negative);
  return UNITY_END();
}
