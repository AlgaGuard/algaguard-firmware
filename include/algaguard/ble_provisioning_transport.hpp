#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "algaguard/ble_advertising_runtime.hpp"
#include "algaguard/ble_provisioning_gatt.hpp"
#include "algaguard/ble_provisioning_gatt_controller.hpp"

namespace algaguard {

using BleProvisioningWriteHandler = void (*)(const std::uint8_t* bytes, std::size_t length,
                                             void* context);

enum class BleProvisioningWriteDisposition : std::uint8_t {
  kAccepted,
  kRejectedEmpty,
  kRejectedTooLong,
  kRejectedNoHandler,
};

class BleProvisioningWriteSeam {
 public:
  void setHandler(BleProvisioningWriteHandler handler, void* context = nullptr) {
    handler_ = handler;
    context_ = context;
  }

  BleProvisioningWriteDisposition dispatch(const std::uint8_t* bytes, std::size_t length) {
    if (bytes == nullptr || length == 0) return BleProvisioningWriteDisposition::kRejectedEmpty;
    if (length > kBleProvisioningMaxWriteBytes)
      return BleProvisioningWriteDisposition::kRejectedTooLong;
    if (handler_ == nullptr) return BleProvisioningWriteDisposition::kRejectedNoHandler;

    std::array<std::uint8_t, kBleProvisioningMaxWriteBytes> temporary{};
    std::memcpy(temporary.data(), bytes, length);
    handler_(temporary.data(), length, context_);
    secure_clear(temporary);
    return BleProvisioningWriteDisposition::kAccepted;
  }

  void clear() noexcept {
    handler_ = nullptr;
    context_ = nullptr;
  }

 private:
  static void secure_clear(std::array<std::uint8_t, kBleProvisioningMaxWriteBytes>& bytes) noexcept {
    volatile std::uint8_t* cursor = bytes.data();
    for (std::size_t index = 0; index < bytes.size(); ++index) cursor[index] = 0;
  }

  BleProvisioningWriteHandler handler_{};
  void* context_{};
};

class BleProvisioningTransport {
 public:
  virtual ~BleProvisioningTransport() = default;
  virtual bool init() = 0;
  virtual bool startGattService() = 0;
  virtual void stopGattService() = 0;
  virtual void setWriteHandler(BleProvisioningWriteHandler handler, void* context = nullptr) = 0;
  virtual bool publishSafeStatus(BleProvisioningSafeStatus status,
                                 BleProvisioningSafeReason reason) = 0;
  virtual void clearPendingWrite() = 0;
  virtual void shutdown() = 0;
};

#if defined(ESP_PLATFORM)
class EspIdfBleProvisioningTransport final : public BleProvisioningTransport {
 public:
  bool init() override;
  bool startGattService() override;
  void stopGattService() override;
  void setWriteHandler(BleProvisioningWriteHandler handler, void* context = nullptr) override;
  bool publishSafeStatus(BleProvisioningSafeStatus status,
                         BleProvisioningSafeReason reason) override;
  void clearPendingWrite() override;
  void shutdown() override;

  bool startAdvertising();
  void onGapConnected(std::uint16_t connectionId);
  void onGapDisconnected(std::uint16_t connectionId);
  void onStatusSubscription(std::uint16_t attributeHandle, bool subscribed);
  void publishPendingNotification();
  bool hasActiveConnection(std::uint16_t connectionId) const;
  bool installDevelopmentProvisioningSession(std::string_view sessionId,
                                             std::string_view deviceId,
                                             std::string_view sessionToken,
                                             std::uint64_t expiryTick);
  bool installDevelopmentSession(std::string_view sessionId, std::string_view deviceId,
                                 std::string_view sessionToken, std::uint64_t expiryTick) {
    return installDevelopmentProvisioningSession(sessionId, deviceId, sessionToken, expiryTick);
  }
  BleWifiCredentialHandoff takeAcceptedWifiCredentials();
  void recordAdvertisingStage(BleAdvertisingStage stage, std::int32_t returnCode = 0);
  BleAdvertisingRuntimeStatus advertisingRuntimeStatus() const { return advertisingStatus_; }

  BleProvisioningWriteDisposition dispatchBoundedWrite(const std::uint8_t* bytes,
                                                        std::size_t length);
  void pollProvisioningTransport(std::uint64_t nowTick);
  const BleProvisioningSafeStatusMessage& latestSafeStatus() const {
    return controller_.readSafeStatus();
  }

 private:
  BleProvisioningWriteSeam writeSeam_;
  BleProvisioningGattController controller_;
  bool initialized_{};
  bool serviceRegistered_{};
  bool serviceStarted_{};
  bool advertising_{};
  bool hostTaskStarted_{};
  bool statusSubscribed_{};
  bool shutdown_{};
  std::uint8_t ownAddressType_{};
  std::uint16_t statusValueHandle_{};
  BleAdvertisingRuntimeStatus advertisingStatus_{};
};
#endif

}  // namespace algaguard
