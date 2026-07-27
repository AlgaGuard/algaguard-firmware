#pragma once

// Host-test only. OpenSSL is invoked at test runtime; no fixture credential is
// stored in this repository or returned through this interface.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace algaguard::host_test {

enum class CryptoFixtureReason { OK, COMMAND_FAILED, IDENTITY_INVALID, KEY_MISMATCH, CHAIN_INVALID, USAGE_INVALID, VALIDITY_INVALID, CLEANUP_FAILED };

struct CryptoFixtureMetadata {
  std::string key_type;
  int key_bits{};
  std::string public_fingerprint;
  std::string certificate_serial;
  std::string certificate_fingerprint;
  std::string subject;
  std::string san_uri;
  std::string not_before;
  std::string not_after;
  bool cleanup_succeeded{};
};
struct HostFixtureCredentialMaterial {
  std::vector<unsigned char> private_key;
  std::vector<unsigned char> client_certificate;
  std::vector<unsigned char> ca_certificate;
  CryptoFixtureMetadata metadata;
};

class EphemeralHostCryptoFixture {
 public:
  EphemeralHostCryptoFixture() {
    static std::atomic<bool> stale_cleanup_complete{false};
    bool expected = false;
    if (stale_cleanup_complete.compare_exchange_strong(expected, true)) cleanupResidualFixtures();
    createDirectory();
  }
  EphemeralHostCryptoFixture(const EphemeralHostCryptoFixture&) = delete;
  EphemeralHostCryptoFixture& operator=(const EphemeralHostCryptoFixture&) = delete;
  ~EphemeralHostCryptoFixture() { cleanup(); }

  bool generateDeviceKey() {
    if (!run("openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -pkeyopt rsa_keygen_pubexp:65537 -out " + quote(deviceKey()))) return false;
    metadata_.key_type = "RSA";
    metadata_.key_bits = 3072;
    metadata_.public_fingerprint = digestPublicKey(deviceKey(), "device-public.der");
    return !metadata_.public_fingerprint.empty();
  }

  bool generateCsr(const std::string& device_id, const std::string& device_uuid) {
    device_id_ = device_id;
    device_uuid_ = device_uuid;
    const auto uri = "urn:algaguard:device:" + device_uuid;
    return run("openssl req -new -sha256 -key " + quote(deviceKey()) + " -subj " + quote("/CN=" + device_id) +
               " -addext " + quote("subjectAltName=URI:" + uri) + " -out " + quote(csr()));
  }

  bool createTestCa() {
    return run("openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -out " + quote(caKey())) &&
           run("openssl req -x509 -new -sha256 -key " + quote(caKey()) + " -days 7 -subj " + quote(std::string("/CN=AlgaGuard Host Test CA")) +
               " -out " + quote(caCertificate()));
  }

  bool issueClientCertificate(const std::string& device_id, const std::string& device_uuid) {
    if (device_id != device_id_ || device_uuid != device_uuid_) return false;
    const auto extension_file = path("client.ext");
    {
      std::ofstream extensions(extension_file, std::ios::binary | std::ios::trunc);
      extensions << "[v3_client]\nsubjectAltName=URI:urn:algaguard:device:" << device_uuid
                 << "\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\n"
                 << "extendedKeyUsage=clientAuth\n";
    }
    if (!run("openssl x509 -req -sha256 -in " + quote(csr()) + " -CA " + quote(caCertificate()) + " -CAkey " + quote(caKey()) +
             " -CAcreateserial -days 2 -extfile " + quote(extension_file) + " -extensions v3_client -out " + quote(clientCertificate()))) return false;
    const auto details = inspect("openssl x509 -in " + quote(clientCertificate()) + " -noout -subject -serial -fingerprint -sha256 -dates -text");
    metadata_.certificate_serial = lineAfter(details, "serial=");
    metadata_.certificate_fingerprint = lineAfter(details, "sha256 Fingerprint=");
    metadata_.subject = lineContaining(details, "CN =");
    metadata_.san_uri = "urn:algaguard:device:" + device_uuid;
    metadata_.not_before = lineAfter(details, "notBefore=");
    metadata_.not_after = lineAfter(details, "notAfter=");
    return !metadata_.certificate_fingerprint.empty();
  }

