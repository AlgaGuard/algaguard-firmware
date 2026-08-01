#include "algaguard/ble_provisioning_transport.hpp"

#if defined(ESP_PLATFORM)

#include <array>

extern "C" {
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_id.h"
#include "host/ble_hs_mbuf.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
}

namespace algaguard {
namespace {

ble_uuid128_t kServiceUuid = BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                                              0x00, 0x10, 0x00, 0x00, 0xa0, 0xa1, 0x00, 0x00);
ble_uuid128_t kRequestUuid = BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                                              0x00, 0x10, 0x00, 0x00, 0xa1, 0xa1, 0x00, 0x00);
ble_uuid128_t kStatusUuid = BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                                             0x00, 0x10, 0x00, 0x00, 0xa2, 0xa1, 0x00, 0x00);
EspIdfBleProvisioningTransport* gTransport{};

void record_gatt_contract_status() {
  std::uint16_t serviceHandle{};
  std::uint16_t requestDefinitionHandle{};
  std::uint16_t requestValueHandle{};
  std::uint16_t statusDefinitionHandle{};
  std::uint16_t statusValueHandle{};

  const bool servicePresent = ble_gatts_find_svc(&kServiceUuid.u, &serviceHandle) == 0;
  const bool requestPresent =
      ble_gatts_find_chr(&kServiceUuid.u, &kRequestUuid.u, &requestDefinitionHandle,
                         &requestValueHandle) == 0;
  const bool statusPresent =
      ble_gatts_find_chr(&kServiceUuid.u, &kStatusUuid.u, &statusDefinitionHandle,
                         &statusValueHandle) == 0;
  ESP_LOGI("algaguard_ble", "gatt_contract service=%s request=%s status=%s",
           servicePresent ? "present" : "missing", requestPresent ? "present" : "missing",
           statusPresent ? "present" : "missing");
}

void clear_bytes(std::array<std::uint8_t, kBleProvisioningMaxFrameBytes>& bytes) noexcept {
  volatile std::uint8_t* cursor = bytes.data();
  for (std::size_t index = 0; index < bytes.size(); ++index) cursor[index] = 0;
}

int request_access(std::uint16_t, std::uint16_t, ble_gatt_access_ctxt* context, void* argument) {
  if (context == nullptr || context->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
    return BLE_ATT_ERR_UNLIKELY;
  const auto length = static_cast<std::size_t>(OS_MBUF_PKTLEN(context->om));
  if (length == 0 || length > kBleProvisioningMaxFrameBytes)
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

  std::array<std::uint8_t, kBleProvisioningMaxFrameBytes> temporary{};
  std::uint16_t copied{};
  if (ble_hs_mbuf_to_flat(context->om, temporary.data(), static_cast<std::uint16_t>(length),
                          &copied) != 0 ||
      copied != length) {
    clear_bytes(temporary);
    return BLE_ATT_ERR_UNLIKELY;
  }
  const auto disposition = static_cast<EspIdfBleProvisioningTransport*>(argument)
                               ->dispatchBoundedWrite(temporary.data(), length);
  clear_bytes(temporary);
  return disposition == BleProvisioningWriteDisposition::kAccepted ? 0 : BLE_ATT_ERR_UNLIKELY;
}

int status_access(std::uint16_t, std::uint16_t, ble_gatt_access_ctxt* context, void* argument) {
  if (context == nullptr || context->op != BLE_GATT_ACCESS_OP_READ_CHR)
    return BLE_ATT_ERR_UNLIKELY;
  const auto& status = static_cast<EspIdfBleProvisioningTransport*>(argument)->latestSafeStatus();
  return os_mbuf_append(context->om, status.bytes.data(), static_cast<std::uint16_t>(status.size)) == 0
             ? 0
             : BLE_ATT_ERR_UNLIKELY;
}

int gap_event(ble_gap_event* event, void* argument) {
  auto* transport = static_cast<EspIdfBleProvisioningTransport*>(argument);
  if (event == nullptr || transport == nullptr) return 0;
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0) {
        transport->onGapConnected(event->connect.conn_handle);
        if (!transport->hasActiveConnection(event->connect.conn_handle))
          (void)ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
      } else {
        transport->startAdvertising();
      }
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      transport->onGapDisconnected(event->disconnect.conn.conn_handle);
      transport->startAdvertising();
      break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
      transport->startAdvertising();
      break;
    case BLE_GAP_EVENT_SUBSCRIBE:
      transport->onStatusSubscription(event->subscribe.attr_handle,
                                      event->subscribe.cur_notify != 0);
      break;
    default:
      break;
  }
  return 0;
}

void on_sync() {
  if (gTransport != nullptr) {
    gTransport->recordAdvertisingStage(BleAdvertisingStage::kHostSynced);
    record_gatt_contract_status();
    (void)gTransport->startAdvertising();
  }
}

