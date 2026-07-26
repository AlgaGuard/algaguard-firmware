#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "algaguard/domain.hpp"

namespace algaguard {

enum class StartupState : std::uint8_t {
  kPlatformInit,
  kStorageInit,
  kDisplayInit,
  kInputInit,
  kProvisioningStateLoad,
  kUnprovisioned,
  kBleProvisioning,
  kWifiConnecting,
  kTimeSync,
  kCredentialCheck,
  kCredentialBootstrap,
  kMqttConnecting,
  kOnline,
  kOtaPendingValidation,
  kDegraded,
  kFault,
  kControlledReset,
};

enum class OperationStatus : std::uint8_t {
  kSuccess,
  kPending,
  kMissing,
  kUnavailable,
  kNotImplemented,
  kRecoverableFailure,
  kFatalFailure,
};

enum class StartupReason : std::uint8_t {
  kNone,
  kBoardProfileMismatch,
  kStorageUnavailable,
  kStorageCorrupt,
  kModuleUnavailable,
  kModuleNotImplemented,
  kRetryExhausted,
  kFatalModuleFailure,
  kExplicitReset,
};

struct StartupResult {
  OperationStatus status{OperationStatus::kPending};
  StartupReason reason{StartupReason::kNone};
  bool provisioned{};
  bool credential_present{};
  bool ota_pending{};
};

class StartupServices {
 public:
  virtual ~StartupServices() = default;
  virtual StartupResult platform_init() = 0;
  virtual StartupResult storage_init() = 0;
  virtual StartupResult display_init() = 0;
  virtual StartupResult input_init() = 0;
  virtual StartupResult load_provisioning_state() = 0;
  virtual StartupResult start_ble_provisioning() = 0;
  virtual StartupResult poll_wifi() = 0;
  virtual StartupResult poll_time_sync() = 0;
  virtual StartupResult check_credentials() = 0;
  virtual StartupResult poll_credential_bootstrap() = 0;
  virtual StartupResult poll_mqtt() = 0;
  virtual StartupResult validate_pending_ota() = 0;
  virtual StartupResult controlled_reset() = 0;
};

struct RetryPolicy {
  std::uint32_t initial_delay_ms{250};
  std::uint32_t maximum_delay_ms{8000};
  std::uint8_t maximum_attempts{6};

  std::uint32_t delay_ms(std::uint8_t attempt) const {
    std::uint32_t value = initial_delay_ms;
    for (std::uint8_t index = 0; index < attempt && value < maximum_delay_ms; ++index)
      value = value > maximum_delay_ms / 2 ? maximum_delay_ms : value * 2;
    return value;
  }
};

inline constexpr std::string_view startup_state_name(StartupState state) {
  constexpr std::array<std::string_view, 17> names{
      "PLATFORM_INIT",          "STORAGE_INIT",       "DISPLAY_INIT",
      "INPUT_INIT",             "PROVISIONING_LOAD",  "UNPROVISIONED",
      "BLE_PROVISIONING",       "WIFI_CONNECTING",    "TIME_SYNC",
      "CREDENTIAL_CHECK",       "CREDENTIAL_BOOTSTRAP", "MQTT_CONNECTING",
      "ONLINE",                 "OTA_PENDING_VALIDATION", "DEGRADED",
      "FAULT",                  "CONTROLLED_RESET",
  };
  return names[static_cast<std::size_t>(state)];
}

inline LedState startup_led_state(StartupState state, bool remote_indicator = false) {
  if (state == StartupState::kFault) return led_state(LedPriority::kFault, remote_indicator);
  if (state == StartupState::kOnline) return led_state(LedPriority::kConnected, remote_indicator);
  if (state == StartupState::kUnprovisioned ||
      state == StartupState::kBleProvisioning ||
      state == StartupState::kOtaPendingValidation)
    return led_state(LedPriority::kSetupOrOta, remote_indicator);
  return led_state(LedPriority::kDisconnected, remote_indicator);
}

class StartupStateMachine {
 public:
  explicit StartupStateMachine(StartupServices& services, RetryPolicy retry_policy = {})
      : services_(services), retry_policy_(retry_policy) {}

  StartupState state() const { return state_; }
  StartupReason reason() const { return reason_; }
  std::uint8_t retry_attempt() const { return retry_attempt_; }
  std::uint32_t retry_delay_ms() const { return retry_policy_.delay_ms(retry_attempt_); }

