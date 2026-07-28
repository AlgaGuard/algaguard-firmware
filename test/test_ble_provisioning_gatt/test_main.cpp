#include <array>
#include <cstring>

#include <unity.h>

#include "algaguard/ble_provisioning_gatt.hpp"
#include "algaguard/ble_provisioning_transport.hpp"

using algaguard::BleGattProperty;
using algaguard::BleProvisioningSafeReason;
using algaguard::BleProvisioningSafeStatus;
using algaguard::BleProvisioningWriteDisposition;

namespace {
std::size_t g_received_length{};

void record_write(const std::uint8_t*, std::size_t length, void*) { g_received_length = length; }
}  // namespace

void setUp() { g_received_length = 0; }
void tearDown() {}

void test_130_service_and_characteristic_uuids_are_unique() {
  TEST_ASSERT_TRUE(algaguard::ble_provisioning_uuids_are_unique());
}

void test_131_request_is_write_with_response_only() {
  const auto& request = algaguard::ble_provisioning_request_contract();
  TEST_ASSERT_TRUE(algaguard::has_gatt_property(request.properties,
                                                 BleGattProperty::kWriteWithResponse));
  TEST_ASSERT_FALSE(algaguard::has_gatt_property(request.properties, BleGattProperty::kRead));
  TEST_ASSERT_FALSE(algaguard::has_gatt_property(request.properties, BleGattProperty::kNotify));
}

void test_132_status_is_read_notify_and_not_writable() {
  const auto& status = algaguard::ble_provisioning_status_contract();
  TEST_ASSERT_TRUE(algaguard::has_gatt_property(status.properties, BleGattProperty::kRead));
  TEST_ASSERT_TRUE(algaguard::has_gatt_property(status.properties, BleGattProperty::kNotify));
  TEST_ASSERT_FALSE(algaguard::has_gatt_property(status.properties,
                                                  BleGattProperty::kWriteWithResponse));
}

void test_133_request_and_status_limits_are_nonzero_and_bounded() {
  TEST_ASSERT_GREATER_THAN(0, algaguard::kBleProvisioningMaxWriteBytes);
  TEST_ASSERT_LESS_OR_EQUAL(1024, algaguard::kBleProvisioningMaxWriteBytes);
  TEST_ASSERT_GREATER_THAN(0, algaguard::kBleProvisioningMaxStatusBytes);
  TEST_ASSERT_LESS_OR_EQUAL(128, algaguard::kBleProvisioningMaxStatusBytes);
}

void test_134_safe_status_encoder_exposes_no_request_secrets() {
  const auto message = algaguard::encode_ble_provisioning_safe_status(
      BleProvisioningSafeStatus::kRejected, BleProvisioningSafeReason::kSessionMismatch);
  const auto value = message.view();
  TEST_ASSERT_NOT_NULL(std::strstr(value.data(), "REJECTED"));
  TEST_ASSERT_NOT_NULL(std::strstr(value.data(), "SESSION_MISMATCH"));
  TEST_ASSERT_NULL(std::strstr(value.data(), "password"));
  TEST_ASSERT_NULL(std::strstr(value.data(), "sessionToken"));
  TEST_ASSERT_NULL(std::strstr(value.data(), "ssid"));
  TEST_ASSERT_NULL(std::strstr(value.data(), "payload"));
}

void test_135_empty_and_oversized_writes_are_rejected_by_transport_seam() {
  algaguard::BleProvisioningWriteSeam seam;
  seam.setHandler(record_write);
  const std::array<std::uint8_t, 1> one_byte{{0x01}};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleProvisioningWriteDisposition::kRejectedEmpty),
                        static_cast<int>(seam.dispatch(one_byte.data(), 0)));
  std::array<std::uint8_t, algaguard::kBleProvisioningMaxWriteBytes + 1> oversized{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BleProvisioningWriteDisposition::kRejectedTooLong),
                        static_cast<int>(seam.dispatch(oversized.data(), oversized.size())));
  TEST_ASSERT_EQUAL_UINT32(0, g_received_length);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_130_service_and_characteristic_uuids_are_unique);
  RUN_TEST(test_131_request_is_write_with_response_only);
  RUN_TEST(test_132_status_is_read_notify_and_not_writable);
  RUN_TEST(test_133_request_and_status_limits_are_nonzero_and_bounded);
  RUN_TEST(test_134_safe_status_encoder_exposes_no_request_secrets);
  RUN_TEST(test_135_empty_and_oversized_writes_are_rejected_by_transport_seam);
  return UNITY_END();
}
