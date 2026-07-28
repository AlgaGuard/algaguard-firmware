#include <cstddef>
#include <cstdint>
#include <string>

#include <unity.h>

#include "algaguard/ble_provisioning_gatt_controller.hpp"
#include "algaguard/wifi_connection_state_machine.hpp"

namespace {
constexpr const char* kSessionId = "50000000-0000-4000-8000-000000000001";
constexpr const char* kDeviceId = "AG-000001";
constexpr const char* kSessionToken = "sE2vR8yN5kM1pQ7xT4bW9dF6aC3uH0zL";
constexpr const char* kSsid = "AlgaGuard-Lab";
constexpr const char* kPassword = "synthetic-wifi-password";

class FakeWifiConnectionAdapter final : public algaguard::WifiConnectionAdapter {
 public:
  bool beginConnect(std::string_view ssid, std::string_view password) override {
    ++beginCalls;
    ssidLength = ssid.size();
    passwordPresent = !password.empty();
    inProgress = beginSucceeds;
    return beginSucceeds;
  }

  void cancelConnect() override {
    ++cancelCalls;
    inProgress = false;
  }

  void disconnect() override {
    ++disconnectCalls;
    inProgress = false;
  }

  bool isConnectInProgress() const override { return inProgress; }

  void clearSensitiveDriverInput() override { ++clearSensitiveCalls; }

  bool beginSucceeds{true};
  bool inProgress{};
  std::uint32_t beginCalls{};
  std::uint32_t cancelCalls{};
  std::uint32_t disconnectCalls{};
  std::uint32_t clearSensitiveCalls{};
  std::size_t ssidLength{};
  bool passwordPresent{};
};

std::string payload() {
  return std::string{"{\"schema\":\"urn:algaguard:schema:onboarding:ble-provisioning-request:v1\","
                     "\"schemaVersion\":\"1.0.0\",\"sessionId\":\""} +
         kSessionId + "\",\"deviceId\":\"" + kDeviceId + "\",\"sessionToken\":\"" +
         kSessionToken + "\",\"ssid\":\"" + kSsid + "\",\"password\":\"" + kPassword + "\"}";
}

algaguard::BleWifiCredentialHandoff acceptedHandoff() {
  algaguard::BleProvisioningGattController controller;
  TEST_ASSERT_TRUE(controller.installDevelopmentSession(kSessionId, kDeviceId, kSessionToken, 200));
  TEST_ASSERT_TRUE(controller.onConnected(7).accepted);
  const auto body = payload();
  const auto split = body.size() > 180 ? 180U : body.size();
  const auto fragmentCount = split == body.size() ? 1U : 2U;
  for (std::uint16_t index = 0; index < fragmentCount; ++index) {
    const auto offset = index == 0 ? 0U : split;
    const auto size = index == 0 ? split : body.size() - split;
    algaguard::BleProvisioningFrame frame;
    frame.messageId = 901;
    frame.fragmentIndex = index;
    frame.fragmentCount = fragmentCount;
    TEST_ASSERT_TRUE(frame.setPayload(reinterpret_cast<const std::uint8_t*>(body.data() + offset), size));
    algaguard::BleProvisioningEncodedFrame encoded;
    TEST_ASSERT_TRUE(algaguard::encode_ble_provisioning_frame(frame, encoded));
    TEST_ASSERT_TRUE(controller.onRequestWrite(encoded.bytes.data(), encoded.size, 10 + index).accepted);
  }
  return controller.takeAcceptedWifiCredentials();
}

void acceptAndStart(algaguard::WifiConnectionStateMachine& manager, std::uint64_t tick = 10) {
  auto handoff = acceptedHandoff();
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionReason::kOk,
                    manager.acceptCredentials(std::move(handoff)).safeReasonCode);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionReason::kConnectInProgress,
                    manager.start(tick).safeReasonCode);
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_158_handoff_moves_once_and_starts_one_connection_attempt() {
  FakeWifiConnectionAdapter adapter;
  algaguard::WifiConnectionStateMachine manager{adapter};
  auto handoff = acceptedHandoff();
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionState::kCredentialsReady,
                    manager.acceptCredentials(std::move(handoff)).state);
  TEST_ASSERT_FALSE(handoff.available());
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionReason::kConnectInProgress, manager.start(10).safeReasonCode);
  TEST_ASSERT_EQUAL_UINT32(1, adapter.beginCalls);
  TEST_ASSERT_EQUAL_UINT32(std::string{kSsid}.size(), adapter.ssidLength);
  TEST_ASSERT_TRUE(adapter.passwordPresent);
}

void test_159_empty_consumed_or_duplicate_handoff_cannot_overwrite_active_credentials() {
  FakeWifiConnectionAdapter adapter;
  algaguard::WifiConnectionStateMachine manager{adapter};
  algaguard::BleWifiCredentialHandoff empty;
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionReason::kInvalidCredentials,
                    manager.acceptCredentials(std::move(empty)).safeReasonCode);
  auto first = acceptedHandoff();
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionReason::kOk,
                    manager.acceptCredentials(std::move(first)).safeReasonCode);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionReason::kHandoffAlreadyPresent,
                    manager.acceptCredentials(std::move(first)).safeReasonCode);
  auto second = acceptedHandoff();
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionReason::kHandoffAlreadyPresent,
                    manager.acceptCredentials(std::move(second)).safeReasonCode);
  TEST_ASSERT_TRUE(manager.credentialsPresent());
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionState::kCredentialsReady, manager.state());
}

