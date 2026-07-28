#define ALGAGUARD_DEVELOPMENT_BUILD 1
#define ALGAGUARD_DEVELOPMENT_PHYSICAL_TEST_OPT_IN 1
#define ALGAGUARD_PHYSICAL_TEST_MODE 1
#define ALGAGUARD_WIFI_CREDENTIAL_PERSISTENCE_DISABLED 1

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
  bool installDevelopmentSession(std::string_view sessionId, std::string_view deviceId,
                                 std::string_view token, std::uint64_t expiry) {
    ++installCalls;
    installed = sessionId == kSessionId && deviceId == kDeviceId && token.size() >= 32 && expiry > 0;
    return installed;
  }
  algaguard::BleWifiCredentialHandoff takeAcceptedWifiCredentials() { return std::move(handoff); }
  unsigned installCalls{};
  bool installed{};
  algaguard::BleWifiCredentialHandoff handoff;
};

std::uint32_t crc32(const std::uint8_t* bytes, std::size_t length) {
  std::uint32_t value = 0xffffffffU;
  for (std::size_t index = 0; index < length; ++index) {
    value ^= bytes[index];
    for (unsigned bit = 0; bit < 8; ++bit) value = (value >> 1U) ^ (0xedb88320U & (0U - (value & 1U)));
  }
  return ~value;
}

std::array<std::uint8_t, algaguard::kPhysicalSessionMaxFrameBytes> frame(
    std::size_t& length, std::string_view device = kDeviceId, std::uint64_t expiry = 200,
    std::string_view token = kToken) {
  std::array<std::uint8_t, algaguard::kPhysicalSessionMaxFrameBytes> bytes{};
  std::size_t payload = 0;
  bytes[payload++] = static_cast<std::uint8_t>(std::strlen(kSessionId));
  bytes[payload++] = static_cast<std::uint8_t>(device.size());
  bytes[payload++] = static_cast<std::uint8_t>(token.size() >> 8U);
  bytes[payload++] = static_cast<std::uint8_t>(token.size());
  for (int index = 7; index >= 0; --index) bytes[payload++] = static_cast<std::uint8_t>(expiry >> (index * 8));
  std::memcpy(bytes.data() + payload, kSessionId, std::strlen(kSessionId)); payload += std::strlen(kSessionId);
  std::memcpy(bytes.data() + payload, device.data(), device.size()); payload += device.size();
  std::memcpy(bytes.data() + payload, token.data(), token.size()); payload += token.size();
  std::array<std::uint8_t, algaguard::kPhysicalSessionMaxFrameBytes> result{};
  std::memcpy(result.data(), algaguard::kPhysicalSessionMagic.data(), 4);
  result[4] = algaguard::kPhysicalSessionProtocolVersion;
  result[5] = static_cast<std::uint8_t>(algaguard::PhysicalSessionControlCommand::kInstallVolatileSession);
  result[6] = static_cast<std::uint8_t>(payload >> 8U); result[7] = static_cast<std::uint8_t>(payload);
  std::memcpy(result.data() + 8, bytes.data(), payload);
  const auto checksum = crc32(result.data(), 8 + payload);
  result[8 + payload] = static_cast<std::uint8_t>(checksum >> 24U);
  result[9 + payload] = static_cast<std::uint8_t>(checksum >> 16U);
  result[10 + payload] = static_cast<std::uint8_t>(checksum >> 8U);
  result[11 + payload] = static_cast<std::uint8_t>(checksum);
  length = 12 + payload;
  return result;
}

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

void test_208_physical_installer_is_development_only() {
  TEST_ASSERT_TRUE(algaguard::physical_test_profile_allowed(false, false, true, true));
  TEST_ASSERT_FALSE(algaguard::physical_test_profile_allowed(true, false, true, true));
  TEST_ASSERT_FALSE(algaguard::physical_test_profile_allowed(false, true, true, true));
}

void test_209_valid_frame_arms_one_session_and_clears_receive_buffer() {
  FakeTransport transport; algaguard::PhysicalTestSessionInstaller installer; algaguard::PhysicalSessionControlProtocol protocol;
  std::size_t length{}; auto bytes = frame(length);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(algaguard::PhysicalSessionControlAck::kArmed), static_cast<int>(protocol.ingest(transport, installer, bytes.data(), length, 10)));
  TEST_ASSERT_TRUE(installer.armed()); TEST_ASSERT_TRUE(installer.secretsCleared()); TEST_ASSERT_TRUE(protocol.secretsCleared());
}

