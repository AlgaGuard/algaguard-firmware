#pragma once

#if defined(ESP_PLATFORM) && defined(ALGAGUARD_ENABLE_DEVICE_MQTT_TELEMETRY)

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "algaguard/credentials.hpp"
#include "algaguard/device_telemetry.hpp"
#include "algaguard/secure_identity.hpp"

namespace algaguard {

enum class SampleOrigin : std::uint8_t { kLive, kReplayed };

class EspDeviceTelemetryRuntime {
 public:
  EspDeviceTelemetryRuntime();
  ~EspDeviceTelemetryRuntime();
  EspDeviceTelemetryRuntime(const EspDeviceTelemetryRuntime&) = delete;
  EspDeviceTelemetryRuntime& operator=(const EspDeviceTelemetryRuntime&) = delete;

  bool start(std::string deviceId, BrokerEndpoint endpoint,
             SoftwareTlsIdentity identity);
  // originalObservedAtUtc is required (and used as-is) when origin is
  // kReplayed -- a replayed sample must keep its original capture time,
  // distinct from "now" (when this call is actually transmitting it).
  // qualityFlag: "REAL" / "DEGRADED" / "SIMULATED", caller's judgment call
  // based on whether every sensor read succeeded this tick. The wire
  // "simulationScenario" value is decided internally from which sensor
  // source this build was compiled with, not passed by the caller.
  void poll(const LocalDemoReading& reading, std::uint64_t uptimeMs,
            SampleOrigin origin = SampleOrigin::kLive,
            std::string_view qualityFlag = "SIMULATED",
            std::optional<std::string> originalObservedAtUtc = std::nullopt);
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