void test_160_connected_event_clears_manager_credentials() {
  FakeWifiConnectionAdapter adapter;
  algaguard::WifiConnectionStateMachine manager{adapter};
  acceptAndStart(manager);
  const auto result = manager.onDriverEvent(algaguard::WifiDriverEvent::kConnected, 11);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionState::kConnected, result.state);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionReason::kOk, result.safeReasonCode);
  TEST_ASSERT_FALSE(result.credentialsPresent);
  TEST_ASSERT_TRUE(result.secretsCleared);
}

void test_161_authentication_failure_is_terminal_without_retry_and_clears_credentials() {
  FakeWifiConnectionAdapter adapter;
  algaguard::WifiConnectionStateMachine manager{adapter};
  acceptAndStart(manager);
  const auto result = manager.onDriverEvent(algaguard::WifiDriverEvent::kAuthenticationFailed, 11);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionState::kAuthFailed, result.state);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionReason::kAuthenticationFailed, result.safeReasonCode);
  TEST_ASSERT_FALSE(result.retryAllowed);
  TEST_ASSERT_FALSE(result.credentialsPresent);
  TEST_ASSERT_TRUE(result.secretsCleared);
  TEST_ASSERT_EQUAL_UINT32(1, adapter.beginCalls);
}

void test_162_network_and_transient_failure_use_bounded_retry_delay() {
  FakeWifiConnectionAdapter adapter;
  algaguard::WifiConnectionStateMachine manager{adapter};
  acceptAndStart(manager, 10);
  auto result = manager.onDriverEvent(algaguard::WifiDriverEvent::kNetworkNotFound, 11);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionState::kRetryWait, result.state);
  TEST_ASSERT_TRUE(result.retryAllowed);
  TEST_ASSERT_EQUAL_UINT64(11 + algaguard::kWifiRetryDelayTicks, result.nextRetryTick);
  manager.onTick(result.nextRetryTick);
  result = manager.onDriverEvent(algaguard::WifiDriverEvent::kTransientFailure, 17);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionState::kRetryWait, result.state);
  manager.onTick(result.nextRetryTick);
  result = manager.onDriverEvent(algaguard::WifiDriverEvent::kNetworkNotFound, 23);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionState::kNetworkNotFound, result.state);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionReason::kRetryExhausted, result.safeReasonCode);
  TEST_ASSERT_FALSE(result.credentialsPresent);
  TEST_ASSERT_EQUAL_UINT32(3, adapter.beginCalls);
}

void test_163_timeout_uses_injected_ticks_and_clears_after_retry_exhaustion() {
  FakeWifiConnectionAdapter adapter;
  algaguard::WifiConnectionStateMachine manager{adapter};
  acceptAndStart(manager, 100);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionState::kConnecting, manager.onTick(119).state);
  auto result = manager.onTick(120);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionState::kRetryWait, result.state);
  manager.onTick(125);
  result = manager.onTick(145);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionState::kRetryWait, result.state);
  manager.onTick(150);
  result = manager.onTick(170);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionState::kTimedOut, result.state);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionReason::kConnectTimeout, result.safeReasonCode);
  TEST_ASSERT_FALSE(result.credentialsPresent);
  TEST_ASSERT_TRUE(result.secretsCleared);
  TEST_ASSERT_EQUAL_UINT32(3, adapter.beginCalls);
}

void test_164_cancellation_disconnect_and_repeated_cleanup_are_safe() {
  FakeWifiConnectionAdapter adapter;
  algaguard::WifiConnectionStateMachine manager{adapter};
  acceptAndStart(manager);
  auto result = manager.cancel();
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionState::kCancelled, result.state);
  TEST_ASSERT_TRUE(result.secretsCleared);
  result = manager.reset();
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionState::kCleared, result.state);
  result = manager.reset();
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionState::kCleared, result.state);
  auto next = acceptedHandoff();
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionReason::kOk,
                    manager.acceptCredentials(std::move(next)).safeReasonCode);
  manager.start(20);
  result = manager.onDriverEvent(algaguard::WifiDriverEvent::kDisconnected, 21);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionState::kRetryWait, result.state);
  manager.reset();
  TEST_ASSERT_FALSE(manager.credentialsPresent());
  TEST_ASSERT_TRUE(manager.secretsCleared());
}

void test_165_safe_results_and_host_fake_retain_no_credential_text() {
  FakeWifiConnectionAdapter adapter;
  {
    algaguard::WifiConnectionStateMachine manager{adapter};
    acceptAndStart(manager);
    const auto result = manager.onDriverEvent(algaguard::WifiDriverEvent::kConnected, 11);
    TEST_ASSERT_FALSE(result.credentialsPresent);
    TEST_ASSERT_TRUE(result.secretsCleared);
    TEST_ASSERT_EQUAL_UINT32(std::string{kSsid}.size(), adapter.ssidLength);
    TEST_ASSERT_TRUE(adapter.passwordPresent);
  }
  TEST_ASSERT_TRUE(adapter.clearSensitiveCalls >= 2);
  TEST_ASSERT_FALSE(adapter.inProgress);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_158_handoff_moves_once_and_starts_one_connection_attempt);
  RUN_TEST(test_159_empty_consumed_or_duplicate_handoff_cannot_overwrite_active_credentials);
  RUN_TEST(test_160_connected_event_clears_manager_credentials);
  RUN_TEST(test_161_authentication_failure_is_terminal_without_retry_and_clears_credentials);
  RUN_TEST(test_162_network_and_transient_failure_use_bounded_retry_delay);
  RUN_TEST(test_163_timeout_uses_injected_ticks_and_clears_after_retry_exhaustion);
  RUN_TEST(test_164_cancellation_disconnect_and_repeated_cleanup_are_safe);
  RUN_TEST(test_165_safe_results_and_host_fake_retain_no_credential_text);
  return UNITY_END();
}