  void tick() {
    if (state_ == StartupState::kUnprovisioned ||
        state_ == StartupState::kOnline ||
        state_ == StartupState::kDegraded ||
        state_ == StartupState::kFault)
      return;
    StartupResult result;
    switch (state_) {
      case StartupState::kPlatformInit:
        result = services_.platform_init();
        advance(result, StartupState::kStorageInit);
        break;
      case StartupState::kStorageInit:
        result = services_.storage_init();
        advance(result, StartupState::kDisplayInit);
        break;
      case StartupState::kDisplayInit:
        result = services_.display_init();
        advance(result, StartupState::kInputInit);
        break;
      case StartupState::kInputInit:
        result = services_.input_init();
        advance(result, StartupState::kProvisioningStateLoad);
        break;
      case StartupState::kProvisioningStateLoad:
        result = services_.load_provisioning_state();
        if (result.status == OperationStatus::kSuccess) {
          retry_attempt_ = 0;
          reason_ = StartupReason::kNone;
          state_ = result.ota_pending
                       ? StartupState::kOtaPendingValidation
                       : (result.provisioned ? StartupState::kWifiConnecting
                                             : StartupState::kUnprovisioned);
        } else {
          handle_failure(result);
        }
        break;
      case StartupState::kBleProvisioning:
        result = services_.start_ble_provisioning();
        if (result.status != OperationStatus::kSuccess &&
            result.status != OperationStatus::kPending)
          handle_failure(result);
        break;
      case StartupState::kWifiConnecting:
        result = services_.poll_wifi();
        advance(result, StartupState::kTimeSync);
        break;
      case StartupState::kTimeSync:
        result = services_.poll_time_sync();
        advance(result, StartupState::kCredentialCheck);
        break;
      case StartupState::kCredentialCheck:
        result = services_.check_credentials();
        if (result.status == OperationStatus::kMissing)
          state_ = StartupState::kCredentialBootstrap;
        else if (result.status == OperationStatus::kSuccess)
          state_ = result.credential_present ? StartupState::kMqttConnecting
                                             : StartupState::kCredentialBootstrap;
        else
          handle_failure(result);
        break;
      case StartupState::kCredentialBootstrap:
        result = services_.poll_credential_bootstrap();
        advance(result, StartupState::kMqttConnecting);
        break;
      case StartupState::kMqttConnecting:
        result = services_.poll_mqtt();
        advance(result, StartupState::kOnline);
        break;
      case StartupState::kOtaPendingValidation:
        result = services_.validate_pending_ota();
        advance(result, StartupState::kWifiConnecting);
        break;
      case StartupState::kControlledReset:
        result = services_.controlled_reset();
        if (result.status == OperationStatus::kFatalFailure) handle_failure(result);
        break;
      case StartupState::kUnprovisioned:
      case StartupState::kOnline:
      case StartupState::kDegraded:
      case StartupState::kFault:
        break;
    }
  }

  bool begin_provisioning() {
    if (state_ != StartupState::kUnprovisioned) return false;
    state_ = StartupState::kBleProvisioning;
    return true;
  }

  bool wifi_credentials_received() {
    if (state_ != StartupState::kBleProvisioning) return false;
    state_ = StartupState::kWifiConnecting;
    return true;
  }

  bool retry() {
    if (state_ != StartupState::kDegraded || retry_attempt_ >= retry_policy_.maximum_attempts)
      return false;
    ++retry_attempt_;
    state_ = retry_state_;
    reason_ = StartupReason::kNone;
    return true;
  }

  bool confirmed_reset(bool explicit_confirmation) {
    if (!explicit_confirmation) return false;
    state_ = StartupState::kControlledReset;
    reason_ = StartupReason::kExplicitReset;
    return true;
  }

 private:
  void advance(const StartupResult& result, StartupState next) {
    if (result.status == OperationStatus::kSuccess) {
      retry_attempt_ = 0;
      reason_ = StartupReason::kNone;
      state_ = next;
    } else if (result.status != OperationStatus::kPending) {
      handle_failure(result);
    }
  }

  void handle_failure(const StartupResult& result) {
    retry_state_ = state_;
    if (result.status == OperationStatus::kFatalFailure) {
      state_ = StartupState::kFault;
      reason_ = result.reason == StartupReason::kNone
                    ? StartupReason::kFatalModuleFailure
                    : result.reason;
      return;
    }
    if (retry_attempt_ >= retry_policy_.maximum_attempts) {
      state_ = StartupState::kFault;
      reason_ = StartupReason::kRetryExhausted;
      return;
    }
    state_ = StartupState::kDegraded;
    if (result.reason != StartupReason::kNone)
      reason_ = result.reason;
    else if (result.status == OperationStatus::kNotImplemented)
      reason_ = StartupReason::kModuleNotImplemented;
    else
      reason_ = StartupReason::kModuleUnavailable;
  }

  StartupServices& services_;
  RetryPolicy retry_policy_;
  StartupState state_{StartupState::kPlatformInit};
  StartupState retry_state_{StartupState::kPlatformInit};
  StartupReason reason_{StartupReason::kNone};
  std::uint8_t retry_attempt_{};
};

}  // namespace algaguard
