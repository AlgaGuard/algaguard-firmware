#include "algaguard/secure_identity.hpp"
#include "algaguard/config.hpp"
#include "algaguard/security_profile_guards.hpp"

#include "esp_random.h"
#include "esp_tls.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "mbedtls/pk.h"
#include "mbedtls/asn1write.h"
#include "mbedtls/bignum.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/x509.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509_csr.h"
#include "psa/crypto.h"
#include "psa/crypto_sizes.h"

#include <array>
#include <cstring>

namespace algaguard {
namespace {
constexpr char kNamespace[] = "algaguard_dev_identity";
constexpr std::size_t kRsaBits = 3072;
constexpr std::size_t kEcBits = 256;
constexpr std::size_t kPrivateKeyExportMax = PSA_EXPORT_KEY_OUTPUT_SIZE(PSA_KEY_TYPE_RSA_KEY_PAIR, kRsaBits);
constexpr std::size_t kCsrMaxBytes = ALGAGUARD_CREDENTIAL_MAX_CSR_BYTES;

struct VolatileKey {
  mbedtls_svc_key_id_t id{MBEDTLS_SVC_KEY_ID_INIT};
  PrivateKeyHandle handle{};
  KeyAlgorithm algorithm{KeyAlgorithm::kEcP256};
  bool active{};
};

bool ecdsa_raw_signature_to_der(
    const unsigned char* raw, std::size_t raw_size,
    std::array<unsigned char, 80>* output, const unsigned char** der,
    std::size_t* der_size) {
  if (raw == nullptr || raw_size != 64 || output == nullptr || der == nullptr ||
      der_size == nullptr)
    return false;
  mbedtls_mpi r;
  mbedtls_mpi s;
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&s);
  auto* cursor = output->data() + output->size();
  const auto* begin = output->data();
  int written = 0;
  bool ok = mbedtls_mpi_read_binary(&r, raw, 32) == 0 &&
            mbedtls_mpi_read_binary(&s, raw + 32, 32) == 0;
  if (ok) {
    const int part = mbedtls_asn1_write_mpi(&cursor, begin, &s);
    ok = part >= 0;
    if (ok) written += part;
  }
  if (ok) {
    const int part = mbedtls_asn1_write_mpi(&cursor, begin, &r);
    ok = part >= 0;
    if (ok) written += part;
  }
  if (ok) {
    const int part =
        mbedtls_asn1_write_len(&cursor, begin, static_cast<std::size_t>(written));
    ok = part >= 0;
    if (ok) written += part;
  }
  if (ok) {
    const int part = mbedtls_asn1_write_tag(
        &cursor, begin, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
    ok = part >= 0;
    if (ok) written += part;
  }
  mbedtls_mpi_free(&r);
  mbedtls_mpi_free(&s);
  if (!ok) return false;
  *der = cursor;
  *der_size = static_cast<std::size_t>(written);
  return true;
}

VolatileKey g_key;

bool dev_profile_enabled(SecurityProfile profile) {
#if defined(ALGAGUARD_SECURITY_PROFILE_DEV_SOFTWARE_KEY) && defined(ALGAGUARD_ALLOW_INSECURE_KEY_STORAGE)
  return profile == SecurityProfile::kDevSoftwareKey;
#else
  (void)profile;
  return false;
#endif
}

void wipe(void* data, std::size_t size) {
  if (data != nullptr && size != 0) mbedtls_platform_zeroize(data, size);
}

bool valid_uuid_text(std::string_view value) {
  if (value.size() != 36) return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') return false;
    } else if (!std::isxdigit(static_cast<unsigned char>(value[i]))) {
      return false;
    }
  }
  return true;
}

bool valid_binding(const DeviceBinding& binding) {
  return valid_device_id(binding.device_id) && valid_uuid_text(binding.device_uuid);
}

void clear_volatile_key() {
  if (g_key.active) psa_destroy_key(g_key.id);
  wipe(&g_key, sizeof(g_key));
}

bool current_key(const PrivateKeyHandle& handle) {
  return g_key.active && handle.slot == g_key.handle.slot && handle.generation == g_key.handle.generation;
}

bool write_blob(nvs_handle_t handle, const char* name, const void* data, std::size_t size) {
  return nvs_set_blob(handle, name, data, size) == ESP_OK;
}

