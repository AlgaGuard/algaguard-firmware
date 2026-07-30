#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace algaguard {

enum class QrOnboardingState : std::uint8_t {
  kDisabled,
  kGenerating,
  kReady,
  kExpired,
  kConsumed,
  kError,
};

inline constexpr std::size_t kQrInvitationBytes = 31;
inline constexpr std::size_t kQrInvitationUriBytes = 49;
inline constexpr std::uint32_t kQrMaximumLifetimeSeconds = 300;
inline constexpr std::size_t kQrBindingDataBytes = 81;
inline constexpr std::size_t kQrBindingSignatureBytes = 64;
inline constexpr std::size_t kQrBindingGrantBytes =
    kQrBindingDataBytes + kQrBindingSignatureBytes;

class QrRandomSource {
 public:
  virtual ~QrRandomSource() = default;
  virtual bool fill(std::uint8_t* output, std::size_t size) = 0;
};

class QrBindingCrypto {
 public:
  virtual ~QrBindingCrypto() = default;
  virtual bool sha256(std::string_view input,
                      std::array<std::uint8_t, 32>& output) = 0;
  virtual bool verifyP256Sha256(
      const std::uint8_t* data, std::size_t dataSize,
      const std::uint8_t* signature, std::size_t signatureSize) = 0;
};

struct QrAuthorizedSession {
  std::string sessionId;
  std::string deviceId;
  std::string sessionToken;
  std::uint32_t expiresAt{};
  bool available() const {
    return !sessionId.empty() && !deviceId.empty() && !sessionToken.empty() &&
           expiresAt != 0;
  }
  void clear() noexcept {
    std::fill(sessionId.begin(), sessionId.end(), '\0');
    std::fill(deviceId.begin(), deviceId.end(), '\0');
    std::fill(sessionToken.begin(), sessionToken.end(), '\0');
    sessionId.clear();
    deviceId.clear();
    sessionToken.clear();
    expiresAt = 0;
  }
  ~QrAuthorizedSession() { clear(); }
};

struct QrCredentialBootstrapContext {
  QrCredentialBootstrapContext() = default;
  QrCredentialBootstrapContext(const QrCredentialBootstrapContext&) = delete;
  QrCredentialBootstrapContext& operator=(const QrCredentialBootstrapContext&) = delete;
  QrCredentialBootstrapContext(QrCredentialBootstrapContext&& other) noexcept
      : sessionId(std::move(other.sessionId)), deviceId(std::move(other.deviceId)),
        sessionToken(std::move(other.sessionToken)) { other.clear(); }
  QrCredentialBootstrapContext& operator=(QrCredentialBootstrapContext&& other) noexcept {
    if (this != &other) {
      clear();
      sessionId = std::move(other.sessionId);
      deviceId = std::move(other.deviceId);
      sessionToken = std::move(other.sessionToken);
      other.clear();
    }
    return *this;
  }
  std::string sessionId;
  std::string deviceId;
  std::string sessionToken;
  bool pending() const {
    return !sessionId.empty() && !deviceId.empty() && !sessionToken.empty();
  }
  void clear() noexcept {
    std::fill(sessionId.begin(), sessionId.end(), '\0');
    std::fill(deviceId.begin(), deviceId.end(), '\0');
    std::fill(sessionToken.begin(), sessionToken.end(), '\0');
    sessionId.clear();
    deviceId.clear();
    sessionToken.clear();
  }
  ~QrCredentialBootstrapContext() { clear(); }
};

class QrOnboardingManager {
 public:
  explicit QrOnboardingManager(QrRandomSource& random) : random_(random) {}
  QrOnboardingManager(const QrOnboardingManager&) = delete;
  QrOnboardingManager& operator=(const QrOnboardingManager&) = delete;
  ~QrOnboardingManager() { clear(); }

