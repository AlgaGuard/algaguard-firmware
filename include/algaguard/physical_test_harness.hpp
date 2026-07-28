#pragma once

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "algaguard/display.hpp"
#include "algaguard/hardware.hpp"
#include "algaguard/security_profile_guards.hpp"

namespace algaguard {

enum class PhysicalTestState : std::uint8_t {
  kPhysicalTestMode,
  kBleAdvertisingInitializing,
  kBleAdvertisingActive,
  kBleAdvertisingFailed,
  kSessionInstallerReady,
  kSessionArmed,
  kSessionInstallRejected,
  kBleReady,
  kBleConnected,
  kReceiving,
  kValidating,
  kAccepted,
  kWifiConnecting,
  kWifiConnected,
  kWifiRuntimeReady,
  kWifiHandoffReady,
  kConnectTestDisabled,
  kConnectTestArmed,
  kConnectTestConsumed,
  kConnectTestExpired,
  kAuthFailed,
  kNetworkNotFound,
  kTimedOut,
  kCancelled,
  kResetRequired,
};

struct PhysicalLedPattern {
  bool red{};
  bool green{};
  bool blue{};
  bool blink{};
  bool bounded{true};
};

struct PhysicalBoardPreflightInput {
  bool chipIsEsp32S3{};
  std::uint32_t flashBytes{};
  bool psramPresent{};
  std::uint32_t psramBytes{};
  bool partitionLayoutValid{};
  bool physicalProfileActive{};
  bool oledInitialized{};
};

enum class PhysicalBoardPreflightReason : std::uint8_t {
  kOk,
  kWrongChip,
  kFlashMismatch,
  kPsramMissing,
  kPartitionMismatch,
  kProfileMismatch,
  kDisplayUnavailable,
};

struct PhysicalBoardPreflightResult {
  bool safeForReadiness{};
  bool displayValidated{};
  PhysicalBoardPreflightReason reason{PhysicalBoardPreflightReason::kProfileMismatch};
};

struct PhysicalReadinessPlan {
  bool startBle{};
  bool startWifi{};
  bool startBootstrap{};
  bool startMqtt{};
  bool startOta{};
};

inline constexpr bool physical_test_profile_allowed(bool production, bool release,
                                                     bool developmentOptIn,
                                                     bool volatileWifiOnly) {
  return !production && !release && developmentOptIn && volatileWifiOnly;
}

inline constexpr bool physical_test_profile_compiled() {
#if defined(ALGAGUARD_PHYSICAL_TEST_MODE)
  return true;
#else
  return false;
#endif
}

inline constexpr bool physical_test_oled_address_is_expected() {
  return hardware::kOledAddress == 0x3C && hardware::kSda == 8 && hardware::kScl == 9;
}

inline PhysicalBoardPreflightResult physical_board_preflight(
    const PhysicalBoardPreflightInput& input) {
  if (!input.physicalProfileActive)
    return {false, false, PhysicalBoardPreflightReason::kProfileMismatch};
  if (!input.chipIsEsp32S3)
    return {false, false, PhysicalBoardPreflightReason::kWrongChip};
  if (input.flashBytes != 16U * 1024U * 1024U)
    return {false, false, PhysicalBoardPreflightReason::kFlashMismatch};
  if (!input.psramPresent || input.psramBytes < 8U * 1024U * 1024U)
    return {false, false, PhysicalBoardPreflightReason::kPsramMissing};
  if (!input.partitionLayoutValid)
    return {false, false, PhysicalBoardPreflightReason::kPartitionMismatch};
  if (!input.oledInitialized)
    return {true, false, PhysicalBoardPreflightReason::kDisplayUnavailable};
  return {true, true, PhysicalBoardPreflightReason::kOk};
}

inline constexpr PhysicalReadinessPlan physical_readiness_plan() {
  return {true, false, false, false, false};
}

inline PhysicalLedPattern physical_test_led_pattern(PhysicalTestState state) {
  switch (state) {
    case PhysicalTestState::kBleAdvertisingInitializing:
      return {false, false, true, true, true};
    case PhysicalTestState::kBleAdvertisingActive:
    case PhysicalTestState::kBleReady:
    case PhysicalTestState::kSessionInstallerReady:
    case PhysicalTestState::kWifiRuntimeReady:
    case PhysicalTestState::kConnectTestDisabled:
    case PhysicalTestState::kConnectTestConsumed:
      return {false, false, true, false, true};
    case PhysicalTestState::kConnectTestArmed:
      return {false, true, true, true, true};
    case PhysicalTestState::kConnectTestExpired:
      return {true, false, false, true, true};
    case PhysicalTestState::kSessionArmed:
      return {false, false, true, true, true};
    case PhysicalTestState::kSessionInstallRejected:
      return {true, false, false, true, true};
    case PhysicalTestState::kBleAdvertisingFailed:
      return {true, false, false, true, true};
    case PhysicalTestState::kBleConnected:
    case PhysicalTestState::kReceiving:
    case PhysicalTestState::kValidating:
      return {false, false, true, true, true};
    case PhysicalTestState::kWifiConnecting:
      return {false, true, true, true, true};
    case PhysicalTestState::kWifiConnected:
    case PhysicalTestState::kAccepted:
      return {false, true, false, false, true};
    case PhysicalTestState::kWifiHandoffReady:
      return {false, false, true, false, true};
    case PhysicalTestState::kAuthFailed:
    case PhysicalTestState::kNetworkNotFound:
    case PhysicalTestState::kTimedOut:
    case PhysicalTestState::kCancelled:
      return {true, false, false, true, true};
    case PhysicalTestState::kResetRequired:
      return {true, false, false, false, true};
    case PhysicalTestState::kPhysicalTestMode:
      return {false, false, true, false, true};
  }
  return {true, false, false, false, true};
}

inline DiagnosticScreen physical_test_screen(PhysicalTestState state, std::int32_t safeCode = 0) {
  std::string status{"PHYSICAL TEST MODE"};
  switch (state) {
    case PhysicalTestState::kBleAdvertisingInitializing: status = "BLE ADV INIT"; break;
    case PhysicalTestState::kBleAdvertisingActive: status = "BLE ADV ACTIVE"; break;
    case PhysicalTestState::kBleAdvertisingFailed:
      status = "BLE ADV FAILED";
      break;
    case PhysicalTestState::kSessionInstallerReady: status = "SESSION INSTALLER READY"; break;
    case PhysicalTestState::kSessionArmed: status = "SESSION ARMED"; break;
    case PhysicalTestState::kSessionInstallRejected: status = "SESSION INSTALL REJECTED"; break;
    case PhysicalTestState::kBleReady: status = "BLE READY"; break;
    case PhysicalTestState::kBleConnected: status = "BLE CONNECTED"; break;
    case PhysicalTestState::kReceiving: status = "RECEIVING"; break;
    case PhysicalTestState::kValidating: status = "VALIDATING"; break;
    case PhysicalTestState::kAccepted: status = "ACCEPTED"; break;
    case PhysicalTestState::kWifiConnecting: status = "WIFI CONNECTING"; break;
    case PhysicalTestState::kWifiConnected: status = "WIFI CONNECTED"; break;
    case PhysicalTestState::kWifiRuntimeReady: status = "WIFI RUNTIME READY"; break;
    case PhysicalTestState::kWifiHandoffReady: status = "WIFI HANDOFF READY"; break;
    case PhysicalTestState::kConnectTestDisabled: status = "CONNECT TEST DISABLED"; break;
    case PhysicalTestState::kConnectTestArmed: status = "WIFI TEST ARMED"; break;
    case PhysicalTestState::kConnectTestConsumed: status = "WIFI TEST CONSUMED"; break;
    case PhysicalTestState::kConnectTestExpired: status = "WIFI TEST EXPIRED"; break;
    case PhysicalTestState::kAuthFailed: status = "AUTH FAILED"; break;
    case PhysicalTestState::kNetworkNotFound: status = "NETWORK NOT FOUND"; break;
    case PhysicalTestState::kTimedOut: status = "TIMED OUT"; break;
    case PhysicalTestState::kCancelled: status = "CANCELLED"; break;
    case PhysicalTestState::kResetRequired: status = "RESET REQUIRED"; break;
    case PhysicalTestState::kPhysicalTestMode: break;
  }
  const std::string detail = state == PhysicalTestState::kBleAdvertisingFailed
                                 ? "CODE " + std::to_string(safeCode)
                                 : "VOLATILE ONLY";
  return {{{"PHYSICAL TEST MODE", status, detail, "NO SECRETS"}}};
}

inline bool physical_text_is_safe(std::string_view text) {
  for (const std::string_view forbidden :
       {"ssid", "password", "sessionid", "sessiontoken", "private key", "certificate", "ip "})
    if (text.find(forbidden) != std::string_view::npos) return false;
  return true;
}

inline bool physical_screen_is_safe(const DiagnosticScreen& screen) {
  if (!screen.safe()) return false;
  for (const auto& line : screen.lines) {
    std::string normalized;
    normalized.reserve(line.size());
    for (const unsigned char character : line)
      normalized.push_back(static_cast<char>(std::tolower(character)));
    if (!physical_text_is_safe(normalized)) return false;
  }
  return true;
}

inline std::string physical_safe_serial_diagnostic(PhysicalBoardPreflightResult preflight,
                                                    PhysicalTestState state,
                                                    std::uint8_t attempt) {
  return "profile=INSECURE_DEVELOPMENT_PHYSICAL_PROVISIONING_TEST preflight=" +
         std::to_string(static_cast<unsigned>(preflight.reason)) + " state=" +
         std::to_string(static_cast<unsigned>(state)) + " attempt=" +
         std::to_string(static_cast<unsigned>(attempt));
}

class VolatilePhysicalTestSession {
 public:
  VolatilePhysicalTestSession() = default;
  VolatilePhysicalTestSession(const VolatilePhysicalTestSession&) = delete;
  VolatilePhysicalTestSession& operator=(const VolatilePhysicalTestSession&) = delete;
  ~VolatilePhysicalTestSession() { clear(); }

