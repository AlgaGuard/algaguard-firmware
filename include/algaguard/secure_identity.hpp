#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "algaguard/credentials.hpp"

namespace algaguard {

enum class SecurityProfile : std::uint8_t { kHostTest, kSecurityCi, kDevSoftwareKey, kDeviceProduction };
enum class SecurityStatus : std::uint8_t { kReady, kNotProvisioned, kWrongEfusePurpose, kCorrupt, kUnavailable, kExpired };
enum class EfusePurpose : std::uint8_t { kUnused, kDsHmacDownstream, kNvsHmacUpstream, kFlashEncryption, kSecureBootDigest, kReserved };

inline constexpr std::string_view security_profile_name(SecurityProfile profile) {
  switch (profile) {
    case SecurityProfile::kHostTest: return "HOST_TEST";
    case SecurityProfile::kSecurityCi: return "SECURITY_CI";
    case SecurityProfile::kDevSoftwareKey: return "DEV_SOFTWARE_KEY";
    case SecurityProfile::kDeviceProduction: return "DEVICE_PRODUCTION";
  }
  return "UNKNOWN";
}

struct EfuseInventory {
  std::array<EfusePurpose, 6> key_blocks{};
  bool secure_boot_enabled{};
  bool flash_encryption_enabled{};
  bool download_disabled{};
  bool jtag_disabled{};
};

inline bool valid_production_efuse_plan(const EfuseInventory& inventory) {
  return inventory.key_blocks[0] == EfusePurpose::kDsHmacDownstream &&
         inventory.key_blocks[1] == EfusePurpose::kNvsHmacUpstream &&
         inventory.key_blocks[2] == EfusePurpose::kFlashEncryption &&
         inventory.key_blocks[3] == EfusePurpose::kSecureBootDigest;
}

inline SecurityStatus security_status_for(const EfuseInventory& inventory, SecurityProfile profile) {
  if (profile == SecurityProfile::kHostTest || profile == SecurityProfile::kSecurityCi ||
      profile == SecurityProfile::kDevSoftwareKey) return SecurityStatus::kReady;
  if (inventory.key_blocks[0] == EfusePurpose::kUnused || inventory.key_blocks[1] == EfusePurpose::kUnused)
    return SecurityStatus::kNotProvisioned;
  return valid_production_efuse_plan(inventory) ? SecurityStatus::kReady : SecurityStatus::kWrongEfusePurpose;
}

struct OpaqueDsContext {
  std::uint32_t storage_generation{};
  std::uint16_t rsa_bits{};
  std::uint8_t hmac_key_block{};
  bool sealed{};
  bool usable() const { return sealed && rsa_bits == 3072 && hmac_key_block == 0; }
};

struct CredentialMetadata {
  std::string device_uuid;
  std::string credential_id;
  std::string certificate_fingerprint_sha256;
  std::uint64_t expires_at_epoch{};
  std::uint32_t version{};
};

struct SecureCredentialRecord {
  CredentialMetadata metadata;
  std::string certificate_pem;
  std::vector<std::string> ca_chain_pem;
  OpaqueDsContext signing_context;
};

// Ownership of the buffers remains with this object while ESP-TLS uses cfg.
// DEV_SOFTWARE_KEY intentionally uses clientkey_buf and always leaves ds_data null.
struct SoftwareTlsIdentity {
  std::vector<unsigned char> ca_certificate;
  std::vector<unsigned char> client_certificate;
  std::vector<unsigned char> client_private_key;
  SecurityStatus status{SecurityStatus::kNotProvisioned};
  bool software_private_key_in_use{};
  bool ds_data_absent{true};
};

inline bool valid_secure_credential_record(const SecureCredentialRecord& record) {
  return !record.metadata.device_uuid.empty() && !record.metadata.credential_id.empty() &&
         record.metadata.version > 0 && !record.certificate_pem.empty() && !record.ca_chain_pem.empty() &&
         record.signing_context.usable() && record.certificate_pem.find("PRIVATE KEY") == std::string::npos &&
         std::all_of(record.ca_chain_pem.begin(), record.ca_chain_pem.end(), [](const std::string& pem) {
           return pem.find("PRIVATE KEY") == std::string::npos;
         });
}

// Two-record journal: public certificate data plus opaque DS state only.
class CredentialJournal {
 public:
  bool stage(SecureCredentialRecord record) {
    if (!valid_secure_credential_record(record)) return false;
    staged_ = std::move(record); return true;
  }
  bool commit() {
    if (!staged_) return false;
    active_ = std::move(staged_); staged_.reset(); ++generation_; return true;
  }
  void discard_staged() { staged_.reset(); }
  const std::optional<SecureCredentialRecord>& active() const { return active_; }
  std::uint32_t generation() const { return generation_; }
 private:
  std::optional<SecureCredentialRecord> staged_;
  std::optional<SecureCredentialRecord> active_;
  std::uint32_t generation_{};
};

// DEVELOPMENT ONLY. This provider deliberately persists a software RSA key in
// ordinary NVS. It is extractable with physical flash access and must never be
// enabled for production. There is intentionally no public key-export method.
class EspDevelopmentSoftwareKeyProvider final : public LocalPrivateKeyProvider {
 public:
  explicit EspDevelopmentSoftwareKeyProvider(SecurityProfile profile);
  ~EspDevelopmentSoftwareKeyProvider() override;
  std::optional<PrivateKeyHandle> generate(KeyAlgorithm algorithm) override;
  std::optional<CsrSubmission> create_csr(const PrivateKeyHandle& key, const DeviceBinding& binding) override;
  bool destroy(const PrivateKeyHandle& key) override;
  SecurityStatus status() const;
  bool software_key_in_use() const;
 private:
  struct Impl;
  SecurityProfile profile_;
  std::unique_ptr<Impl> impl_;
};

class EspDevelopmentCredentialStorage final : public SecureCredentialStorage {
 public:
  explicit EspDevelopmentCredentialStorage(SecurityProfile profile);
  ~EspDevelopmentCredentialStorage() override;
  bool stage(const PrivateKeyHandle& key, const PublicCredentialBundle& bundle) override;
  bool activate_staged() override;
  void discard_staged() override;
  std::optional<PublicCredentialBundle> active_public_bundle() const override;
  SecurityStatus status() const;
  // Reloads the active ordinary-NVS key into the process-local TLS handle.
  // This is intentionally development-only and does not make the key secure.
  bool reload_active_identity();
  std::optional<SoftwareTlsIdentity> software_tls_identity();
  // Confirmation is supplied by the UI flow; this does not claim forensic erase.
  bool confirmed_reset(bool confirmed);
 private:
  struct Impl;
  SecurityProfile profile_;
  std::unique_ptr<Impl> impl_;
};

enum class DevelopmentIdentityDisplayState : std::uint8_t {
  kUnprovisioned, kGenerating, kCsrReady, kCertificatePending, kActive, kExpired, kCorrupt,
};

inline constexpr std::string_view development_identity_display_text(DevelopmentIdentityDisplayState state) {
  switch (state) {
    case DevelopmentIdentityDisplayState::kUnprovisioned: return "UNPROVISIONED";
    case DevelopmentIdentityDisplayState::kGenerating: return "DEV KEY";
    case DevelopmentIdentityDisplayState::kCsrReady: return "DEV CSR READY";
    case DevelopmentIdentityDisplayState::kCertificatePending: return "DEV CERT PENDING";
    case DevelopmentIdentityDisplayState::kActive: return "INSECURE DEV KEY";
    case DevelopmentIdentityDisplayState::kExpired: return "DEV IDENTITY EXPIRED";
    case DevelopmentIdentityDisplayState::kCorrupt: return "DEV IDENTITY CORRUPT";
  }
  return "INSECURE DEV";
}

}  // namespace algaguard
