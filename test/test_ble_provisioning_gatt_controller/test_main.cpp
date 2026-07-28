#include <array>
#include <cstring>

#include <unity.h>

#include "algaguard/ble_provisioning_gatt_controller.hpp"

namespace {

algaguard::BleProvisioningEncodedFrame encoded_frame(std::uint32_t messageId, std::uint16_t index,
                                                     std::uint16_t count, const char* payload) {
  algaguard::BleProvisioningFrame frame;
  frame.messageId = messageId;
  frame.fragmentIndex = index;
  frame.fragmentCount = count;
  TEST_ASSERT_TRUE(frame.setPayload(reinterpret_cast<const std::uint8_t*>(payload),
                                    std::strlen(payload)));
  algaguard::BleProvisioningEncodedFrame encoded;
  TEST_ASSERT_TRUE(algaguard::encode_ble_provisioning_frame(frame, encoded));
  return encoded;
}

bool contains(std::string_view value, const char* needle) {
  return value.find(needle) != std::string_view::npos;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_144_first_connection_is_accepted_second_connection_is_rejected() {
  algaguard::BleProvisioningGattController controller;
  TEST_ASSERT_TRUE(controller.onConnected(7).accepted);
  const auto rejected = controller.onConnected(8);
  TEST_ASSERT_FALSE(rejected.accepted);
  TEST_ASSERT_FALSE(rejected.ignored);
  TEST_ASSERT_TRUE(controller.hasActiveConnection());
  TEST_ASSERT_TRUE(contains(controller.readSafeStatus().view(), "REJECTED"));
}

void test_145_valid_framed_writes_produce_one_completed_payload_handoff() {
  algaguard::BleProvisioningGattController controller;
  TEST_ASSERT_TRUE(controller.onConnected(7).accepted);
  const auto first = encoded_frame(45, 0, 2, "safe-");
  const auto second = encoded_frame(45, 1, 2, "payload");
  TEST_ASSERT_TRUE(controller.onRequestWrite(first.bytes.data(), first.size, 1).accepted);
  TEST_ASSERT_TRUE(controller.onRequestWrite(second.bytes.data(), second.size, 2).accepted);
  TEST_ASSERT_TRUE(contains(controller.readSafeStatus().view(), "COMPLETE"));
  auto handoff = controller.takeCompletedPayload();
  TEST_ASSERT_TRUE(handoff.available());
  TEST_ASSERT_EQUAL_UINT32(12, handoff.size());
  TEST_ASSERT_EQUAL_MEMORY("safe-payload", handoff.data(), handoff.size());
  TEST_ASSERT_FALSE(controller.takeCompletedPayload().available());
}

void test_146_active_disconnect_clears_payload_and_allows_reconnection() {
  algaguard::BleProvisioningGattController controller;
  TEST_ASSERT_TRUE(controller.onConnected(7).accepted);
  const auto pending = encoded_frame(46, 0, 2, "partial");
  TEST_ASSERT_TRUE(controller.onRequestWrite(pending.bytes.data(), pending.size, 1).accepted);
  const auto disconnected = controller.onDisconnected(7);
  TEST_ASSERT_TRUE(disconnected.secretsCleared);
  TEST_ASSERT_FALSE(controller.hasActiveConnection());
  TEST_ASSERT_FALSE(controller.takeCompletedPayload().available());
  TEST_ASSERT_TRUE(contains(controller.takePendingNotification().view(), "DISCONNECTED"));
  TEST_ASSERT_TRUE(controller.onConnected(8).accepted);
}

void test_147_timeout_notifies_and_clears_pending_data() {
  algaguard::BleProvisioningGattController controller;
  TEST_ASSERT_TRUE(controller.onConnected(7).accepted);
  const auto pending = encoded_frame(47, 0, 2, "partial");
  TEST_ASSERT_TRUE(controller.onRequestWrite(pending.bytes.data(), pending.size, 10).accepted);
  const auto timeout = controller.onTick(
      10 + algaguard::kBleProvisioningDevelopmentTransportTimeoutTicks);
  TEST_ASSERT_FALSE(timeout.accepted);
  TEST_ASSERT_TRUE(timeout.secretsCleared);
  TEST_ASSERT_TRUE(contains(controller.takePendingNotification().view(), "TIMED_OUT"));
  TEST_ASSERT_FALSE(controller.takeCompletedPayload().available());
}

void test_148_failed_oversized_and_unknown_connection_writes_do_not_leak_payload() {
  algaguard::BleProvisioningGattController controller;
  TEST_ASSERT_TRUE(controller.onConnected(7).accepted);
  const auto valid = encoded_frame(48, 0, 1, "request-body");
  const auto unknown = controller.onRequestWrite(8, valid.bytes.data(), valid.size, 1);
  TEST_ASSERT_FALSE(unknown.accepted);
  TEST_ASSERT_TRUE(unknown.ignored);
  TEST_ASSERT_FALSE(controller.takeCompletedPayload().available());
  std::array<std::uint8_t, algaguard::kBleProvisioningMaxFrameBytes + 1> oversized{};
  const auto rejected = controller.onRequestWrite(oversized.data(), oversized.size(), 2);
  TEST_ASSERT_FALSE(rejected.accepted);
  TEST_ASSERT_TRUE(rejected.secretsCleared);
  TEST_ASSERT_FALSE(controller.takeCompletedPayload().available());
}

void test_149_notification_and_advertising_metadata_never_contain_private_fields() {
  algaguard::BleProvisioningGattController controller;
  TEST_ASSERT_TRUE(controller.onConnected(7).accepted);
  const auto notification = controller.takePendingNotification();
  const auto name = std::string_view{algaguard::kBleProvisioningAdvertisingName};
  for (const char* prohibited : {"ssid", "password", "sessionToken", "organizationId", "private",
                                 "request"}) {
    TEST_ASSERT_FALSE(contains(notification.view(), prohibited));
    TEST_ASSERT_FALSE(contains(name, prohibited));
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_144_first_connection_is_accepted_second_connection_is_rejected);
  RUN_TEST(test_145_valid_framed_writes_produce_one_completed_payload_handoff);
  RUN_TEST(test_146_active_disconnect_clears_payload_and_allows_reconnection);
  RUN_TEST(test_147_timeout_notifies_and_clears_pending_data);
  RUN_TEST(test_148_failed_oversized_and_unknown_connection_writes_do_not_leak_payload);
  RUN_TEST(test_149_notification_and_advertising_metadata_never_contain_private_fields);
  return UNITY_END();
}
