#include <array>
#include <cstring>

#include <unity.h>

#include "algaguard/ble_provisioning_framed_transport.hpp"

using algaguard::BleProvisioningFrame;
using algaguard::BleProvisioningFramedTransport;
using algaguard::BleProvisioningSafeReason;
using algaguard::BleProvisioningTransportState;

namespace {
BleProvisioningFrame frame(std::uint32_t message_id, std::uint16_t index, std::uint16_t count,
                           const char* payload, std::uint8_t version = 1) {
  BleProvisioningFrame value;
  value.protocolVersion = version;
  value.messageId = message_id;
  value.fragmentIndex = index;
  value.fragmentCount = count;
  value.setPayload(reinterpret_cast<const std::uint8_t*>(payload), std::strlen(payload));
  return value;
}

void connect(BleProvisioningFramedTransport& transport) { TEST_ASSERT_TRUE(transport.connect().retryAllowed); }
}  // namespace

void setUp() {}
void tearDown() {}

void test_136_valid_multi_fragment_message_reassembles_in_order() {
  BleProvisioningFrame original = frame(7, 0, 1, "codec");
  algaguard::BleProvisioningEncodedFrame encoded;
  TEST_ASSERT_TRUE(algaguard::encode_ble_provisioning_frame(original, encoded));
  BleProvisioningFrame decoded;
  TEST_ASSERT_TRUE(algaguard::decode_ble_provisioning_frame(encoded.bytes.data(), encoded.size, decoded));

  BleProvisioningFramedTransport transport;
  connect(transport);
  TEST_ASSERT_TRUE(transport.acceptFrame(frame(42, 0, 3, "one"), 1).retryAllowed);
  TEST_ASSERT_TRUE(transport.acceptFrame(frame(42, 1, 3, "two"), 2).retryAllowed);
  const auto complete = transport.acceptFrame(frame(42, 2, 3, "three"), 3);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleProvisioningTransportState::kComplete),
                        static_cast<int>(transport.state()));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleProvisioningSafeReason::kOk),
                        static_cast<int>(complete.safeReason));
  auto handoff = transport.takeCompletedPayload();
  TEST_ASSERT_EQUAL_UINT32(11, handoff.size());
  TEST_ASSERT_EQUAL_MEMORY("onetwothree", handoff.data(), handoff.size());
}

void test_137_duplicate_fragment_is_rejected_and_does_not_refresh_timeout() {
  BleProvisioningFramedTransport transport;
  connect(transport);
  transport.acceptFrame(frame(9, 0, 2, "first"), 10);
  const auto duplicate = transport.acceptFrame(frame(9, 0, 2, "first"), 20);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleProvisioningSafeReason::kDuplicateFragment),
                        static_cast<int>(duplicate.safeReason));
  TEST_ASSERT_EQUAL_UINT32(10, transport.lastAcceptedTick());
  TEST_ASSERT_TRUE(duplicate.secretsCleared);
}

void test_138_out_of_order_fragment_is_rejected() {
  BleProvisioningFramedTransport transport;
  connect(transport);
  const auto result = transport.acceptFrame(frame(10, 1, 2, "later"), 1);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleProvisioningSafeReason::kOutOfOrderFragment),
                        static_cast<int>(result.safeReason));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleProvisioningTransportState::kRejected),
                        static_cast<int>(transport.state()));
}

void test_139_message_id_or_fragment_count_change_is_rejected() {
  BleProvisioningFramedTransport message_mismatch;
  connect(message_mismatch);
  message_mismatch.acceptFrame(frame(1, 0, 2, "one"), 1);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleProvisioningSafeReason::kMessageIdMismatch),
                        static_cast<int>(message_mismatch.acceptFrame(frame(2, 1, 2, "two"), 2)
                                             .safeReason));

  BleProvisioningFramedTransport count_mismatch;
  connect(count_mismatch);
  count_mismatch.acceptFrame(frame(1, 0, 2, "one"), 1);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleProvisioningSafeReason::kFragmentCountMismatch),
                        static_cast<int>(count_mismatch.acceptFrame(frame(1, 1, 3, "two"), 2)
                                             .safeReason));
}

