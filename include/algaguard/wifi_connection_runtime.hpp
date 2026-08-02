#pragma once

#include <cstdint>
#include <utility>

#include "algaguard/wifi_connection_state_machine.hpp"

namespace algaguard {

enum class WifiDisconnectClassification : std::uint8_t {
  kAuthenticationFailed,
  kNetworkNotFound,
  kTransientFailure,
  kLocalDisconnect,
  kUnknown,
};

enum class WifiRuntimeWifiEvent : std::uint8_t {
  kStationStarted,
  kStationAssociated,
  kStationDisconnected,
};

enum class WifiRuntimeIpEvent : std::uint8_t {
  kGotIp,
  kLostIp,
};

inline WifiDriverEvent wifi_driver_event_for_disconnect(
    WifiDisconnectClassification classification) {
  switch (classification) {
    case WifiDisconnectClassification::kAuthenticationFailed:
      return WifiDriverEvent::kAuthenticationFailed;
    case WifiDisconnectClassification::kNetworkNotFound:
      return WifiDriverEvent::kNetworkNotFound;
    case WifiDisconnectClassification::kTransientFailure:
      return WifiDriverEvent::kTransientFailure;
    case WifiDisconnectClassification::kLocalDisconnect:
      return WifiDriverEvent::kDisconnected;
    case WifiDisconnectClassification::kUnknown:
      return WifiDriverEvent::kTransientFailure;
  }
  return WifiDriverEvent::kTransientFailure;
}

class WifiConnectionRuntime {
 public:
  explicit WifiConnectionRuntime(WifiConnectionAdapter& adapter) : manager_(adapter) {}
  WifiConnectionRuntime(const WifiConnectionRuntime&) = delete;
  WifiConnectionRuntime& operator=(const WifiConnectionRuntime&) = delete;

  WifiConnectionResult installAcceptedCredentials(BleWifiCredentialHandoff&& handoff) {
    return manager_.acceptCredentials(std::move(handoff));
  }

  WifiConnectionResult startConnection(std::uint64_t nowTick) { return manager_.start(nowTick); }
  WifiConnectionResult restoreSavedNetworkStarted(std::uint64_t nowTick) {
    return manager_.restoreSavedNetworkStarted(nowTick);
  }
  WifiConnectionResult poll(std::uint64_t nowTick) { return manager_.onTick(nowTick); }

  WifiConnectionResult onWifiEvent(WifiRuntimeWifiEvent event,
                                   WifiDisconnectClassification classification,
                                   std::uint64_t nowTick) {
    if (event == WifiRuntimeWifiEvent::kStationDisconnected)
      return manager_.onDriverEvent(wifi_driver_event_for_disconnect(classification), nowTick);
    return manager_.status();
  }

  WifiConnectionResult onIpEvent(WifiRuntimeIpEvent event, std::uint64_t nowTick) {
    if (event == WifiRuntimeIpEvent::kGotIp)
      return manager_.onDriverEvent(WifiDriverEvent::kConnected, nowTick);
    return manager_.onDriverEvent(WifiDriverEvent::kDisconnected, nowTick);
  }

  WifiConnectionResult cancel() { return manager_.cancel(); }
  WifiConnectionResult reset() noexcept { return manager_.reset(); }
  void shutdown() noexcept { manager_.shutdown(); }
  WifiConnectionState state() const { return manager_.state(); }
  bool credentialsPresent() const { return manager_.credentialsPresent(); }
  bool secretsCleared() const { return manager_.secretsCleared(); }

 private:
  WifiConnectionStateMachine manager_;
};

}  // namespace algaguard
