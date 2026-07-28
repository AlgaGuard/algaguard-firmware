#pragma once

#if defined(ALGAGUARD_PHYSICAL_TEST_MODE)

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

#include "algaguard/wifi_connection_runtime.hpp"

#if defined(ESP_PLATFORM)
extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}
#endif

namespace algaguard {

// This entire boundary is compiled only by the explicitly opted-in physical-test profile.
inline constexpr std::uint8_t kPhysicalSessionProtocolVersion = 1;
inline constexpr std::array<std::uint8_t, 4> kPhysicalSessionMagic{{'A', 'G', 'S', '1'}};
inline constexpr std::size_t kPhysicalSessionMaxPayloadBytes = 640;
inline constexpr std::size_t kPhysicalSessionMaxFrameBytes = 4 + 1 + 1 + 2 +
                                                              kPhysicalSessionMaxPayloadBytes + 4;
inline constexpr std::uint64_t kPhysicalSessionFrameTimeoutTicks = 100;
inline constexpr std::uint64_t kPhysicalWifiGateMaxLifetimeTicks = 600;

inline std::uint64_t physical_gate_monotonic_tick() noexcept {
#if defined(ESP_PLATFORM)
  return static_cast<std::uint64_t>(xTaskGetTickCount());
#else
  return 0;
#endif
}

enum class PhysicalSessionInstallerState : std::uint8_t {
  kReady, kArmed, kRejected, kExpired, kCleared,
};

inline constexpr std::string_view physical_session_state_code(PhysicalSessionInstallerState state) {
  switch (state) {
    case PhysicalSessionInstallerState::kReady: return "SESSION_INSTALLER_READY";
    case PhysicalSessionInstallerState::kArmed: return "SESSION_ARMED";
    case PhysicalSessionInstallerState::kRejected: return "SESSION_INSTALL_REJECTED";
    case PhysicalSessionInstallerState::kExpired: return "SESSION_EXPIRED";
    case PhysicalSessionInstallerState::kCleared: return "SESSION_CLEARED";
  }
  return "SESSION_INSTALL_REJECTED";
}

enum class PhysicalSessionControlCommand : std::uint8_t {
  kInstallVolatileSession = 1,
  kClearVolatileSession = 2,
  kQuerySafeSessionState = 3,
  kArmOneWifiConnectionTest = 4,
  kQueryOledAddress = 5,
};

enum class PhysicalSessionControlAck : std::uint8_t {
  kArmed, kRejected, kCleared, kDisabled, kWifiTestArmed, kWifiTestAlreadyActive,
  kWifiTestRejected, kWifiTestConsumed, kWifiTestExpired, kWifiTestCleared,
  kWifiTestDisabled, kOledAddressQuery,
};

inline constexpr std::string_view physical_session_ack_code(PhysicalSessionControlAck ack) {
  switch (ack) {
    case PhysicalSessionControlAck::kArmed: return "SESSION_ARMED";
    case PhysicalSessionControlAck::kRejected: return "SESSION_INSTALL_REJECTED";
    case PhysicalSessionControlAck::kCleared: return "SESSION_CLEARED";
    case PhysicalSessionControlAck::kDisabled: return "SESSION_INSTALLER_DISABLED";
    case PhysicalSessionControlAck::kWifiTestArmed: return "WIFI_CONNECT_TEST_ARMED";
    case PhysicalSessionControlAck::kWifiTestAlreadyActive: return "WIFI_CONNECT_TEST_ALREADY_ACTIVE";
    case PhysicalSessionControlAck::kWifiTestRejected: return "WIFI_CONNECT_TEST_REJECTED";
    case PhysicalSessionControlAck::kWifiTestConsumed: return "WIFI_CONNECT_TEST_CONSUMED";
    case PhysicalSessionControlAck::kWifiTestExpired: return "WIFI_CONNECT_TEST_EXPIRED";
    case PhysicalSessionControlAck::kWifiTestCleared: return "WIFI_CONNECT_TEST_CLEARED";
    case PhysicalSessionControlAck::kWifiTestDisabled: return "WIFI_CONNECT_TEST_DISABLED";
    case PhysicalSessionControlAck::kOledAddressQuery: return "OLED_ADDRESS_QUERY";
  }
  return "SESSION_INSTALL_REJECTED";
}

enum class PhysicalWifiConnectGateState : std::uint8_t { kDisabled, kArmed, kConsumed, kExpired, kCleared };

inline constexpr std::string_view physical_wifi_gate_state_code(PhysicalWifiConnectGateState state) {
  switch (state) {
    case PhysicalWifiConnectGateState::kDisabled: return "WIFI_CONNECT_TEST_DISABLED";
    case PhysicalWifiConnectGateState::kArmed: return "WIFI_CONNECT_TEST_ARMED";
    case PhysicalWifiConnectGateState::kConsumed: return "WIFI_CONNECT_TEST_CONSUMED";
    case PhysicalWifiConnectGateState::kExpired: return "WIFI_CONNECT_TEST_EXPIRED";
    case PhysicalWifiConnectGateState::kCleared: return "WIFI_CONNECT_TEST_CLEARED";
  }
  return "WIFI_CONNECT_TEST_DISABLED";
}

class PhysicalWifiConnectGate {
 public:
  bool arm(std::uint64_t now, std::uint64_t lifetime, bool handoffPresent, bool attemptActive) noexcept {
    expire(now);
    if (state_ == PhysicalWifiConnectGateState::kArmed || handoffPresent || attemptActive || lifetime == 0 ||
        lifetime > kPhysicalWifiGateMaxLifetimeTicks) return false;
    expiryTick_ = now + lifetime;
    state_ = PhysicalWifiConnectGateState::kArmed;
    return true;
  }
  bool consume(std::uint64_t now) noexcept {
    expire(now);
    if (state_ != PhysicalWifiConnectGateState::kArmed) return false;
    state_ = PhysicalWifiConnectGateState::kConsumed;
    return true;
  }
  bool expire(std::uint64_t now) noexcept {
    if (state_ == PhysicalWifiConnectGateState::kArmed && now >= expiryTick_) {
      state_ = PhysicalWifiConnectGateState::kExpired;
      expiryTick_ = 0;
      return true;
    }
    return false;
  }
  void clear() noexcept { state_ = PhysicalWifiConnectGateState::kCleared; expiryTick_ = 0; }
  void reset() noexcept { state_ = PhysicalWifiConnectGateState::kDisabled; expiryTick_ = 0; }
  PhysicalWifiConnectGateState state() const noexcept { return state_; }
 private:
  PhysicalWifiConnectGateState state_{PhysicalWifiConnectGateState::kDisabled};
  std::uint64_t expiryTick_{};
};

inline PhysicalWifiConnectGate physical_wifi_connect_gate{};

inline void physical_secure_clear(std::uint8_t* bytes, std::size_t size) noexcept {
  volatile std::uint8_t* cursor = bytes;
  for (std::size_t index = 0; index < size; ++index) cursor[index] = 0;
}

template <std::size_t Size>
inline bool physical_buffer_is_zero(const std::array<std::uint8_t, Size>& bytes) noexcept {
  for (const auto byte : bytes)
    if (byte != 0) return false;
  return true;
}

class PhysicalTestSessionInstaller {
 public:
  explicit PhysicalTestSessionInstaller(std::string_view expectedDeviceId = "AG-000001") {
    if (expectedDeviceId.size() <= expectedDeviceId_.size())
      std::memcpy(expectedDeviceId_.data(), expectedDeviceId.data(), expectedDeviceId.size());
  }
  PhysicalTestSessionInstaller(const PhysicalTestSessionInstaller&) = delete;
  PhysicalTestSessionInstaller& operator=(const PhysicalTestSessionInstaller&) = delete;
  ~PhysicalTestSessionInstaller() { clear(); }

