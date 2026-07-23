#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef ALGAGUARD_MQTT_MAX_BATCH_SAMPLES
#define ALGAGUARD_MQTT_MAX_BATCH_SAMPLES 120
#endif
#ifndef ALGAGUARD_QUEUE_MAX_SAMPLES
#define ALGAGUARD_QUEUE_MAX_SAMPLES 1440
#endif
#ifndef ALGAGUARD_REPLAY_MAX_BATCHES
#define ALGAGUARD_REPLAY_MAX_BATCHES 4
#endif
#ifndef ALGAGUARD_REPLAY_MAX_SAMPLES
#define ALGAGUARD_REPLAY_MAX_SAMPLES 480
#endif
#ifndef ALGAGUARD_MQTT_DUPLICATE_MAX_RETRIES
#define ALGAGUARD_MQTT_DUPLICATE_MAX_RETRIES 3
#endif
#ifndef ALGAGUARD_OTA_MAX_ARTIFACT_BYTES
#define ALGAGUARD_OTA_MAX_ARTIFACT_BYTES 4194304
#endif
#ifndef ALGAGUARD_OTA_MAX_MANIFEST_BYTES
#define ALGAGUARD_OTA_MAX_MANIFEST_BYTES 65536
#endif
#ifndef ALGAGUARD_CREDENTIAL_MAX_CERT_BYTES
#define ALGAGUARD_CREDENTIAL_MAX_CERT_BYTES 4096
#endif
#ifndef ALGAGUARD_CREDENTIAL_MAX_CHAIN_BYTES
#define ALGAGUARD_CREDENTIAL_MAX_CHAIN_BYTES 12288
#endif
#ifndef ALGAGUARD_CREDENTIAL_MAX_CSR_BYTES
#define ALGAGUARD_CREDENTIAL_MAX_CSR_BYTES 4096
#endif
#ifndef ALGAGUARD_CREDENTIAL_MAX_UNSYNCED_HOLDOVER_SECONDS
#define ALGAGUARD_CREDENTIAL_MAX_UNSYNCED_HOLDOVER_SECONDS 86400
#endif

namespace algaguard {

struct CredentialLimits {
  std::size_t mqtt_max_batch_samples{ALGAGUARD_MQTT_MAX_BATCH_SAMPLES};
  std::size_t queue_max_samples{ALGAGUARD_QUEUE_MAX_SAMPLES};
  std::size_t replay_max_batches{ALGAGUARD_REPLAY_MAX_BATCHES};
  std::size_t replay_max_samples{ALGAGUARD_REPLAY_MAX_SAMPLES};
  std::size_t mqtt_duplicate_max_retries{ALGAGUARD_MQTT_DUPLICATE_MAX_RETRIES};
  std::size_t ota_max_artifact_bytes{ALGAGUARD_OTA_MAX_ARTIFACT_BYTES};
  std::size_t ota_max_manifest_bytes{ALGAGUARD_OTA_MAX_MANIFEST_BYTES};
  std::size_t certificate_max_bytes{ALGAGUARD_CREDENTIAL_MAX_CERT_BYTES};
  std::size_t certificate_chain_max_bytes{ALGAGUARD_CREDENTIAL_MAX_CHAIN_BYTES};
  std::size_t csr_max_bytes{ALGAGUARD_CREDENTIAL_MAX_CSR_BYTES};
  std::uint64_t unsynchronized_holdover_seconds{
      ALGAGUARD_CREDENTIAL_MAX_UNSYNCED_HOLDOVER_SECONDS};

