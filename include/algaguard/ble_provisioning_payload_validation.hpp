#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "algaguard/ble_provisioning_framed_transport.hpp"
#include "algaguard/ble_wifi_provisioning.hpp"

namespace algaguard {

inline constexpr std::string_view kBleProvisioningPayloadSchema =
    "urn:algaguard:schema:onboarding:ble-provisioning-request:v1";
inline constexpr std::string_view kBleProvisioningPayloadSchemaVersion = "1.0.0";
inline constexpr std::string_view kQrBleProvisioningPayloadSchema =
    "urn:algaguard:schema:onboarding:ble-provisioning-request:v2";
inline constexpr std::string_view kQrBleProvisioningPayloadSchemaVersion = "2.0.0";

struct BleProvisioningPayloadParseResult {
  BleWifiProvisioningReason safeReason{BleWifiProvisioningReason::MALFORMED_PAYLOAD};
  std::optional<BleWifiProvisioningRequest> request;
  bool accepted() const { return request.has_value(); }
};

class BleProvisioningPayloadParser {
 public:
  BleProvisioningPayloadParseResult parse(const std::uint8_t* bytes, std::size_t length) {
    clearScratch();
    if (bytes == nullptr || length == 0 || length > kBleProvisioningMaxWriteBytes)
      return failure(BleWifiProvisioningReason::MALFORMED_PAYLOAD);
    input_ = bytes;
    length_ = length;
    index_ = 0;
    BleWifiProvisioningRequestBuilder builder;
    std::uint16_t fields{};
    protocolVersion_ = 0;
    schemaVersion_ = 0;
    skipWhitespace();
    if (!consume('{')) return failure(BleWifiProvisioningReason::MALFORMED_PAYLOAD);
    skipWhitespace();
    if (peek('}')) return failure(BleWifiProvisioningReason::MALFORMED_PAYLOAD);
    while (true) {
      if (!readString() || scratchSize_ >= key_.size())
        return failure(BleWifiProvisioningReason::MALFORMED_PAYLOAD);
      copyScratch(key_);
      clearValueScratch();
      skipWhitespace();
      if (!consume(':')) return failure(BleWifiProvisioningReason::MALFORMED_PAYLOAD);
      skipWhitespace();
      if (!readString()) return failure(BleWifiProvisioningReason::MALFORMED_PAYLOAD);
      const std::string_view value{scratch_.data(), scratchSize_};
      const auto key = std::string_view{key_.data()};
      if (!assignField(builder, fields, key, value)) {
        clearValueScratch();
        return failure(BleWifiProvisioningReason::MALFORMED_PAYLOAD);
      }
      clearValueScratch();
      skipWhitespace();
      if (consume('}')) break;
      if (!consume(',')) return failure(BleWifiProvisioningReason::MALFORMED_PAYLOAD);
      skipWhitespace();
    }
    skipWhitespace();
    const auto required = protocolVersion_ == 2 ? kQrRequiredFields : kRequiredFields;
    if (index_ != length_ || protocolVersion_ == 0 ||
        schemaVersion_ != protocolVersion_ || (fields & required) != required)
      return failure(BleWifiProvisioningReason::MALFORMED_PAYLOAD);
    builder.protocolVersion(protocolVersion_);
    auto request = builder.build();
    const auto structural = request.structuralReasonForAdapter();
    if (structural != BleWifiProvisioningReason::OK) return failure(structural);
    clearScratch();
    return {BleWifiProvisioningReason::OK, std::optional<BleWifiProvisioningRequest>{std::move(request)}};
  }

  bool scratchCleared() const { return scratchSize_ == 0 && scratchIsZero(); }
  void clear() noexcept { clearScratch(); }

 private:
  static constexpr std::uint16_t kSchema = 0x01;
  static constexpr std::uint16_t kSchemaVersion = 0x02;
  static constexpr std::uint16_t kSessionId = 0x04;
  static constexpr std::uint16_t kDeviceId = 0x08;
  static constexpr std::uint16_t kSessionToken = 0x10;
  static constexpr std::uint16_t kSsid = 0x20;
  static constexpr std::uint16_t kPassword = 0x40;
  static constexpr std::uint16_t kBindingGrant = 0x80;
  static constexpr std::uint16_t kRequiredFields =
      kSchema | kSchemaVersion | kSessionId | kDeviceId | kSessionToken | kSsid | kPassword;
  static constexpr std::uint16_t kQrRequiredFields = kRequiredFields | kBindingGrant;

  BleProvisioningPayloadParseResult failure(BleWifiProvisioningReason reason) noexcept {
    clearScratch();
    return {reason, std::nullopt};
  }