  template <typename Transport>
  bool install(Transport& transport, std::string_view sessionId, std::string_view deviceId,
               std::string_view token, std::uint64_t expiry, std::uint64_t now) {
    if (armed_ || processingStarted_ || !valid(sessionId, deviceId, token, expiry, now) ||
        !transport.installDevelopmentSession(sessionId, deviceId, token, expiry)) {
      clearBuffers();
      state_ = expiry <= now ? PhysicalSessionInstallerState::kExpired
                             : PhysicalSessionInstallerState::kRejected;
      return false;
    }
    // The BLE controller is now the sole owner of the active session. Do not retain a token copy.
    clearBuffers();
    armed_ = true;
    state_ = PhysicalSessionInstallerState::kArmed;
    return true;
  }

  void markProcessingStarted() noexcept { processingStarted_ = true; }
  void clear() noexcept {
    clearBuffers();
    armed_ = false;
    processingStarted_ = false;
    state_ = PhysicalSessionInstallerState::kCleared;
  }
  PhysicalSessionInstallerState state() const noexcept { return state_; }
  bool armed() const noexcept { return armed_; }
  bool processingStarted() const noexcept { return processingStarted_; }
  bool secretsCleared() const noexcept {
    return physical_buffer_is_zero(sessionId_) && physical_buffer_is_zero(deviceId_) &&
           physical_buffer_is_zero(token_);
  }

