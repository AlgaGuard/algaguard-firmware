#include <cstring>
#include <string>
#include <utility>

#include <unity.h>

#include "algaguard/ble_wifi_provisioning.hpp"

using algaguard::BleWifiProvisioningReason;
using algaguard::BleWifiProvisioningRequest;
using algaguard::BleWifiProvisioningRequestBuilder;
using algaguard::BleWifiProvisioningState;
using algaguard::BleWifiProvisioningStateMachine;

namespace {
constexpr const char* kSessionId = "session-0001";
constexpr const char* kDeviceId = "AG-000001";
constexpr const char* kSessionSecret = "synthetic-session-secret";
constexpr const char* kPasswordSecret = "synthetic-wifi-password";
constexpr std::uint64_t kExpiry = 200;

BleWifiProvisioningRequest request(std::string_view session_id = kSessionId,
                                   std::string_view device_id = kDeviceId,
                                   std::string_view token = kSessionSecret,
                                   std::string_view ssid = "AlgaGuardLab",
                                   std::string_view password = kPasswordSecret) {
  BleWifiProvisioningRequestBuilder builder;
  builder.sessionId(session_id)
      .deviceId(device_id)
      .sessionToken(token)
      .ssid(ssid)
      .password(password);
  return builder.build();
}

void receiving(BleWifiProvisioningStateMachine& machine) {
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(BleWifiProvisioningReason::OK),
      static_cast<int>(machine.prepareSession(kSessionId, kDeviceId, kSessionSecret, kExpiry)
                           .safeReasonCode));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleWifiProvisioningState::BLE_CONNECTED),
                        static_cast<int>(machine.connectBle().finalState));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleWifiProvisioningState::PAYLOAD_RECEIVING),
                        static_cast<int>(machine.beginPayload().finalState));
}

void assert_no_secrets(const algaguard::BleWifiProvisioningResult& result) {
  const auto diagnostics = result.diagnostics();
  TEST_ASSERT_NULL(std::strstr(diagnostics.c_str(), kSessionSecret));
  TEST_ASSERT_NULL(std::strstr(diagnostics.c_str(), kPasswordSecret));
  TEST_ASSERT_TRUE(result.secretsCleared);
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_120_valid_request_reaches_accepted() {
  BleWifiProvisioningStateMachine machine;
  receiving(machine);
  auto payload = request();
  const auto validated = machine.validatePayload(std::move(payload), 100);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleWifiProvisioningState::PAYLOAD_VALIDATED),
                        static_cast<int>(validated.finalState));
  const auto accepted = machine.acceptValidated();
  TEST_ASSERT_TRUE(accepted.accepted);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleWifiProvisioningState::ACCEPTED),
                        static_cast<int>(accepted.finalState));
  assert_no_secrets(accepted);
}

void test_121_malformed_request_is_rejected() {
  BleWifiProvisioningStateMachine missing;
  receiving(missing);
  BleWifiProvisioningRequestBuilder incomplete;
  incomplete.sessionId(kSessionId).deviceId(kDeviceId).sessionToken(kSessionSecret).ssid("Lab");
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(BleWifiProvisioningReason::MALFORMED_PAYLOAD),
      static_cast<int>(missing.validatePayload(incomplete.build(), 100).safeReasonCode));

  BleWifiProvisioningStateMachine embedded_nul;
  receiving(embedded_nul);
  const std::string invalid_ssid{"Lab\0Hidden", 10};
  auto malformed = request(kSessionId, kDeviceId, kSessionSecret, invalid_ssid);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(BleWifiProvisioningReason::MALFORMED_PAYLOAD),
      static_cast<int>(embedded_nul.validatePayload(std::move(malformed), 100).safeReasonCode));

  BleWifiProvisioningStateMachine duplicate;
  receiving(duplicate);
  BleWifiProvisioningRequestBuilder ambiguous;
  ambiguous.sessionId(kSessionId)
      .sessionId("session-0002")
      .deviceId(kDeviceId)
      .sessionToken(kSessionSecret)
      .ssid("Lab")
      .password(kPasswordSecret);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(BleWifiProvisioningReason::MALFORMED_PAYLOAD),
      static_cast<int>(duplicate.validatePayload(ambiguous.build(), 100).safeReasonCode));

  BleWifiProvisioningStateMachine unsupported;
  receiving(unsupported);
  BleWifiProvisioningRequestBuilder wrong_version;
  wrong_version.protocolVersion(3)
      .sessionId(kSessionId)
      .deviceId(kDeviceId)
      .sessionToken(kSessionSecret)
      .ssid("Lab")
      .password(kPasswordSecret);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(BleWifiProvisioningReason::UNSUPPORTED_VERSION),
      static_cast<int>(unsupported.validatePayload(wrong_version.build(), 100).safeReasonCode));

  BleWifiProvisioningStateMachine malformed_id;
  receiving(malformed_id);
  auto invalid_id = request(kSessionId, "invalid");
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(BleWifiProvisioningReason::MALFORMED_PAYLOAD),
      static_cast<int>(malformed_id.validatePayload(std::move(invalid_id), 100).safeReasonCode));
  TEST_ASSERT_TRUE(invalid_id.secretsCleared());
}

