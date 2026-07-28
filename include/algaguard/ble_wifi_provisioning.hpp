#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "algaguard/config.hpp"

namespace algaguard {

enum class BleWifiProvisioningState {
  IDLE,
  SESSION_READY,
  BLE_CONNECTED,
  PAYLOAD_RECEIVING,
  PAYLOAD_VALIDATED,
  ACCEPTED,
  REJECTED,
  EXPIRED,
  CANCELLED,
  CLEARED,
};

enum class BleWifiProvisioningReason {
  OK,
  MALFORMED_PAYLOAD,
  UNSUPPORTED_VERSION,
  DEVICE_ID_MISMATCH,
  SESSION_MISMATCH,
  SESSION_EXPIRED,
  REPLAY_REJECTED,
  FIELD_TOO_LONG,
  INVALID_TRANSITION,
  CANCELLED,
};

inline const char* provisioning_state_code(BleWifiProvisioningState state) {
  switch (state) {
    case BleWifiProvisioningState::IDLE: return "IDLE";
    case BleWifiProvisioningState::SESSION_READY: return "SESSION_READY";
    case BleWifiProvisioningState::BLE_CONNECTED: return "BLE_CONNECTED";
    case BleWifiProvisioningState::PAYLOAD_RECEIVING: return "PAYLOAD_RECEIVING";
    case BleWifiProvisioningState::PAYLOAD_VALIDATED: return "PAYLOAD_VALIDATED";
    case BleWifiProvisioningState::ACCEPTED: return "ACCEPTED";
    case BleWifiProvisioningState::REJECTED: return "REJECTED";
    case BleWifiProvisioningState::EXPIRED: return "EXPIRED";
    case BleWifiProvisioningState::CANCELLED: return "CANCELLED";
    case BleWifiProvisioningState::CLEARED: return "CLEARED";
  }
  return "REJECTED";
}

inline const char* provisioning_reason_code(BleWifiProvisioningReason reason) {
  switch (reason) {
    case BleWifiProvisioningReason::OK: return "OK";
    case BleWifiProvisioningReason::MALFORMED_PAYLOAD: return "MALFORMED_PAYLOAD";
    case BleWifiProvisioningReason::UNSUPPORTED_VERSION: return "UNSUPPORTED_VERSION";
    case BleWifiProvisioningReason::DEVICE_ID_MISMATCH: return "DEVICE_ID_MISMATCH";
    case BleWifiProvisioningReason::SESSION_MISMATCH: return "SESSION_MISMATCH";
    case BleWifiProvisioningReason::SESSION_EXPIRED: return "SESSION_EXPIRED";
    case BleWifiProvisioningReason::REPLAY_REJECTED: return "REPLAY_REJECTED";
    case BleWifiProvisioningReason::FIELD_TOO_LONG: return "FIELD_TOO_LONG";
    case BleWifiProvisioningReason::INVALID_TRANSITION: return "INVALID_TRANSITION";
    case BleWifiProvisioningReason::CANCELLED: return "CANCELLED";
  }
  return "INVALID_TRANSITION";
}

struct BleWifiProvisioningResult {
  bool accepted{};
  BleWifiProvisioningState finalState{BleWifiProvisioningState::IDLE};
  BleWifiProvisioningReason safeReasonCode{BleWifiProvisioningReason::OK};
  bool retryAllowed{};
  bool secretsCleared{};

  std::string diagnostics() const {
    return std::string{"state="} + provisioning_state_code(finalState) +
           " reason=" + provisioning_reason_code(safeReasonCode);
  }
};

namespace detail {

enum class FieldAssignment { OK, TOO_LONG, EMBEDDED_NUL };

template <std::size_t Maximum>
class ProvisioningFieldBuffer {
 public:
  ProvisioningFieldBuffer() = default;
  ProvisioningFieldBuffer(const ProvisioningFieldBuffer&) = delete;
  ProvisioningFieldBuffer& operator=(const ProvisioningFieldBuffer&) = delete;

  ProvisioningFieldBuffer(ProvisioningFieldBuffer&& other) noexcept { move_from(other); }
  ProvisioningFieldBuffer& operator=(ProvisioningFieldBuffer&& other) noexcept {
    if (this != &other) {
      clear();
      move_from(other);
    }
    return *this;
  }