  bool install(std::string_view sessionId, std::string_view deviceId, std::string_view sessionToken) {
#if defined(ALGAGUARD_PHYSICAL_TEST_MODE)
    if (sessionId.empty() || sessionId.size() > sessionId_.size() || deviceId.empty() ||
        deviceId.size() > deviceId_.size() || sessionToken.empty() ||
        sessionToken.size() > sessionToken_.size()) {
      clear();
      return false;
    }
    clear();
    std::memcpy(sessionId_.data(), sessionId.data(), sessionId.size());
    std::memcpy(deviceId_.data(), deviceId.data(), deviceId.size());
    std::memcpy(sessionToken_.data(), sessionToken.data(), sessionToken.size());
    active_ = true;
    return true;
#else
    (void)sessionId;
    (void)deviceId;
    (void)sessionToken;
    clear();
    return false;
#endif
  }

  bool active() const { return active_; }
  bool secretsCleared() const {
    for (const auto byte : sessionId_)
      if (byte != 0) return false;
    for (const auto byte : deviceId_)
      if (byte != 0) return false;
    for (const auto byte : sessionToken_)
      if (byte != 0) return false;
    return true;
  }
  void clear() noexcept {
    secureClear(sessionId_);
    secureClear(deviceId_);
    secureClear(sessionToken_);
    active_ = false;
  }

 private:
  template <std::size_t Size>
  static void secureClear(std::array<char, Size>& value) noexcept {
    volatile char* cursor = value.data();
    for (std::size_t index = 0; index < value.size(); ++index) cursor[index] = 0;
  }

  std::array<char, 64> sessionId_{};
  std::array<char, 9> deviceId_{};
  std::array<char, 512> sessionToken_{};
  bool active_{};
};

static_assert(hardware::kOledAddress == 0x3C && hardware::kSda == 8 && hardware::kScl == 9,
              "Physical-test OLED wiring changed without approval");
static_assert(hardware::kLedRed == 14 && hardware::kLedGreen == 15 && hardware::kLedBlue == 16,
              "Physical-test LED wiring changed without approval");
static_assert(hardware::kLedRed != 38 && hardware::kLedGreen != 38 && hardware::kLedBlue != 38,
              "Onboard RGB must remain unused");

}  // namespace algaguard