void test_122_oversized_field_is_rejected() {
  BleWifiProvisioningStateMachine machine;
  receiving(machine);
  const std::string oversized(BleWifiProvisioningRequest::kMaxSsidBytes + 1, 'x');
  auto payload = request(kSessionId, kDeviceId, kSessionSecret, oversized);
  const auto result = machine.validatePayload(std::move(payload), 100);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleWifiProvisioningReason::FIELD_TOO_LONG),
                        static_cast<int>(result.safeReasonCode));
  assert_no_secrets(result);
}

void test_123_device_id_mismatch_is_rejected() {
  BleWifiProvisioningStateMachine machine;
  receiving(machine);
  auto payload = request(kSessionId, "AG-000002");
  const auto result = machine.validatePayload(std::move(payload), 100);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleWifiProvisioningReason::DEVICE_ID_MISMATCH),
                        static_cast<int>(result.safeReasonCode));
  assert_no_secrets(result);
}

void test_124_session_mismatch_is_rejected() {
  BleWifiProvisioningStateMachine wrong_id;
  receiving(wrong_id);
  auto mismatched_id = request("session-0002");
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(BleWifiProvisioningReason::SESSION_MISMATCH),
      static_cast<int>(wrong_id.validatePayload(std::move(mismatched_id), 100).safeReasonCode));

  BleWifiProvisioningStateMachine wrong_token;
  receiving(wrong_token);
  auto mismatched_token = request(kSessionId, kDeviceId, "different-session-secret");
  const auto result = wrong_token.validatePayload(std::move(mismatched_token), 100);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleWifiProvisioningReason::SESSION_MISMATCH),
                        static_cast<int>(result.safeReasonCode));
  assert_no_secrets(result);
}

void test_125_expired_session_is_rejected() {
  BleWifiProvisioningStateMachine machine;
  receiving(machine);
  auto payload = request();
  const auto result = machine.validatePayload(std::move(payload), kExpiry);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleWifiProvisioningState::EXPIRED),
                        static_cast<int>(result.finalState));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleWifiProvisioningReason::SESSION_EXPIRED),
                        static_cast<int>(result.safeReasonCode));
  TEST_ASSERT_TRUE(payload.secretsCleared());
  assert_no_secrets(result);
}

void test_126_replay_after_acceptance_is_rejected() {
  BleWifiProvisioningStateMachine machine;
  receiving(machine);
  auto first = request();
  machine.validatePayload(std::move(first), 100);
  TEST_ASSERT_TRUE(machine.acceptValidated().accepted);
  auto replay = request();
  const auto result = machine.validatePayload(std::move(replay), 101);
  TEST_ASSERT_FALSE(result.accepted);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleWifiProvisioningReason::REPLAY_REJECTED),
                        static_cast<int>(result.safeReasonCode));
  assert_no_secrets(result);
}

void test_127_invalid_transition_fails_closed() {
  BleWifiProvisioningStateMachine machine;
  const auto result = machine.connectBle();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleWifiProvisioningState::REJECTED),
                        static_cast<int>(result.finalState));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleWifiProvisioningReason::INVALID_TRANSITION),
                        static_cast<int>(result.safeReasonCode));
  TEST_ASSERT_FALSE(result.retryAllowed);
  assert_no_secrets(result);
}

void test_128_cancellation_clears_password_and_session_token() {
  BleWifiProvisioningStateMachine machine;
  receiving(machine);
  auto payload = request();
  machine.validatePayload(std::move(payload), 100);
  TEST_ASSERT_FALSE(machine.secretsCleared());
  const auto result = machine.cancel();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleWifiProvisioningState::CANCELLED),
                        static_cast<int>(result.finalState));
  TEST_ASSERT_TRUE(machine.secretsCleared());
  assert_no_secrets(result);
}

void test_129_terminal_teardown_clears_secrets_and_diagnostics_are_safe() {
  BleWifiProvisioningStateMachine accepted;
  receiving(accepted);
  auto accepted_payload = request();
  accepted.validatePayload(std::move(accepted_payload), 100);
  assert_no_secrets(accepted.acceptValidated());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleWifiProvisioningState::CLEARED),
                        static_cast<int>(accepted.clear().finalState));

  BleWifiProvisioningStateMachine rejected;
  receiving(rejected);
  auto rejected_payload = request(kSessionId, "AG-000002");
  assert_no_secrets(rejected.validatePayload(std::move(rejected_payload), 100));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleWifiProvisioningState::CLEARED),
                        static_cast<int>(rejected.clear().finalState));

  BleWifiProvisioningStateMachine expired;
  receiving(expired);
  auto expired_payload = request();
  assert_no_secrets(expired.validatePayload(std::move(expired_payload), kExpiry));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleWifiProvisioningState::CLEARED),
                        static_cast<int>(expired.clear().finalState));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_120_valid_request_reaches_accepted);
  RUN_TEST(test_121_malformed_request_is_rejected);
  RUN_TEST(test_122_oversized_field_is_rejected);
  RUN_TEST(test_123_device_id_mismatch_is_rejected);
  RUN_TEST(test_124_session_mismatch_is_rejected);
  RUN_TEST(test_125_expired_session_is_rejected);
  RUN_TEST(test_126_replay_after_acceptance_is_rejected);
  RUN_TEST(test_127_invalid_transition_fails_closed);
  RUN_TEST(test_128_cancellation_clears_password_and_session_token);
  RUN_TEST(test_129_terminal_teardown_clears_secrets_and_diagnostics_are_safe);
  return UNITY_END();
}