std::string slot_name(const char* field, std::uint8_t slot) {
  return std::string(field) + static_cast<char>('0' + slot);
}

bool read_blob(nvs_handle_t handle, const char* name, std::vector<std::uint8_t>* output) {
  std::size_t size = 0;
  if (nvs_get_blob(handle, name, nullptr, &size) != ESP_OK || size == 0 || size > ALGAGUARD_CREDENTIAL_MAX_CHAIN_BYTES) return false;
  output->resize(size);
  return nvs_get_blob(handle, name, output->data(), &size) == ESP_OK;
}

bool validate_bundle(const PublicCredentialBundle& bundle) {
  return !bundle.credential_id.empty() && !bundle.certificate_pem.empty() && !bundle.ca_chain_pem.empty() &&
         !bundle.identity.common_name.empty() && bundle.identity.san_uris.size() == 1 &&
         !bundle.revoked && !bundle.compromised && bundle.not_after_epoch > bundle.not_before_epoch &&
         bundle.certificate_pem.find("PRIVATE KEY") == std::string::npos &&
         std::all_of(bundle.ca_chain_pem.begin(), bundle.ca_chain_pem.end(), [](const std::string& cert) {
           return cert.find("PRIVATE KEY") == std::string::npos;
         });
}

bool read_slot(nvs_handle_t nvs, std::uint8_t slot, PublicCredentialBundle* bundle,
               std::vector<std::uint8_t>* key_der, std::uint32_t* generation, bool require_committed = true) {
  std::uint8_t committed = 0;
  std::uint32_t schema = 0;
  std::vector<std::uint8_t> cert, chain, cred, device, uuid;
  std::uint64_t not_before = 0, not_after = 0;
  const bool valid = nvs_get_u8(nvs, slot_name("commit", slot).c_str(), &committed) == ESP_OK &&
      (!require_committed || committed == 0) &&
      nvs_get_u32(nvs, slot_name("schema", slot).c_str(), &schema) == ESP_OK && schema == 1 &&
      nvs_get_u32(nvs, slot_name("gen", slot).c_str(), generation) == ESP_OK && *generation > 0 &&
      read_blob(nvs, slot_name("pkey", slot).c_str(), key_der) &&
      read_blob(nvs, slot_name("cert", slot).c_str(), &cert) && read_blob(nvs, slot_name("chain", slot).c_str(), &chain) &&
      read_blob(nvs, slot_name("cred", slot).c_str(), &cred) && read_blob(nvs, slot_name("device", slot).c_str(), &device) &&
      read_blob(nvs, slot_name("uuid", slot).c_str(), &uuid) &&
      nvs_get_u64(nvs, slot_name("before", slot).c_str(), &not_before) == ESP_OK &&
      nvs_get_u64(nvs, slot_name("after", slot).c_str(), &not_after) == ESP_OK;
  if (!valid) return false;
  *bundle = {std::string(cred.begin(), cred.end()), std::string(cert.begin(), cert.end()),
             {std::string(chain.begin(), chain.end())},
             {std::string(device.begin(), device.end()), {std::string(uuid.begin(), uuid.end())}},
             not_before, not_after, false, false};
  return validate_bundle(*bundle);
}

