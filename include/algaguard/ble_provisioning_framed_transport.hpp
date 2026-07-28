#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include "algaguard/ble_provisioning_gatt.hpp"

namespace algaguard {

inline constexpr std::uint8_t kBleProvisioningFrameProtocolVersion = 1;
inline constexpr std::size_t kBleProvisioningFrameHeaderBytes = 11;
inline constexpr std::size_t kBleProvisioningMaxFramePayloadBytes = 256;
inline constexpr std::size_t kBleProvisioningMaxFrameBytes =
    kBleProvisioningFrameHeaderBytes + kBleProvisioningMaxFramePayloadBytes;
inline constexpr std::uint64_t kBleProvisioningDevelopmentTransportTimeoutTicks = 30;

struct BleProvisioningFrame {
  std::uint8_t protocolVersion{kBleProvisioningFrameProtocolVersion};
  std::uint32_t messageId{};
  std::uint16_t fragmentIndex{};
  std::uint16_t fragmentCount{};
  std::uint16_t payloadLength{};
  std::array<std::uint8_t, kBleProvisioningMaxFramePayloadBytes> payload{};

  bool setPayload(const std::uint8_t* bytes, std::size_t length) {
    if (bytes == nullptr || length > payload.size()) return false;
    std::memcpy(payload.data(), bytes, length);
    payloadLength = static_cast<std::uint16_t>(length);
    return true;
  }
};

struct BleProvisioningEncodedFrame {
  std::array<std::uint8_t, kBleProvisioningMaxFrameBytes> bytes{};
  std::size_t size{};
};

inline bool encode_ble_provisioning_frame(const BleProvisioningFrame& frame,
                                          BleProvisioningEncodedFrame& encoded) {
  if (frame.fragmentCount == 0 || frame.fragmentIndex >= frame.fragmentCount ||
      frame.payloadLength > frame.payload.size())
    return false;
  encoded.size = kBleProvisioningFrameHeaderBytes + frame.payloadLength;
  encoded.bytes[0] = frame.protocolVersion;
  encoded.bytes[1] = static_cast<std::uint8_t>(frame.messageId >> 24U);
  encoded.bytes[2] = static_cast<std::uint8_t>(frame.messageId >> 16U);
  encoded.bytes[3] = static_cast<std::uint8_t>(frame.messageId >> 8U);
  encoded.bytes[4] = static_cast<std::uint8_t>(frame.messageId);
  encoded.bytes[5] = static_cast<std::uint8_t>(frame.fragmentIndex >> 8U);
  encoded.bytes[6] = static_cast<std::uint8_t>(frame.fragmentIndex);
  encoded.bytes[7] = static_cast<std::uint8_t>(frame.fragmentCount >> 8U);
  encoded.bytes[8] = static_cast<std::uint8_t>(frame.fragmentCount);
  encoded.bytes[9] = static_cast<std::uint8_t>(frame.payloadLength >> 8U);
  encoded.bytes[10] = static_cast<std::uint8_t>(frame.payloadLength);
  std::memcpy(encoded.bytes.data() + kBleProvisioningFrameHeaderBytes, frame.payload.data(),
              frame.payloadLength);
  return true;
}

inline bool decode_ble_provisioning_frame(const std::uint8_t* bytes, std::size_t length,
                                          BleProvisioningFrame& frame) {
  if (bytes == nullptr || length < kBleProvisioningFrameHeaderBytes ||
      length > kBleProvisioningMaxFrameBytes)
    return false;
  const auto payload_length = static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[9]) << 8U) |
                                                          bytes[10]);
  if (payload_length > kBleProvisioningMaxFramePayloadBytes ||
      length != kBleProvisioningFrameHeaderBytes + payload_length)
    return false;
  frame.protocolVersion = bytes[0];
  frame.messageId = (static_cast<std::uint32_t>(bytes[1]) << 24U) |
                    (static_cast<std::uint32_t>(bytes[2]) << 16U) |
                    (static_cast<std::uint32_t>(bytes[3]) << 8U) | bytes[4];
  frame.fragmentIndex = static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[5]) << 8U) |
                                                   bytes[6]);
  frame.fragmentCount = static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[7]) << 8U) |
                                                   bytes[8]);
  frame.payloadLength = payload_length;
  std::memcpy(frame.payload.data(), bytes + kBleProvisioningFrameHeaderBytes, payload_length);
  return frame.fragmentCount != 0 && frame.fragmentIndex < frame.fragmentCount;
}

