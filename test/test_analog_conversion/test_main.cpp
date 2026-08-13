#include <unity.h>

#include "algaguard/analog_conversion.hpp"

namespace {
void assertNear(double expected, double actual) {
  TEST_ASSERT_FLOAT_WITHIN(0.0001F, static_cast<float>(expected),
                           static_cast<float>(actual));
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_ph_neutral_voltage_reads_exactly_seven() {
  assertNear(7.0, algaguard::ph_from_voltage(algaguard::kPhNeutralVoltage));
}

void test_ph_one_unit_below_neutral_voltage_reads_eight() {
  // Lower voltage than neutral -> higher (more alkaline) pH, matching the
  // formula's sign convention. A regression here would mean the calibration
  // formula's sign was flipped.
  const double voltage = algaguard::kPhNeutralVoltage - algaguard::kPhVoltsPerUnit;
  assertNear(8.0, algaguard::ph_from_voltage(voltage));
}

void test_ph_clamps_to_valid_range() {
  TEST_ASSERT_TRUE(algaguard::ph_from_voltage(0.0) <= 14.0);
  TEST_ASSERT_TRUE(algaguard::ph_from_voltage(100.0) >= 0.0);
}

void test_tds_zero_voltage_is_zero_ppm() {
  assertNear(0.0, algaguard::tds_ppm_from_voltage(0.0, 25.0));
}

void test_tds_increases_with_voltage_at_reference_temperature() {
  const double low = algaguard::tds_ppm_from_voltage(0.5, 25.0);
  const double high = algaguard::tds_ppm_from_voltage(1.5, 25.0);
  TEST_ASSERT_TRUE(high > low);
}

void test_tds_never_negative() {
  TEST_ASSERT_TRUE(algaguard::tds_ppm_from_voltage(0.01, 45.0) >= 0.0);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ph_neutral_voltage_reads_exactly_seven);
  RUN_TEST(test_ph_one_unit_below_neutral_voltage_reads_eight);
  RUN_TEST(test_ph_clamps_to_valid_range);
  RUN_TEST(test_tds_zero_voltage_is_zero_ppm);
  RUN_TEST(test_tds_increases_with_voltage_at_reference_temperature);
  RUN_TEST(test_tds_never_negative);
  return UNITY_END();
}
