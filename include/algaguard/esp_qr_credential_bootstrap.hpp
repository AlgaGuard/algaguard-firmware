#pragma once

#if defined(ESP_PLATFORM) && defined(ALGAGUARD_ENABLE_QR_ONBOARDING)

#include <string>
#include <string_view>
#include <utility>

#include "algaguard/qr_credential_bootstrap.hpp"

namespace algaguard {

// Synchronizes the system clock via SNTP if it isn't already trusted
// (>= 2024-01-01 UTC). The ESP32 has no battery-backed RTC, so every reboot
// loses the clock; this is used both during QR bootstrap and by a restored
// (already-paired) device on every boot, since utcNow() (device_telemetry.hpp)
// refuses to build a telemetry payload without a trusted clock.
bool ensureTrustedClock();

class EspQrCredentialBootstrapTransport final
    : public QrCredentialBootstrapTransport {
 public:
  explicit EspQrCredentialBootstrapTransport(std::string baseUrl)
      : baseUrl_(std::move(baseUrl)) {}
  std::optional<QrBootstrapAuthorization> exchange(
      std::string_view sessionToken, std::string_view deviceId) override;
  std::optional<Issuance> issue(
      std::string_view bootstrapToken, const CsrSubmission& csr) override;

 private:
  std::string baseUrl_;
};

}  // namespace algaguard

#endif
