#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "algaguard/ble_provisioning_gatt.hpp"

namespace algaguard {

enum class BleAdvertisingStage : std::uint8_t {
  kBleInitStart,
  kNvsReady,
  kNimbleInitOk,
  kHostTaskStarted,
  kHostSynced,
  kAddressReady,
  kAdvFieldsReady,
  kAdvStartOk,
  kAdvStartFailed,
};

struct BleAdvertisingRuntimeStatus {
  BleAdvertisingStage stage{BleAdvertisingStage::kBleInitStart};
  std::int32_t returnCode{};
  std::uint8_t retryCount{};
  bool advertisingActive{};
};

struct BleLegacyAdvertisingLayout {
  bool flagsInAdvertising{true};
  bool serviceUuidInAdvertising{true};
  bool completeNameInScanResponse{true};
  bool advertisingFitsLegacyLimit{true};
};

inline constexpr std::size_t kBleLegacyAdvertisingMaxBytes = 31;
inline constexpr BleLegacyAdvertisingLayout kBleLegacyAdvertisingLayout{};

inline constexpr std::string_view ble_advertising_stage_code(BleAdvertisingStage stage) {
  switch (stage) {
    case BleAdvertisingStage::kBleInitStart: return "BLE_INIT_START";
    case BleAdvertisingStage::kNvsReady: return "NVS_READY";
    case BleAdvertisingStage::kNimbleInitOk: return "NIMBLE_INIT_OK";
    case BleAdvertisingStage::kHostTaskStarted: return "HOST_TASK_STARTED";
    case BleAdvertisingStage::kHostSynced: return "HOST_SYNCED";
    case BleAdvertisingStage::kAddressReady: return "ADDRESS_READY";
    case BleAdvertisingStage::kAdvFieldsReady: return "ADV_FIELDS_READY";
    case BleAdvertisingStage::kAdvStartOk: return "ADV_START_OK";
    case BleAdvertisingStage::kAdvStartFailed: return "ADV_START_FAILED";
  }
  return "ADV_START_FAILED";
}

inline BleAdvertisingRuntimeStatus ble_advertising_start_result(
    BleAdvertisingRuntimeStatus status, std::int32_t returnCode) {
  status.returnCode = returnCode;
  status.stage = returnCode == 0 ? BleAdvertisingStage::kAdvStartOk
                                 : BleAdvertisingStage::kAdvStartFailed;
  status.advertisingActive = returnCode == 0;
  if (returnCode != 0 && status.retryCount < 255) ++status.retryCount;
  return status;
}

inline std::string ble_advertising_safe_diagnostic(const BleAdvertisingRuntimeStatus& status) {
  return "stage=" + std::string(ble_advertising_stage_code(status.stage)) +
         " code=" + std::to_string(status.returnCode) +
         " retry=" + std::to_string(status.retryCount) +
         " advertising_active=" + (status.advertisingActive ? "true" : "false");
}

}  // namespace algaguard
