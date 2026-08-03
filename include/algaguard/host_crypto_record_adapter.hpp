#pragma once

// Host-test-only bridge between ephemeral OpenSSL fixtures and the two-slot
// record model. It deliberately has no target-firmware or TLS identity API.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "algaguard/host_crypto_fixture.hpp"
#include "algaguard/host_two_slot.hpp"

namespace algaguard::host_test {

enum class HostCredentialValidation {
  VALID,
  RECORD_INVALID,
  KEY_INVALID,
  CERTIFICATE_INVALID,
  CA_INVALID,
  KEY_MISMATCH,
  CHAIN_INVALID,
  IDENTITY_INVALID,
  USAGE_INVALID,
  VALIDITY_INVALID,
};

struct HostCredentialRecord {
  Record record;
  std::string certificate_fingerprint;
  std::string certificate_serial;
};

inline std::optional<HostCredentialRecord> adaptFixtureCredential(const EphemeralHostCryptoFixture& fixture,
                                                                    const std::string& device_id,
                                                                    const std::string& device_uuid) {
  const auto material = fixture.credentialMaterialForHostTest();
  if (!material || material->private_key.empty() || material->client_certificate.empty() || material->ca_certificate.empty() ||
      material->private_key.size() > kMaximumHostBlobSize || material->client_certificate.size() > kMaximumHostBlobSize ||
      material->ca_certificate.size() > kMaximumHostBlobSize) return std::nullopt;
  return HostCredentialRecord{{1, 1, false, device_id, device_uuid, "OK", material->private_key,
                               material->client_certificate, material->ca_certificate},
                              material->metadata.certificate_fingerprint, material->metadata.certificate_serial};
}

class HostCredentialValidationWorkspace {
 public:
  HostCredentialValidationWorkspace() {
    directory_ = std::filesystem::temp_directory_path() /
                 ("algaguard-host-record-validation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory_);
  }
  ~HostCredentialValidationWorkspace() { cleanup(); }
  bool write(const std::string& name, const std::vector<std::uint8_t>& bytes) const {
    std::ofstream output(directory_ / name, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return output.good();
  }
  std::filesystem::path file(const std::string& name) const { return directory_ / name; }
  bool run(const std::string& command) const { return std::system((command + " > NUL 2>&1").c_str()) == 0; }
  std::string inspect(const std::string& command) const {
    const auto output = file("inspect.txt");
    if (std::system((command + " > \"" + output.string() + "\" 2>&1").c_str()) != 0) return {};
    std::ifstream input(output, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }
  void cleanup() const {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }

 private:
  std::filesystem::path directory_;
};

inline std::string hostQuote(const std::filesystem::path& path) { return "\"" + path.string() + "\""; }
inline bool hostContains(const std::string& value, const std::string& needle) { return value.find(needle) != std::string::npos; }
inline std::string hostLineAfter(const std::string& text, const std::string& prefix) {
  const auto begin = text.find(prefix);
  if (begin == std::string::npos) return {};
  const auto end = text.find_first_of("\r\n", begin);
  return text.substr(begin + prefix.size(), end - begin - prefix.size());
}

inline std::string hostCertificateFingerprint(const Record& record) {
  HostCredentialValidationWorkspace workspace;
  if (!workspace.write("client.crt", record.certificate_blob)) return {};
  return hostLineAfter(workspace.inspect("openssl x509 -in " + hostQuote(workspace.file("client.crt")) + " -noout -fingerprint -sha256"),
                       "sha256 Fingerprint=");
}

inline HostCredentialValidation validateHostCredentialRecord(const Record& record) {
  auto normalized = record;
  normalized.committed = true;
  if (slotValidity(normalized) != SlotValidity::SLOT_VALID) return HostCredentialValidation::RECORD_INVALID;
  HostCredentialValidationWorkspace workspace;
  if (!workspace.write("device.key", record.key_blob)) return HostCredentialValidation::KEY_INVALID;
  if (!workspace.write("client.crt", record.certificate_blob)) return HostCredentialValidation::CERTIFICATE_INVALID;
  if (!workspace.write("ca.crt", record.ca_chain_blob)) return HostCredentialValidation::CA_INVALID;
  const auto key = hostQuote(workspace.file("device.key"));
  const auto certificate = hostQuote(workspace.file("client.crt"));
  const auto ca = hostQuote(workspace.file("ca.crt"));
  if (!workspace.run("openssl pkey -in " + key + " -noout")) return HostCredentialValidation::KEY_INVALID;
  if (!workspace.run("openssl x509 -in " + certificate + " -noout")) return HostCredentialValidation::CERTIFICATE_INVALID;
  if (!workspace.run("openssl x509 -in " + ca + " -noout")) return HostCredentialValidation::CA_INVALID;
  const auto key_modulus = workspace.inspect("openssl rsa -in " + key + " -noout -modulus");
  const auto cert_modulus = workspace.inspect("openssl x509 -in " + certificate + " -noout -modulus");
  if (key_modulus.empty() || key_modulus != cert_modulus) return HostCredentialValidation::KEY_MISMATCH;
  if (!workspace.run("openssl verify -CAfile " + ca + " " + certificate)) return HostCredentialValidation::CHAIN_INVALID;
  const auto identity = workspace.inspect("openssl x509 -in " + certificate + " -noout -subject -ext subjectAltName");
  const auto common_name = "CN = " + record.device_id;
  if (!(hostContains(identity, common_name) || hostContains(identity, "CN=" + record.device_id)) ||
      !hostContains(identity, "URI:urn:algaguard:device:" + record.device_uuid)) return HostCredentialValidation::IDENTITY_INVALID;
  const auto usages = workspace.inspect("openssl x509 -in " + certificate + " -noout -text");
  if (!hostContains(usages, "Digital Signature") || !hostContains(usages, "TLS Web Client Authentication")) return HostCredentialValidation::USAGE_INVALID;
  if (!workspace.run("openssl x509 -in " + certificate + " -noout -checkend 1")) return HostCredentialValidation::VALIDITY_INVALID;
  return HostCredentialValidation::VALID;
}

inline bool validateCryptoBackedHostRecord(const Record& record) {
  return validateHostCredentialRecord(record) == HostCredentialValidation::VALID;
}

inline void zeroizeHostCredentialRecord(Record& record) {
  for (auto* blob : {&record.key_blob, &record.certificate_blob, &record.ca_chain_blob}) {
    for (auto& byte : *blob) byte = 0;
    blob->clear();
  }
}

}  // namespace algaguard::host_test