bool certificate_matches_current_key(const std::string& certificate) {
  if (!g_key.active) return false;
  mbedtls_x509_crt crt;
  mbedtls_x509_crt_init(&crt);
  const int parsed = mbedtls_x509_crt_parse(&crt,
      reinterpret_cast<const unsigned char*>(certificate.c_str()), certificate.size() + 1);
  std::array<unsigned char, 32> challenge{};
  std::array<unsigned char, PSA_SIGNATURE_MAX_SIZE> signature{};
  std::size_t signature_size = 0;
  esp_fill_random(challenge.data(), challenge.size());
  const psa_algorithm_t signing_algorithm =
      g_key.algorithm == KeyAlgorithm::kEcP256
          ? PSA_ALG_ECDSA(PSA_ALG_SHA_256)
          : PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_256);
  const psa_status_t signed_value = parsed == 0 ? psa_sign_message(
      g_key.id, signing_algorithm, challenge.data(), challenge.size(),
      signature.data(), signature.size(), &signature_size) : PSA_ERROR_INVALID_ARGUMENT;
  std::array<unsigned char, 32> hash{};
  std::array<unsigned char, 80> encoded_signature{};
  std::size_t hash_size = 0;
  const psa_status_t hashed = signed_value == PSA_SUCCESS ? psa_hash_compute(
      PSA_ALG_SHA_256, challenge.data(), challenge.size(), hash.data(), hash.size(), &hash_size) : PSA_ERROR_INVALID_ARGUMENT;
  const unsigned char* verification_signature = signature.data();
  std::size_t verification_signature_size = signature_size;
  const bool signature_ready =
      g_key.algorithm != KeyAlgorithm::kEcP256 ||
      ecdsa_raw_signature_to_der(signature.data(), signature_size,
                                 &encoded_signature, &verification_signature,
                                 &verification_signature_size);
  const int verified = hashed == PSA_SUCCESS && hash_size == hash.size() &&
                               signature_ready
      ? mbedtls_pk_verify(&crt.pk, MBEDTLS_MD_SHA256, hash.data(), hash.size(),
                          verification_signature, verification_signature_size)
      : -1;
  wipe(challenge.data(), challenge.size());
  wipe(signature.data(), signature.size());
  wipe(encoded_signature.data(), encoded_signature.size());
  wipe(hash.data(), hash.size());
  mbedtls_x509_crt_free(&crt);
  return verified == 0;
}
}  // namespace

struct EspDevelopmentSoftwareKeyProvider::Impl {};

EspDevelopmentSoftwareKeyProvider::EspDevelopmentSoftwareKeyProvider(SecurityProfile profile)
    : profile_(profile), impl_(std::make_unique<Impl>()) {}
EspDevelopmentSoftwareKeyProvider::~EspDevelopmentSoftwareKeyProvider() = default;

SecurityStatus EspDevelopmentSoftwareKeyProvider::status() const {
  return dev_profile_enabled(profile_) ? SecurityStatus::kReady : SecurityStatus::kUnavailable;
}

std::optional<PrivateKeyHandle> EspDevelopmentSoftwareKeyProvider::generate(KeyAlgorithm algorithm) {
  if (status() != SecurityStatus::kReady ||
      (algorithm != KeyAlgorithm::kEcP256 &&
       algorithm != KeyAlgorithm::kRsa3072) ||
      psa_crypto_init() != PSA_SUCCESS)
    return std::nullopt;
  clear_volatile_key();
  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  const bool ec = algorithm == KeyAlgorithm::kEcP256;
  psa_set_key_type(&attributes, ec
      ? PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1)
      : PSA_KEY_TYPE_RSA_KEY_PAIR);
  psa_set_key_bits(&attributes, ec ? kEcBits : kRsaBits);
  psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_EXPORT);
  psa_set_key_algorithm(&attributes, ec
      ? PSA_ALG_ECDSA(PSA_ALG_SHA_256)
      : PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_256));
  if (psa_generate_key(&attributes, &g_key.id) != PSA_SUCCESS) {
    psa_reset_key_attributes(&attributes);
    clear_volatile_key();
    return std::nullopt;
  }
  psa_reset_key_attributes(&attributes);
  g_key.handle = {1, g_key.handle.generation + 1};
  g_key.algorithm = algorithm;
  g_key.active = true;
  return g_key.handle;
}

