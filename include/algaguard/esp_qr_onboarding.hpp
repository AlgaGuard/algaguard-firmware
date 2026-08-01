#pragma once

#if defined(ESP_PLATFORM) && defined(ALGAGUARD_ENABLE_QR_ONBOARDING)

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "algaguard/ble_provisioning_payload_validation.hpp"
#include "algaguard/qr_onboarding.hpp"

namespace algaguard {

class EspQrRandomSource final : public QrRandomSource {
 public:
  bool fill(std::uint8_t* output, std::size_t size) override;
};

class EspQrBindingCrypto final : public QrBindingCrypto {
 public:
  bool sha256(std::string_view input,
              std::array<std::uint8_t, 32>& output) override;
  bool verifyP256Sha256(const std::uint8_t* data, std::size_t dataSize,
                        const std::uint8_t* signature,
                        std::size_t signatureSize) override;
};

class EspQrBleSessionAuthorizer final : public QrBleSessionAuthorizer {
 public:
  EspQrBleSessionAuthorizer(QrOnboardingManager& manager,
                            EspQrBindingCrypto& crypto)
      : manager_(manager),
        crypto_(crypto),
        bootstrapMutex_(xSemaphoreCreateMutexStatic(&bootstrapMutexStorage_)) {}
  std::optional<std::uint64_t> authorize(
      const BleWifiProvisioningRequest& request,
      std::uint64_t nowTick) override;
  QrCredentialBootstrapContext takeBootstrapContext();
  bool bootstrapPending() const;
  void clear() noexcept;

 private:
  QrOnboardingManager& manager_;
  EspQrBindingCrypto& crypto_;
  mutable StaticSemaphore_t bootstrapMutexStorage_{};
  mutable SemaphoreHandle_t bootstrapMutex_{};
  QrCredentialBootstrapContext bootstrap_;
};

}  // namespace algaguard

#endif