void test_210_bad_duplicate_expired_wrong_device_and_late_frames_are_rejected() {
  FakeTransport transport; algaguard::PhysicalTestSessionInstaller installer; algaguard::PhysicalSessionControlProtocol protocol;
  std::size_t length{}; auto valid = frame(length); TEST_ASSERT_EQUAL_INT(0, static_cast<int>(protocol.ingest(transport, installer, valid.data(), length, 10)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(algaguard::PhysicalSessionControlAck::kRejected), static_cast<int>(protocol.ingest(transport, installer, valid.data(), length, 11)));
  TEST_ASSERT_TRUE(installer.armed());
  algaguard::PhysicalTestSessionInstaller wrong; algaguard::PhysicalSessionControlProtocol wrongProtocol; auto badDevice = frame(length, "AG-000002");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(algaguard::PhysicalSessionControlAck::kRejected), static_cast<int>(wrongProtocol.ingest(transport, wrong, badDevice.data(), length, 10)));
  algaguard::PhysicalTestSessionInstaller expired; algaguard::PhysicalSessionControlProtocol expiredProtocol; auto old = frame(length, kDeviceId, 10);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(algaguard::PhysicalSessionControlAck::kRejected), static_cast<int>(expiredProtocol.ingest(transport, expired, old.data(), length, 10)));
  installer.markProcessingStarted();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(algaguard::PhysicalSessionControlAck::kDisabled), static_cast<int>(protocol.ingest(transport, installer, valid.data(), length, 12)));
}

void test_211_safe_states_never_expose_session_or_wifi_values() {
  for (const auto state : {algaguard::PhysicalSessionInstallerState::kReady, algaguard::PhysicalSessionInstallerState::kArmed, algaguard::PhysicalSessionInstallerState::kRejected})
    TEST_ASSERT_TRUE(algaguard::physical_text_is_safe(algaguard::physical_session_state_code(state)));
  TEST_ASSERT_TRUE(algaguard::physical_screen_is_safe(algaguard::physical_test_screen(algaguard::PhysicalTestState::kBleAdvertisingActive)));
}

void test_212_wifi_runtime_is_idle_without_any_connection_start() {
  FakeAdapter adapter; algaguard::WifiConnectionRuntime runtime{adapter};
  runtime.poll(10); TEST_ASSERT_EQUAL(0, adapter.beginCalls); TEST_ASSERT_EQUAL(algaguard::WifiConnectionState::kIdle, runtime.state());
}

void test_213_accepted_handoff_moves_once_without_session_token_or_connection() {
  FakeAdapter adapter; algaguard::WifiConnectionRuntime runtime{adapter}; FakeTransport transport; transport.handoff = accepted_handoff();
  algaguard::PhysicalTestSessionInstaller installer; algaguard::PhysicalProvisioningRuntimeBridge bridge;
  TEST_ASSERT_TRUE(bridge.onAccepted(transport, runtime, installer)); TEST_ASSERT_TRUE(bridge.handoffInstalled());
  TEST_ASSERT_TRUE(runtime.credentialsPresent()); TEST_ASSERT_EQUAL(0, adapter.beginCalls); TEST_ASSERT_FALSE(bridge.connectExecutionEnabled());
}

void test_214_rejected_replay_or_second_handoff_cannot_start_or_overwrite_runtime() {
  FakeAdapter adapter; algaguard::WifiConnectionRuntime runtime{adapter}; FakeTransport transport; transport.handoff = accepted_handoff();
  algaguard::PhysicalTestSessionInstaller installer; algaguard::PhysicalProvisioningRuntimeBridge bridge;
  TEST_ASSERT_TRUE(bridge.onAccepted(transport, runtime, installer)); transport.handoff = accepted_handoff();
  TEST_ASSERT_FALSE(bridge.onAccepted(transport, runtime, installer)); TEST_ASSERT_EQUAL(0, adapter.beginCalls); TEST_ASSERT_TRUE(runtime.credentialsPresent());
}

void test_215_terminal_cleanup_zeroizes_and_is_idempotent() {
  FakeTransport transport; algaguard::PhysicalTestSessionInstaller installer; algaguard::PhysicalSessionControlProtocol protocol; std::size_t length{}; auto bytes = frame(length);
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(protocol.ingest(transport, installer, bytes.data(), length, 10)));
  FakeAdapter adapter; algaguard::WifiConnectionRuntime runtime{adapter}; algaguard::PhysicalProvisioningRuntimeBridge bridge;
  bridge.clear(runtime, installer); bridge.clear(runtime, installer); protocol.clear();
  TEST_ASSERT_TRUE(installer.secretsCleared()); TEST_ASSERT_TRUE(protocol.secretsCleared()); TEST_ASSERT_TRUE(runtime.secretsCleared());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_208_physical_installer_is_development_only);
  RUN_TEST(test_209_valid_frame_arms_one_session_and_clears_receive_buffer);
  RUN_TEST(test_210_bad_duplicate_expired_wrong_device_and_late_frames_are_rejected);
  RUN_TEST(test_211_safe_states_never_expose_session_or_wifi_values);
  RUN_TEST(test_212_wifi_runtime_is_idle_without_any_connection_start);
  RUN_TEST(test_213_accepted_handoff_moves_once_without_session_token_or_connection);
  RUN_TEST(test_214_rejected_replay_or_second_handoff_cannot_start_or_overwrite_runtime);
  RUN_TEST(test_215_terminal_cleanup_zeroizes_and_is_idempotent);
  return UNITY_END();
}