  bool bounded() const {
    return mqtt_max_batch_samples > 0 && mqtt_max_batch_samples <= 120 && queue_max_samples > 0 &&
           replay_max_batches > 0 && replay_max_samples >= mqtt_max_batch_samples &&
           mqtt_duplicate_max_retries > 0 && mqtt_duplicate_max_retries <= 10 && ota_max_artifact_bytes > 0 &&
           ota_max_artifact_bytes <= 16U * 1024U * 1024U && ota_max_manifest_bytes > 0 &&
           certificate_max_bytes >= 1024 && certificate_chain_max_bytes >= certificate_max_bytes &&
           csr_max_bytes >= 1024 && unsynchronized_holdover_seconds > 0;
  }
};

struct DeviceBinding {
  std::string device_id;
  std::string device_uuid;
};

struct CertificateIdentity {
  std::string common_name;
  std::vector<std::string> san_uris;
};

inline bool exact_certificate_binding(const DeviceBinding& expected, const CertificateIdentity& certificate) {
  return certificate.common_name == expected.device_id && certificate.san_uris.size() == 1 &&
         certificate.san_uris.front() == "urn:algaguard:device:" + expected.device_uuid;
}

enum class KeyAlgorithm { kEcP256, kRsa3072 };
struct PrivateKeyHandle {
  std::uint32_t slot{};
  std::uint32_t generation{};
};
struct CsrSubmission {
  std::string pem;
  DeviceBinding binding;
  KeyAlgorithm algorithm{KeyAlgorithm::kEcP256};
};

// Implementations generate and sign inside encrypted NVS/flash-backed storage.
// Deliberately no method exports private-key bytes.
class LocalPrivateKeyProvider {
 public:
  virtual ~LocalPrivateKeyProvider() = default;
  virtual std::optional<PrivateKeyHandle> generate(KeyAlgorithm algorithm) = 0;
  virtual std::optional<CsrSubmission> create_csr(const PrivateKeyHandle& key, const DeviceBinding& binding) = 0;
  virtual bool destroy(const PrivateKeyHandle& key) = 0;
};

struct PublicCredentialBundle {
  std::string credential_id;
  std::string certificate_pem;
  std::vector<std::string> ca_chain_pem;
  CertificateIdentity identity;
  std::uint64_t not_before_epoch{};
  std::uint64_t not_after_epoch{};
  bool revoked{};
  bool compromised{};
};

class SecureCredentialStorage {
 public:
  virtual ~SecureCredentialStorage() = default;
  virtual bool stage(const PrivateKeyHandle& key, const PublicCredentialBundle& bundle) = 0;
  virtual bool activate_staged() = 0;
  virtual void discard_staged() = 0;
  virtual std::optional<PublicCredentialBundle> active_public_bundle() const = 0;
};

struct BrokerEndpoint {
  std::string host;
  std::uint16_t port{8883};
  std::string server_name;
  std::uint32_t keepalive_seconds{60};
  std::uint32_t session_expiry_seconds{3600};
};

class MqttTlsCredentialLoader {
 public:
  virtual ~MqttTlsCredentialLoader() = default;
  virtual bool load(const PrivateKeyHandle& key, const PublicCredentialBundle& public_bundle,
                    const BrokerEndpoint& endpoint) = 0;
};

class CredentialBootstrapClient {
 public:
  virtual ~CredentialBootstrapClient() = default;
  virtual std::optional<PublicCredentialBundle> submit_csr(std::string_view one_time_authorization,
                                                            const CsrSubmission& csr) = 0;
};

inline std::optional<std::vector<std::string>> parse_public_certificate_chain(std::string_view pem,
                                                                              const CredentialLimits& limits) {
  if (pem.empty() || pem.size() > limits.certificate_chain_max_bytes || pem.find("PRIVATE KEY") != std::string_view::npos)
    return std::nullopt;
  constexpr std::string_view begin = "-----BEGIN CERTIFICATE-----";
  constexpr std::string_view end = "-----END CERTIFICATE-----";
  std::vector<std::string> certificates;
  std::size_t cursor = 0;
  while (cursor < pem.size()) {
    const auto first = pem.find(begin, cursor);
    if (first == std::string_view::npos) break;
    const auto last = pem.find(end, first + begin.size());
    if (last == std::string_view::npos) return std::nullopt;
    const auto length = last + end.size() - first;
    if (length > limits.certificate_max_bytes || certificates.size() >= 3) return std::nullopt;
    certificates.emplace_back(pem.substr(first, length));
    cursor = last + end.size();
  }
  if (certificates.empty()) return std::nullopt;
  return certificates;
}

inline std::string redact_credential_log(std::string_view value) {
  if (value.find("PRIVATE KEY") != std::string_view::npos) return "[REDACTED_PRIVATE_MATERIAL]";
  if (value.find("BEGIN CERTIFICATE") != std::string_view::npos ||
      value.find("BEGIN CERTIFICATE REQUEST") != std::string_view::npos)
    return "[REDACTED_PEM_BODY]";
  if (value.size() > 128) return "[REDACTED_OVERSIZED_VALUE]";
  return std::string{value};
}

enum class CredentialState {
  kEmpty,
  kActive,
  kRotating,
  kAwaitingConnectionProof,
  kAwaitingAcknowledgement,
  kRecovery,
  kRevoked,
  kExpired,
  kCompromised,
};

class CredentialRotationStateMachine {
 public:
  explicit CredentialRotationStateMachine(std::string active_credential_id)
      : active_credential_id_(std::move(active_credential_id)), state_(CredentialState::kActive) {}