  bool validateKeyCertificateMatch() const { return modulusDigest(deviceKey()) == certificateModulusDigest(clientCertificate()); }
  bool validateCsrSignature() const { return run("openssl req -in " + quote(csr()) + " -noout -verify"); }
  bool validateCertificateIdentity(const std::string& device_id, const std::string& device_uuid) const {
    const auto details = inspect("openssl x509 -in " + quote(clientCertificate()) + " -noout -subject -ext subjectAltName");
    return hasCommonName(details, device_id) && contains(details, "URI:urn:algaguard:device:" + device_uuid);
  }
  bool validateCsrIdentity(const std::string& device_id, const std::string& device_uuid) const {
    const auto details = inspect("openssl req -in " + quote(csr()) + " -noout -subject -text");
    return hasCommonName(details, device_id) && contains(details, "URI:urn:algaguard:device:" + device_uuid);
  }
  bool validateClientAuthUsage() const {
    const auto details = inspect("openssl x509 -in " + quote(clientCertificate()) + " -noout -text");
    return contains(details, "Digital Signature") && contains(details, "TLS Web Client Authentication");
  }
  bool validateCertificateChain() const { return run("openssl verify -CAfile " + quote(caCertificate()) + " " + quote(clientCertificate())); }
  bool validateCertificateValidity() const { return run("openssl x509 -in " + quote(clientCertificate()) + " -noout -checkend 1"); }
  bool validateWrongKeyRejected() {
    const auto alternate = path("alternate.key");
    return run("openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -out " + quote(alternate)) &&
           modulusDigest(alternate) != certificateModulusDigest(clientCertificate());
  }
  bool validateWrongCaRejected() {
    const auto alternate_key = path("alternate-ca.key");
    const auto alternate_ca = path("alternate-ca.crt");
    return run("openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -out " + quote(alternate_key)) &&
           run("openssl req -x509 -new -key " + quote(alternate_key) + " -days 2 -subj " + quote(std::string("/CN=Other Host CA")) + " -out " + quote(alternate_ca)) &&
           !run("openssl verify -CAfile " + quote(alternate_ca) + " " + quote(clientCertificate()));
  }
  bool validateExpiredCertificateRejected() const {
    const auto expired = path("expired-client.crt");
    const auto extensions = path("client.ext");
    return run("openssl x509 -req -sha256 -in " + quote(csr()) + " -CA " + quote(caCertificate()) + " -CAkey " + quote(caKey()) +
               " -days 0 -extfile " + quote(extensions) + " -extensions v3_client -out " + quote(expired)) &&
           !run("openssl x509 -in " + quote(expired) + " -noout -checkend 1");
  }

  const CryptoFixtureMetadata& metadata() const { return metadata_; }
  std::optional<HostFixtureCredentialMaterial> credentialMaterialForHostTest() const {
    const auto key = readBytes(deviceKey());
    const auto certificate = readBytes(clientCertificate());
    const auto ca = readBytes(caCertificate());
    if (key.empty() || certificate.empty() || ca.empty()) return std::nullopt;
    return HostFixtureCredentialMaterial{key, certificate, ca, metadata_};
  }
  const std::filesystem::path& temporaryDirectory() const { return directory_; }
  CryptoFixtureReason lastReason() const { return last_reason_; }

  static bool cleanupResidualFixtures() {
    std::error_code error;
    const auto temp = std::filesystem::temp_directory_path(error);
    if (error) return false;
    for (const auto& entry : std::filesystem::directory_iterator(temp, error)) {
      if (error) return false;
      const auto name = entry.path().filename().string();
      if (entry.is_directory() && name.rfind("algaguard-host-crypto-", 0) == 0) {
        std::filesystem::remove_all(entry.path(), error);
        if (error) return false;
      }
    }
    return true;
  }

