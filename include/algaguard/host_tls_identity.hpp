#pragma once

#include <optional>
#include <string>

#include "algaguard/host_crypto_record_adapter.hpp"

namespace algaguard::host_test {
enum class HostTlsBuildReason { OK, RECORD_INVALID, IDENTITY_MISMATCH, DS_IDENTITY_FORBIDDEN, CRYPTO_INVALID };
class HostTlsIdentity {
 public:
  HostTlsIdentity() = default; HostTlsIdentity(const HostTlsIdentity&) = delete; HostTlsIdentity& operator=(const HostTlsIdentity&) = delete;
  HostTlsIdentity(HostTlsIdentity&&) = default; HostTlsIdentity& operator=(HostTlsIdentity&&) = default;
  ~HostTlsIdentity() { clear(); }
  const std::string& securityProfile() const { return profile_; } const std::string& warningCode() const { return warning_; }
  const std::string& deviceId() const { return device_id_; } const std::string& deviceUuid() const { return device_uuid_; }
  const std::string& certificateFingerprint() const { return fingerprint_; } const std::string& certificateSerial() const { return serial_; }
  bool dsIdentityPresent() const { return false; } std::size_t privateKeySizeForTest() const { return key_.size(); }
  void clear() { for (auto& b : key_) b = 0; key_.clear(); ca_.clear(); cert_.clear(); }
 private:
  friend std::optional<HostTlsIdentity> buildHostTlsIdentity(const HostCredentialRecord&, const std::string&, const std::string&, bool, HostTlsBuildReason*);
  std::string profile_{"DEV_SOFTWARE_KEY"}, warning_{"SOFTWARE_PRIVATE_KEY_IN_USE"}, device_id_, device_uuid_, fingerprint_, serial_;
  std::vector<std::uint8_t> ca_, cert_, key_;
};
inline std::optional<HostTlsIdentity> buildHostTlsIdentity(const HostCredentialRecord& credential, const std::string& id, const std::string& uuid, bool ds_requested=false, HostTlsBuildReason* reason=nullptr) {
  if (ds_requested) { if(reason)*reason=HostTlsBuildReason::DS_IDENTITY_FORBIDDEN; return std::nullopt; }
  if (!credential.record.committed) { if(reason)*reason=HostTlsBuildReason::RECORD_INVALID; return std::nullopt; }
  if (credential.record.device_id != id || credential.record.device_uuid != uuid) { if(reason)*reason=HostTlsBuildReason::IDENTITY_MISMATCH; return std::nullopt; }
  if (validateHostCredentialRecord(credential.record) != HostCredentialValidation::VALID) { if(reason)*reason=HostTlsBuildReason::CRYPTO_INVALID; return std::nullopt; }
  HostTlsIdentity out; out.device_id_=id; out.device_uuid_=uuid; out.fingerprint_=hostCertificateFingerprint(credential.record); out.serial_=credential.certificate_serial;
  out.ca_=credential.record.ca_chain_blob; out.cert_=credential.record.certificate_blob; out.key_=credential.record.key_blob; if(reason)*reason=HostTlsBuildReason::OK; return out;
}
}