  CredentialState state() const { return state_; }
  const std::string& active_credential_id() const { return active_credential_id_; }
  const std::string& staged_credential_id() const { return staged_credential_id_; }
  bool old_revocation_required() const { return old_revocation_required_; }

  bool begin(std::uint64_t now_epoch, std::uint64_t overlap_seconds) {
    if (state_ != CredentialState::kActive || overlap_seconds == 0) return false;
    deadline_epoch_ = now_epoch + overlap_seconds;
    state_ = CredentialState::kRotating;
    return true;
  }

  bool stage(std::string credential_id) {
    if (state_ != CredentialState::kRotating || credential_id.empty() || credential_id == active_credential_id_)
      return false;
    staged_credential_id_ = std::move(credential_id);
    state_ = CredentialState::kAwaitingConnectionProof;
    return true;
  }

  bool prove_connection(std::string_view credential_id, std::uint64_t now_epoch) {
    if (state_ != CredentialState::kAwaitingConnectionProof || credential_id != staged_credential_id_ ||
        now_epoch > deadline_epoch_)
      return false;
    state_ = CredentialState::kAwaitingAcknowledgement;
    return true;
  }

  bool acknowledge(std::string_view credential_id) {
    if (state_ != CredentialState::kAwaitingAcknowledgement || credential_id != staged_credential_id_) return false;
    previous_credential_id_ = active_credential_id_;
    active_credential_id_ = staged_credential_id_;
    staged_credential_id_.clear();
    old_revocation_required_ = true;
    state_ = CredentialState::kActive;
    return true;
  }

  void fail_or_timeout() {
    if (state_ == CredentialState::kRotating || state_ == CredentialState::kAwaitingConnectionProof ||
        state_ == CredentialState::kAwaitingAcknowledgement) {
      staged_credential_id_.clear();
      state_ = CredentialState::kRecovery;
    }
  }

  bool recover_with_working_credential() {
    if (state_ != CredentialState::kRecovery || active_credential_id_.empty()) return false;
    state_ = CredentialState::kActive;
    return true;
  }

  void revoke(bool compromised) {
    active_credential_id_.clear();
    staged_credential_id_.clear();
    state_ = compromised ? CredentialState::kCompromised : CredentialState::kRevoked;
  }

 private:
  std::string active_credential_id_;
  std::string previous_credential_id_;
  std::string staged_credential_id_;
  CredentialState state_{CredentialState::kEmpty};
  std::uint64_t deadline_epoch_{};
  bool old_revocation_required_{false};
};

enum class TimeQuality { kSynchronized, kUnsynchronized };
enum class ValidityDecision { kAllow, kWaitForTime, kExpired, kRevoked };

inline ValidityDecision credential_validity(const PublicCredentialBundle& credential, TimeQuality quality,
                                             std::uint64_t now_epoch, std::uint64_t last_synchronized_epoch,
                                             const CredentialLimits& limits) {
  if (credential.revoked || credential.compromised) return ValidityDecision::kRevoked;
  if (quality == TimeQuality::kUnsynchronized) {
    if (last_synchronized_epoch == 0 || now_epoch < last_synchronized_epoch ||
        now_epoch - last_synchronized_epoch > limits.unsynchronized_holdover_seconds)
      return ValidityDecision::kWaitForTime;
  }
  if (now_epoch < credential.not_before_epoch) return ValidityDecision::kWaitForTime;
  if (now_epoch >= credential.not_after_epoch) return ValidityDecision::kExpired;
  return ValidityDecision::kAllow;
}

}  // namespace algaguard
