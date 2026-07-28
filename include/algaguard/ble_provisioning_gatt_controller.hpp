#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "algaguard/ble_provisioning_framed_transport.hpp"
#include "algaguard/ble_provisioning_payload_validation.hpp"

namespace algaguard {

inline constexpr const char kBleProvisioningAdvertisingName[] = "AlgaGuard-Setup";

struct BleProvisioningGattControllerResult {
  bool accepted{};
  bool ignored{};
  bool secretsCleared{};
  BleProvisioningSafeStatus safeStatus{BleProvisioningSafeStatus::kReady};
  BleProvisioningSafeReason safeReason{BleProvisioningSafeReason::kOk};
};

class BleProvisioningGattController {
 public:
  BleProvisioningGattController() { setStatus(BleProvisioningSafeStatus::kReady,
                                              BleProvisioningSafeReason::kOk, false); }

  BleProvisioningGattControllerResult onConnected(std::uint16_t connectionId) {
    if (shutdown_) return reject(BleProvisioningSafeReason::kInvalidTransition, true);
    if (connected_) return reject(BleProvisioningSafeReason::kInvalidTransition, false);
    const auto result = transport_.connect();
    if (result.state != BleProvisioningTransportState::kConnected)
      return publish(result, false);
    connected_ = true;
    connectionId_ = connectionId;
    validation_.onBleConnected();
    return publish(result, true);
  }

  BleProvisioningGattControllerResult onDisconnected(std::uint16_t connectionId) {
    if (!connected_ || connectionId != connectionId_)
      return reject(BleProvisioningSafeReason::kInvalidTransition, true);
    transport_.reset();
    pendingPayload_.clear();
    validation_.onDisconnected();
    connected_ = false;
    connectionId_ = 0;
    notificationPending_ = false;
    setStatus(BleProvisioningSafeStatus::kReady, BleProvisioningSafeReason::kDisconnected, true);
    return {true, false, true, BleProvisioningSafeStatus::kReady,
            BleProvisioningSafeReason::kDisconnected};
  }

  BleProvisioningGattControllerResult onRequestWrite(const std::uint8_t* bytes, std::size_t length,
                                                      std::uint64_t nowTick) {
    return onRequestWrite(connectionId_, bytes, length, nowTick);
  }

  BleProvisioningGattControllerResult onRequestWrite(std::uint16_t connectionId,
                                                      const std::uint8_t* bytes, std::size_t length,
                                                      std::uint64_t nowTick) {
    if (!connected_ || connectionId != connectionId_ || shutdown_)
      return reject(BleProvisioningSafeReason::kInvalidTransition, true);
    if (bytes == nullptr || length == 0 || length > kBleProvisioningMaxFrameBytes) {
      if (transport_.state() == BleProvisioningTransportState::kConnected ||
          transport_.state() == BleProvisioningTransportState::kReceiving)
        transport_.cancel();
      pendingPayload_.clear();
      setStatus(BleProvisioningSafeStatus::kRejected,
                length > kBleProvisioningMaxFrameBytes ? BleProvisioningSafeReason::kPayloadTooLarge
                                                       : BleProvisioningSafeReason::kMalformedFrame,
                true);
      return {false, false, true, BleProvisioningSafeStatus::kRejected,
              length > kBleProvisioningMaxFrameBytes ? BleProvisioningSafeReason::kPayloadTooLarge
                                                     : BleProvisioningSafeReason::kMalformedFrame};
    }

    BleProvisioningFrame frame;
    if (!decode_ble_provisioning_frame(bytes, length, frame)) {
      transport_.cancel();
      pendingPayload_.clear();
      setStatus(BleProvisioningSafeStatus::kRejected, BleProvisioningSafeReason::kMalformedFrame,
                true);
      return {false, false, true, BleProvisioningSafeStatus::kRejected,
              BleProvisioningSafeReason::kMalformedFrame};
    }
    const auto result = transport_.acceptFrame(frame, nowTick);
    const bool accepted = result.safeStatus == BleProvisioningSafeStatus::kReceiving ||
                          result.safeStatus == BleProvisioningSafeStatus::kComplete;
    if (result.safeStatus == BleProvisioningSafeStatus::kComplete) {
      pendingPayload_.clear();
      auto completed = transport_.takeCompletedPayload();
      if (!completed.available())
        return reject(BleProvisioningSafeReason::kInvalidTransition, false);
      if (validation_.hasSession()) {
        const auto validated = validation_.consume(std::move(completed), nowTick);
        setStatus(validated.safeStatus, validated.safeReason, true);
        return {validated.accepted, false, validated.secretsCleared, validated.safeStatus,
                validated.safeReason};
      }
      pendingPayload_ = std::move(completed);
    }
    return publish(result, accepted);
  }

