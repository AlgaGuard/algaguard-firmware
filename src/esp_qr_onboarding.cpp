#include "algaguard/esp_qr_onboarding.hpp"

#if defined(ESP_PLATFORM) && defined(ALGAGUARD_ENABLE_QR_ONBOARDING)

#include <algorithm>
#include <array>

#include "algaguard/physical_provisioning_runtime_bridge.hpp"
#include "algaguard/qr_onboarding_public_key.hpp"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "mbedtls/asn1write.h"
#include "mbedtls/bignum.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"

namespace algaguard {
namespace {
bool raw_signature_to_der(const std::uint8_t* raw, std::size_t size,
                          std::array<std::uint8_t, 80>& output,
                          const std::uint8_t** der,
                          std::size_t* derSize) {
  if (raw == nullptr || size != 64 || der == nullptr || derSize == nullptr)
    return false;
  mbedtls_mpi r;
  mbedtls_mpi s;
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&s);
  auto* cursor = output.data() + output.size();
  const auto* begin = output.data();
  int written = 0;
  bool ok = mbedtls_mpi_read_binary(&r, raw, 32) == 0 &&
            mbedtls_mpi_read_binary(&s, raw + 32, 32) == 0;
  if (ok) {
    int part = mbedtls_asn1_write_mpi(&cursor, begin, &s);
    if (part < 0) ok = false;
    else written += part;
  }
  if (ok) {
    int part = mbedtls_asn1_write_mpi(&cursor, begin, &r);
    if (part < 0) ok = false;
    else written += part;
  }
  if (ok) {
    int part = mbedtls_asn1_write_len(&cursor, begin,
                                     static_cast<std::size_t>(written));
    if (part < 0) ok = false;
    else written += part;
  }
  if (ok) {
    int part = mbedtls_asn1_write_tag(
        &cursor, begin, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
    if (part < 0) ok = false;
    else written += part;
  }
  mbedtls_mpi_free(&r);
  mbedtls_mpi_free(&s);
  if (!ok) return false;
  *der = cursor;
  *derSize = static_cast<std::size_t>(written);
  return true;
}
}  // namespace

bool EspQrRandomSource::fill(std::uint8_t* output, std::size_t size) {
  if (output == nullptr || size == 0) return false;
  esp_fill_random(output, size);
  std::uint8_t aggregate{};
  for (std::size_t index = 0; index < size; ++index) aggregate |= output[index];
  return aggregate != 0;
}

bool EspQrBindingCrypto::sha256(
    std::string_view input, std::array<std::uint8_t, 32>& output) {
  return mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    reinterpret_cast<const unsigned char*>(input.data()),
                    input.size(), output.data()) == 0;
}

bool EspQrBindingCrypto::verifyP256Sha256(
    const std::uint8_t* data, std::size_t dataSize,
    const std::uint8_t* signature, std::size_t signatureSize) {
  if (data == nullptr || dataSize != kQrBindingDataBytes) return false;
  mbedtls_pk_context key;
  mbedtls_pk_init(&key);
  const int parsed = mbedtls_pk_parse_public_key(
      &key, reinterpret_cast<const unsigned char*>(kQrOnboardingSigningPublicKeyPem),
      sizeof(kQrOnboardingSigningPublicKeyPem));
  std::array<std::uint8_t, 32> hash{};
  std::array<std::uint8_t, 80> encoded{};
  const std::uint8_t* der{};
  std::size_t derSize{};
  const bool ready = parsed == 0 && sha256(
      std::string_view{reinterpret_cast<const char*>(data), dataSize}, hash) &&
      raw_signature_to_der(signature, signatureSize, encoded, &der, &derSize);
  const int verified = ready
                           ? mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256,
                                               hash.data(), hash.size(), der,
                                               derSize)
                           : -1;
  std::fill(hash.begin(), hash.end(), 0);
  std::fill(encoded.begin(), encoded.end(), 0);
  mbedtls_pk_free(&key);
  return verified == 0;
}

std::optional<std::uint64_t> EspQrBleSessionAuthorizer::authorize(
    const BleWifiProvisioningRequest& request, std::uint64_t nowTick) {
  if (bootstrap_.pending()) return std::nullopt;
  const auto nowSeconds =
      static_cast<std::uint32_t>(nowTick / configTICK_RATE_HZ);
  auto authorized = manager_.authorize(
      request.bindingGrant(), request.sessionId(), request.deviceId(),
      request.sessionToken(), nowSeconds, crypto_);
  if (!authorized || !authorized->available()) return std::nullopt;
  constexpr std::uint64_t kSessionTicks = 10ULL * 60ULL * configTICK_RATE_HZ;
  if (!physical_wifi_connect_gate.arm(nowTick, kSessionTicks, false, false)) {
    authorized->clear();
    return std::nullopt;
  }
  bootstrap_.sessionId = std::move(authorized->sessionId);
  bootstrap_.deviceId = std::move(authorized->deviceId);
  bootstrap_.sessionToken = std::move(authorized->sessionToken);
  return nowTick + kSessionTicks;
}

QrCredentialBootstrapContext EspQrBleSessionAuthorizer::takeBootstrapContext() {
  QrCredentialBootstrapContext result;
  result.sessionId = std::move(bootstrap_.sessionId);
  result.deviceId = std::move(bootstrap_.deviceId);
  result.sessionToken = std::move(bootstrap_.sessionToken);
  bootstrap_.clear();
  return result;
}

}  // namespace algaguard

#endif
