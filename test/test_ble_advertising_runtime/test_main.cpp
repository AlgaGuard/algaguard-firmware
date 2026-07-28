#include <unity.h>

#include "algaguard/ble_advertising_runtime.hpp"
#include "algaguard/physical_test_harness.hpp"

void setUp() {}
void tearDown() {}

void test_178_advertising_is_active_only_after_start_success() {
  algaguard::BleAdvertisingRuntimeStatus status{};
  status = algaguard::ble_advertising_start_result(status, -7);
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::BleAdvertisingStage::kAdvStartFailed),
                    static_cast<int>(status.stage));
  TEST_ASSERT_FALSE(status.advertisingActive);
  status = algaguard::ble_advertising_start_result(status, 0);
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::BleAdvertisingStage::kAdvStartOk),
                    static_cast<int>(status.stage));
  TEST_ASSERT_TRUE(status.advertisingActive);
}

void test_179_startup_failure_has_only_safe_stage_and_numeric_code() {
  const auto status = algaguard::ble_advertising_start_result({}, -12);
  const auto diagnostic = algaguard::ble_advertising_safe_diagnostic(status);
  TEST_ASSERT_TRUE(algaguard::physical_text_is_safe(diagnostic));
  TEST_ASSERT_EQUAL_STRING("ADV_START_FAILED",
                           algaguard::ble_advertising_stage_code(status.stage).data());
}

void test_180_legacy_layout_keeps_uuid_and_complete_name_without_truncation() {
  const auto layout = algaguard::kBleLegacyAdvertisingLayout;
  TEST_ASSERT_TRUE(layout.flagsInAdvertising);
  TEST_ASSERT_TRUE(layout.serviceUuidInAdvertising);
  TEST_ASSERT_TRUE(layout.completeNameInScanResponse);
  TEST_ASSERT_TRUE(layout.advertisingFitsLegacyLimit);
  TEST_ASSERT_LESS_OR_EQUAL(algaguard::kBleLegacyAdvertisingMaxBytes,
                            static_cast<int>(3 + 18));
}

void test_181_advertising_failure_is_red_and_never_green() {
  const auto pattern =
      algaguard::physical_test_led_pattern(algaguard::PhysicalTestState::kBleAdvertisingFailed);
  TEST_ASSERT_TRUE(pattern.red);
  TEST_ASSERT_FALSE(pattern.green);
  TEST_ASSERT_FALSE(pattern.blue);
  TEST_ASSERT_TRUE(pattern.bounded);
}

void test_182_advertising_diagnostics_and_oled_are_secret_free() {
  const auto screen = algaguard::physical_test_screen(
      algaguard::PhysicalTestState::kBleAdvertisingFailed, -5);
  const auto diagnostic = algaguard::ble_advertising_safe_diagnostic(
      {algaguard::BleAdvertisingStage::kAdvStartFailed, -5, 1, false});
  TEST_ASSERT_TRUE(algaguard::physical_screen_is_safe(screen));
  TEST_ASSERT_TRUE(algaguard::physical_text_is_safe(diagnostic));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_178_advertising_is_active_only_after_start_success);
  RUN_TEST(test_179_startup_failure_has_only_safe_stage_and_numeric_code);
  RUN_TEST(test_180_legacy_layout_keeps_uuid_and_complete_name_without_truncation);
  RUN_TEST(test_181_advertising_failure_is_red_and_never_green);
  RUN_TEST(test_182_advertising_diagnostics_and_oled_are_secret_free);
  return UNITY_END();
}