  bool assignField(BleWifiProvisioningRequestBuilder& builder, std::uint16_t& fields,
                   std::string_view key, std::string_view value) {
    const auto assign = [&fields](std::uint16_t bit) {
      if ((fields & bit) != 0) return false;
      fields |= bit;
      return true;
    };
    if (key == "schema") {
      if (!assign(kSchema)) return false;
      protocolVersion_ = value == kBleProvisioningPayloadSchema ? 1
                         : value == kQrBleProvisioningPayloadSchema ? 2 : 0;
      return protocolVersion_ != 0;
    }
    if (key == "schemaVersion") {
      if (!assign(kSchemaVersion)) return false;
      schemaVersion_ = value == kBleProvisioningPayloadSchemaVersion ? 1
                       : value == kQrBleProvisioningPayloadSchemaVersion ? 2 : 0;
      return schemaVersion_ != 0;
    }
    if (key == "sessionId")
      return assign(kSessionId) && validUuid(value) && (builder.sessionId(value), true);
    if (key == "deviceId") return assign(kDeviceId) && (builder.deviceId(value), true);
    if (key == "sessionToken")
      return assign(kSessionToken) && validSessionToken(value) && (builder.sessionToken(value), true);
    if (key == "ssid") return assign(kSsid) && (builder.ssid(value), true);
    if (key == "password") return assign(kPassword) && (builder.password(value), true);
    if (key == "bindingGrant")
      return assign(kBindingGrant) && validBindingGrant(value) &&
             (builder.bindingGrant(value), true);
    return false;
  }

  static bool validUuid(std::string_view value) {
    if (value.size() != 36) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
      if (index == 8 || index == 13 || index == 18 || index == 23) {
        if (value[index] != '-') return false;
      } else if (!((value[index] >= '0' && value[index] <= '9') ||
                   (value[index] >= 'a' && value[index] <= 'f') ||
                   (value[index] >= 'A' && value[index] <= 'F'))) {
        return false;
      }
    }
    return true;
  }

