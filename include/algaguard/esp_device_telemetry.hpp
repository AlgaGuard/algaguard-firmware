#pragma once

#if defined(ESP_PLATFORM) && defined(ALGAGUARD_ENABLE_DEVICE_MQTT_TELEMETRY)

#include <cstdint>
#include <memory>
#include <string>

#include "algaguard/credentials.hpp"
#include "algaguard/device_telemetry.hpp"
#include "algaguard/secure_identity.hpp"

namespace algaguard {

class EspDeviceTelemetryRuntime {
 public:
  EspDeviceTelemetryRuntime();
  ~EspDeviceTelemetryRuntime();
  EspDeviceTelemetryRuntime(const EspDeviceTelemetryRuntime&) = delete;
  EspDeviceTelemetryRuntime& operator=(const EspDeviceTelemetryRuntime&) = delete;

  bool start(std::string deviceId, BrokerEndpoint endpoint,
             SoftwareTlsIdentity identity);
  void poll(const LocalDemoReading& reading, std::uint64_t uptimeMs);
  bool started() const;
  bool connected() const;
  bool profileInstalled() const;
  bool physicalUnpairPending() const;
  bool confirmPhysicalUnpair(bool localStateCleared);
  bool cancelPhysicalUnpair();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace algaguard

#endif
