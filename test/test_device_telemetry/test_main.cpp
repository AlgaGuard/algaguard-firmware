#include <unity.h>

#include <string>

#include "algaguard/device_telemetry.hpp"

void setUp() {}
void tearDown() {}

namespace {
const algaguard::ActiveProfileReference profile{
    "10000000-0000-4000-8000-000000000001", "1.0.0"};

algaguard::LocalDemoReading reading() { return {5, 24.2, 7.1, 900.0, 63.4}; }

void test_device_payload_uses_canonical_schema_and_explicit_source() {
  const auto value = algaguard::build_device_telemetry_payload(
      "AG-000001", profile, reading(), "2026-08-02T10:00:00Z",
      "2026-08-02T10:00:00Z", "20000000-0000-4000-8000-000000000002",
      "30000000-0000-4000-8000-000000000003", 5000, false, false, "SIMULATED",
      algaguard::kScenarioLocalDemo);
  TEST_ASSERT_TRUE(value.has_value());
  TEST_ASSERT_NOT_EQUAL(std::string::npos, value->find(
      "\"schema\":\"urn:algaguard:schema:mqtt:telemetry-batch:v1\""));
  TEST_ASSERT_NOT_EQUAL(std::string::npos,
                        value->find("\"qualityFlags\":[\"SIMULATED\"]"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos,
                        value->find("\"simulationScenario\":\"device-local-demo\""));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, value->find("\"sampleCount\":1"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, value->find("\"nutrientPercent\":63.4"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, value->find("\"isReplay\":false"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, value->find("\"createdFromSd\":false"));
  TEST_ASSERT_EQUAL(std::string::npos, value->find("sessionToken"));
  TEST_ASSERT_EQUAL(std::string::npos, value->find("password"));
}

void test_real_sensor_payload_reports_real_quality_and_scenario() {
  const auto value = algaguard::build_device_telemetry_payload(
      "AG-000001", profile, reading(), "2026-08-02T10:00:00Z",
      "2026-08-02T10:00:00Z", "20000000-0000-4000-8000-000000000002",
      "30000000-0000-4000-8000-000000000003", 5000, false, false, "REAL",
      algaguard::kScenarioRealSensors);
  TEST_ASSERT_TRUE(value.has_value());
  TEST_ASSERT_NOT_EQUAL(std::string::npos, value->find("\"qualityFlags\":[\"REAL\"]"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos,
                        value->find("\"simulationScenario\":\"device-real-sensors\""));
}

void test_degraded_reading_is_flagged() {
  const auto value = algaguard::build_device_telemetry_payload(
      "AG-000001", profile, reading(), "2026-08-02T10:00:00Z",
      "2026-08-02T10:00:00Z", "20000000-0000-4000-8000-000000000002",
      "30000000-0000-4000-8000-000000000003", 5000, false, false, "DEGRADED",
      algaguard::kScenarioRealSensors);
  TEST_ASSERT_TRUE(value.has_value());
  TEST_ASSERT_NOT_EQUAL(std::string::npos,
                        value->find("\"qualityFlags\":[\"DEGRADED\"]"));
}

void test_replayed_sd_sample_preserves_original_observed_at_distinct_from_sent_at() {
  // sentAt is when this replay is actually being transmitted; observedAt
  // must remain the original capture time from when it was buffered.
  const auto value = algaguard::build_device_telemetry_payload(
      "AG-000001", profile, reading(), "2026-08-02T12:00:00Z",
      "2026-08-02T10:00:00Z", "20000000-0000-4000-8000-000000000002",
      "30000000-0000-4000-8000-000000000003", 5000, true, true, "REAL",
      algaguard::kScenarioRealSensors);
  TEST_ASSERT_TRUE(value.has_value());
  TEST_ASSERT_NOT_EQUAL(std::string::npos, value->find("\"isReplay\":true"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, value->find("\"createdFromSd\":true"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos,
                        value->find("\"sentAt\":\"2026-08-02T12:00:00Z\""));
  TEST_ASSERT_NOT_EQUAL(std::string::npos,
                        value->find("\"observedAt\":\"2026-08-02T10:00:00Z\""));
}

void test_invalid_profile_or_sensor_bounds_fail_closed() {
  auto invalid = reading();
  invalid.ph = 15;
  TEST_ASSERT_FALSE(algaguard::build_device_telemetry_payload(
      "AG-000001", profile, invalid, "2026-08-02T10:00:00Z",
      "2026-08-02T10:00:00Z", "20000000-0000-4000-8000-000000000002",
      "30000000-0000-4000-8000-000000000003", 5000, false, false, "REAL",
      algaguard::kScenarioRealSensors).has_value());
  auto outOfRangeNutrient = reading();
  outOfRangeNutrient.nutrientPercent = 150;
  TEST_ASSERT_FALSE(algaguard::build_device_telemetry_payload(
      "AG-000001", profile, outOfRangeNutrient, "2026-08-02T10:00:00Z",
      "2026-08-02T10:00:00Z", "20000000-0000-4000-8000-000000000002",
      "30000000-0000-4000-8000-000000000003", 5000, false, false, "REAL",
      algaguard::kScenarioRealSensors).has_value());
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
  RUN_TEST(test_device_payload_uses_canonical_schema_and_explicit_source);
  RUN_TEST(test_real_sensor_payload_reports_real_quality_and_scenario);
  RUN_TEST(test_degraded_reading_is_flagged);
  RUN_TEST(test_replayed_sd_sample_preserves_original_observed_at_distinct_from_sent_at);
  RUN_TEST(test_invalid_profile_or_sensor_bounds_fail_closed);
  RUN_TEST(test_qos1_window_is_single_inflight_bounded_and_acknowledged);
  RUN_TEST(test_ack_mismatch_and_retry_exhaustion_do_not_release_early);
  return UNITY_END();
}