enum class BleProvisioningTransportState : std::uint8_t {
  kDisconnected,
  kConnected,
  kReceiving,
  kComplete,
  kRejected,
  kTimedOut,
  kCancelled,
  kCleared,
};

struct BleProvisioningFramedTransportResult {
  BleProvisioningTransportState state{BleProvisioningTransportState::kDisconnected};
  BleProvisioningSafeStatus safeStatus{BleProvisioningSafeStatus::kReady};
  BleProvisioningSafeReason safeReason{BleProvisioningSafeReason::kOk};
  bool retryAllowed{};
  bool secretsCleared{};

  BleProvisioningSafeStatusMessage statusMessage() const {
    return encode_ble_provisioning_safe_status(safeStatus, safeReason);
  }
};

class BleProvisioningPayloadHandoff {
 public:
  BleProvisioningPayloadHandoff() = default;
  BleProvisioningPayloadHandoff(const BleProvisioningPayloadHandoff&) = delete;
  BleProvisioningPayloadHandoff& operator=(const BleProvisioningPayloadHandoff&) = delete;

  BleProvisioningPayloadHandoff(BleProvisioningPayloadHandoff&& other) noexcept { moveFrom(other); }
  BleProvisioningPayloadHandoff& operator=(BleProvisioningPayloadHandoff&& other) noexcept {
    if (this != &other) {
      clear();
      moveFrom(other);
    }
    return *this;
  }

  ~BleProvisioningPayloadHandoff() { clear(); }

  bool available() const { return length_ != 0; }
  std::size_t size() const { return length_; }
  const std::uint8_t* data() const { return bytes_.data(); }

  void clear() noexcept {
    secureClear(bytes_);
    length_ = 0;
  }

 private:
  friend class BleProvisioningFramedTransport;

  void append(const std::uint8_t* bytes, std::size_t length) {
    std::memcpy(bytes_.data() + length_, bytes, length);
    length_ += length;
  }

  void moveFrom(BleProvisioningPayloadHandoff& other) noexcept {
    std::memcpy(bytes_.data(), other.bytes_.data(), other.length_);
    length_ = other.length_;
    other.clear();
  }

  static void secureClear(std::array<std::uint8_t, kBleProvisioningMaxWriteBytes>& bytes) noexcept {
    volatile std::uint8_t* cursor = bytes.data();
    for (std::size_t index = 0; index < bytes.size(); ++index) cursor[index] = 0;
  }

  std::array<std::uint8_t, kBleProvisioningMaxWriteBytes> bytes_{};
  std::size_t length_{};
};

class BleProvisioningFramedTransport {
 public:
  BleProvisioningTransportState state() const { return state_; }
  bool hasPendingData() const { return activeMessage_; }
  bool hasActiveMessageId() const { return activeMessage_; }
  std::uint32_t activeMessageId() const { return activeMessageId_; }
  std::uint64_t lastAcceptedTick() const { return lastAcceptedTick_; }
  bool secretsCleared() const { return !activeMessage_ && !completed_.available(); }

  BleProvisioningFramedTransportResult connect() {
    if (state_ != BleProvisioningTransportState::kDisconnected &&
        state_ != BleProvisioningTransportState::kCleared)
      return rejectInvalidTransition();
    clearAll();
    state_ = BleProvisioningTransportState::kConnected;
    return result(BleProvisioningSafeStatus::kReady, BleProvisioningSafeReason::kOk, true, true);
  }