  BleProvisioningGattControllerResult onTick(std::uint64_t nowTick) {
    if (!connected_ || shutdown_) return {false, true, true, status_, reason_};
    const auto result = transport_.expire(nowTick);
    if (result.safeStatus == BleProvisioningSafeStatus::kTimedOut) pendingPayload_.clear();
    if (result.safeStatus == BleProvisioningSafeStatus::kTimedOut) return publish(result, false);
    const auto validation = validation_.onTick(nowTick);
    if (validation.safeReason == BleProvisioningSafeReason::kSessionExpired) {
      pendingPayload_.clear();
      setStatus(validation.safeStatus, validation.safeReason, true);
      return {false, false, validation.secretsCleared, validation.safeStatus, validation.safeReason};
    }
    return {true, false, result.secretsCleared, status_, reason_};
  }

  const BleProvisioningSafeStatusMessage& readSafeStatus() const { return statusMessage_; }

  BleProvisioningSafeStatusMessage takePendingNotification() {
    if (!notificationPending_) return {};
    notificationPending_ = false;
    return statusMessage_;
  }

  BleProvisioningPayloadHandoff takeCompletedPayload() {
    return std::move(pendingPayload_);
  }

  bool installDevelopmentSession(std::string_view sessionId, std::string_view deviceId,
                                 std::string_view sessionToken, std::uint64_t expiryTick) {
    pendingPayload_.clear();
    return validation_.installDevelopmentSession(sessionId, deviceId, sessionToken, expiryTick);
  }

  BleWifiCredentialHandoff takeAcceptedWifiCredentials() {
    return validation_.takeAcceptedCredentials();
  }

  void reset() noexcept {
    transport_.reset();
    pendingPayload_.clear();
    validation_.clear();
    connected_ = false;
    connectionId_ = 0;
    notificationPending_ = false;
    shutdown_ = false;
    setStatus(BleProvisioningSafeStatus::kReady, BleProvisioningSafeReason::kOk, false);
  }

  void shutdown() noexcept {
    transport_.reset();
    pendingPayload_.clear();
    validation_.clear();
    connected_ = false;
    connectionId_ = 0;
    notificationPending_ = false;
    shutdown_ = true;
    setStatus(BleProvisioningSafeStatus::kCancelled, BleProvisioningSafeReason::kCancelled, false);
  }

  bool hasActiveConnection() const { return connected_; }
  bool connectionIdMatches(std::uint16_t connectionId) const {
    return connected_ && connectionId == connectionId_;
  }
  bool notificationPending() const { return notificationPending_; }

 private:
  void setStatus(BleProvisioningSafeStatus status, BleProvisioningSafeReason reason,
                 bool notificationPending) {
    status_ = status;
    reason_ = reason;
    statusMessage_ = encode_ble_provisioning_safe_status(status, reason);
    notificationPending_ = notificationPending;
  }

  BleProvisioningGattControllerResult publish(const BleProvisioningFramedTransportResult& result,
                                              bool accepted) {
    setStatus(result.safeStatus, result.safeReason, true);
    return {accepted, false, result.secretsCleared, result.safeStatus, result.safeReason};
  }

  BleProvisioningGattControllerResult reject(BleProvisioningSafeReason reason, bool ignored) {
    if (!ignored) setStatus(BleProvisioningSafeStatus::kRejected, reason, true);
    return {false, ignored, true, BleProvisioningSafeStatus::kRejected, reason};
  }

  BleProvisioningFramedTransport transport_;
  BleProvisioningPayloadValidation validation_;
  BleProvisioningPayloadHandoff pendingPayload_;
  BleProvisioningSafeStatusMessage statusMessage_{};
  BleProvisioningSafeStatus status_{BleProvisioningSafeStatus::kReady};
  BleProvisioningSafeReason reason_{BleProvisioningSafeReason::kOk};
  std::uint16_t connectionId_{};
  bool connected_{};
  bool notificationPending_{};
  bool shutdown_{};
};

}  // namespace algaguard
