#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace algaguard {

enum class BleGattProperty : std::uint8_t {
  kNone = 0,
  kRead = 1U << 0U,
  kWriteWithResponse = 1U << 1U,
  kNotify = 1U << 2U,
};

constexpr BleGattProperty operator|(BleGattProperty left, BleGattProperty right) {
  return static_cast<BleGattProperty>(static_cast<std::uint8_t>(left) |
                                      static_cast<std::uint8_t>(right));
}

constexpr bool has_gatt_property(BleGattProperty properties, BleGattProperty property) {
  return (static_cast<std::uint8_t>(properties) & static_cast<std::uint8_t>(property)) != 0U;
}

enum class BleLinkSecurityPolicy : std::uint8_t {
  kNotYetEnabled,
};

inline constexpr std::string_view kBleLinkSecurityPolicyCode =
    "BLE_LINK_SECURITY_NOT_YET_ENABLED";
inline constexpr std::uint16_t kBleProvisioningGattProtocolVersion = 1;
inline constexpr std::size_t kBleProvisioningMaxWriteBytes = 1024;
inline constexpr std::size_t kBleProvisioningMaxStatusBytes = 96;

inline constexpr std::string_view kBleProvisioningServiceUuid =
    "0000a1a0-0000-1000-8000-00805f9b34fb";
inline constexpr std::string_view kBleProvisioningRequestUuid =
    "0000a1a1-0000-1000-8000-00805f9b34fb";
inline constexpr std::string_view kBleProvisioningStatusUuid =
    "0000a1a2-0000-1000-8000-00805f9b34fb";

enum class BleProvisioningCharacteristic : std::uint8_t {
  kRequest,
  kStatus,
};

struct BleGattCharacteristicContract {
  BleProvisioningCharacteristic kind;
  std::string_view uuid;
  BleGattProperty properties;
  std::size_t maximumLength;
};

inline constexpr std::array<BleGattCharacteristicContract, 2> kBleProvisioningCharacteristics{{
    {BleProvisioningCharacteristic::kRequest, kBleProvisioningRequestUuid,
     BleGattProperty::kWriteWithResponse, kBleProvisioningMaxWriteBytes},
    {BleProvisioningCharacteristic::kStatus, kBleProvisioningStatusUuid,
     BleGattProperty::kRead | BleGattProperty::kNotify, kBleProvisioningMaxStatusBytes},
}};

constexpr bool ble_provisioning_uuids_are_unique() {
  return kBleProvisioningServiceUuid != kBleProvisioningRequestUuid &&
         kBleProvisioningServiceUuid != kBleProvisioningStatusUuid &&
         kBleProvisioningRequestUuid != kBleProvisioningStatusUuid;
}

constexpr const BleGattCharacteristicContract& ble_provisioning_request_contract() {
  return kBleProvisioningCharacteristics[0];
}

constexpr const BleGattCharacteristicContract& ble_provisioning_status_contract() {
  return kBleProvisioningCharacteristics[1];
}

static_assert(ble_provisioning_uuids_are_unique(), "BLE service and characteristic UUIDs must differ");
static_assert(kBleProvisioningGattProtocolVersion == 1,
              "Only the documented BLE provisioning protocol is supported");
static_assert(kBleProvisioningMaxWriteBytes > 0 && kBleProvisioningMaxWriteBytes <= 1024,
              "BLE write length must be bounded");
static_assert(kBleProvisioningMaxStatusBytes > 0 && kBleProvisioningMaxStatusBytes <= 128,
              "BLE status length must be bounded");
static_assert(!has_gatt_property(ble_provisioning_request_contract().properties,
                                 BleGattProperty::kRead),
              "Provisioning request characteristic must not be readable");
static_assert(has_gatt_property(ble_provisioning_request_contract().properties,
                                BleGattProperty::kWriteWithResponse),
              "Provisioning request characteristic must require write response");
static_assert(!has_gatt_property(ble_provisioning_status_contract().properties,
                                 BleGattProperty::kWriteWithResponse),
              "Provisioning status characteristic must not be writable");
static_assert(has_gatt_property(ble_provisioning_status_contract().properties,
                                BleGattProperty::kRead) &&
                  has_gatt_property(ble_provisioning_status_contract().properties,
                                    BleGattProperty::kNotify),
              "Provisioning status characteristic must be readable and notifiable");

