#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "algaguard/credentials.hpp"
#include "algaguard/domain.hpp"

namespace algaguard {

inline constexpr std::string_view kStorageNamespace = "algaguard-v1";
inline constexpr std::uint16_t kStorageSchemaVersion = 1;

struct TelemetryAckMetadata {
  std::uint64_t first_sequence{};
  std::uint64_t last_sequence{};
  std::uint16_t sample_count{};
};

struct StorageRecord {
  std::uint16_t schema_version{kStorageSchemaVersion};
  ProvisioningState provisioning_state{ProvisioningState::kUnprovisioned};
  std::string device_uuid;
  std::string certificate_pem;
  std::vector<std::string> ca_chain_pem;
  std::optional<PrivateKeyHandle> private_key_handle;
  std::string active_firmware_version;
  bool ota_pending_validation{};
  std::optional<TelemetryAckMetadata> unacknowledged_telemetry;
};

enum class StorageStatus : std::uint8_t {
  kOk,
  kEmpty,
  kCorrupt,
  kPartial,
  kUnavailable,
};

struct StorageLoadResult {
  StorageStatus status{StorageStatus::kEmpty};
  std::optional<StorageRecord> record;
};

inline StorageStatus validate_storage_record(const StorageRecord& record) {
  if (record.schema_version != kStorageSchemaVersion) return StorageStatus::kCorrupt;
  if (record.certificate_pem.find("PRIVATE KEY") != std::string::npos)
    return StorageStatus::kCorrupt;
  for (const auto& certificate : record.ca_chain_pem)
    if (certificate.find("PRIVATE KEY") != std::string::npos)
      return StorageStatus::kCorrupt;
  if (record.provisioning_state == ProvisioningState::kProvisioned &&
      (record.device_uuid.empty() || record.certificate_pem.empty() ||
       record.ca_chain_pem.empty() || !record.private_key_handle.has_value()))
    return StorageStatus::kPartial;
  if (record.ota_pending_validation && record.active_firmware_version.empty())
    return StorageStatus::kPartial;
  if (record.unacknowledged_telemetry.has_value()) {
    const auto& metadata = *record.unacknowledged_telemetry;
    if (metadata.sample_count == 0 || metadata.sample_count > 120 ||
        metadata.last_sequence < metadata.first_sequence)
      return StorageStatus::kCorrupt;
  }
  return StorageStatus::kOk;
}

class FirmwareStorage {
 public:
  virtual ~FirmwareStorage() = default;
  virtual StorageStatus initialize() = 0;
  virtual StorageLoadResult load() = 0;
  virtual StorageStatus save(const StorageRecord& record) = 0;
  virtual StorageStatus confirmed_factory_reset() = 0;
};

class FakeFirmwareStorage final : public FirmwareStorage {
 public:
  StorageStatus initialize() override { return available_ ? StorageStatus::kOk : StorageStatus::kUnavailable; }
  StorageLoadResult load() override {
    if (!available_) return {StorageStatus::kUnavailable, std::nullopt};
    if (!record_.has_value()) return {StorageStatus::kEmpty, std::nullopt};
    const auto status = validate_storage_record(*record_);
    return {status, status == StorageStatus::kOk ? record_ : std::nullopt};
  }
  StorageStatus save(const StorageRecord& record) override {
    if (!available_) return StorageStatus::kUnavailable;
    const auto status = validate_storage_record(record);
    if (status == StorageStatus::kOk) record_ = record;
    return status;
  }
  StorageStatus confirmed_factory_reset() override {
    if (!available_) return StorageStatus::kUnavailable;
    record_.reset();
    return StorageStatus::kOk;
  }
  void set_available(bool available) { available_ = available; }
  void inject(StorageRecord record) { record_ = std::move(record); }

 private:
  bool available_{true};
  std::optional<StorageRecord> record_;
};

class ResetConfirmation {
 public:
  void request() { requested_ = true; }
  bool confirm(bool explicit_confirmation) {
    if (!requested_ || !explicit_confirmation) return false;
    confirmed_ = true;
    return true;
  }
  bool confirmed() const { return confirmed_; }
  void cancel() {
    requested_ = false;
    confirmed_ = false;
  }

 private:
  bool requested_{};
  bool confirmed_{};
};

}  // namespace algaguard
