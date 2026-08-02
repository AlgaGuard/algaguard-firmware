#include <unity.h>

#include <string>

#include "algaguard/device_telemetry.hpp"

void setUp() {}
void tearDown() {}

namespace {
const algaguard::ActiveProfileReference profile{
    "10000000-0000-4000-8000-000000000001", "1.0.0"};

algaguard::LocalDemoReading reading() {
  return {5, 24.2, 7.1, 900.0, 2.4, 0.35, 1.8};
}

void test_device_payload_uses_canonical_schema_and_explicit_simulation_source() {
  const auto value = algaguard::build_device_simulated_telemetry(
      "AG-000001", profile, reading(), "2026-08-02T10:00:00Z",
      "20000000-0000-4000-8000-000000000002",
      "30000000-0000-4000-8000-000000000003", 5000);
  TEST_ASSERT_TRUE(value.has_value());
  TEST_ASSERT_NOT_EQUAL(std::string::npos, value->find(
      "\"schema\":\"urn:algaguard:schema:mqtt:telemetry-batch:v1\""));
  TEST_ASSERT_NOT_EQUAL(std::string::npos,
                        value->find("\"qualityFlags\":[\"SIMULATED\"]"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos,
                        value->find("\"simulationScenario\":\"device-local-demo\""));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, value->find("\"sampleCount\":1"));
  TEST_ASSERT_EQUAL(std::string::npos, value->find("sessionToken"));
  TEST_ASSERT_EQUAL(std::string::npos, value->find("password"));
}

void test_invalid_profile_or_sensor_bounds_fail_closed() {
  auto invalid = reading();
  invalid.ph = 15;
  TEST_ASSERT_FALSE(algaguard::build_device_simulated_telemetry(
      "AG-000001", profile, invalid, "2026-08-02T10:00:00Z",
      "20000000-0000-4000-8000-000000000002",
      "30000000-0000-4000-8000-000000000003", 5000).has_value());
  TEST_ASSERT_FALSE(algaguard::valid_profile_reference(
      {"not-a-uuid", "1.0.0"}));
}

void test_qos1_window_is_single_inflight_bounded_and_acknowledged() {
  algaguard::DeviceTelemetryPublishWindow window;
  TEST_ASSERT_TRUE(window.installProfile(profile));
  TEST_ASSERT_TRUE(window.begin(
      "30000000-0000-4000-8000-000000000003", "safe-payload", 1000));
  TEST_ASSERT_FALSE(window.begin(
      "40000000-0000-4000-8000-000000000004", "second", 1001));
  TEST_ASSERT_TRUE(window.retryDue(11000));
  TEST_ASSERT_TRUE(window.retry(11000));
  TEST_ASSERT_TRUE(window.acknowledge(
      "30000000-0000-4000-8000-000000000003", "ACCEPTED"));
  TEST_ASSERT_FALSE(window.pending());
}

void test_ack_mismatch_and_retry_exhaustion_do_not_release_early() {
  algaguard::DeviceTelemetryPublishWindow window;
  TEST_ASSERT_TRUE(window.installProfile(profile));
  TEST_ASSERT_TRUE(window.begin(
      "30000000-0000-4000-8000-000000000003", "safe-payload", 0));
  TEST_ASSERT_FALSE(window.acknowledge(
      "40000000-0000-4000-8000-000000000004", "ACCEPTED"));
  TEST_ASSERT_TRUE(window.retry(10000));
  TEST_ASSERT_TRUE(window.retry(20000));
  TEST_ASSERT_TRUE(window.retry(30000));
  TEST_ASSERT_TRUE(window.exhausted(40000));
  TEST_ASSERT_TRUE(window.pending());
}
}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_device_payload_uses_canonical_schema_and_explicit_simulation_source);
  RUN_TEST(test_invalid_profile_or_sensor_bounds_fail_closed);
  RUN_TEST(test_qos1_window_is_single_inflight_bounded_and_acknowledged);
  RUN_TEST(test_ack_mismatch_and_retry_exhaustion_do_not_release_early);
  return UNITY_END();
}
