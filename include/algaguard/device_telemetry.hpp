#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "algaguard/local_demo.hpp"

namespace algaguard {

inline constexpr std::string_view kDeviceTelemetrySchema =
    "urn:algaguard:schema:mqtt:telemetry-batch:v1";
inline constexpr std::string_view kScenarioLocalDemo = "device-local-demo";
inline constexpr std::string_view kScenarioRealSensors = "device-real-sensors";

struct ActiveProfileReference {
  std::string profileId;
  std::string profileVersion;
};

inline bool valid_uuid(std::string_view value) {
  if (value.size() != 36) return false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (value[index] != '-') return false;
    } else if (!std::isxdigit(static_cast<unsigned char>(value[index]))) {
      return false;
    }
  }
  return true;
}

inline bool valid_semver(std::string_view value) {
  if (value.empty() || value.size() > 64) return false;
  unsigned dots = 0;
  bool digitInPart = false;
  for (const unsigned char character : value) {
    if (character == '.') {
      if (!digitInPart || dots >= 2) return false;
      ++dots;
      digitInPart = false;
    } else if (std::isdigit(character)) {
      digitInPart = true;
    } else {
      return false;
    }
  }
  return dots == 2 && digitInPart;
}

inline bool valid_profile_reference(const ActiveProfileReference& value) {
  return valid_uuid(value.profileId) && valid_semver(value.profileVersion);
}

inline bool valid_telemetry_device_id(std::string_view value) {
  if (value.size() != 9 || value.substr(0, 3) != "AG-") return false;
  for (std::size_t index = 3; index < value.size(); ++index)
    if (!std::isdigit(static_cast<unsigned char>(value[index]))) return false;
  return true;
}

inline bool valid_utc_timestamp(std::string_view value) {
  return value.size() == 20 && value[4] == '-' && value[7] == '-' &&
         value[10] == 'T' && value[13] == ':' && value[16] == ':' &&
         value[19] == 'Z';
}

inline std::optional<std::string> build_device_telemetry_payload(
    std::string_view deviceId, const std::optional<ActiveProfileReference>& profile,
    const LocalDemoReading& reading, std::string_view sentAt,
    std::string_view observedAt, std::string_view messageId,
    std::string_view batchId, std::uint64_t uptimeMs, bool isReplay,
    bool createdFromSd, std::string_view qualityFlag,
    std::string_view scenario) {
  if (!valid_telemetry_device_id(deviceId) ||
      (profile && !valid_profile_reference(*profile)) ||
      !valid_utc_timestamp(sentAt) || !valid_utc_timestamp(observedAt) ||
      !valid_uuid(messageId) || !valid_uuid(batchId) || reading.sequence == 0 ||
      reading.ph < 0 || reading.ph > 14 || reading.lightLux < 0 ||
      reading.nutrientPercent < 0 || reading.nutrientPercent > 100 ||
      qualityFlag.empty() || scenario.empty())
    return std::nullopt;

  // activeProfile is optional on the wire: a freshly-paired device with no
  // profile installed yet still has real sensor readings to publish. Profile
  // assignment's only purpose is routing threshold notifications, not
  // gating whether telemetry reaches the platform.
  char activeProfileFragment[160]{};
  if (profile) {
    const int fragmentWritten = std::snprintf(
        activeProfileFragment, sizeof(activeProfileFragment),
        "\"activeProfile\":{\"profileId\":\"%s\",\"profileVersion\":\"%s\"},",
        profile->profileId.c_str(), profile->profileVersion.c_str());
    if (fragmentWritten <= 0 ||
        static_cast<std::size_t>(fragmentWritten) >= sizeof(activeProfileFragment))
      return std::nullopt;
  }

  char output[2048]{};
  const int written = std::snprintf(
      output, sizeof(output),
      "{\"schema\":\"%.*s\",\"schemaVersion\":\"1.0.0\","
      "\"messageId\":\"%.*s\",\"deviceId\":\"%.*s\","
      "\"sentAt\":\"%.*s\",\"payload\":{\"batchId\":\"%.*s\","
      "\"firstSequence\":\"%llu\",\"lastSequence\":\"%llu\","
      "\"sampleCount\":1,%s\"samples\":[{\"sequence\":\"%llu\","
      "\"observedAt\":\"%.*s\",\"timestampQuality\":\"NTP_SYNCED\","
      "\"uptimeMs\":\"%llu\",\"values\":{\"temperatureC\":%.2f,"
      "\"ph\":%.2f,\"lightLux\":%.0f,\"nutrientPercent\":%.1f},"
      "\"qualityFlags\":[\"%.*s\"],"
      "\"simulationScenario\":\"%.*s\"}],\"isReplay\":%s,"
      "\"createdFromSd\":%s}}",
      static_cast<int>(kDeviceTelemetrySchema.size()), kDeviceTelemetrySchema.data(),
      static_cast<int>(messageId.size()), messageId.data(),
      static_cast<int>(deviceId.size()), deviceId.data(),
      static_cast<int>(sentAt.size()), sentAt.data(),
      static_cast<int>(batchId.size()), batchId.data(),
      static_cast<unsigned long long>(reading.sequence),
      static_cast<unsigned long long>(reading.sequence), activeProfileFragment,
      static_cast<unsigned long long>(reading.sequence),
      static_cast<int>(observedAt.size()), observedAt.data(),
      static_cast<unsigned long long>(uptimeMs), reading.temperatureC, reading.ph,
      reading.lightLux, reading.nutrientPercent,
      static_cast<int>(qualityFlag.size()), qualityFlag.data(),
      static_cast<int>(scenario.size()), scenario.data(),
      isReplay ? "true" : "false", createdFromSd ? "true" : "false");
  if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(output))
    return std::nullopt;
  return std::string{output, static_cast<std::size_t>(written)};
}