  bool cleanup() {
    if (cleaned_) return metadata_.cleanup_succeeded;
    for (auto& byte : scratch_) byte = 0;
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
    cleaned_ = true;
    metadata_.cleanup_succeeded = !error && !std::filesystem::exists(directory_);
    last_reason_ = metadata_.cleanup_succeeded ? CryptoFixtureReason::OK : CryptoFixtureReason::CLEANUP_FAILED;
    return metadata_.cleanup_succeeded;
  }

 private:
  static std::string quote(const std::filesystem::path& value) { return quote(value.string()); }
  static std::string quote(const std::string& value) { return "\"" + value + "\""; }
  static bool contains(const std::string& value, const std::string& needle) { return value.find(needle) != std::string::npos; }
  static bool hasCommonName(const std::string& text, const std::string& device_id) {
    return contains(text, "CN = " + device_id) || contains(text, "CN=" + device_id);
  }
  static std::string lineAfter(const std::string& text, const std::string& prefix) {
    const auto begin = text.find(prefix); if (begin == std::string::npos) return {};
    const auto end = text.find_first_of("\r\n", begin); return text.substr(begin + prefix.size(), end - begin - prefix.size());
  }
  static std::string lineContaining(const std::string& text, const std::string& needle) {
    const auto begin = text.find(needle); if (begin == std::string::npos) return {};
    const auto end = text.find_first_of("\r\n", begin); return text.substr(begin, end - begin);
  }
  void createDirectory() {
    static std::atomic<unsigned long long> sequence{0};
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    directory_ = std::filesystem::temp_directory_path() / ("algaguard-host-crypto-" + std::to_string(nonce) + "-" + std::to_string(++sequence));
    std::filesystem::create_directories(directory_);
  }
  std::filesystem::path path(const std::string& name) const { return directory_ / name; }
  std::filesystem::path deviceKey() const { return path("device.key"); }
  std::filesystem::path csr() const { return path("device.csr"); }
  std::filesystem::path caKey() const { return path("ca.key"); }
  std::filesystem::path caCertificate() const { return path("ca.crt"); }
  std::filesystem::path clientCertificate() const { return path("client.crt"); }
  bool run(const std::string& command) const { return std::system((command + " > NUL 2>&1").c_str()) == 0; }
  std::string inspect(const std::string& command) const {
    const auto output = path("inspect.txt");
    if (std::system((command + " > " + quote(output) + " 2>&1").c_str()) != 0) return {};
    std::ifstream input(output, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }
  std::string digestPublicKey(const std::filesystem::path& key, const std::string& public_name) const {
    const auto public_key = path(public_name); const auto output = path("public.digest");
    if (!run("openssl pkey -in " + quote(key) + " -pubout -outform DER -out " + quote(public_key))) return {};
    if (std::system(("openssl dgst -sha256 " + quote(public_key) + " > " + quote(output) + " 2>&1").c_str()) != 0) return {};
    std::ifstream input(output); std::string value; std::getline(input, value); return value;
  }
  static std::vector<unsigned char> readBytes(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }
  std::string modulusDigest(const std::filesystem::path& key) const { return inspect("openssl rsa -in " + quote(key) + " -noout -modulus"); }
  std::string certificateModulusDigest(const std::filesystem::path& certificate) const { return inspect("openssl x509 -in " + quote(certificate) + " -noout -modulus"); }

  std::filesystem::path directory_;
  std::string device_id_;
  std::string device_uuid_;
  mutable std::vector<unsigned char> scratch_{64, 0};
  CryptoFixtureMetadata metadata_;
  CryptoFixtureReason last_reason_{CryptoFixtureReason::OK};
  bool cleaned_{};
};

}  // namespace algaguard::host_test