std::optional<CsrSubmission> EspDevelopmentSoftwareKeyProvider::create_csr(const PrivateKeyHandle& key,
                                                                              const DeviceBinding& binding) {
  if (status() != SecurityStatus::kReady || !current_key(key) || !valid_binding(binding)) return std::nullopt;
  mbedtls_pk_context wrapped;
  mbedtls_pk_init(&wrapped);
  mbedtls_x509write_csr request;
  mbedtls_x509write_csr_init(&request);
  std::array<unsigned char, kCsrMaxBytes> pem{};
  const std::string subject = "CN=" + binding.device_id;
  const std::string uri = "urn:algaguard:device:" + binding.device_uuid;
  mbedtls_x509_san_list san{};
  san.node.type = MBEDTLS_X509_SAN_UNIFORM_RESOURCE_IDENTIFIER;
  san.node.san.unstructured_name.tag = MBEDTLS_ASN1_IA5_STRING;
  san.node.san.unstructured_name.p = reinterpret_cast<unsigned char*>(const_cast<char*>(uri.data()));
  san.node.san.unstructured_name.len = uri.size();
  const int result = mbedtls_pk_wrap_psa(&wrapped, g_key.id) |
      mbedtls_x509write_csr_set_subject_name(&request, subject.c_str());
  mbedtls_x509write_csr_set_key(&request, &wrapped);
  mbedtls_x509write_csr_set_md_alg(&request, MBEDTLS_MD_SHA256);
  const int san_result = result == 0 ? mbedtls_x509write_csr_set_subject_alternative_name(&request, &san) : -1;
  const int pem_result = san_result == 0 ? mbedtls_x509write_csr_pem(&request, pem.data(), pem.size()) : -1;
  mbedtls_x509write_csr_free(&request);
  mbedtls_pk_free(&wrapped);
  if (pem_result != 0 || std::strstr(reinterpret_cast<const char*>(pem.data()), "PRIVATE KEY") != nullptr) {
    wipe(pem.data(), pem.size());
    return std::nullopt;
  }
  CsrSubmission csr{reinterpret_cast<const char*>(pem.data()), binding,
                    g_key.algorithm};
  wipe(pem.data(), pem.size());
  return csr;
}

bool EspDevelopmentSoftwareKeyProvider::destroy(const PrivateKeyHandle& key) {
  if (!current_key(key)) return false;
  clear_volatile_key();
  return true;
}
bool EspDevelopmentSoftwareKeyProvider::software_key_in_use() const { return status() == SecurityStatus::kReady; }

struct EspDevelopmentCredentialStorage::Impl {
  CredentialJournal journal;
  std::optional<PublicCredentialBundle> active;
  std::optional<PublicCredentialBundle> pending;
};

EspDevelopmentCredentialStorage::EspDevelopmentCredentialStorage(SecurityProfile profile)
    : profile_(profile), impl_(std::make_unique<Impl>()) {}
EspDevelopmentCredentialStorage::~EspDevelopmentCredentialStorage() = default;
SecurityStatus EspDevelopmentCredentialStorage::status() const {
  return dev_profile_enabled(profile_) ? SecurityStatus::kReady : SecurityStatus::kUnavailable;
}

bool EspDevelopmentCredentialStorage::stage(const PrivateKeyHandle& key, const PublicCredentialBundle& bundle) {
  if (status() != SecurityStatus::kReady || !current_key(key) || !validate_bundle(bundle) ||
      !certificate_matches_current_key(bundle.certificate_pem)) return false;
  std::array<unsigned char, kPrivateKeyExportMax> private_der{};
  std::size_t private_size = 0;
  if (psa_export_key(g_key.id, private_der.data(), private_der.size(), &private_size) != PSA_SUCCESS || private_size == 0) return false;
  nvs_handle_t nvs{};
  const esp_err_t opened = nvs_open(kNamespace, NVS_READWRITE, &nvs);
  std::uint8_t active_slot = 0;
  std::uint32_t active_generation = 0;
  if (opened == ESP_OK) {
    nvs_get_u8(nvs, "active_slot", &active_slot);
    nvs_get_u32(nvs, "active_gen", &active_generation);
  }
  const std::uint8_t inactive_slot = active_slot == 0 ? 1 : 0;
  const std::uint32_t next_generation = active_generation + 1;
  const bool written = opened == ESP_OK &&
      nvs_set_u8(nvs, "pending_slot", inactive_slot) == ESP_OK &&
      nvs_set_u8(nvs, slot_name("commit", inactive_slot).c_str(), 0) == ESP_OK &&
      nvs_set_u32(nvs, slot_name("schema", inactive_slot).c_str(), 1) == ESP_OK &&
      nvs_set_u32(nvs, slot_name("gen", inactive_slot).c_str(), next_generation) == ESP_OK &&
      write_blob(nvs, slot_name("pkey", inactive_slot).c_str(), private_der.data(), private_size) &&
      write_blob(nvs, slot_name("cert", inactive_slot).c_str(), bundle.certificate_pem.data(), bundle.certificate_pem.size()) &&
      write_blob(nvs, slot_name("chain", inactive_slot).c_str(), bundle.ca_chain_pem.front().data(), bundle.ca_chain_pem.front().size()) &&
      write_blob(nvs, slot_name("cred", inactive_slot).c_str(), bundle.credential_id.data(), bundle.credential_id.size()) &&
      write_blob(nvs, slot_name("device", inactive_slot).c_str(), bundle.identity.common_name.data(), bundle.identity.common_name.size()) &&
      write_blob(nvs, slot_name("uuid", inactive_slot).c_str(), bundle.identity.san_uris.front().data(), bundle.identity.san_uris.front().size()) &&
      nvs_set_u64(nvs, slot_name("before", inactive_slot).c_str(), bundle.not_before_epoch) == ESP_OK &&
      nvs_set_u64(nvs, slot_name("after", inactive_slot).c_str(), bundle.not_after_epoch) == ESP_OK && nvs_commit(nvs) == ESP_OK;
  PublicCredentialBundle readback;
  std::vector<std::uint8_t> readback_key;
  std::uint32_t readback_generation = 0;
  const bool readback_valid = written && read_slot(nvs, inactive_slot, &readback, &readback_key, &readback_generation, false) &&
      readback_generation == next_generation && readback.certificate_pem == bundle.certificate_pem;
  wipe(readback_key.data(), readback_key.size());
  const bool committed = readback_valid && nvs_set_u8(nvs, slot_name("commit", inactive_slot).c_str(), 1) == ESP_OK && nvs_commit(nvs) == ESP_OK;
  if (opened == ESP_OK) nvs_close(nvs);
  wipe(private_der.data(), private_der.size());
  if (!committed) return false;
  impl_->pending = bundle;
  return true;
}