class DeviceTelemetryPublishWindow {
 public:
  bool installProfile(ActiveProfileReference value) {
    if (!valid_profile_reference(value)) return false;
    profile_ = std::move(value);
    return true;
  }
  const std::optional<ActiveProfileReference>& profile() const { return profile_; }
  bool begin(std::string batchId, std::string payload, std::uint64_t nowMs) {
    if (pending_ || !valid_uuid(batchId) || payload.empty() ||
        payload.size() > 8192)
      return false;
    pending_ = Pending{std::move(batchId), std::move(payload), nowMs, 0};
    return true;
  }
  bool acknowledge(std::string_view batchId, std::string_view status) {
    if (!pending_ || pending_->batchId != batchId) return false;
    if (status != "ACCEPTED" && status != "PARTIALLY_ACCEPTED" &&
        status != "DUPLICATE" && status != "REJECTED")
      return false;
    pending_.reset();
    return true;
  }
  bool retryDue(std::uint64_t nowMs, std::uint64_t timeoutMs = 10000) const {
    return pending_ && nowMs >= pending_->lastAttemptMs + timeoutMs &&
           pending_->retries < 3;
  }
  bool retry(std::uint64_t nowMs) {
    if (!retryDue(nowMs)) return false;
    pending_->lastAttemptMs = nowMs;
    ++pending_->retries;
    return true;
  }
  bool exhausted(std::uint64_t nowMs, std::uint64_t timeoutMs = 10000) const {
    return pending_ && pending_->retries >= 3 &&
           nowMs >= pending_->lastAttemptMs + timeoutMs;
  }
  void clearPending() { pending_.reset(); }
  bool pending() const { return pending_.has_value(); }
  std::string_view pendingPayload() const {
    return pending_ ? std::string_view{pending_->payload} : std::string_view{};
  }
  std::string_view pendingBatchId() const {
    return pending_ ? std::string_view{pending_->batchId} : std::string_view{};
  }
 private:
  struct Pending {
    std::string batchId;
    std::string payload;
    std::uint64_t lastAttemptMs{};
    std::uint8_t retries{};
  };
  std::optional<ActiveProfileReference> profile_;
  std::optional<Pending> pending_;
};

}  // namespace algaguard
