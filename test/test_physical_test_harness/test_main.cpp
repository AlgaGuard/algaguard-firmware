#define ALGAGUARD_DEVELOPMENT_BUILD 1
#define ALGAGUARD_DEVELOPMENT_PHYSICAL_TEST_OPT_IN 1
#define ALGAGUARD_PHYSICAL_TEST_MODE 1
#define ALGAGUARD_WIFI_CREDENTIAL_PERSISTENCE_DISABLED 1

#include <string>

#include <unity.h>

#include "algaguard/physical_test_harness.hpp"

void setUp() {}
void tearDown() {}

void test_172_physical_test_profile_is_rejected_for_production_or_release() {
  TEST_ASSERT_FALSE(algaguard::physical_test_profile_allowed(true, false, true, true));
  TEST_ASSERT_FALSE(algaguard::physical_test_profile_allowed(false, true, true, true));
  TEST_ASSERT_FALSE(algaguard::physical_test_profile_allowed(false, false, false, true));
  TEST_ASSERT_TRUE(algaguard::physical_test_profile_allowed(false, false, true, true));
}

void test_173_volatile_session_clears_all_secret_storage() {
  algaguard::VolatilePhysicalTestSession session;
  TEST_ASSERT_TRUE(session.install("50000000-0000-4000-8000-000000000001", "AG-000001",
                                   "synthetic-development-session-token"));
  TEST_ASSERT_TRUE(session.active());
  session.clear();
  TEST_ASSERT_FALSE(session.active());
  TEST_ASSERT_TRUE(session.secretsCleared());
}

void test_174_oled_states_are_safe_and_retain_physical_warning() {
  for (const auto state : {algaguard::PhysicalTestState::kPhysicalTestMode,
                           algaguard::PhysicalTestState::kBleReady,
                           algaguard::PhysicalTestState::kBleConnected,
                           algaguard::PhysicalTestState::kReceiving,
                           algaguard::PhysicalTestState::kValidating,
                           algaguard::PhysicalTestState::kAccepted,
                           algaguard::PhysicalTestState::kWifiConnecting,
                           algaguard::PhysicalTestState::kWifiConnected,
                           algaguard::PhysicalTestState::kAuthFailed,
                           algaguard::PhysicalTestState::kNetworkNotFound,
                           algaguard::PhysicalTestState::kTimedOut,
                           algaguard::PhysicalTestState::kCancelled,
                           algaguard::PhysicalTestState::kResetRequired}) {
    const auto screen = algaguard::physical_test_screen(state);
    TEST_ASSERT_TRUE(algaguard::physical_screen_is_safe(screen));
    TEST_ASSERT_EQUAL_STRING("PHYSICAL TEST MODE", screen.lines[0].c_str());
  }
}

void test_175_led_mapping_uses_only_external_led_pins() {
  for (const auto state : {algaguard::PhysicalTestState::kBleReady,
                           algaguard::PhysicalTestState::kWifiConnecting,
                           algaguard::PhysicalTestState::kWifiConnected,
                           algaguard::PhysicalTestState::kAuthFailed,
                           algaguard::PhysicalTestState::kResetRequired})
    TEST_ASSERT_TRUE(algaguard::physical_test_led_pattern(state).bounded);
  TEST_ASSERT_EQUAL(14, algaguard::hardware::kLedRed);
  TEST_ASSERT_EQUAL(15, algaguard::hardware::kLedGreen);
  TEST_ASSERT_EQUAL(16, algaguard::hardware::kLedBlue);
  TEST_ASSERT_NOT_EQUAL(38, algaguard::hardware::kLedRed);
  TEST_ASSERT_NOT_EQUAL(38, algaguard::hardware::kLedGreen);
  TEST_ASSERT_NOT_EQUAL(38, algaguard::hardware::kLedBlue);
}

void test_175a_physical_oled_uses_0x3c_on_the_approved_i2c_pins() {
  TEST_ASSERT_TRUE(algaguard::physical_test_oled_address_is_expected());
  TEST_ASSERT_EQUAL_HEX8(0x3C, algaguard::hardware::kOledAddress);
  TEST_ASSERT_NOT_EQUAL(0x70, algaguard::hardware::kOledAddress);
  TEST_ASSERT_EQUAL(8, algaguard::hardware::kSda);
  TEST_ASSERT_EQUAL(9, algaguard::hardware::kScl);
}

void test_176_board_preflight_fails_closed_for_required_capability_mismatch() {
  const algaguard::PhysicalBoardPreflightInput valid{true, 16U * 1024U * 1024U, true,
                                                      8U * 1024U * 1024U, true, true, true};
  TEST_ASSERT_TRUE(algaguard::physical_board_preflight(valid).safeForReadiness);
  auto wrongChip = valid;
  wrongChip.chipIsEsp32S3 = false;
  TEST_ASSERT_FALSE(algaguard::physical_board_preflight(wrongChip).safeForReadiness);
  auto wrongFlash = valid;
  wrongFlash.flashBytes = 8U * 1024U * 1024U;
  TEST_ASSERT_FALSE(algaguard::physical_board_preflight(wrongFlash).safeForReadiness);
  auto missingPsram = valid;
  missingPsram.psramPresent = false;
  TEST_ASSERT_FALSE(algaguard::physical_board_preflight(missingPsram).safeForReadiness);
  auto badPartition = valid;
  badPartition.partitionLayoutValid = false;
  TEST_ASSERT_FALSE(algaguard::physical_board_preflight(badPartition).safeForReadiness);
  auto wrongProfile = valid;
  wrongProfile.physicalProfileActive = false;
  TEST_ASSERT_FALSE(algaguard::physical_board_preflight(wrongProfile).safeForReadiness);
  auto oledUnavailable = valid;
  oledUnavailable.oledInitialized = false;
  const auto oledFailure = algaguard::physical_board_preflight(oledUnavailable);
  TEST_ASSERT_TRUE(oledFailure.safeForReadiness);
  TEST_ASSERT_FALSE(oledFailure.displayValidated);
  TEST_ASSERT_EQUAL(algaguard::PhysicalBoardPreflightReason::kDisplayUnavailable,
                    oledFailure.reason);
}

void test_177_readiness_starts_ble_only_and_serial_diagnostics_are_safe() {
  const auto plan = algaguard::physical_readiness_plan();
  TEST_ASSERT_TRUE(plan.startBle);
  TEST_ASSERT_FALSE(plan.startWifi);
  TEST_ASSERT_FALSE(plan.startBootstrap);
  TEST_ASSERT_FALSE(plan.startMqtt);
  TEST_ASSERT_FALSE(plan.startOta);
  const auto preflight = algaguard::physical_board_preflight(
      {true, 16U * 1024U * 1024U, true, 8U * 1024U * 1024U, true, true, true});
  const auto diagnostic =
      algaguard::physical_safe_serial_diagnostic(preflight, algaguard::PhysicalTestState::kBleReady, 1);
  TEST_ASSERT_TRUE(algaguard::physical_text_is_safe(diagnostic));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_172_physical_test_profile_is_rejected_for_production_or_release);
  RUN_TEST(test_173_volatile_session_clears_all_secret_storage);
  RUN_TEST(test_174_oled_states_are_safe_and_retain_physical_warning);
  RUN_TEST(test_175_led_mapping_uses_only_external_led_pins);
  RUN_TEST(test_175a_physical_oled_uses_0x3c_on_the_approved_i2c_pins);
  RUN_TEST(test_176_board_preflight_fails_closed_for_required_capability_mismatch);
  RUN_TEST(test_177_readiness_starts_ble_only_and_serial_diagnostics_are_safe);
  return UNITY_END();
}