void nimble_host_task(void*) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

}  // namespace

bool EspIdfBleProvisioningTransport::init() {
  if (initialized_) return true;
  recordAdvertisingStage(BleAdvertisingStage::kBleInitStart);
  const esp_err_t initResult = nimble_port_init();
  if (initResult != ESP_OK) {
    recordAdvertisingStage(BleAdvertisingStage::kAdvStartFailed, initResult);
    return false;
  }
  ble_svc_gap_init();
  ble_svc_gatt_init();
  if (ble_svc_gap_device_name_set(kBleProvisioningAdvertisingName) != 0) {
    (void)nimble_port_deinit();
    recordAdvertisingStage(BleAdvertisingStage::kAdvStartFailed, -1);
    return false;
  }
  gTransport = this;
  ble_hs_cfg.sync_cb = on_sync;
  controller_.setDeferredValidation(true);
  shutdown_ = false;
  initialized_ = true;
  recordAdvertisingStage(BleAdvertisingStage::kNimbleInitOk);
  return true;
}

bool EspIdfBleProvisioningTransport::startGattService() {
  if (serviceStarted_) return true;
  if (!init()) return false;

  if (!serviceRegistered_) {
    static ble_gatt_chr_def characteristics[3];
    static ble_gatt_svc_def services[2];
    characteristics[0] = {&kRequestUuid.u, request_access, this, nullptr, BLE_GATT_CHR_F_WRITE, 0,
                          nullptr, nullptr};
    characteristics[1] = {&kStatusUuid.u, status_access, this, nullptr,
                          static_cast<ble_gatt_chr_flags>(BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY),
                          0, &statusValueHandle_, nullptr};
    characteristics[2] = {};
    services[0] = {BLE_GATT_SVC_TYPE_PRIMARY, &kServiceUuid.u, nullptr, characteristics};
    services[1] = {};
    const int countResult = ble_gatts_count_cfg(services);
    if (countResult != 0) {
      ESP_LOGE("algaguard_ble", "gatt_service_count_failed code=%d", countResult);
      return false;
    }
    const int addResult = ble_gatts_add_svcs(services);
    if (addResult != 0) {
      ESP_LOGE("algaguard_ble", "gatt_service_add_failed code=%d", addResult);
      return false;
    }
    ESP_LOGI("algaguard_ble", "gatt_service_registered=true");
    serviceRegistered_ = true;
  }
  serviceStarted_ = true;
  if (!hostTaskStarted_) {
    nimble_port_freertos_init(nimble_host_task);
    hostTaskStarted_ = true;
    recordAdvertisingStage(BleAdvertisingStage::kHostTaskStarted);
  }
  return true;
}

void EspIdfBleProvisioningTransport::stopGattService() {
  if (!serviceStarted_) return;
  if (advertising_) (void)ble_gap_adv_stop();
  advertising_ = false;
  serviceStarted_ = false;
}

bool EspIdfBleProvisioningTransport::startAdvertising() {
  if (!initialized_ || !serviceStarted_ || shutdown_ || controller_.hasActiveConnection()) {
    recordAdvertisingStage(BleAdvertisingStage::kAdvStartFailed, -1);
    return false;
  }
  if (advertising_ || ble_gap_adv_active()) {
    advertising_ = true;
    recordAdvertisingStage(BleAdvertisingStage::kAdvStartOk);
    return true;
  }
  const int addressResult = ble_hs_id_infer_auto(0, &ownAddressType_);
  if (addressResult != 0) {
    recordAdvertisingStage(BleAdvertisingStage::kAdvStartFailed, addressResult);
    return false;
  }
  recordAdvertisingStage(BleAdvertisingStage::kAddressReady);

  ble_hs_adv_fields fields{};
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.uuids128 = &kServiceUuid;
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;
  const int fieldsResult = ble_gap_adv_set_fields(&fields);
  if (fieldsResult != 0) {
    recordAdvertisingStage(BleAdvertisingStage::kAdvStartFailed, fieldsResult);
    return false;
  }
  ble_hs_adv_fields scanResponse{};
  scanResponse.name = reinterpret_cast<const std::uint8_t*>(kBleProvisioningAdvertisingName);
  scanResponse.name_len = sizeof(kBleProvisioningAdvertisingName) - 1;
  scanResponse.name_is_complete = 1;
  const int scanResponseResult = ble_gap_adv_rsp_set_fields(&scanResponse);
  if (scanResponseResult != 0) {
    recordAdvertisingStage(BleAdvertisingStage::kAdvStartFailed, scanResponseResult);
    return false;
  }
  recordAdvertisingStage(BleAdvertisingStage::kAdvFieldsReady);

  ble_gap_adv_params parameters{};
  parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
  parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
  const int startResult =
      ble_gap_adv_start(ownAddressType_, nullptr, BLE_HS_FOREVER, &parameters, gap_event, this);
  if (startResult != 0) {
    recordAdvertisingStage(BleAdvertisingStage::kAdvStartFailed, startResult);
    return false;
  }
  advertising_ = true;
  recordAdvertisingStage(BleAdvertisingStage::kAdvStartOk);
  return true;
}

