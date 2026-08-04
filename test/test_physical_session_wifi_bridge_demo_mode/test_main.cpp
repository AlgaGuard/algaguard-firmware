// Companion to test_physical_session_wifi_bridge: that suite compiles
// WITHOUT ALGAGUARD_LOCAL_DEMO_MODE and locks in the strict harness-only
// behavior (accepting a handoff must never start a connection without an
// explicit UART arm command). This suite compiles WITH it and locks in the
// opposite: a demo build must be usable standalone from just the phone app,
// with no UART operator required.
#define ALGAGUARD_DEVELOPMENT_BUILD 1
#define ALGAGUARD_DEVELOPMENT_PHYSICAL_TEST_OPT_IN 1
#define ALGAGUARD_PHYSICAL_TEST_MODE 1
#define ALGAGUARD_WIFI_CREDENTIAL_PERSISTENCE_DISABLED 1
#define ALGAGUARD_LOCAL_DEMO_MODE 1

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include <unity.h>

#include "algaguard/ble_provisioning_gatt_controller.hpp"
#include "algaguard/physical_test_harness.hpp"
#include "algaguard/physical_provisioning_runtime_bridge.hpp"

namespace {
constexpr const char* kSessionId = "50000000-0000-4000-8000-000000000001";
constexpr const char* kDeviceId = "AG-000001";
constexpr const char* kToken = "sE2vR8yN5kM1pQ7xT4bW9dF6aC3uH0zL";

class FakeAdapter final : public algaguard::WifiConnectionAdapter {
 public:
  bool beginConnect(std::string_view, std::string_view) override { ++beginCalls; return true; }
  void cancelConnect() override { ++cancelCalls; }
  void disconnect() override { ++disconnectCalls; }
  bool isConnectInProgress() const override { return false; }
  void clearSensitiveDriverInput() override { ++clearCalls; }
  unsigned beginCalls{};
  unsigned cancelCalls{};
  unsigned disconnectCalls{};
  unsigned clearCalls{};
};

class FakeTransport {
 public:
  bool installDevelopmentSession(std::string_view, std::string_view, std::string_view, std::uint64_t) {
    return true;
  }
  algaguard::BleWifiCredentialHandoff takeAcceptedWifiCredentials() { return std::move(handoff); }
  algaguard::BleWifiCredentialHandoff handoff;
};

algaguard::BleWifiCredentialHandoff accepted_handoff() {
  algaguard::BleProvisioningGattController controller;
  TEST_ASSERT_TRUE(controller.installDevelopmentSession(kSessionId, kDeviceId, kToken, 200));
  TEST_ASSERT_TRUE(controller.onConnected(1).accepted);
  const std::string body = std::string{"{\"schema\":\"urn:algaguard:schema:onboarding:ble-provisioning-request:v1\",\"schemaVersion\":\"1.0.0\",\"sessionId\":\""} +
      kSessionId + "\",\"deviceId\":\"" + kDeviceId + "\",\"sessionToken\":\"" + kToken +
      "\",\"ssid\":\"TestNet\",\"password\":\"synthetic-password\"}";
  const auto split = body.size() > 180 ? 180U : body.size();
  const auto count = split == body.size() ? 1U : 2U;
  for (std::uint16_t index = 0; index < count; ++index) {
    const auto offset = index == 0 ? 0U : split;
    const auto size = index == 0 ? split : body.size() - split;
    algaguard::BleProvisioningFrame request;
    request.messageId = 99; request.fragmentIndex = index; request.fragmentCount = count;
    TEST_ASSERT_TRUE(request.setPayload(reinterpret_cast<const std::uint8_t*>(body.data() + offset), size));
    algaguard::BleProvisioningEncodedFrame encoded;
    TEST_ASSERT_TRUE(algaguard::encode_ble_provisioning_frame(request, encoded));
    TEST_ASSERT_TRUE(controller.onRequestWrite(encoded.bytes.data(), encoded.size, 10 + index).accepted);
  }
  return controller.takeAcceptedWifiCredentials();
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_demo_mode_accepted_handoff_starts_connection_without_uart_arm() {
  FakeAdapter adapter;
  algaguard::WifiConnectionRuntime runtime{adapter};
  FakeTransport transport;
  transport.handoff = accepted_handoff();
  algaguard::PhysicalTestSessionInstaller installer;
  algaguard::PhysicalProvisioningRuntimeBridge bridge;

  // No kArmOneWifiConnectionTest UART command was ever sent -- this is the
  // entire point: a demo build has no harness operator driving it.
  TEST_ASSERT_TRUE(bridge.onAccepted(transport, runtime, installer));
  TEST_ASSERT_TRUE(bridge.handoffInstalled());
  TEST_ASSERT_TRUE(runtime.credentialsPresent());
  TEST_ASSERT_EQUAL(1, adapter.beginCalls);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_demo_mode_accepted_handoff_starts_connection_without_uart_arm);
  return UNITY_END();
}