  ~ProvisioningFieldBuffer() { clear(); }

  FieldAssignment assign(std::string_view value) {
    clear();
    if (value.size() > Maximum) return FieldAssignment::TOO_LONG;
    if (value.find('\0') != std::string_view::npos) return FieldAssignment::EMBEDDED_NUL;
    std::copy(value.begin(), value.end(), bytes_.begin());
    size_ = value.size();
    return FieldAssignment::OK;
  }

  void clear() noexcept {
    volatile char* output = bytes_.data();
    for (std::size_t index = 0; index < bytes_.size(); ++index) output[index] = 0;
    size_ = 0;
  }

  bool empty() const { return size_ == 0; }
  std::string_view view() const { return {bytes_.data(), size_}; }

  bool secure_equals(const ProvisioningFieldBuffer& other) const {
    std::size_t difference = size_ ^ other.size_;
    for (std::size_t index = 0; index < Maximum; ++index) {
      difference |= static_cast<unsigned char>(bytes_[index]) ^
                    static_cast<unsigned char>(other.bytes_[index]);
    }
    return difference == 0;
  }

 private:
  void move_from(ProvisioningFieldBuffer& other) noexcept {
    bytes_ = other.bytes_;
    size_ = other.size_;
    other.clear();
  }

  std::array<char, Maximum + 1> bytes_{};
  std::size_t size_{};
};

}  // namespace detail

class BleWifiProvisioningRequestBuilder;

class BleWifiCredentialHandoff {
 public:
  BleWifiCredentialHandoff() = default;
  BleWifiCredentialHandoff(const BleWifiCredentialHandoff&) = delete;
  BleWifiCredentialHandoff& operator=(const BleWifiCredentialHandoff&) = delete;
  BleWifiCredentialHandoff(BleWifiCredentialHandoff&&) noexcept = default;
  BleWifiCredentialHandoff& operator=(BleWifiCredentialHandoff&&) noexcept = default;
  ~BleWifiCredentialHandoff() { clear(); }

  bool available() const { return !ssid_.empty() && !password_.empty(); }
  std::string_view ssid() const { return ssid_.view(); }
  std::string_view password() const { return password_.view(); }
  bool secretsCleared() const { return password_.empty(); }
  void clear() noexcept {
    ssid_.clear();
    password_.clear();
  }

 private:
  friend class BleWifiProvisioningStateMachine;
  detail::ProvisioningFieldBuffer<32> ssid_;
  detail::ProvisioningFieldBuffer<63> password_;
};

class BleWifiProvisioningRequest {
 public:
  static constexpr std::uint16_t kProtocolVersion = 1;
  static constexpr std::size_t kMaxSessionIdBytes = 64;
  static constexpr std::size_t kMaxDeviceIdBytes = 9;
  static constexpr std::size_t kMaxSessionTokenBytes = 512;
  static constexpr std::size_t kMaxSsidBytes = 32;
  static constexpr std::size_t kMaxPasswordBytes = 63;

  BleWifiProvisioningRequest(const BleWifiProvisioningRequest&) = delete;
  BleWifiProvisioningRequest& operator=(const BleWifiProvisioningRequest&) = delete;
  BleWifiProvisioningRequest(BleWifiProvisioningRequest&&) noexcept = default;
  BleWifiProvisioningRequest& operator=(BleWifiProvisioningRequest&&) noexcept = default;
  ~BleWifiProvisioningRequest() { clear(); }

  std::uint16_t protocolVersion() const { return protocol_version_; }
  std::string_view sessionId() const { return session_id_.view(); }
  std::string_view deviceId() const { return device_id_.view(); }
  std::string_view ssid() const { return ssid_.view(); }
  bool secretsCleared() const { return session_token_.empty() && password_.empty(); }
  BleWifiProvisioningReason structuralReasonForAdapter() const { return structural_reason(); }

  void clear() noexcept {
    session_id_.clear();
    device_id_.clear();
    session_token_.clear();
    ssid_.clear();
    password_.clear();
    present_fields_ = 0;
  }

 private:
  friend class BleWifiProvisioningRequestBuilder;
  friend class BleWifiProvisioningStateMachine;

  BleWifiProvisioningRequest() = default;