  static bool validSessionToken(std::string_view value) {
    if (value.size() < 32 || value.size() > 96) return false;
    for (const char character : value)
      if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '_' || character == '-'))
        return false;
    return true;
  }
  static bool validBindingGrant(std::string_view value) {
    if (value.size() < 160 || value.size() > 256) return false;
    for (const char character : value)
      if (!((character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '_' ||
            character == '-')) return false;
    return true;
  }

  void skipWhitespace() {
    while (index_ < length_ && (input_[index_] == ' ' || input_[index_] == '\n' ||
                                input_[index_] == '\r' || input_[index_] == '\t'))
      ++index_;
  }

  bool consume(char expected) {
    if (index_ >= length_ || input_[index_] != static_cast<std::uint8_t>(expected)) return false;
    ++index_;
    return true;
  }

  bool peek(char expected) const {
    return index_ < length_ && input_[index_] == static_cast<std::uint8_t>(expected);
  }

  bool readString() {
    clearValueScratch();
    if (!consume('"')) return false;
    while (index_ < length_) {
      const auto byte = input_[index_++];
      if (byte == '"') return true;
      if (byte < 0x20) return false;
      if (byte == '\\') {
        if (!readEscape()) return false;
      } else if (byte < 0x80) {
        if (!append(static_cast<char>(byte))) return false;
      } else if (!readUtf8(byte)) {
        return false;
      }
    }
    return false;
  }

  bool readEscape() {
    if (index_ >= length_) return false;
    const auto escaped = static_cast<char>(input_[index_++]);
    switch (escaped) {
      case '"': return append('"');
      case '\\': return append('\\');
      case '/': return append('/');
      case 'b': return append('\b');
      case 'f': return append('\f');
      case 'n': return append('\n');
      case 'r': return append('\r');
      case 't': return append('\t');
      case 'u': return readUnicodeEscape();
      default: return false;
    }
  }

  bool readUnicodeEscape() {
    if (index_ + 4 > length_) return false;
    std::uint32_t value{};
    for (std::size_t count = 0; count < 4; ++count) {
      const auto byte = input_[index_++];
      value <<= 4U;
      if (byte >= '0' && byte <= '9') value |= byte - '0';
      else if (byte >= 'a' && byte <= 'f') value |= byte - 'a' + 10U;
      else if (byte >= 'A' && byte <= 'F') value |= byte - 'A' + 10U;
      else return false;
    }
    if (value == 0 || (value >= 0xd800 && value <= 0xdfff)) return false;
    if (value <= 0x7f) return append(static_cast<char>(value));
    if (value <= 0x7ff)
      return append(static_cast<char>(0xc0 | (value >> 6U))) &&
             append(static_cast<char>(0x80 | (value & 0x3fU)));
    return append(static_cast<char>(0xe0 | (value >> 12U))) &&
           append(static_cast<char>(0x80 | ((value >> 6U) & 0x3fU))) &&
           append(static_cast<char>(0x80 | (value & 0x3fU)));
  }

  bool readUtf8(std::uint8_t first) {
    std::size_t trailing{};
    std::uint32_t codepoint{};
    if (first >= 0xc2 && first <= 0xdf) { trailing = 1; codepoint = first & 0x1f; }
    else if (first >= 0xe0 && first <= 0xef) { trailing = 2; codepoint = first & 0x0f; }
    else if (first >= 0xf0 && first <= 0xf4) { trailing = 3; codepoint = first & 0x07; }
    else return false;
    std::array<std::uint8_t, 4> encoded{};
    encoded[0] = first;
    for (std::size_t count = 0; count < trailing; ++count) {
      if (index_ >= length_) return false;
      const auto continuation = input_[index_++];
      if ((continuation & 0xc0U) != 0x80U) return false;
      encoded[count + 1] = continuation;
      codepoint = (codepoint << 6U) | (continuation & 0x3fU);
    }
    const auto minimum = trailing == 1 ? 0x80U : trailing == 2 ? 0x800U : 0x10000U;
    if (codepoint < minimum || codepoint > 0x10ffffU ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU)) return false;
    for (std::size_t count = 0; count <= trailing; ++count)
      if (!append(static_cast<char>(encoded[count]))) return false;
    return true;
  }

  template <std::size_t Count>
  void copyScratch(std::array<char, Count>& destination) {
    destination.fill(0);
    for (std::size_t count = 0; count < scratchSize_; ++count) destination[count] = scratch_[count];
  }

  bool append(char value) {
    if (scratchSize_ >= kBleProvisioningMaxWriteBytes) return false;
    scratch_[scratchSize_++] = value;
    return true;
  }

  bool scratchIsZero() const {
    for (const char value : scratch_) if (value != 0) return false;
    return true;
  }

  void clearScratch() noexcept {
    clearValueScratch();
    key_.fill(0);
  }

  void clearValueScratch() noexcept {
    volatile char* destination = scratch_.data();
    for (std::size_t count = 0; count < scratch_.size(); ++count) destination[count] = 0;
    scratchSize_ = 0;
  }

  std::array<char, kBleProvisioningMaxWriteBytes + 1> scratch_{};
  std::array<char, 32> key_{};
  const std::uint8_t* input_{};
  std::size_t length_{};
  std::size_t index_{};
  std::size_t scratchSize_{};
  std::uint16_t protocolVersion_{};
  std::uint16_t schemaVersion_{};
};

class QrBleSessionAuthorizer {
 public:
  virtual ~QrBleSessionAuthorizer() = default;
  virtual std::optional<std::uint64_t> authorize(
      const BleWifiProvisioningRequest& request, std::uint64_t nowTick) = 0;
};

struct BleProvisioningValidationResult {
  bool accepted{};
  BleProvisioningSafeStatus safeStatus{BleProvisioningSafeStatus::kRejected};
  BleProvisioningSafeReason safeReason{BleProvisioningSafeReason::kMalformedPayload};
  bool secretsCleared{};
};

class BleProvisioningPayloadValidation {
 public:
  void setQrAuthorizer(QrBleSessionAuthorizer* authorizer) {
    qrAuthorizer_ = authorizer;
  }
  bool qrAuthorizerAvailable() const { return qrAuthorizer_ != nullptr; }
  bool installDevelopmentSession(std::string_view sessionId, std::string_view deviceId,
                                 std::string_view sessionToken, std::uint64_t expiryTick) {
    clear();
    machine_.reset();
    const auto result = machine_.prepareSession(sessionId, deviceId, sessionToken, expiryTick);
    configured_ = result.finalState == BleWifiProvisioningState::SESSION_READY;
    return configured_;
  }

  void onBleConnected() {
    if (configured_ && machine_.state() == BleWifiProvisioningState::SESSION_READY)
      machine_.connectBle();
  }