 private:
  bool valid(std::string_view sessionId, std::string_view deviceId, std::string_view token,
             std::uint64_t expiry, std::uint64_t now) const noexcept {
    if (sessionId.size() != 36 || deviceId.empty() || token.size() < 32 || token.size() > 96 ||
        expiry <= now || sessionId.size() > sessionId_.size() || deviceId.size() > deviceId_.size())
      return false;
    if (deviceId != std::string_view{reinterpret_cast<const char*>(expectedDeviceId_.data()),
                                     expectedDeviceIdLength()})
      return false;
    for (const char character : token)
      if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '_' || character == '-'))
        return false;
    return true;
  }
  std::size_t expectedDeviceIdLength() const noexcept {
    std::size_t length{};
    while (length < expectedDeviceId_.size() && expectedDeviceId_[length] != 0) ++length;
    return length;
  }
  void clearBuffers() noexcept {
    physical_secure_clear(sessionId_.data(), sessionId_.size());
    physical_secure_clear(deviceId_.data(), deviceId_.size());
    physical_secure_clear(token_.data(), token_.size());
  }

  std::array<std::uint8_t, 64> expectedDeviceId_{};
  std::array<std::uint8_t, 64> sessionId_{};
  std::array<std::uint8_t, 16> deviceId_{};
  std::array<std::uint8_t, 512> token_{};
  PhysicalSessionInstallerState state_{PhysicalSessionInstallerState::kReady};
  bool armed_{};
  bool processingStarted_{};
};

class PhysicalSessionControlProtocol {
 public:
  PhysicalSessionControlProtocol() = default;
  PhysicalSessionControlProtocol(const PhysicalSessionControlProtocol&) = delete;
  PhysicalSessionControlProtocol& operator=(const PhysicalSessionControlProtocol&) = delete;
  ~PhysicalSessionControlProtocol() { clear(); }

  template <typename Transport>
  PhysicalSessionControlAck ingest(Transport& transport, PhysicalTestSessionInstaller& installer,
                                   const std::uint8_t* input, std::size_t length,
                                   std::uint64_t now, bool handoffPresent = false,
                                   bool connectionAttemptActive = false) {
    if (input == nullptr || length == 0 || length > kPhysicalSessionMaxFrameBytes ||
        (size_ != 0 && now - lastTick_ > kPhysicalSessionFrameTimeoutTicks))
      return reject(installer);
    if (size_ + length > buffer_.size()) return reject(installer);
    std::memcpy(buffer_.data() + size_, input, length);
    size_ += length;
    lastTick_ = now;
    if (size_ < 8) return PhysicalSessionControlAck::kRejected;
    const auto payloadLength = read16(6);
    const auto expected = 8U + payloadLength + 4U;
    if (payloadLength > kPhysicalSessionMaxPayloadBytes || expected > buffer_.size())
      return reject(installer);
    if (size_ < expected) return PhysicalSessionControlAck::kRejected;
    if (size_ != expected) return reject(installer);
    return process(transport, installer, now, handoffPresent, connectionAttemptActive);
  }

