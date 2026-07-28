#include "algaguard/esp_idf_wifi_connection_adapter.hpp"

#if defined(ESP_PLATFORM)

#include <cstddef>
#include <cstring>

extern "C" {
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace algaguard {
namespace {

void secure_clear_bytes(void* bytes, std::size_t length) noexcept {
  volatile auto* cursor = static_cast<volatile std::uint8_t*>(bytes);
  for (std::size_t index = 0; index < length; ++index) cursor[index] = 0;
}

WifiDisconnectClassification classify_esp_idf_disconnect_reason(std::uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_AUTH_LEAVE:
    case WIFI_REASON_ASSOC_NOT_AUTHED:
    case WIFI_REASON_802_1X_AUTH_FAILED:
    case WIFI_REASON_AUTH_FAIL:
      return WifiDisconnectClassification::kAuthenticationFailed;
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
      return WifiDisconnectClassification::kNetworkNotFound;
    case WIFI_REASON_ASSOC_LEAVE:
      return WifiDisconnectClassification::kLocalDisconnect;
    default:
      return WifiDisconnectClassification::kTransientFailure;
  }
}

bool acceptable_init_result(esp_err_t result) {
  return result == ESP_OK || result == ESP_ERR_INVALID_STATE || result == ESP_ERR_WIFI_INIT_STATE;
}

bool acceptable_disconnect_result(esp_err_t result) {
  return result == ESP_OK || result == ESP_ERR_WIFI_NOT_CONNECT ||
         result == ESP_ERR_WIFI_NOT_STARTED || result == ESP_ERR_WIFI_NOT_INIT;
}

}  // namespace

bool EspIdfWifiConnectionAdapter::init() {
  if (initialized_) return true;
  if (!acceptable_init_result(esp_netif_init())) return false;
  if (!acceptable_init_result(esp_event_loop_create_default())) return false;
  if (stationNetif_ == nullptr) stationNetif_ = esp_netif_create_default_wifi_sta();
  if (stationNetif_ == nullptr) return false;

  wifi_init_config_t initialization = WIFI_INIT_CONFIG_DEFAULT();
  if (!acceptable_init_result(esp_wifi_init(&initialization))) return false;
  if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) return false;
  if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
  if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, this,
                                          &wifiEventHandlerInstance_) != ESP_OK)
    return false;
  if (esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ipEventHandler, this,
                                          &ipEventHandlerInstance_) != ESP_OK) {
    (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                wifiEventHandlerInstance_);
    wifiEventHandlerInstance_ = nullptr;
    return false;
  }
  handlersRegistered_ = true;
  initialized_ = true;
  return true;
}

bool EspIdfWifiConnectionAdapter::start() {
  if (started_) return true;
  if (!init()) return false;
  if (esp_wifi_start() != ESP_OK) return false;
  started_ = true;
  return true;
}

void EspIdfWifiConnectionAdapter::stop() {
  cancelConnect();
  if (started_) (void)esp_wifi_stop();
  started_ = false;
}

void EspIdfWifiConnectionAdapter::shutdown() {
  stop();
  if (handlersRegistered_) {
    (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                wifiEventHandlerInstance_);
    (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                ipEventHandlerInstance_);
  }
  wifiEventHandlerInstance_ = nullptr;
  ipEventHandlerInstance_ = nullptr;
  handlersRegistered_ = false;
  runtime_ = nullptr;
  initialized_ = false;
  clearSensitiveDriverInput();
}

bool EspIdfWifiConnectionAdapter::beginConnect(std::string_view ssid, std::string_view password) {
  if (!started_ || ssid.empty() || password.empty() ||
      ssid.size() > BleWifiProvisioningRequest::kMaxSsidBytes ||
      password.size() > BleWifiProvisioningRequest::kMaxPasswordBytes)
    return false;

  wifi_config_t configuration{};
  std::memcpy(configuration.sta.ssid, ssid.data(), ssid.size());
  std::memcpy(configuration.sta.password, password.data(), password.size());
  configuration.sta.failure_retry_cnt = 0;
  const esp_err_t configured = esp_wifi_set_config(WIFI_IF_STA, &configuration);
  secure_clear_bytes(&configuration, sizeof(configuration));
  if (configured != ESP_OK) return false;
  if (esp_wifi_connect() != ESP_OK) return false;
  connectionInProgress_ = true;
  return true;
}

void EspIdfWifiConnectionAdapter::cancelConnect() {
  if (!connectionInProgress_) return;
  (void)acceptable_disconnect_result(esp_wifi_disconnect());
  connectionInProgress_ = false;
  clearSensitiveDriverInput();
}

void EspIdfWifiConnectionAdapter::disconnect() { cancelConnect(); }

void EspIdfWifiConnectionAdapter::clearSensitiveDriverInput() {
  // Credentials exist only in beginConnect's stack-local wifi_config_t.
}

void EspIdfWifiConnectionAdapter::wifiEventHandler(void* context, esp_event_base_t,
                                                    std::int32_t eventId, void* eventData) {
  auto* adapter = static_cast<EspIdfWifiConnectionAdapter*>(context);
  if (adapter != nullptr) adapter->onWifiEvent(eventId, eventData);
}

void EspIdfWifiConnectionAdapter::ipEventHandler(void* context, esp_event_base_t,
                                                  std::int32_t eventId, void*) {
  auto* adapter = static_cast<EspIdfWifiConnectionAdapter*>(context);
  if (adapter != nullptr) adapter->onIpEvent(eventId);
}

void EspIdfWifiConnectionAdapter::onWifiEvent(std::int32_t eventId, void* eventData) {
  if (runtime_ == nullptr) return;
  const auto nowTick = static_cast<std::uint64_t>(xTaskGetTickCount());
  if (eventId == WIFI_EVENT_STA_START) {
    (void)runtime_->onWifiEvent(WifiRuntimeWifiEvent::kStationStarted,
                                WifiDisconnectClassification::kTransientFailure, nowTick);
  } else if (eventId == WIFI_EVENT_STA_CONNECTED) {
    (void)runtime_->onWifiEvent(WifiRuntimeWifiEvent::kStationAssociated,
                                WifiDisconnectClassification::kTransientFailure, nowTick);
  } else if (eventId == WIFI_EVENT_STA_DISCONNECTED) {
    connectionInProgress_ = false;
    const auto* disconnected = static_cast<const wifi_event_sta_disconnected_t*>(eventData);
    const auto reason = disconnected == nullptr ? WifiDisconnectClassification::kTransientFailure
                                                : classify_esp_idf_disconnect_reason(disconnected->reason);
    (void)runtime_->onWifiEvent(WifiRuntimeWifiEvent::kStationDisconnected, reason, nowTick);
  }
}

void EspIdfWifiConnectionAdapter::onIpEvent(std::int32_t eventId) {
  if (runtime_ == nullptr || eventId != IP_EVENT_STA_GOT_IP) return;
  connectionInProgress_ = false;
  (void)runtime_->onIpEvent(WifiRuntimeIpEvent::kGotIp,
                            static_cast<std::uint64_t>(xTaskGetTickCount()));
}

}  // namespace algaguard

#endif
