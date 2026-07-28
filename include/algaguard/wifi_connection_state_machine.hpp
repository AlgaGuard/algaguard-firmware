#pragma once

#include <cstdint>
#include <string_view>
#include <utility>

#include "algaguard/ble_wifi_provisioning.hpp"

namespace algaguard {

inline constexpr std::uint64_t kWifiConnectTimeoutTicks = 20;
inline constexpr std::uint64_t kWifiRetryDelayTicks = 5;
inline constexpr std::uint8_t kWifiMaxConnectAttempts = 3;

enum class WifiConnectionState : std::uint8_t {
  kIdle,
  kCredentialsReady,
  kConnecting,
  kRetryWait,
  kConnected,
  kAuthFailed,
  kNetworkNotFound,
  kTimedOut,
  kCancelled,
  kDisconnected,
  kCleared,
};

enum class WifiConnectionReason : std::uint8_t {
  kOk,
  kNoCredentials,
  kInvalidCredentials,
  kConnectInProgress,
  kAuthenticationFailed,
  kNetworkNotFound,
  kTransientFailure,
  kConnectTimeout,
  kRetryExhausted,
  kCancelled,
  kDisconnected,
  kInvalidTransition,
  kHandoffAlreadyPresent,
};

enum class WifiDriverEvent : std::uint8_t {
  kConnected,
  kAuthenticationFailed,
  kNetworkNotFound,
  kTransientFailure,
  kDisconnected,
  kCancelled,
};

struct WifiConnectionResult {
  WifiConnectionState state{WifiConnectionState::kIdle};
  WifiConnectionReason safeReasonCode{WifiConnectionReason::kOk};
  std::uint8_t attemptNumber{};
  bool retryAllowed{};
  std::uint64_t nextRetryTick{};
  bool credentialsPresent{};
  bool secretsCleared{};
};

class WifiConnectionAdapter {
 public:
  virtual ~WifiConnectionAdapter() = default;
  virtual bool beginConnect(std::string_view ssid, std::string_view password) = 0;
  virtual void cancelConnect() = 0;
  virtual void disconnect() = 0;
  virtual bool isConnectInProgress() const = 0;
  virtual void clearSensitiveDriverInput() = 0;
};

class WifiConnectionStateMachine {
 public:
  explicit WifiConnectionStateMachine(WifiConnectionAdapter& adapter) : adapter_(adapter) {}
  WifiConnectionStateMachine(const WifiConnectionStateMachine&) = delete;
  WifiConnectionStateMachine& operator=(const WifiConnectionStateMachine&) = delete;
  ~WifiConnectionStateMachine() { shutdown(); }

  WifiConnectionResult acceptCredentials(BleWifiCredentialHandoff&& handoff) {
    if (credentials_.available()) return result(WifiConnectionReason::kHandoffAlreadyPresent, false);
    if (!handoff.available()) return result(WifiConnectionReason::kInvalidCredentials, false);
    if (state_ != WifiConnectionState::kIdle && state_ != WifiConnectionState::kCleared)
      return result(WifiConnectionReason::kInvalidTransition, false);
    credentials_ = std::move(handoff);
    state_ = WifiConnectionState::kCredentialsReady;
    return result(WifiConnectionReason::kOk, true);
  }

  WifiConnectionResult start(std::uint64_t nowTick) {
    if (state_ != WifiConnectionState::kCredentialsReady) {
      return result(credentials_.available() ? WifiConnectionReason::kInvalidTransition
                                             : WifiConnectionReason::kNoCredentials,
                    false);
    }
    return beginAttempt(nowTick);
  }

  WifiConnectionResult onDriverEvent(WifiDriverEvent event, std::uint64_t nowTick) {
    if (event == WifiDriverEvent::kConnected) {
      if (state_ != WifiConnectionState::kConnecting)
        return result(WifiConnectionReason::kInvalidTransition, false);
      adapter_.clearSensitiveDriverInput();
      credentials_.clear();
      state_ = WifiConnectionState::kConnected;
      return result(WifiConnectionReason::kOk, false);
    }
    if (event == WifiDriverEvent::kAuthenticationFailed) {
      if (!activeAttempt()) return result(WifiConnectionReason::kInvalidTransition, false);
      terminal(WifiConnectionState::kAuthFailed);
      return result(WifiConnectionReason::kAuthenticationFailed, false);
    }
    if (event == WifiDriverEvent::kCancelled) return cancel();
    if (event == WifiDriverEvent::kDisconnected) {
      if (state_ == WifiConnectionState::kConnected) {
        adapter_.disconnect();
        adapter_.clearSensitiveDriverInput();
        credentials_.clear();
        state_ = WifiConnectionState::kDisconnected;
        return result(WifiConnectionReason::kDisconnected, false);
      }
      if (state_ == WifiConnectionState::kConnecting || state_ == WifiConnectionState::kRetryWait)
        return retryOrTerminal(WifiConnectionReason::kDisconnected, nowTick,
                               WifiConnectionState::kDisconnected);
      return result(WifiConnectionReason::kInvalidTransition, false);
    }
    if (event == WifiDriverEvent::kNetworkNotFound || event == WifiDriverEvent::kTransientFailure) {
      if (!activeAttempt()) return result(WifiConnectionReason::kInvalidTransition, false);
      const auto reason = event == WifiDriverEvent::kNetworkNotFound
                              ? WifiConnectionReason::kNetworkNotFound
                              : WifiConnectionReason::kTransientFailure;
      const auto terminalState = event == WifiDriverEvent::kNetworkNotFound
                                     ? WifiConnectionState::kNetworkNotFound
                                     : WifiConnectionState::kTimedOut;
      return retryOrTerminal(reason, nowTick, terminalState);
    }
    return result(WifiConnectionReason::kInvalidTransition, false);
  }