  BleWifiProvisioningReason structural_reason() const {
    constexpr std::uint8_t required = 0x1f;
    if (input_reason_ != BleWifiProvisioningReason::OK) return input_reason_;
    if ((present_fields_ & required) != required || session_id_.empty() ||
        device_id_.empty() || session_token_.empty() || ssid_.empty() ||
        password_.empty()) {
      return BleWifiProvisioningReason::MALFORMED_PAYLOAD;
    }
    if (protocol_version_ != kProtocolVersion) {
      return BleWifiProvisioningReason::UNSUPPORTED_VERSION;
    }
    if (!valid_device_id(device_id_.view())) {
      return BleWifiProvisioningReason::MALFORMED_PAYLOAD;
    }
    return BleWifiProvisioningReason::OK;
  }

  std::uint16_t protocol_version_{kProtocolVersion};
  std::uint8_t present_fields_{};
  BleWifiProvisioningReason input_reason_{BleWifiProvisioningReason::OK};
  detail::ProvisioningFieldBuffer<kMaxSessionIdBytes> session_id_;
  detail::ProvisioningFieldBuffer<kMaxDeviceIdBytes> device_id_;
  detail::ProvisioningFieldBuffer<kMaxSessionTokenBytes> session_token_;
  detail::ProvisioningFieldBuffer<kMaxSsidBytes> ssid_;
  detail::ProvisioningFieldBuffer<kMaxPasswordBytes> password_;
};

class BleWifiProvisioningRequestBuilder {
 public:
  BleWifiProvisioningRequestBuilder() = default;
  BleWifiProvisioningRequestBuilder(const BleWifiProvisioningRequestBuilder&) = delete;
  BleWifiProvisioningRequestBuilder& operator=(const BleWifiProvisioningRequestBuilder&) = delete;

  BleWifiProvisioningRequestBuilder& protocolVersion(std::uint16_t value) {
    if (protocol_set_) mark_duplicate();
    protocol_set_ = true;
    request_.protocol_version_ = value;
    return *this;
  }

  BleWifiProvisioningRequestBuilder& sessionId(std::string_view value) {
    assign(0x01, request_.session_id_, value);
    return *this;
  }
  BleWifiProvisioningRequestBuilder& deviceId(std::string_view value) {
    assign(0x02, request_.device_id_, value);
    return *this;
  }
  BleWifiProvisioningRequestBuilder& sessionToken(std::string_view value) {
    assign(0x04, request_.session_token_, value);
    return *this;
  }
  BleWifiProvisioningRequestBuilder& ssid(std::string_view value) {
    assign(0x08, request_.ssid_, value);
    return *this;
  }
  BleWifiProvisioningRequestBuilder& password(std::string_view value) {
    assign(0x10, request_.password_, value);
    return *this;
  }

  BleWifiProvisioningRequest build() { return std::move(request_); }

 private:
  template <std::size_t Maximum>
  void assign(std::uint8_t field, detail::ProvisioningFieldBuffer<Maximum>& target,
              std::string_view value) {
    if ((request_.present_fields_ & field) != 0) {
      mark_duplicate();
      target.clear();
      return;
    }
    request_.present_fields_ |= field;
    const auto result = target.assign(value);
    if (result == detail::FieldAssignment::TOO_LONG) {
      mark_error(BleWifiProvisioningReason::FIELD_TOO_LONG);
    } else if (result == detail::FieldAssignment::EMBEDDED_NUL) {
      mark_error(BleWifiProvisioningReason::MALFORMED_PAYLOAD);
    }
  }

  void mark_duplicate() { mark_error(BleWifiProvisioningReason::MALFORMED_PAYLOAD); }
  void mark_error(BleWifiProvisioningReason reason) {
    if (request_.input_reason_ == BleWifiProvisioningReason::OK ||
        reason == BleWifiProvisioningReason::FIELD_TOO_LONG) {
      request_.input_reason_ = reason;
    }
  }

  bool protocol_set_{};
  BleWifiProvisioningRequest request_;
};

class BleWifiProvisioningStateMachine {
 public:
  BleWifiProvisioningStateMachine() = default;
  BleWifiProvisioningStateMachine(const BleWifiProvisioningStateMachine&) = delete;
  BleWifiProvisioningStateMachine& operator=(const BleWifiProvisioningStateMachine&) = delete;
  ~BleWifiProvisioningStateMachine() { clear(); }