enum class BleProvisioningSafeStatus : std::uint8_t {
  kReady,
  kReceiving,
  kComplete,
  kAccepted,
  kRejected,
  kTimedOut,
  kExpired,
  kCancelled,
};

enum class BleProvisioningSafeReason : std::uint8_t {
  kOk,
  kMalformedPayload,
  kSessionMismatch,
  kSessionExpired,
  kReplayRejected,
  kFieldTooLong,
  kInvalidTransition,
  kCancelled,
  kMalformedFrame,
  kUnsupportedVersion,
  kDuplicateFragment,
  kOutOfOrderFragment,
  kMessageIdMismatch,
  kFragmentCountMismatch,
  kPayloadTooLarge,
  kTransportTimeout,
  kDisconnected,
  kDeviceIdMismatch,
};

inline constexpr std::string_view ble_safe_status_code(BleProvisioningSafeStatus status) {
  switch (status) {
    case BleProvisioningSafeStatus::kReady: return "READY";
    case BleProvisioningSafeStatus::kReceiving: return "RECEIVING";
    case BleProvisioningSafeStatus::kComplete: return "COMPLETE";
    case BleProvisioningSafeStatus::kAccepted: return "ACCEPTED";
    case BleProvisioningSafeStatus::kRejected: return "REJECTED";
    case BleProvisioningSafeStatus::kTimedOut: return "TIMED_OUT";
    case BleProvisioningSafeStatus::kExpired: return "EXPIRED";
    case BleProvisioningSafeStatus::kCancelled: return "CANCELLED";
  }
  return "REJECTED";
}

inline constexpr std::string_view ble_safe_reason_code(BleProvisioningSafeReason reason) {
  switch (reason) {
    case BleProvisioningSafeReason::kOk: return "OK";
    case BleProvisioningSafeReason::kMalformedPayload: return "MALFORMED_PAYLOAD";
    case BleProvisioningSafeReason::kSessionMismatch: return "SESSION_MISMATCH";
    case BleProvisioningSafeReason::kSessionExpired: return "SESSION_EXPIRED";
    case BleProvisioningSafeReason::kReplayRejected: return "REPLAY_REJECTED";
    case BleProvisioningSafeReason::kFieldTooLong: return "FIELD_TOO_LONG";
    case BleProvisioningSafeReason::kInvalidTransition: return "INVALID_TRANSITION";
    case BleProvisioningSafeReason::kCancelled: return "CANCELLED";
    case BleProvisioningSafeReason::kMalformedFrame: return "MALFORMED_FRAME";
    case BleProvisioningSafeReason::kUnsupportedVersion: return "UNSUPPORTED_VERSION";
    case BleProvisioningSafeReason::kDuplicateFragment: return "DUPLICATE_FRAGMENT";
    case BleProvisioningSafeReason::kOutOfOrderFragment: return "OUT_OF_ORDER_FRAGMENT";
    case BleProvisioningSafeReason::kMessageIdMismatch: return "MESSAGE_ID_MISMATCH";
    case BleProvisioningSafeReason::kFragmentCountMismatch: return "FRAGMENT_COUNT_MISMATCH";
    case BleProvisioningSafeReason::kPayloadTooLarge: return "PAYLOAD_TOO_LARGE";
    case BleProvisioningSafeReason::kTransportTimeout: return "TRANSPORT_TIMEOUT";
    case BleProvisioningSafeReason::kDisconnected: return "DISCONNECTED";
    case BleProvisioningSafeReason::kDeviceIdMismatch: return "DEVICE_ID_MISMATCH";
  }
  return "INVALID_TRANSITION";
}

struct BleProvisioningSafeStatusMessage {
  std::array<char, kBleProvisioningMaxStatusBytes> bytes{};
  std::size_t size{};

  std::string_view view() const { return {bytes.data(), size}; }
};

inline BleProvisioningSafeStatusMessage encode_ble_provisioning_safe_status(
    BleProvisioningSafeStatus status, BleProvisioningSafeReason reason) {
  BleProvisioningSafeStatusMessage message;
  const auto append = [&message](std::string_view value) {
    for (const char character : value) {
      if (message.size < message.bytes.size()) message.bytes[message.size++] = character;
    }
  };
  append("status=");
  append(ble_safe_status_code(status));
  append(" reason=");
  append(ble_safe_reason_code(reason));
  return message;
}

}  // namespace algaguard