  WifiConnectionResult onTick(std::uint64_t nowTick) {
    if (state_ == WifiConnectionState::kConnecting && nowTick >= attemptStartedTick_ &&
        nowTick - attemptStartedTick_ >= kWifiConnectTimeoutTicks)
      return retryOrTerminal(WifiConnectionReason::kConnectTimeout, nowTick,
                             WifiConnectionState::kTimedOut);
    if (state_ == WifiConnectionState::kRetryWait && nowTick >= nextRetryTick_)
      return beginAttempt(nowTick);
    return result(WifiConnectionReason::kOk, state_ == WifiConnectionState::kRetryWait);
  }

  WifiConnectionResult cancel() {
    if (state_ != WifiConnectionState::kCredentialsReady && state_ != WifiConnectionState::kConnecting &&
        state_ != WifiConnectionState::kRetryWait)
      return result(WifiConnectionReason::kInvalidTransition, false);
    terminal(WifiConnectionState::kCancelled);
    return result(WifiConnectionReason::kCancelled, false);
  }

  WifiConnectionResult reset() noexcept {
    adapter_.cancelConnect();
    adapter_.disconnect();
    adapter_.clearSensitiveDriverInput();
    credentials_.clear();
    attemptNumber_ = 0;
    attemptStartedTick_ = 0;
    nextRetryTick_ = 0;
    state_ = WifiConnectionState::kCleared;
    return result(WifiConnectionReason::kOk, false);
  }

  void shutdown() noexcept { (void)reset(); }
  WifiConnectionState state() const { return state_; }
  bool credentialsPresent() const { return credentials_.available(); }
  bool secretsCleared() const { return credentials_.secretsCleared(); }
  WifiConnectionResult status() const {
    return result(WifiConnectionReason::kOk, state_ == WifiConnectionState::kRetryWait);
  }

 private:
  bool activeAttempt() const {
    return state_ == WifiConnectionState::kConnecting || state_ == WifiConnectionState::kRetryWait;
  }

  WifiConnectionResult beginAttempt(std::uint64_t nowTick) {
    if (!credentials_.available()) return result(WifiConnectionReason::kNoCredentials, false);
    if (attemptNumber_ >= kWifiMaxConnectAttempts) {
      terminal(WifiConnectionState::kTimedOut);
      return result(WifiConnectionReason::kRetryExhausted, false);
    }
    ++attemptNumber_;
    const bool started = adapter_.beginConnect(credentials_.ssid(), credentials_.password());
    adapter_.clearSensitiveDriverInput();
    if (!started) return retryOrTerminal(WifiConnectionReason::kTransientFailure, nowTick,
                                         WifiConnectionState::kTimedOut);
    attemptStartedTick_ = nowTick;
    nextRetryTick_ = 0;
    state_ = WifiConnectionState::kConnecting;
    return result(WifiConnectionReason::kConnectInProgress, true);
  }

  WifiConnectionResult retryOrTerminal(WifiConnectionReason reason, std::uint64_t nowTick,
                                       WifiConnectionState terminalState) {
    adapter_.cancelConnect();
    adapter_.clearSensitiveDriverInput();
    if (attemptNumber_ < kWifiMaxConnectAttempts) {
      state_ = WifiConnectionState::kRetryWait;
      nextRetryTick_ = nowTick + kWifiRetryDelayTicks;
      return result(reason, true);
    }
    terminal(terminalState);
    return result(reason == WifiConnectionReason::kConnectTimeout ? reason
                                                                    : WifiConnectionReason::kRetryExhausted,
                  false);
  }

  void terminal(WifiConnectionState state) noexcept {
    adapter_.cancelConnect();
    adapter_.clearSensitiveDriverInput();
    credentials_.clear();
    nextRetryTick_ = 0;
    state_ = state;
  }

  WifiConnectionResult result(WifiConnectionReason reason, bool retryAllowed) const {
    return {state_, reason, attemptNumber_, retryAllowed, nextRetryTick_, credentials_.available(),
            credentials_.secretsCleared()};
  }

  WifiConnectionAdapter& adapter_;
  BleWifiCredentialHandoff credentials_;
  WifiConnectionState state_{WifiConnectionState::kIdle};
  std::uint8_t attemptNumber_{};
  std::uint64_t attemptStartedTick_{};
  std::uint64_t nextRetryTick_{};
};

}  // namespace algaguard
