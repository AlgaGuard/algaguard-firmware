#pragma once

#if defined(ESP_PLATFORM)

#include <cstdint>
#include <string_view>

#include "esp_event.h"
#include "esp_netif.h"

#include "algaguard/wifi_connection_runtime.hpp"

namespace algaguard {

class EspIdfWifiConnectionAdapter final : public WifiConnectionAdapter {
 public:
  EspIdfWifiConnectionAdapter() = default;
  EspIdfWifiConnectionAdapter(const EspIdfWifiConnectionAdapter&) = delete;
  EspIdfWifiConnectionAdapter& operator=(const EspIdfWifiConnectionAdapter&) = delete;

  bool init();
  bool start();
  void stop();
  void shutdown();
  void setRuntime(WifiConnectionRuntime* runtime) { runtime_ = runtime; }

  bool beginConnect(std::string_view ssid, std::string_view password) override;
  void cancelConnect() override;
  void disconnect() override;
  bool isConnectInProgress() const override { return connectionInProgress_; }
  void clearSensitiveDriverInput() override;

 private:
  static void wifiEventHandler(void* context, esp_event_base_t eventBase,
                               std::int32_t eventId, void* eventData);
  static void ipEventHandler(void* context, esp_event_base_t eventBase,
                             std::int32_t eventId, void* eventData);
  void onWifiEvent(std::int32_t eventId, void* eventData);
  void onIpEvent(std::int32_t eventId);

  esp_netif_t* stationNetif_{};
  esp_event_handler_instance_t wifiEventHandlerInstance_{};
  esp_event_handler_instance_t ipEventHandlerInstance_{};
  WifiConnectionRuntime* runtime_{};
  bool initialized_{};
  bool started_{};
  bool handlersRegistered_{};
  bool connectionInProgress_{};
};

}  // namespace algaguard

#endif
