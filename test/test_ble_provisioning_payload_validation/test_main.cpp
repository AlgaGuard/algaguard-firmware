#include <cstring>
#include <string>

#include <unity.h>

#include "algaguard/ble_provisioning_gatt_controller.hpp"

namespace {
constexpr const char* kSessionId = "50000000-0000-4000-8000-000000000001";
constexpr const char* kDeviceId = "AG-000001";
constexpr const char* kSessionToken = "sE2vR8yN5kM1pQ7xT4bW9dF6aC3uH0zL";
constexpr const char* kPassword = "synthetic-wifi-password";

std::string payload(std::string_view deviceId = kDeviceId, std::string_view token = kSessionToken,
                    std::string_view ssid = "AlgaGuard-Lab", std::string_view password = kPassword) {
  return std::string{"{\"schema\":\"urn:algaguard:schema:onboarding:ble-provisioning-request:v1\","
                     "\"schemaVersion\":\"1.0.0\",\"sessionId\":\""} +
         kSessionId + "\",\"deviceId\":\"" + std::string{deviceId} +
         "\",\"sessionToken\":\"" + std::string{token} + "\",\"ssid\":\"" +
         std::string{ssid} + "\",\"password\":\"" + std::string{password} + "\"}";
}

std::string qrPayload() {
  return std::string{"{\"schema\":\"urn:algaguard:schema:onboarding:ble-provisioning-request:v2\","
                     "\"schemaVersion\":\"2.0.0\",\"sessionId\":\""} +
         kSessionId + "\",\"deviceId\":\"" + kDeviceId +
         "\",\"sessionToken\":\"" + kSessionToken +
         "\",\"bindingGrant\":\"" + std::string(194, 'g') +
         "\",\"ssid\":\"AlgaGuard-Lab\",\"password\":\"" + kPassword + "\"}";
}

class QrAuthorizer final : public algaguard::QrBleSessionAuthorizer {
 public:
  int calls{};
  std::optional<std::uint64_t> authorize(
      const algaguard::BleWifiProvisioningRequest& request,
      std::uint64_t) override {
    ++calls;
    return request.bindingGrant().size() == 194 ? std::optional<std::uint64_t>{200}
                                                : std::nullopt;
  }
};

void send(algaguard::BleProvisioningGattController& controller, const std::string& body,
          std::uint32_t messageId, std::uint64_t tick) {
  const auto split = body.size() > 180 ? 180 : body.size();
  const auto frameCount = static_cast<std::uint16_t>((body.size() + split - 1U) / split);
  for (std::uint16_t index = 0; index < frameCount; ++index) {
    const auto offset = static_cast<std::size_t>(index) * split;
    const auto size = std::min(split, body.size() - offset);
    algaguard::BleProvisioningFrame frame;
    frame.messageId = messageId;
    frame.fragmentIndex = index;
    frame.fragmentCount = frameCount;
    TEST_ASSERT_TRUE(frame.setPayload(reinterpret_cast<const std::uint8_t*>(body.data() + offset), size));
    algaguard::BleProvisioningEncodedFrame encoded;
    TEST_ASSERT_TRUE(algaguard::encode_ble_provisioning_frame(frame, encoded));
    controller.onRequestWrite(encoded.bytes.data(), encoded.size, tick + index);
  }
}

void configure(algaguard::BleProvisioningGattController& controller, std::uint64_t expiry = 200) {
  TEST_ASSERT_TRUE(controller.installDevelopmentSession(kSessionId, kDeviceId, kSessionToken, expiry));
  TEST_ASSERT_TRUE(controller.onConnected(7).accepted);
}

bool contains(std::string_view text, std::string_view value) {
  return text.find(value) != std::string_view::npos;
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_150_valid_canonical_payload_reaches_accepted() {
  algaguard::BleProvisioningGattController controller;
  configure(controller);
  send(controller, payload(), 101, 10);
  TEST_ASSERT_TRUE(contains(controller.readSafeStatus().view(), "ACCEPTED"));
  auto credentials = controller.takeAcceptedWifiCredentials();
  TEST_ASSERT_TRUE(credentials.available());
  TEST_ASSERT_EQUAL_STRING("AlgaGuard-Lab", std::string{credentials.ssid()}.c_str());
}

void test_151_missing_duplicate_unknown_or_malformed_payload_is_rejected() {
  algaguard::BleProvisioningGattController missing;
  configure(missing);
  send(missing, "{\"schema\":\"urn:algaguard:schema:onboarding:ble-provisioning-request:v1\"}", 102, 10);
  TEST_ASSERT_TRUE(contains(missing.readSafeStatus().view(), "MALFORMED_PAYLOAD"));

  algaguard::BleProvisioningGattController duplicate;
  configure(duplicate);
  auto repeated = payload();
  repeated.insert(repeated.size() - 1, ",\"ssid\":\"Other\"");
  send(duplicate, repeated, 103, 10);
  TEST_ASSERT_TRUE(contains(duplicate.readSafeStatus().view(), "MALFORMED_PAYLOAD"));

  algaguard::BleProvisioningGattController unknown;
  configure(unknown);
  auto extra = payload();
  extra.insert(extra.size() - 1, ",\"bootstrapToken\":\"synthetic\"");
  send(unknown, extra, 104, 10);
  TEST_ASSERT_TRUE(contains(unknown.readSafeStatus().view(), "MALFORMED_PAYLOAD"));
}

void test_152_oversized_decoded_field_is_rejected_before_request_validation() {
  algaguard::BleProvisioningGattController controller;
  configure(controller);
  const std::string oversized(33, 's');
  send(controller, payload(kDeviceId, kSessionToken, oversized), 105, 10);
  TEST_ASSERT_TRUE(contains(controller.readSafeStatus().view(), "FIELD_TOO_LONG"));
}

void test_153_device_id_mismatch_maps_to_safe_reason() {
  algaguard::BleProvisioningGattController controller;
  configure(controller);
  send(controller, payload("AG-000002"), 106, 10);
  TEST_ASSERT_TRUE(contains(controller.readSafeStatus().view(), "DEVICE_ID_MISMATCH"));
}

void test_154_session_mismatch_and_expiry_are_safe() {
  algaguard::BleProvisioningGattController mismatch;
  configure(mismatch);
  send(mismatch, payload(kDeviceId, "A1234567890123456789012345678901"), 107, 10);
  TEST_ASSERT_TRUE(contains(mismatch.readSafeStatus().view(), "SESSION_MISMATCH"));
  TEST_ASSERT_FALSE(contains(mismatch.readSafeStatus().view(), kSessionToken));

  algaguard::BleProvisioningGattController expired;
  configure(expired, 20);
  send(expired, payload(), 108, 20);
  TEST_ASSERT_TRUE(contains(expired.readSafeStatus().view(), "SESSION_EXPIRED"));
}

void test_155_accepted_request_creates_one_wifi_handoff_without_session_token() {
  algaguard::BleProvisioningGattController controller;
  configure(controller);
  send(controller, payload(), 109, 10);
  auto handoff = controller.takeAcceptedWifiCredentials();
  TEST_ASSERT_TRUE(handoff.available());
  TEST_ASSERT_EQUAL_STRING(kPassword, std::string{handoff.password()}.c_str());
  TEST_ASSERT_FALSE(contains(handoff.ssid(), "session"));
  TEST_ASSERT_FALSE(controller.takeAcceptedWifiCredentials().available());
}

void test_156_replay_cannot_overwrite_pending_accepted_credentials() {
  algaguard::BleProvisioningGattController controller;
  configure(controller);
  send(controller, payload(), 110, 10);
  send(controller, payload(kDeviceId, kSessionToken, "OtherNetwork"), 111, 20);
  TEST_ASSERT_TRUE(contains(controller.readSafeStatus().view(), "REPLAY_REJECTED"));
  auto handoff = controller.takeAcceptedWifiCredentials();
  TEST_ASSERT_EQUAL_STRING("AlgaGuard-Lab", std::string{handoff.ssid()}.c_str());
}

void test_157_terminal_paths_zeroize_secret_buffers_and_safe_status() {
  algaguard::BleProvisioningGattController accepted;
  configure(accepted);
  send(accepted, payload(), 112, 10);
  auto handoff = accepted.takeAcceptedWifiCredentials();
  handoff.clear();
  TEST_ASSERT_TRUE(handoff.secretsCleared());
  accepted.reset();

  algaguard::BleProvisioningGattController rejected;
  configure(rejected);
  send(rejected, "{not-json}", 113, 10);
  TEST_ASSERT_FALSE(rejected.takeAcceptedWifiCredentials().available());
  const auto status = rejected.readSafeStatus().view();
  TEST_ASSERT_FALSE(contains(status, "ssid"));
  TEST_ASSERT_FALSE(contains(status, "password"));
  TEST_ASSERT_FALSE(contains(status, "sessionToken"));
  TEST_ASSERT_FALSE(contains(status, kPassword));
}

void test_158_qr_bound_v2_authorizes_one_session_without_com16() {
  const auto body = qrPayload();
  algaguard::BleProvisioningPayloadParser parser;
  const auto parsed = parser.parse(
      reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(algaguard::BleWifiProvisioningReason::OK),
                        static_cast<int>(parsed.safeReason));
  TEST_ASSERT_TRUE(parsed.accepted());
  algaguard::BleProvisioningGattController controller;
  QrAuthorizer authorizer;
  controller.setQrAuthorizer(&authorizer);
  TEST_ASSERT_TRUE(controller.onConnected(7).accepted);
  send(controller, body, 114, 10);
  TEST_ASSERT_EQUAL(1, authorizer.calls);
  TEST_ASSERT_TRUE(contains(controller.readSafeStatus().view(), "ACCEPTED"));
  TEST_ASSERT_TRUE(controller.takeAcceptedWifiCredentials().available());
}

void test_159_deferred_qr_validation_keeps_completed_write_callback_bounded() {
  const auto body = qrPayload();
  algaguard::BleProvisioningGattController controller;
  QrAuthorizer authorizer;
  controller.setQrAuthorizer(&authorizer);
  controller.setDeferredValidation(true);
  TEST_ASSERT_TRUE(controller.onConnected(7).accepted);

  send(controller, body, 115, 10);

  TEST_ASSERT_EQUAL(0, authorizer.calls);
  TEST_ASSERT_TRUE(contains(controller.readSafeStatus().view(), "COMPLETE"));
  TEST_ASSERT_FALSE(controller.takeAcceptedWifiCredentials().available());

  const auto validated = controller.processDeferredValidation(20);
  TEST_ASSERT_TRUE(validated.accepted);
  TEST_ASSERT_EQUAL(1, authorizer.calls);
  TEST_ASSERT_TRUE(contains(controller.readSafeStatus().view(), "ACCEPTED"));
  TEST_ASSERT_TRUE(controller.takeAcceptedWifiCredentials().available());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_150_valid_canonical_payload_reaches_accepted);
  RUN_TEST(test_151_missing_duplicate_unknown_or_malformed_payload_is_rejected);
  RUN_TEST(test_152_oversized_decoded_field_is_rejected_before_request_validation);
  RUN_TEST(test_153_device_id_mismatch_maps_to_safe_reason);
  RUN_TEST(test_154_session_mismatch_and_expiry_are_safe);
  RUN_TEST(test_155_accepted_request_creates_one_wifi_handoff_without_session_token);
  RUN_TEST(test_156_replay_cannot_overwrite_pending_accepted_credentials);
  RUN_TEST(test_157_terminal_paths_zeroize_secret_buffers_and_safe_status);
  RUN_TEST(test_158_qr_bound_v2_authorizes_one_session_without_com16);
  RUN_TEST(test_159_deferred_qr_validation_keeps_completed_write_callback_bounded);
  return UNITY_END();
}