  BleProvisioningValidationResult consume(BleProvisioningPayloadHandoff&& payload,
                                          std::uint64_t nowTick) {
    auto parsed = parser_.parse(payload.data(), payload.size());
    payload.clear();
    if (!parsed.accepted()) return rejected(parsed.safeReason);
    auto request = std::move(*parsed.request);
    if (request.protocolVersion() == BleWifiProvisioningRequest::kQrProtocolVersion) {
      if (configured_ || qrAuthorizer_ == nullptr)
        return rejected(BleWifiProvisioningReason::INVALID_TRANSITION);
      const auto expiryTick = qrAuthorizer_->authorize(request, nowTick);
      if (!expiryTick ||
          machine_.prepareSession(request.sessionId(), request.deviceId(),
                                  request.sessionToken(), *expiryTick).finalState !=
              BleWifiProvisioningState::SESSION_READY) {
        request.clear();
        return rejected(BleWifiProvisioningReason::SESSION_MISMATCH);
      }
      configured_ = true;
      machine_.connectBle();
    }
    if (!configured_) return rejected(BleWifiProvisioningReason::INVALID_TRANSITION);
    if (machine_.state() == BleWifiProvisioningState::BLE_CONNECTED) machine_.beginPayload();
    const auto validated = machine_.validatePayload(std::move(request), nowTick);
    if (validated.safeReasonCode != BleWifiProvisioningReason::OK)
      return rejected(validated.safeReasonCode);
    auto candidate = machine_.takeValidatedCredentials();
    const auto accepted = machine_.acceptValidated();
    if (!accepted.accepted || !candidate.available()) {
      candidate.clear();
      return rejected(BleWifiProvisioningReason::INVALID_TRANSITION);
    }
    acceptedCredentials_.clear();
    acceptedCredentials_ = std::move(candidate);
    return {true, BleProvisioningSafeStatus::kAccepted, BleProvisioningSafeReason::kOk,
            machine_.secretsCleared() && parser_.scratchCleared()};
  }

  BleWifiCredentialHandoff takeAcceptedCredentials() { return std::move(acceptedCredentials_); }
  bool hasSession() const { return configured_; }
  bool hasAcceptedCredentials() const { return acceptedCredentials_.available(); }
  bool secretsCleared() const { return machine_.secretsCleared() && parser_.scratchCleared(); }

  BleProvisioningValidationResult onTick(std::uint64_t nowTick) {
    if (!configured_ || machine_.state() == BleWifiProvisioningState::ACCEPTED)
      return {false, BleProvisioningSafeStatus::kReady, BleProvisioningSafeReason::kOk,
              secretsCleared()};
    const auto result = machine_.pollExpiry(nowTick);
    if (result.safeReasonCode == BleWifiProvisioningReason::SESSION_EXPIRED)
      return rejected(result.safeReasonCode);
    return {false, BleProvisioningSafeStatus::kReady, BleProvisioningSafeReason::kOk,
            secretsCleared()};
  }

  void onDisconnected() noexcept {
    machine_.reset();
    configured_ = false;
  }

  void clear() noexcept {
    parser_.clear();
    machine_.reset();
    acceptedCredentials_.clear();
    configured_ = false;
  }

 private:
  static BleProvisioningSafeReason mapReason(BleWifiProvisioningReason reason) {
    switch (reason) {
      case BleWifiProvisioningReason::OK: return BleProvisioningSafeReason::kOk;
      case BleWifiProvisioningReason::MALFORMED_PAYLOAD: return BleProvisioningSafeReason::kMalformedPayload;
      case BleWifiProvisioningReason::UNSUPPORTED_VERSION: return BleProvisioningSafeReason::kUnsupportedVersion;
      case BleWifiProvisioningReason::DEVICE_ID_MISMATCH: return BleProvisioningSafeReason::kDeviceIdMismatch;
      case BleWifiProvisioningReason::SESSION_MISMATCH: return BleProvisioningSafeReason::kSessionMismatch;
      case BleWifiProvisioningReason::SESSION_EXPIRED: return BleProvisioningSafeReason::kSessionExpired;
      case BleWifiProvisioningReason::REPLAY_REJECTED: return BleProvisioningSafeReason::kReplayRejected;
      case BleWifiProvisioningReason::FIELD_TOO_LONG: return BleProvisioningSafeReason::kFieldTooLong;
      case BleWifiProvisioningReason::INVALID_TRANSITION: return BleProvisioningSafeReason::kInvalidTransition;
      case BleWifiProvisioningReason::CANCELLED: return BleProvisioningSafeReason::kCancelled;
    }
    return BleProvisioningSafeReason::kInvalidTransition;
  }

  BleProvisioningValidationResult rejected(BleWifiProvisioningReason reason) {
    parser_.clear();
    machine_.reset();
    acceptedCredentials_.clear();
    configured_ = false;
    return {false, BleProvisioningSafeStatus::kRejected, mapReason(reason), true};
  }

  BleProvisioningPayloadParser parser_;
  BleWifiProvisioningStateMachine machine_;
  BleWifiCredentialHandoff acceptedCredentials_;
  bool configured_{};
  QrBleSessionAuthorizer* qrAuthorizer_{};
};

}  // namespace algaguard
