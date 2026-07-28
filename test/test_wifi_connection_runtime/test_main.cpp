#include <cstddef>
#include <cstdint>
#include <string>

#include <unity.h>

#include "algaguard/ble_provisioning_gatt_controller.hpp"
#include "algaguard/wifi_connection_runtime.hpp"

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
    inProgress = true;
    return true;
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
  const auto count = split == body.size() ? 1U : 2U;
  for (std::uint16_t index = 0; index < count; ++index) {
    const auto offset = index == 0 ? 0U : split;
    const auto size = index == 0 ? split : body.size() - split;
    algaguard::BleProvisioningFrame frame;
    frame.messageId = 902;
    frame.fragmentIndex = index;
    frame.fragmentCount = count;
    TEST_ASSERT_TRUE(frame.setPayload(reinterpret_cast<const std::uint8_t*>(body.data() + offset), size));
    algaguard::BleProvisioningEncodedFrame encoded;
    TEST_ASSERT_TRUE(algaguard::encode_ble_provisioning_frame(frame, encoded));
    TEST_ASSERT_TRUE(controller.onRequestWrite(encoded.bytes.data(), encoded.size, 10 + index).accepted);
  }
  return controller.takeAcceptedWifiCredentials();
}

void installAndStart(algaguard::WifiConnectionRuntime& runtime, std::uint64_t tick = 10) {
  auto handoff = acceptedHandoff();
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionReason::kOk,
                    runtime.installAcceptedCredentials(std::move(handoff)).safeReasonCode);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionReason::kConnectInProgress,
                    runtime.startConnection(tick).safeReasonCode);
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_166_runtime_passes_accepted_credentials_once_without_fake_text_retention() {
  FakeWifiConnectionAdapter adapter;
  algaguard::WifiConnectionRuntime runtime{adapter};
  installAndStart(runtime);
  TEST_ASSERT_EQUAL_UINT32(1, adapter.beginCalls);
  TEST_ASSERT_EQUAL_UINT32(std::string{kSsid}.size(), adapter.ssidLength);
  TEST_ASSERT_TRUE(adapter.passwordPresent);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionState::kConnecting, runtime.state());
}

void test_167_authentication_disconnect_is_terminal() {
  FakeWifiConnectionAdapter adapter;
  algaguard::WifiConnectionRuntime runtime{adapter};
  installAndStart(runtime);
  const auto result = runtime.onWifiEvent(
      algaguard::WifiRuntimeWifiEvent::kStationDisconnected,
      algaguard::WifiDisconnectClassification::kAuthenticationFailed, 11);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionState::kAuthFailed, result.state);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionReason::kAuthenticationFailed, result.safeReasonCode);
  TEST_ASSERT_FALSE(result.retryAllowed);
  TEST_ASSERT_FALSE(result.credentialsPresent);
}

void test_168_no_ap_disconnect_maps_to_network_not_found() {
  FakeWifiConnectionAdapter adapter;
  algaguard::WifiConnectionRuntime runtime{adapter};
  installAndStart(runtime);
  const auto result = runtime.onWifiEvent(
      algaguard::WifiRuntimeWifiEvent::kStationDisconnected,
      algaguard::WifiDisconnectClassification::kNetworkNotFound, 11);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionState::kRetryWait, result.state);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionReason::kNetworkNotFound, result.safeReasonCode);
  TEST_ASSERT_TRUE(result.retryAllowed);
}

void test_169_transient_and_unknown_disconnects_follow_bounded_host_retry_policy() {
  FakeWifiConnectionAdapter adapter;
  algaguard::WifiConnectionRuntime runtime{adapter};
  installAndStart(runtime);
  TEST_ASSERT_EQUAL(algaguard::WifiDriverEvent::kTransientFailure,
                    algaguard::wifi_driver_event_for_disconnect(
                        algaguard::WifiDisconnectClassification::kUnknown));
  auto result = runtime.onWifiEvent(
      algaguard::WifiRuntimeWifiEvent::kStationDisconnected,
      algaguard::WifiDisconnectClassification::kUnknown, 11);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionState::kRetryWait, result.state);
  runtime.poll(result.nextRetryTick);
  result = runtime.onWifiEvent(
      algaguard::WifiRuntimeWifiEvent::kStationDisconnected,
      algaguard::WifiDisconnectClassification::kTransientFailure, 17);
  TEST_ASSERT_TRUE(result.retryAllowed);
  runtime.poll(result.nextRetryTick);
  result = runtime.onWifiEvent(
      algaguard::WifiRuntimeWifiEvent::kStationDisconnected,
      algaguard::WifiDisconnectClassification::kTransientFailure, 23);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionReason::kRetryExhausted, result.safeReasonCode);
  TEST_ASSERT_FALSE(result.credentialsPresent);
}

void test_170_got_ip_is_connected_success_and_clears_host_credentials() {
  FakeWifiConnectionAdapter adapter;
  algaguard::WifiConnectionRuntime runtime{adapter};
  installAndStart(runtime);
  const auto result = runtime.onIpEvent(algaguard::WifiRuntimeIpEvent::kGotIp, 11);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionState::kConnected, result.state);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionReason::kOk, result.safeReasonCode);
  TEST_ASSERT_FALSE(result.credentialsPresent);
  TEST_ASSERT_TRUE(result.secretsCleared);
}

void test_171_cancel_reset_shutdown_are_idempotent_and_keep_diagnostics_safe() {
  FakeWifiConnectionAdapter adapter;
  algaguard::WifiConnectionRuntime runtime{adapter};
  installAndStart(runtime);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionState::kCancelled, runtime.cancel().state);
  TEST_ASSERT_EQUAL(algaguard::WifiConnectionState::kCleared, runtime.reset().state);
  runtime.reset();
  runtime.shutdown();
  runtime.shutdown();
  TEST_ASSERT_FALSE(runtime.credentialsPresent());
  TEST_ASSERT_TRUE(runtime.secretsCleared());
  TEST_ASSERT_FALSE(adapter.inProgress);
  TEST_ASSERT_TRUE(adapter.clearSensitiveCalls >= 3);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_166_runtime_passes_accepted_credentials_once_without_fake_text_retention);
  RUN_TEST(test_167_authentication_disconnect_is_terminal);
  RUN_TEST(test_168_no_ap_disconnect_maps_to_network_not_found);
  RUN_TEST(test_169_transient_and_unknown_disconnects_follow_bounded_host_retry_policy);
  RUN_TEST(test_170_got_ip_is_connected_success_and_clears_host_credentials);
  RUN_TEST(test_171_cancel_reset_shutdown_are_idempotent_and_keep_diagnostics_safe);
  return UNITY_END();
}
