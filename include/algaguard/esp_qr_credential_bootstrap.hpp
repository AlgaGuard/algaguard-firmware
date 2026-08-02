#pragma once

#if defined(ESP_PLATFORM) && defined(ALGAGUARD_ENABLE_QR_ONBOARDING)

#include <string>
#include <string_view>
#include <utility>

#include "algaguard/qr_credential_bootstrap.hpp"

namespace algaguard {

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