  bool generate(std::string_view deviceId, std::uint32_t issuedAt,
                std::uint32_t lifetimeSeconds = 180) {
    clear();
    state_ = QrOnboardingState::kGenerating;
    const auto deviceNumber = parseDevice(deviceId);
    if (!deviceNumber || issuedAt == 0 || lifetimeSeconds == 0 ||
        lifetimeSeconds > kQrMaximumLifetimeSeconds ||
        issuedAt > UINT32_MAX - lifetimeSeconds ||
        !random_.fill(nonce_.data(), nonce_.size())) {
      clearBytes();
      state_ = QrOnboardingState::kError;
      return false;
    }
    deviceId_.assign(deviceId);
    issuedAt_ = issuedAt;
    expiresAt_ = issuedAt + lifetimeSeconds;
    std::array<std::uint8_t, kQrInvitationBytes> bytes{};
    bytes[0] = 1;
    bytes[1] = static_cast<std::uint8_t>(*deviceNumber >> 16U);
    bytes[2] = static_cast<std::uint8_t>(*deviceNumber >> 8U);
    bytes[3] = static_cast<std::uint8_t>(*deviceNumber);
    std::copy(nonce_.begin(), nonce_.end(), bytes.begin() + 4);
    write32(bytes.data() + 20, issuedAt_);
    write32(bytes.data() + 24, expiresAt_);
    bytes[28] = 1;
    const auto checksum = crc16(bytes.data(), 29);
    bytes[29] = static_cast<std::uint8_t>(checksum >> 8U);
    bytes[30] = static_cast<std::uint8_t>(checksum);
    uri_ = "ag://q/" + base64Url(bytes.data(), bytes.size());
    secureZero(bytes.data(), bytes.size());
    if (uri_.size() != kQrInvitationUriBytes) {
      clear();
      state_ = QrOnboardingState::kError;
      return false;
    }
    state_ = QrOnboardingState::kReady;
    return true;
  }

  void tick(std::uint32_t now) {
    if (state_ == QrOnboardingState::kReady && now >= expiresAt_) {
      clearBytes();
      state_ = QrOnboardingState::kExpired;
    }
  }

  std::optional<QrAuthorizedSession> authorize(
      std::string_view encodedGrant, std::string_view sessionId,
      std::string_view deviceId, std::string_view sessionToken,
      std::uint32_t now, QrBindingCrypto& crypto) {
    tick(now);
    if (state_ != QrOnboardingState::kReady || deviceId != deviceId_ ||
        !validUuid(sessionId) || sessionToken.size() < 32 ||
        sessionToken.size() > 96)
      return std::nullopt;
    std::array<std::uint8_t, kQrBindingGrantBytes> grant{};
    if (!decodeBase64Url(encodedGrant, grant.data(), grant.size())) return std::nullopt;
    std::array<std::uint8_t, 32> tokenHash{};
    const auto grantDevice = read24(grant.data() + 1);
    const auto grantExpiry = read32(grant.data() + 36);
    const bool structural =
        grant[0] == 1 && grant[80] == 1 && grantDevice == *parseDevice(deviceId) &&
        grantExpiry > now && grantExpiry >= expiresAt_ &&
        constantEqual(grant.data() + 4, nonce_.data(), nonce_.size()) &&
        uuidMatches(grant.data() + 20, sessionId) &&
        crypto.sha256(sessionToken, tokenHash) &&
        constantEqual(grant.data() + 48, tokenHash.data(), tokenHash.size()) &&
        crypto.verifyP256Sha256(grant.data(), kQrBindingDataBytes,
                                grant.data() + kQrBindingDataBytes,
                                kQrBindingSignatureBytes);
    secureZero(tokenHash.data(), tokenHash.size());
    secureZero(grant.data(), grant.size());
    if (!structural) return std::nullopt;
    QrAuthorizedSession result{std::string{sessionId}, std::string{deviceId},
                               std::string{sessionToken}, grantExpiry};
    clearBytes();
    state_ = QrOnboardingState::kConsumed;
    return result;
  }

  void disable() noexcept {
    clear();
    state_ = QrOnboardingState::kDisabled;
  }
  void clear() noexcept {
    clearBytes();
    issuedAt_ = 0;
    expiresAt_ = 0;
    state_ = QrOnboardingState::kDisabled;
  }
  QrOnboardingState state() const { return state_; }
  std::string_view uri() const { return uri_; }
  std::uint32_t expiresAt() const { return expiresAt_; }
  bool nonceCleared() const {
    return std::all_of(nonce_.begin(), nonce_.end(),
                       [](std::uint8_t byte) { return byte == 0; });
  }