void test_140_assembled_payload_above_1024_bytes_is_rejected() {
  BleProvisioningFramedTransport transport;
  connect(transport);
  std::array<std::uint8_t, algaguard::kBleProvisioningMaxFramePayloadBytes> bytes{};
  BleProvisioningFrame item;
  item.messageId = 99;
  item.fragmentCount = 5;
  TEST_ASSERT_TRUE(item.setPayload(bytes.data(), bytes.size()));
  for (std::uint16_t index = 0; index < 4; ++index) {
    item.fragmentIndex = index;
    TEST_ASSERT_TRUE(transport.acceptFrame(item, index).retryAllowed);
  }
  item.fragmentIndex = 4;
  const auto result = transport.acceptFrame(item, 4);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleProvisioningSafeReason::kPayloadTooLarge),
                        static_cast<int>(result.safeReason));
}

void test_141_timeout_clears_all_pending_fragment_data() {
  BleProvisioningFramedTransport transport;
  connect(transport);
  transport.acceptFrame(frame(11, 0, 2, "pending"), 100);
  const auto timeout = transport.expire(100 + algaguard::kBleProvisioningDevelopmentTransportTimeoutTicks);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleProvisioningTransportState::kTimedOut),
                        static_cast<int>(transport.state()));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleProvisioningSafeReason::kTransportTimeout),
                        static_cast<int>(timeout.safeReason));
  TEST_ASSERT_FALSE(transport.hasPendingData());
  TEST_ASSERT_TRUE(timeout.secretsCleared);
}

void test_142_disconnect_clears_message_id_payload_and_temporary_secrets() {
  BleProvisioningFramedTransport transport;
  connect(transport);
  transport.acceptFrame(frame(12, 0, 2, "pending"), 1);
  const auto disconnected = transport.disconnect();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleProvisioningTransportState::kCleared),
                        static_cast<int>(transport.state()));
  TEST_ASSERT_FALSE(transport.hasActiveMessageId());
  TEST_ASSERT_EQUAL_UINT32(0, transport.activeMessageId());
  TEST_ASSERT_FALSE(transport.takeCompletedPayload().available());
  TEST_ASSERT_TRUE(disconnected.secretsCleared);
}

void test_143_completed_payload_is_emitted_once_replay_is_rejected_and_status_is_safe() {
  BleProvisioningFramedTransport transport;
  connect(transport);
  transport.acceptFrame(frame(13, 0, 1, "body"), 1);
  auto first = transport.takeCompletedPayload();
  TEST_ASSERT_TRUE(first.available());
  TEST_ASSERT_FALSE(transport.takeCompletedPayload().available());
  const auto replay = transport.acceptFrame(frame(13, 0, 1, "body"), 2);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleProvisioningSafeReason::kReplayRejected),
                        static_cast<int>(replay.safeReason));
  const auto status = replay.statusMessage();
  TEST_ASSERT_NULL(std::strstr(status.bytes.data(), "ssid"));
  TEST_ASSERT_NULL(std::strstr(status.bytes.data(), "password"));
  TEST_ASSERT_NULL(std::strstr(status.bytes.data(), "sessionToken"));
  TEST_ASSERT_NULL(std::strstr(status.bytes.data(), "body"));
  TEST_ASSERT_LESS_OR_EQUAL(algaguard::kBleProvisioningMaxStatusBytes, status.size);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_136_valid_multi_fragment_message_reassembles_in_order);
  RUN_TEST(test_137_duplicate_fragment_is_rejected_and_does_not_refresh_timeout);
  RUN_TEST(test_138_out_of_order_fragment_is_rejected);
  RUN_TEST(test_139_message_id_or_fragment_count_change_is_rejected);
  RUN_TEST(test_140_assembled_payload_above_1024_bytes_is_rejected);
  RUN_TEST(test_141_timeout_clears_all_pending_fragment_data);
  RUN_TEST(test_142_disconnect_clears_message_id_payload_and_temporary_secrets);
  RUN_TEST(test_143_completed_payload_is_emitted_once_replay_is_rejected_and_status_is_safe);
  return UNITY_END();
}