  void clear() noexcept {
    physical_secure_clear(buffer_.data(), buffer_.size());
    size_ = 0;
    lastTick_ = 0;
  }
  bool awaitingFrame() const noexcept { return size_ != 0; }
  bool secretsCleared() const noexcept { return physical_buffer_is_zero(buffer_); }

 private:
  static std::uint32_t crc32(const std::uint8_t* bytes, std::size_t length) noexcept {
    std::uint32_t value = 0xffffffffU;
    for (std::size_t index = 0; index < length; ++index) {
      value ^= bytes[index];
      for (unsigned bit = 0; bit < 8; ++bit)
        value = (value >> 1U) ^ (0xedb88320U & (0U - (value & 1U)));
    }
    return ~value;
  }
  std::uint16_t read16(std::size_t offset) const noexcept {
    return static_cast<std::uint16_t>((buffer_[offset] << 8U) | buffer_[offset + 1]);
  }
  std::uint32_t read32(std::size_t offset) const noexcept {
    return (static_cast<std::uint32_t>(buffer_[offset]) << 24U) |
           (static_cast<std::uint32_t>(buffer_[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(buffer_[offset + 2]) << 8U) | buffer_[offset + 3];
  }
  std::uint64_t read64(std::size_t offset) const noexcept {
    std::uint64_t value{};
    for (std::size_t index = 0; index < 8; ++index) value = (value << 8U) | buffer_[offset + index];
    return value;
  }
  template <typename Transport>
  PhysicalSessionControlAck process(Transport& transport, PhysicalTestSessionInstaller& installer,
                                    std::uint64_t now, bool handoffPresent,
                                    bool connectionAttemptActive) {
    if (std::memcmp(buffer_.data(), kPhysicalSessionMagic.data(), kPhysicalSessionMagic.size()) != 0 ||
        buffer_[4] != kPhysicalSessionProtocolVersion ||
        crc32(buffer_.data(), size_ - 4) != read32(size_ - 4))
      return reject(installer);
    const auto command = static_cast<PhysicalSessionControlCommand>(buffer_[5]);
    if (command == PhysicalSessionControlCommand::kClearVolatileSession && read16(6) == 0) {
      installer.clear();
      physical_wifi_connect_gate.clear();
      clear();
      return PhysicalSessionControlAck::kCleared;
    }
    if (command == PhysicalSessionControlCommand::kQuerySafeSessionState && read16(6) == 0) {
      physical_wifi_connect_gate.expire(now);
      const auto gateState = physical_wifi_connect_gate.state();
      const auto result = gateState == PhysicalWifiConnectGateState::kArmed
                              ? PhysicalSessionControlAck::kWifiTestArmed
                              : gateState == PhysicalWifiConnectGateState::kConsumed
                                    ? PhysicalSessionControlAck::kWifiTestConsumed
                                    : gateState == PhysicalWifiConnectGateState::kExpired
                                          ? PhysicalSessionControlAck::kWifiTestExpired
                                          : gateState == PhysicalWifiConnectGateState::kCleared
                                                ? PhysicalSessionControlAck::kWifiTestCleared
                                                : PhysicalSessionControlAck::kWifiTestDisabled;
      clear();
      return result;
    }
    if (command == PhysicalSessionControlCommand::kQueryOledAddress && read16(6) == 0) {
      clear();
      return PhysicalSessionControlAck::kOledAddressQuery;
    }
    if (command == PhysicalSessionControlCommand::kArmOneWifiConnectionTest && read16(6) == 8) {
      const bool activityPresent = installer.armed() || installer.processingStarted() ||
                                   handoffPresent || connectionAttemptActive;
      const auto armed = physical_wifi_connect_gate.arm(now, read64(8), activityPresent,
                                                         connectionAttemptActive);
      clear();
      return armed ? PhysicalSessionControlAck::kWifiTestArmed
                   : activityPresent ||
                             physical_wifi_connect_gate.state() == PhysicalWifiConnectGateState::kArmed
                         ? PhysicalSessionControlAck::kWifiTestAlreadyActive
                         : PhysicalSessionControlAck::kWifiTestRejected;
    }
    if (command != PhysicalSessionControlCommand::kInstallVolatileSession || installer.processingStarted()) {
      clear();
      return installer.processingStarted() ? PhysicalSessionControlAck::kDisabled : reject(installer);
    }
    const auto payloadLength = read16(6);
    const std::size_t payload = 8;
    if (payloadLength < 12) return reject(installer);
    const auto sessionLength = buffer_[payload];
    const auto deviceLength = buffer_[payload + 1];
    const auto tokenLength = read16(payload + 2);
    const std::size_t fields = 12U + sessionLength + deviceLength + tokenLength;
    if (fields != payloadLength || sessionLength == 0 || deviceLength == 0 || tokenLength == 0 ||
        payload + fields > size_ - 4)
      return reject(installer);
    const std::size_t sessionOffset = payload + 12;
    const std::size_t deviceOffset = sessionOffset + sessionLength;
    const std::size_t tokenOffset = deviceOffset + deviceLength;
    const bool installed = installer.install(
        transport, std::string_view{reinterpret_cast<const char*>(buffer_.data() + sessionOffset), sessionLength},
        std::string_view{reinterpret_cast<const char*>(buffer_.data() + deviceOffset), deviceLength},
        std::string_view{reinterpret_cast<const char*>(buffer_.data() + tokenOffset), tokenLength},
        read64(payload + 4), now);
    clear();
    return installed ? PhysicalSessionControlAck::kArmed : PhysicalSessionControlAck::kRejected;
  }
  PhysicalSessionControlAck reject(PhysicalTestSessionInstaller& installer) noexcept {
    clear();
    if (installer.state() != PhysicalSessionInstallerState::kArmed) installer.clear();
    return PhysicalSessionControlAck::kRejected;
  }

  std::array<std::uint8_t, kPhysicalSessionMaxFrameBytes> buffer_{};
  std::size_t size_{};
  std::uint64_t lastTick_{};
};

class PhysicalProvisioningRuntimeBridge {
 public:
  template <typename Transport>
  bool onAccepted(Transport& transport, WifiConnectionRuntime& runtime,
                  PhysicalTestSessionInstaller& installer,
                  std::uint64_t now = physical_gate_monotonic_tick()) {
    bool changed{};
    physical_wifi_connect_gate.expire(now);
    if (!handoffInstalled_) {
      auto handoff = transport.takeAcceptedWifiCredentials();
      if (handoff.available()) {
        const auto result = runtime.installAcceptedCredentials(std::move(handoff));
        handoffInstalled_ = result.state == WifiConnectionState::kCredentialsReady;
        if (handoffInstalled_) { installer.clear(); changed = true; }
      }
    }
    if (handoffInstalled_ && !connectionAuthorized_ && physical_wifi_connect_gate.consume(now)) {
      (void)runtime.startConnection(now);
      connectionAuthorized_ = true;
      changed = true;
    }
    return changed;
  }
  void clear(WifiConnectionRuntime& runtime, PhysicalTestSessionInstaller& installer) noexcept {
    runtime.reset();
    installer.clear();
    physical_wifi_connect_gate.clear();
    handoffInstalled_ = false;
    connectionAuthorized_ = false;
  }
  bool handoffInstalled() const noexcept { return handoffInstalled_; }
  static bool connectExecutionEnabled() noexcept {
    return physical_wifi_connect_gate.state() == PhysicalWifiConnectGateState::kArmed;
  }

 private:
  bool handoffInstalled_{};
  bool connectionAuthorized_{};
};

}  // namespace algaguard

#endif  // ALGAGUARD_PHYSICAL_TEST_MODE