  BleProvisioningFramedTransportResult acceptFrame(const BleProvisioningFrame& frame,
                                                    std::uint64_t nowTicks) {
    if (state_ == BleProvisioningTransportState::kComplete) {
      return result(BleProvisioningSafeStatus::kRejected, BleProvisioningSafeReason::kReplayRejected,
                    false, !completed_.available());
    }
    if (state_ != BleProvisioningTransportState::kConnected &&
        state_ != BleProvisioningTransportState::kReceiving)
      return rejectInvalidTransition();
    if (frame.protocolVersion != kBleProvisioningFrameProtocolVersion)
      return reject(BleProvisioningSafeReason::kUnsupportedVersion);
    if (frame.fragmentCount == 0 || frame.fragmentIndex >= frame.fragmentCount ||
        frame.payloadLength > frame.payload.size() || frame.payloadLength == 0)
      return reject(BleProvisioningSafeReason::kMalformedFrame);

    if (!activeMessage_) {
      if (frame.fragmentIndex != 0) return reject(BleProvisioningSafeReason::kOutOfOrderFragment);
      activeMessage_ = true;
      activeMessageId_ = frame.messageId;
      expectedFragmentCount_ = frame.fragmentCount;
      nextFragmentIndex_ = 0;
    } else {
      if (frame.messageId != activeMessageId_)
        return reject(BleProvisioningSafeReason::kMessageIdMismatch);
      if (frame.fragmentCount != expectedFragmentCount_)
        return reject(BleProvisioningSafeReason::kFragmentCountMismatch);
      if (frame.fragmentIndex < nextFragmentIndex_)
        return reject(BleProvisioningSafeReason::kDuplicateFragment);
      if (frame.fragmentIndex > nextFragmentIndex_)
        return reject(BleProvisioningSafeReason::kOutOfOrderFragment);
    }

    if (completed_.size() + frame.payloadLength > kBleProvisioningMaxWriteBytes)
      return reject(BleProvisioningSafeReason::kPayloadTooLarge);

    completed_.append(frame.payload.data(), frame.payloadLength);
    ++nextFragmentIndex_;
    lastAcceptedTick_ = nowTicks;
    if (nextFragmentIndex_ == expectedFragmentCount_) {
      completedMessageId_ = activeMessageId_;
      activeMessage_ = false;
      state_ = BleProvisioningTransportState::kComplete;
      return result(BleProvisioningSafeStatus::kComplete, BleProvisioningSafeReason::kOk, false,
                    false);
    }
    state_ = BleProvisioningTransportState::kReceiving;
    return result(BleProvisioningSafeStatus::kReceiving, BleProvisioningSafeReason::kOk, true, false);
  }

  BleProvisioningFramedTransportResult expire(std::uint64_t nowTicks) {
    if (state_ != BleProvisioningTransportState::kReceiving || nowTicks < lastAcceptedTick_ ||
        nowTicks - lastAcceptedTick_ < kBleProvisioningDevelopmentTransportTimeoutTicks)
      return resultForCurrentState();
    clearPending();
    state_ = BleProvisioningTransportState::kTimedOut;
    return result(BleProvisioningSafeStatus::kTimedOut, BleProvisioningSafeReason::kTransportTimeout,
                  true, true);
  }

  BleProvisioningFramedTransportResult cancel() {
    if (state_ != BleProvisioningTransportState::kConnected &&
        state_ != BleProvisioningTransportState::kReceiving)
      return rejectInvalidTransition();
    clearPending();
    state_ = BleProvisioningTransportState::kCancelled;
    return result(BleProvisioningSafeStatus::kCancelled, BleProvisioningSafeReason::kCancelled, true,
                  true);
  }

  BleProvisioningFramedTransportResult disconnect() {
    if (state_ != BleProvisioningTransportState::kConnected &&
        state_ != BleProvisioningTransportState::kReceiving &&
        state_ != BleProvisioningTransportState::kComplete)
      return rejectInvalidTransition();
    clearAll();
    state_ = BleProvisioningTransportState::kCleared;
    return result(BleProvisioningSafeStatus::kReady, BleProvisioningSafeReason::kDisconnected, true,
                  true);
  }

  BleProvisioningFramedTransportResult clear() {
    if (state_ != BleProvisioningTransportState::kRejected &&
        state_ != BleProvisioningTransportState::kTimedOut &&
        state_ != BleProvisioningTransportState::kCancelled &&
        state_ != BleProvisioningTransportState::kComplete)
      return rejectInvalidTransition();
    clearAll();
    state_ = BleProvisioningTransportState::kCleared;
    return result(BleProvisioningSafeStatus::kReady, BleProvisioningSafeReason::kOk, true, true);
  }