bool EspDevelopmentCredentialStorage::activate_staged() {
  if (status() != SecurityStatus::kReady) return false;
  nvs_handle_t nvs{};
  std::uint8_t pending_slot = 0;
  std::uint32_t generation = 0;
  const bool activated = impl_->pending.has_value() && nvs_open(kNamespace, NVS_READWRITE, &nvs) == ESP_OK &&
      nvs_get_u8(nvs, "pending_slot", &pending_slot) == ESP_OK && pending_slot < 2 &&
      nvs_get_u32(nvs, slot_name("gen", pending_slot).c_str(), &generation) == ESP_OK &&
      nvs_set_u8(nvs, "active_slot", pending_slot) == ESP_OK && nvs_set_u32(nvs, "active_gen", generation) == ESP_OK &&
      nvs_erase_key(nvs, "pending_slot") == ESP_OK && nvs_commit(nvs) == ESP_OK;
  if (nvs != 0) nvs_close(nvs);
  if (activated) {
    impl_->active = std::move(impl_->pending);
    impl_->pending.reset();
  }
  return activated;
}
void EspDevelopmentCredentialStorage::discard_staged() {
  impl_->journal.discard_staged();
  impl_->pending.reset();
}
std::optional<PublicCredentialBundle> EspDevelopmentCredentialStorage::active_public_bundle() const { return impl_->active; }
bool EspDevelopmentCredentialStorage::reload_active_identity() {
  if (status() != SecurityStatus::kReady || psa_crypto_init() != PSA_SUCCESS) return false;
  nvs_handle_t nvs{};
  std::uint8_t active_slot = 0;
  std::uint32_t pointer_generation = 0;
  if (nvs_open(kNamespace, NVS_READONLY, &nvs) != ESP_OK) {
    if (nvs != 0) nvs_close(nvs);
    return false;
  }
  std::vector<std::uint8_t> key_der;
  PublicCredentialBundle bundle;
  std::uint32_t generation = 0;
  const bool pointer_ok = nvs_get_u8(nvs, "active_slot", &active_slot) == ESP_OK && active_slot < 2 &&
      nvs_get_u32(nvs, "active_gen", &pointer_generation) == ESP_OK;
  bool read = pointer_ok && read_slot(nvs, active_slot, &bundle, &key_der, &generation) && generation == pointer_generation;
  if (!read) {
    std::vector<std::uint8_t> alternative_key;
    PublicCredentialBundle alternative;
    std::uint32_t alternative_generation = 0;
    for (std::uint8_t slot = 0; slot < 2; ++slot) {
      std::vector<std::uint8_t> candidate_key;
      PublicCredentialBundle candidate;
      std::uint32_t candidate_generation = 0;
      if (read_slot(nvs, slot, &candidate, &candidate_key, &candidate_generation) &&
          (!read || candidate_generation > alternative_generation)) {
        wipe(alternative_key.data(), alternative_key.size());
        alternative_key = std::move(candidate_key); alternative = std::move(candidate); alternative_generation = candidate_generation; read = true;
      } else wipe(candidate_key.data(), candidate_key.size());
    }
    if (read) { key_der = std::move(alternative_key); bundle = std::move(alternative); generation = alternative_generation; }
  }
  nvs_close(nvs);
  if (!read) { wipe(key_der.data(), key_der.size()); return false; }
  clear_volatile_key();
  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attributes, PSA_KEY_TYPE_RSA_KEY_PAIR);
  psa_set_key_bits(&attributes, kRsaBits);
  psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_EXPORT);
  psa_set_key_algorithm(&attributes, PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_256));
  const psa_status_t imported = psa_import_key(&attributes, key_der.data(), key_der.size(), &g_key.id);
  psa_reset_key_attributes(&attributes);
  wipe(key_der.data(), key_der.size());
  if (imported != PSA_SUCCESS) return false;
  g_key.handle = {1, g_key.handle.generation + 1};
  g_key.active = true;
  if (!validate_bundle(bundle) || !certificate_matches_current_key(bundle.certificate_pem)) {
    clear_volatile_key();
    return false;
  }
  impl_->active = std::move(bundle);
  return true;
}
std::optional<SoftwareTlsIdentity> EspDevelopmentCredentialStorage::software_tls_identity() {
  if (status() != SecurityStatus::kReady && status() != SecurityStatus::kExpired) return std::nullopt;
  if (!impl_->active.has_value() && !reload_active_identity()) return std::nullopt;
  nvs_handle_t nvs{};
  std::uint8_t active_slot = 0;
  std::uint32_t ignored_generation = 0;
  std::vector<std::uint8_t> key_der;
  PublicCredentialBundle bundle;
  const bool loaded = nvs_open(kNamespace, NVS_READONLY, &nvs) == ESP_OK &&
      nvs_get_u8(nvs, "active_slot", &active_slot) == ESP_OK && active_slot < 2 &&
      read_slot(nvs, active_slot, &bundle, &key_der, &ignored_generation);
  if (nvs != 0) nvs_close(nvs);
  if (!loaded || !validate_bundle(bundle) || !certificate_matches_current_key(bundle.certificate_pem)) {
    wipe(key_der.data(), key_der.size());
    return std::nullopt;
  }
  SoftwareTlsIdentity identity;
  identity.ca_certificate.assign(bundle.ca_chain_pem.front().begin(), bundle.ca_chain_pem.front().end());
  identity.ca_certificate.push_back(0);
  identity.client_certificate.assign(bundle.certificate_pem.begin(), bundle.certificate_pem.end());
  identity.client_certificate.push_back(0);
  identity.client_private_key.assign(key_der.begin(), key_der.end());
  wipe(key_der.data(), key_der.size());
  identity.status = SecurityStatus::kReady;
  identity.software_private_key_in_use = true;
  identity.ds_data_absent = true;
  // Concrete non-network caller boundary: the buffers are directly compatible
  // with esp_tls_cfg_t.{cacert_buf,clientcert_buf,clientkey_buf}; ds_data is never set.
  esp_tls_cfg_t config{};
  config.cacert_buf = identity.ca_certificate.data(); config.cacert_bytes = identity.ca_certificate.size();
  config.clientcert_buf = identity.client_certificate.data(); config.clientcert_bytes = identity.client_certificate.size();
  config.clientkey_buf = identity.client_private_key.data(); config.clientkey_bytes = identity.client_private_key.size();
  if (config.ds_data != nullptr || config.clientkey_buf == nullptr || config.clientcert_buf == nullptr || config.cacert_buf == nullptr)
    return std::nullopt;
  return identity;
}
bool EspDevelopmentCredentialStorage::confirmed_reset(bool confirmed) {
  if (!confirmed || status() != SecurityStatus::kReady) return false;
  clear_volatile_key();
  nvs_handle_t nvs{};
  if (nvs_open(kNamespace, NVS_READWRITE, &nvs) != ESP_OK) return false;
  const bool erased = nvs_erase_all(nvs) == ESP_OK && nvs_commit(nvs) == ESP_OK;
  nvs_close(nvs);
  impl_->active.reset();
  impl_->journal.discard_staged();
  return erased;
}

}  // namespace algaguard