  BleWifiProvisioningState state() const { return state_; }
  bool secretsCleared() const {
    return session_token_.empty() &&
           (!pending_request_.has_value() || pending_request_->secretsCleared());
  }

  BleWifiProvisioningResult prepareSession(std::string_view session_id,
                                           std::string_view device_id,
                                           std::string_view session_token,
                                           std::uint64_t expires_at) {
    if (state_ != BleWifiProvisioningState::IDLE) return invalid_transition();
    if (!assign_session(session_id, device_id, session_token) || expires_at == 0) {
      return reject(BleWifiProvisioningReason::MALFORMED_PAYLOAD, true);
    }
    expires_at_ = expires_at;
    state_ = BleWifiProvisioningState::SESSION_READY;
    return result(false, BleWifiProvisioningReason::OK, false);
  }

  BleWifiProvisioningResult connectBle() {
    return transition(BleWifiProvisioningState::SESSION_READY,
                      BleWifiProvisioningState::BLE_CONNECTED);
  }

  BleWifiProvisioningResult beginPayload() {
    return transition(BleWifiProvisioningState::BLE_CONNECTED,
                      BleWifiProvisioningState::PAYLOAD_RECEIVING);
  }

  BleWifiProvisioningResult validatePayload(BleWifiProvisioningRequest&& request,
                                            std::uint64_t now) {
    if (state_ == BleWifiProvisioningState::ACCEPTED) {
      request.clear();
      return result(false, BleWifiProvisioningReason::REPLAY_REJECTED, false);
    }
    if (state_ != BleWifiProvisioningState::PAYLOAD_RECEIVING) {
      request.clear();
      return invalid_transition();
    }
    const auto structural = request.structural_reason();
    if (structural != BleWifiProvisioningReason::OK) {
      request.clear();
      return reject(structural, structural != BleWifiProvisioningReason::UNSUPPORTED_VERSION);
    }
    if (request.device_id_.view() != device_id_.view()) {
      request.clear();
      return reject(BleWifiProvisioningReason::DEVICE_ID_MISMATCH, true);
    }
    if (request.session_id_.view() != session_id_.view() ||
        !request.session_token_.secure_equals(session_token_)) {
      request.clear();
      return reject(BleWifiProvisioningReason::SESSION_MISMATCH, true);
    }
    if (now >= expires_at_) {
      request.clear();
      state_ = BleWifiProvisioningState::EXPIRED;
      clear_secrets();
      return result(false, BleWifiProvisioningReason::SESSION_EXPIRED, false);
    }
    pending_request_.emplace(std::move(request));
    state_ = BleWifiProvisioningState::PAYLOAD_VALIDATED;
    return result(false, BleWifiProvisioningReason::OK, false);
  }

  BleWifiProvisioningResult acceptValidated() {
    if (state_ != BleWifiProvisioningState::PAYLOAD_VALIDATED) {
      return invalid_transition();
    }
    state_ = BleWifiProvisioningState::ACCEPTED;
    accepted_once_ = true;
    clear_secrets();
    return result(true, BleWifiProvisioningReason::OK, false);
  }

  BleWifiCredentialHandoff takeValidatedCredentials() {
    BleWifiCredentialHandoff handoff;
    if (state_ != BleWifiProvisioningState::PAYLOAD_VALIDATED || !pending_request_) return handoff;
    if (handoff.ssid_.assign(pending_request_->ssid_.view()) != detail::FieldAssignment::OK ||
        handoff.password_.assign(pending_request_->password_.view()) != detail::FieldAssignment::OK) {
      handoff.clear();
      return handoff;
    }
    pending_request_->clear();
    pending_request_.reset();
    return handoff;
  }

  void reset() noexcept {
    clear_secrets();
    session_id_.clear();
    device_id_.clear();
    expires_at_ = 0;
    accepted_once_ = false;
    last_reason_ = BleWifiProvisioningReason::OK;
    state_ = BleWifiProvisioningState::IDLE;
  }

  BleWifiProvisioningResult expire(std::uint64_t now) {
    if (!active_state(state_) || now < expires_at_) return invalid_transition();
    state_ = BleWifiProvisioningState::EXPIRED;
    clear_secrets();
    return result(false, BleWifiProvisioningReason::SESSION_EXPIRED, false);
  }