  BleProvisioningPayloadHandoff takeCompletedPayload() {
    if (state_ != BleProvisioningTransportState::kComplete || handoffTaken_)
      return BleProvisioningPayloadHandoff{};
    handoffTaken_ = true;
    return std::move(completed_);
  }

  void reset() noexcept {
    clearAll();
    state_ = BleProvisioningTransportState::kDisconnected;
  }

 private:
  BleProvisioningFramedTransportResult reject(BleProvisioningSafeReason reason) {
    const auto lastAcceptedTick = lastAcceptedTick_;
    clearPending();
    // The timestamp is safe metadata. Retaining it proves a rejected fragment
    // did not extend the active transport deadline while payload bytes are gone.
    lastAcceptedTick_ = lastAcceptedTick;
    state_ = BleProvisioningTransportState::kRejected;
    return result(BleProvisioningSafeStatus::kRejected, reason, false, true);
  }

  BleProvisioningFramedTransportResult rejectInvalidTransition() {
    return reject(BleProvisioningSafeReason::kInvalidTransition);
  }

  BleProvisioningFramedTransportResult resultForCurrentState() const {
    switch (state_) {
      case BleProvisioningTransportState::kDisconnected:
      case BleProvisioningTransportState::kCleared:
        return result(BleProvisioningSafeStatus::kReady, BleProvisioningSafeReason::kOk, true, true);
      case BleProvisioningTransportState::kConnected:
      case BleProvisioningTransportState::kReceiving:
        return result(BleProvisioningSafeStatus::kReceiving, BleProvisioningSafeReason::kOk, true,
                      false);
      case BleProvisioningTransportState::kComplete:
        return result(BleProvisioningSafeStatus::kComplete, BleProvisioningSafeReason::kOk, false,
                      !completed_.available());
      case BleProvisioningTransportState::kRejected:
        return result(BleProvisioningSafeStatus::kRejected, BleProvisioningSafeReason::kMalformedFrame,
                      false, true);
      case BleProvisioningTransportState::kTimedOut:
        return result(BleProvisioningSafeStatus::kTimedOut,
                      BleProvisioningSafeReason::kTransportTimeout, true, true);
      case BleProvisioningTransportState::kCancelled:
        return result(BleProvisioningSafeStatus::kCancelled, BleProvisioningSafeReason::kCancelled, true,
                      true);
    }
    return result(BleProvisioningSafeStatus::kRejected, BleProvisioningSafeReason::kInvalidTransition,
                  false, true);
  }

  BleProvisioningFramedTransportResult result(BleProvisioningSafeStatus status,
                                              BleProvisioningSafeReason reason, bool retryAllowed,
                                              bool secretsCleared) const {
    return {state_, status, reason, retryAllowed, secretsCleared};
  }

  void clearPending() noexcept {
    completed_.clear();
    activeMessage_ = false;
    activeMessageId_ = 0;
    expectedFragmentCount_ = 0;
    nextFragmentIndex_ = 0;
    lastAcceptedTick_ = 0;
  }

  void clearAll() noexcept {
    clearPending();
    completedMessageId_ = 0;
    handoffTaken_ = false;
  }

  BleProvisioningTransportState state_{BleProvisioningTransportState::kDisconnected};
  BleProvisioningPayloadHandoff completed_;
  bool activeMessage_{};
  bool handoffTaken_{};
  std::uint32_t activeMessageId_{};
  std::uint32_t completedMessageId_{};
  std::uint16_t expectedFragmentCount_{};
  std::uint16_t nextFragmentIndex_{};
  std::uint64_t lastAcceptedTick_{};
};

static_assert(kBleProvisioningMaxFrameBytes <= 267, "BLE frame size must remain bounded");
static_assert(kBleProvisioningMaxWriteBytes == 1024,
              "Framed transport must match the provisioning request limit");
static_assert(kBleProvisioningMaxStatusBytes >= 64, "Safe status encoding must remain bounded");

}  // namespace algaguard
