#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

#include "algaguard/credentials.hpp"
#include "algaguard/qr_onboarding.hpp"

namespace algaguard {

struct QrBootstrapAuthorization {
  QrBootstrapAuthorization() = default;
  QrBootstrapAuthorization(const QrBootstrapAuthorization&) = delete;
  QrBootstrapAuthorization& operator=(const QrBootstrapAuthorization&) = delete;
  QrBootstrapAuthorization(QrBootstrapAuthorization&& other) noexcept
      : deviceUuid(std::move(other.deviceUuid)),
        bootstrapToken(std::move(other.bootstrapToken)) { other.clear(); }
  QrBootstrapAuthorization& operator=(QrBootstrapAuthorization&& other) noexcept {
    if (this != &other) {
      clear();
      deviceUuid = std::move(other.deviceUuid);
      bootstrapToken = std::move(other.bootstrapToken);
      other.clear();
    }
    return *this;
  }
  std::string deviceUuid;
  std::string bootstrapToken;

  bool available() const { return !deviceUuid.empty() && !bootstrapToken.empty(); }
  void clear() noexcept {
    std::fill(deviceUuid.begin(), deviceUuid.end(), '\0');
    std::fill(bootstrapToken.begin(), bootstrapToken.end(), '\0');
    deviceUuid.clear();
    bootstrapToken.clear();
  }
  ~QrBootstrapAuthorization() { clear(); }
};

class QrCredentialBootstrapTransport {
 public:
  virtual ~QrCredentialBootstrapTransport() = default;
  virtual std::optional<QrBootstrapAuthorization> exchange(
      std::string_view sessionToken, std::string_view deviceId) = 0;
  virtual std::optional<PublicCredentialBundle> issue(
      std::string_view bootstrapToken, const CsrSubmission& csr) = 0;
};

enum class QrCredentialBootstrapResult {
  kSuccess,
  kNoContext,
  kExchangeRejected,
  kKeyGenerationFailed,
  kCsrFailed,
  kIssueRejected,
  kCertificateBindingMismatch,
  kStorageFailed,
};

// One-shot coordinator. Session and bootstrap authorizations are moved through
// volatile objects, and all failure paths destroy the newly generated key.
class QrCredentialBootstrapCoordinator {
 public:
  QrCredentialBootstrapCoordinator(LocalPrivateKeyProvider& keys,
                                    SecureCredentialStorage& storage,
                                    QrCredentialBootstrapTransport& transport)
      : keys_(keys), storage_(storage), transport_(transport) {}

  QrCredentialBootstrapResult run(QrCredentialBootstrapContext context) {
    if (attempted_ || !context.pending()) return QrCredentialBootstrapResult::kNoContext;
    attempted_ = true;
    const std::string deviceId = context.deviceId;
    auto authorization = transport_.exchange(context.sessionToken, deviceId);
    context.clear();
    if (!authorization || !authorization->available())
      return QrCredentialBootstrapResult::kExchangeRejected;
    auto key = keys_.generate(KeyAlgorithm::kEcP256);
    if (!key) return QrCredentialBootstrapResult::kKeyGenerationFailed;
    const DeviceBinding binding{deviceId, authorization->deviceUuid};
    return finish(*key, std::move(*authorization), binding);
  }

  bool attempted() const { return attempted_; }

 private:
  QrCredentialBootstrapResult finish(PrivateKeyHandle key,
                                     QrBootstrapAuthorization authorization,
                                     const DeviceBinding& binding) {
    auto csr = keys_.create_csr(key, binding);
    if (!csr) {
      keys_.destroy(key);
      return QrCredentialBootstrapResult::kCsrFailed;
    }
    auto bundle = transport_.issue(authorization.bootstrapToken, *csr);
    authorization.clear();
    std::fill(csr->pem.begin(), csr->pem.end(), '\0');
    csr->pem.clear();
    if (!bundle) {
      keys_.destroy(key);
      return QrCredentialBootstrapResult::kIssueRejected;
    }
    if (!exact_certificate_binding(binding, bundle->identity)) {
      keys_.destroy(key);
      return QrCredentialBootstrapResult::kCertificateBindingMismatch;
    }
    if (!storage_.stage(key, *bundle) || !storage_.activate_staged()) {
      storage_.discard_staged();
      keys_.destroy(key);
      return QrCredentialBootstrapResult::kStorageFailed;
    }
    return QrCredentialBootstrapResult::kSuccess;
  }

  LocalPrivateKeyProvider& keys_;
  SecureCredentialStorage& storage_;
  QrCredentialBootstrapTransport& transport_;
  bool attempted_{};
};

}  // namespace algaguard