  BleWifiProvisioningResult pollExpiry(std::uint64_t now) {
    if (!active_state(state_) || now < expires_at_)
      return result(false, BleWifiProvisioningReason::OK, false);
    return expire(now);
  }

  BleWifiProvisioningResult cancel() {
    if (!active_state(state_)) return invalid_transition();
    state_ = BleWifiProvisioningState::CANCELLED;
    clear_secrets();
    return result(false, BleWifiProvisioningReason::CANCELLED, false);
  }

  BleWifiProvisioningResult clear() noexcept {
    clear_secrets();
    session_id_.clear();
    device_id_.clear();
    expires_at_ = 0;
    if (terminal_state(state_)) state_ = BleWifiProvisioningState::CLEARED;
    return result(false, last_reason_, false);
  }

 private:
  using SessionId = detail::ProvisioningFieldBuffer<BleWifiProvisioningRequest::kMaxSessionIdBytes>;
  using DeviceId = detail::ProvisioningFieldBuffer<BleWifiProvisioningRequest::kMaxDeviceIdBytes>;
  using SessionToken = detail::ProvisioningFieldBuffer<BleWifiProvisioningRequest::kMaxSessionTokenBytes>;

  static bool active_state(BleWifiProvisioningState state) {
    return state == BleWifiProvisioningState::SESSION_READY ||
           state == BleWifiProvisioningState::BLE_CONNECTED ||
           state == BleWifiProvisioningState::PAYLOAD_RECEIVING ||
           state == BleWifiProvisioningState::PAYLOAD_VALIDATED;
  }

  static bool terminal_state(BleWifiProvisioningState state) {
    return state == BleWifiProvisioningState::ACCEPTED ||
           state == BleWifiProvisioningState::REJECTED ||
           state == BleWifiProvisioningState::EXPIRED ||
           state == BleWifiProvisioningState::CANCELLED;
  }

  bool assign_session(std::string_view session_id, std::string_view device_id,
                      std::string_view session_token) {
    const bool assigned = session_id_.assign(session_id) == detail::FieldAssignment::OK &&
                          device_id_.assign(device_id) == detail::FieldAssignment::OK &&
                          session_token_.assign(session_token) == detail::FieldAssignment::OK;
    return assigned && !session_id_.empty() && valid_device_id(device_id_.view()) &&
           !session_token_.empty();
  }

  void clear_secrets() noexcept {
    if (pending_request_) pending_request_->clear();
    pending_request_.reset();
    session_token_.clear();
  }

  BleWifiProvisioningResult transition(BleWifiProvisioningState expected,
                                       BleWifiProvisioningState next) {
    if (state_ != expected) return invalid_transition();
    state_ = next;
    return result(false, BleWifiProvisioningReason::OK, false);
  }

  BleWifiProvisioningResult invalid_transition() {
    if (state_ == BleWifiProvisioningState::ACCEPTED || accepted_once_) {
      clear_secrets();
      return result(false, BleWifiProvisioningReason::REPLAY_REJECTED, false);
    }
    state_ = BleWifiProvisioningState::REJECTED;
    clear_secrets();
    return result(false, BleWifiProvisioningReason::INVALID_TRANSITION, false);
  }

  BleWifiProvisioningResult reject(BleWifiProvisioningReason reason, bool retry_allowed) {
    state_ = BleWifiProvisioningState::REJECTED;
    clear_secrets();
    return result(false, reason, retry_allowed);
  }

  BleWifiProvisioningResult result(bool accepted, BleWifiProvisioningReason reason,
                                   bool retry_allowed) noexcept {
    last_reason_ = reason;
    return {accepted, state_, reason, retry_allowed, secretsCleared()};
  }

  BleWifiProvisioningState state_{BleWifiProvisioningState::IDLE};
  BleWifiProvisioningReason last_reason_{BleWifiProvisioningReason::OK};
  bool accepted_once_{};
  std::uint64_t expires_at_{};
  SessionId session_id_;
  DeviceId device_id_;
  SessionToken session_token_;
  std::optional<BleWifiProvisioningRequest> pending_request_;
};

}  // namespace algaguard