 private:
  static std::optional<std::uint32_t> parseDevice(std::string_view value) {
    if (value.size() != 9 || value.substr(0, 3) != "AG-") return std::nullopt;
    std::uint32_t result{};
    for (std::size_t index = 3; index < value.size(); ++index) {
      if (value[index] < '0' || value[index] > '9') return std::nullopt;
      result = result * 10U + static_cast<std::uint32_t>(value[index] - '0');
    }
    return result > 0 && result <= 999999 ? std::optional<std::uint32_t>{result}
                                          : std::nullopt;
  }
  static bool validUuid(std::string_view value) {
    if (value.size() != 36) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
      if (index == 8 || index == 13 || index == 18 || index == 23) {
        if (value[index] != '-') return false;
      } else if (!((value[index] >= '0' && value[index] <= '9') ||
                   (value[index] >= 'a' && value[index] <= 'f') ||
                   (value[index] >= 'A' && value[index] <= 'F'))) return false;
    }
    return true;
  }
  static bool uuidMatches(const std::uint8_t* bytes, std::string_view uuid) {
    std::array<std::uint8_t, 16> decoded{};
    std::size_t output{};
    for (std::size_t index = 0; index < uuid.size(); ++index) {
      if (uuid[index] == '-') continue;
      const auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
      };
      const int high = nibble(uuid[index]);
      if (high < 0 || ++index >= uuid.size()) return false;
      const int low = nibble(uuid[index]);
      if (low < 0 || output >= decoded.size()) return false;
      decoded[output++] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return output == decoded.size() && constantEqual(bytes, decoded.data(), decoded.size());
  }
  static std::string base64Url(const std::uint8_t* input, std::size_t size) {
    constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string output;
    output.reserve((size * 8U + 5U) / 6U);
    std::uint32_t accumulator{};
    unsigned bits{};
    for (std::size_t index = 0; index < size; ++index) {
      accumulator = (accumulator << 8U) | input[index];
      bits += 8U;
      while (bits >= 6U) {
        bits -= 6U;
        output.push_back(alphabet[(accumulator >> bits) & 0x3fU]);
      }
    }
    if (bits != 0) output.push_back(alphabet[(accumulator << (6U - bits)) & 0x3fU]);
    return output;
  }
  static bool decodeBase64Url(std::string_view input, std::uint8_t* output,
                              std::size_t outputSize) {
    if (input.size() != (outputSize * 8U + 5U) / 6U) return false;
    std::uint32_t accumulator{};
    unsigned bits{};
    std::size_t written{};
    for (char value : input) {
      int decoded = value >= 'A' && value <= 'Z' ? value - 'A'
                    : value >= 'a' && value <= 'z' ? value - 'a' + 26
                    : value >= '0' && value <= '9' ? value - '0' + 52
                    : value == '-' ? 62 : value == '_' ? 63 : -1;
      if (decoded < 0) return false;
      accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(decoded);
      bits += 6U;
      if (bits >= 8U) {
        bits -= 8U;
        if (written >= outputSize) return false;
        output[written++] = static_cast<std::uint8_t>(accumulator >> bits);
      }
    }
    return written == outputSize;
  }
  static std::uint16_t crc16(const std::uint8_t* input, std::size_t size) {
    std::uint16_t crc = 0xffff;
    for (std::size_t index = 0; index < size; ++index) {
      crc ^= static_cast<std::uint16_t>(input[index]) << 8U;
      for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc & 0x8000U) != 0 ? static_cast<std::uint16_t>((crc << 1U) ^ 0x1021U)
                                    : static_cast<std::uint16_t>(crc << 1U);
    }
    return crc;
  }
  static void write32(std::uint8_t* output, std::uint32_t value) {
    output[0] = static_cast<std::uint8_t>(value >> 24U);
    output[1] = static_cast<std::uint8_t>(value >> 16U);
    output[2] = static_cast<std::uint8_t>(value >> 8U);
    output[3] = static_cast<std::uint8_t>(value);
  }
  static std::uint32_t read24(const std::uint8_t* input) {
    return (static_cast<std::uint32_t>(input[0]) << 16U) |
           (static_cast<std::uint32_t>(input[1]) << 8U) | input[2];
  }
  static std::uint32_t read32(const std::uint8_t* input) {
    return (static_cast<std::uint32_t>(input[0]) << 24U) |
           (static_cast<std::uint32_t>(input[1]) << 16U) |
           (static_cast<std::uint32_t>(input[2]) << 8U) | input[3];
  }
  static bool constantEqual(const std::uint8_t* left, const std::uint8_t* right,
                            std::size_t size) {
    std::uint8_t difference{};
    for (std::size_t index = 0; index < size; ++index)
      difference |= static_cast<std::uint8_t>(left[index] ^ right[index]);
    return difference == 0;
  }
  static void secureZero(std::uint8_t* output, std::size_t size) noexcept {
    volatile std::uint8_t* cursor = output;
    for (std::size_t index = 0; index < size; ++index) cursor[index] = 0;
  }
  void clearBytes() noexcept {
    secureZero(nonce_.data(), nonce_.size());
    std::fill(uri_.begin(), uri_.end(), '\0');
    uri_.clear();
    std::fill(deviceId_.begin(), deviceId_.end(), '\0');
    deviceId_.clear();
  }

  QrRandomSource& random_;
  QrOnboardingState state_{QrOnboardingState::kDisabled};
  std::array<std::uint8_t, 16> nonce_{};
  std::string uri_;
  std::string deviceId_;
  std::uint32_t issuedAt_{};
  std::uint32_t expiresAt_{};
};

}  // namespace algaguard