void EspIdfBleProvisioningTransport::recordAdvertisingStage(BleAdvertisingStage stage,
                                                             std::int32_t returnCode) {
  advertisingStatus_.stage = stage;
  advertisingStatus_.returnCode = returnCode;
  advertisingStatus_.advertisingActive = stage == BleAdvertisingStage::kAdvStartOk;
  if (stage == BleAdvertisingStage::kAdvStartFailed && advertisingStatus_.retryCount < 255)
    ++advertisingStatus_.retryCount;
  ESP_LOGI("algaguard_ble", "stage=%s code=%ld retry=%u advertising_active=%s",
           ble_advertising_stage_code(advertisingStatus_.stage).data(),
           static_cast<long>(advertisingStatus_.returnCode),
           static_cast<unsigned>(advertisingStatus_.retryCount),
           advertisingStatus_.advertisingActive ? "true" : "false");
}

void EspIdfBleProvisioningTransport::setWriteHandler(BleProvisioningWriteHandler handler, void* context) {
  writeSeam_.setHandler(handler, context);
}

bool EspIdfBleProvisioningTransport::publishSafeStatus(BleProvisioningSafeStatus status,
                                                        BleProvisioningSafeReason reason) {
  (void)status;
  (void)reason;
  publishPendingNotification();
  return true;
}

void EspIdfBleProvisioningTransport::clearPendingWrite() { writeSeam_.clear(); }

void EspIdfBleProvisioningTransport::pollProvisioningTransport(std::uint64_t nowTick) {
  (void)controller_.processDeferredValidation(nowTick);
  controller_.onTick(nowTick);
  publishPendingNotification();
}

void EspIdfBleProvisioningTransport::onGapConnected(std::uint16_t connectionId) {
  advertising_ = false;
  controller_.onConnected(connectionId);
  publishPendingNotification();
}

void EspIdfBleProvisioningTransport::onGapDisconnected(std::uint16_t connectionId) {
  controller_.onDisconnected(connectionId);
  statusSubscribed_ = false;
  publishPendingNotification();
}

void EspIdfBleProvisioningTransport::onStatusSubscription(std::uint16_t attributeHandle,
                                                           bool subscribed) {
  if (attributeHandle == statusValueHandle_) statusSubscribed_ = subscribed;
  publishPendingNotification();
}

bool EspIdfBleProvisioningTransport::hasActiveConnection(std::uint16_t connectionId) const {
  return controller_.hasActiveConnection() && controller_.connectionIdMatches(connectionId);
}

bool EspIdfBleProvisioningTransport::installDevelopmentProvisioningSession(
    std::string_view sessionId, std::string_view deviceId, std::string_view sessionToken,
    std::uint64_t expiryTick) {
  return controller_.installDevelopmentSession(sessionId, deviceId, sessionToken, expiryTick);
}

BleWifiCredentialHandoff EspIdfBleProvisioningTransport::takeAcceptedWifiCredentials() {
  return controller_.takeAcceptedWifiCredentials();
}

void EspIdfBleProvisioningTransport::publishPendingNotification() {
  const auto notification = controller_.takePendingNotification();
  if (notification.size == 0 || !serviceStarted_ || !statusSubscribed_ || statusValueHandle_ == 0)
    return;
  (void)ble_gatts_chr_updated(statusValueHandle_);
}

void EspIdfBleProvisioningTransport::shutdown() {
  shutdown_ = true;
  controller_.shutdown();
  stopGattService();
  clearPendingWrite();
  statusSubscribed_ = false;
  if (initialized_) {
    if (hostTaskStarted_) (void)nimble_port_stop();
    if (serviceRegistered_) (void)ble_gatts_reset();
    (void)nimble_port_deinit();
  }
  if (gTransport == this) gTransport = nullptr;
  initialized_ = false;
  serviceRegistered_ = false;
  hostTaskStarted_ = false;
  statusValueHandle_ = 0;
}

BleProvisioningWriteDisposition EspIdfBleProvisioningTransport::dispatchBoundedWrite(
    const std::uint8_t* bytes, std::size_t length) {
  const auto result = controller_.onRequestWrite(
      bytes, length, static_cast<std::uint64_t>(xTaskGetTickCount()));
  publishPendingNotification();
  return result.accepted ? BleProvisioningWriteDisposition::kAccepted
                         : BleProvisioningWriteDisposition::kRejectedNoHandler;
}

}  // namespace algaguard

#endif
