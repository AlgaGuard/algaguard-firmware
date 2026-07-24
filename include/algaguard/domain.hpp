#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace algaguard {
enum class ProvisioningState { kUnprovisioned, kBleAdvertising, kWifiConnecting, kBootstrap, kProvisioned, kFault };
enum class MenuPage { kBoot, kSetupQr, kFallbackCode, kHome, kTelemetry, kNetwork, kDeviceInformation, kCertificateCloud, kLeds, kOta, kError, kResetConfirmation };
enum class OtaState { kIdle, kEligible, kDownloading, kVerified, kPendingBootValidation, kValid, kRollback };
enum class ButtonEvent { kNone, kShortPress };

struct SimulatedSample {
  std::string sequence;
  std::uint64_t uptime_ms;
  double temperature_c;
  double ph;
  double light_lux;
  double nitrate_mg_l;
  double phosphate_mg_l;
  double potassium_mg_l;
  std::string timestamp_quality;
  std::string profile_id;
  std::string profile_version;
  bool simulated{true};
};

class DeterministicSimulator {
 public:
  explicit DeterministicSimulator(std::uint32_t seed) : state_(seed) {}
  SimulatedSample next(std::uint64_t sequence, std::uint64_t uptime_ms) {
    return {std::to_string(sequence), uptime_ms, 22.0 + unit() * 4.0, 6.5 + unit(), 800.0 + unit() * 400.0,
            2.0 + unit(), 0.2 + unit() * 0.3, 1.0 + unit(), "NTP_SYNCED",
            "00000000-0000-4000-8000-000000000001", "1.0.0", true};
  }
 private:
  double unit() { state_ = 1664525U * state_ + 1013904223U; return static_cast<double>(state_ & 0x00FFFFFFU) / 16777215.0; }
  std::uint32_t state_;
};

class DebouncedButton {
 public:
  explicit DebouncedButton(std::uint32_t debounce_ms = 35) : debounce_ms_(debounce_ms) {}
  ButtonEvent update(bool active_low_pressed, std::uint32_t now_ms) {
    if (active_low_pressed != candidate_) { candidate_ = active_low_pressed; candidate_since_ = now_ms; }
    if (candidate_ != stable_ && now_ms - candidate_since_ >= debounce_ms_) {
      const bool previous = stable_; stable_ = candidate_;
      if (previous && !stable_) return ButtonEvent::kShortPress;
    }
    return ButtonEvent::kNone;
  }
 private:
  std::uint32_t debounce_ms_;
  std::uint32_t candidate_since_{0};
  bool candidate_{false};
  bool stable_{false};
};

class MenuController {
 public:
  MenuPage page() const { return pages_[index_]; }
  void down() { index_ = (index_ + 1) % pages_.size(); }
  void up() { index_ = (index_ + pages_.size() - 1) % pages_.size(); }
  void select() { if (page() == MenuPage::kResetConfirmation) reset_confirmed_ = true; }
  void back() { index_ = 3; reset_confirmed_ = false; }
  bool reset_confirmed() const { return reset_confirmed_; }
 private:
  std::array<MenuPage, 12> pages_{MenuPage::kBoot, MenuPage::kSetupQr, MenuPage::kFallbackCode, MenuPage::kHome,
                                  MenuPage::kTelemetry, MenuPage::kNetwork, MenuPage::kDeviceInformation,
                                  MenuPage::kCertificateCloud, MenuPage::kLeds, MenuPage::kOta, MenuPage::kError,
                                  MenuPage::kResetConfirmation};
  std::size_t index_{3};
  bool reset_confirmed_{false};
};

enum class LedPriority { kDisconnected, kSetupOrOta, kConnected, kFault };
struct LedState { bool red{}; bool green{}; bool blue{}; };
inline LedState led_state(LedPriority priority, bool remote_indicator) {
  if (priority == LedPriority::kFault) return {true, false, false};
  if (priority == LedPriority::kConnected) return {false, true, false};
  if (priority == LedPriority::kSetupOrOta) return {false, false, true};
  return {false, false, remote_indicator};
}

struct SetupQrPayload {
  std::string device_id;
  std::string ble_service_uuid;
  std::string fallback_code;
  bool safe_for_display() const { return !device_id.empty() && !ble_service_uuid.empty() && !fallback_code.empty(); }
};

enum class BleMessageType { kHello, kDeviceInfo, kProvisionBegin, kWifiCredentials, kProvisionStatus, kProvisionComplete, kProvisionError };
class BleProvisioningStateMachine {
 public:
  bool accept(BleMessageType type, std::size_t bytes) {
    if (bytes == 0 || bytes > 1024) return false;
    if (type == BleMessageType::kHello && state_ == 0) { state_ = 1; return true; }
    if (type == BleMessageType::kDeviceInfo && state_ == 1) { state_ = 2; return true; }
    if (type == BleMessageType::kProvisionBegin && state_ == 2) { state_ = 3; return true; }
    if (type == BleMessageType::kWifiCredentials && state_ == 3) { state_ = 4; return true; }
    if (type == BleMessageType::kProvisionComplete && state_ == 4) { state_ = 5; return true; }
    if (type == BleMessageType::kProvisionError && state_ >= 3) { state_ = 6; return true; }
    return false;
  }
  bool complete() const { return state_ == 5; }
 private:
  int state_{0};
};

class SecretBuffer {
 public:
  void assign(std::string value) { value_ = std::move(value); }
  void clear() { std::fill(value_.begin(), value_.end(), '\0'); value_.clear(); }
  bool empty() const { return value_.empty(); }
 private:
  std::string value_;
};

enum class CommandStatus { kReceived, kInProgress, kCompleted, kRejectedExpired, kRejectedDuplicate };
class CommandProcessor {
 public:
  CommandStatus accept(std::string command_id, std::string type, std::uint64_t now_epoch, std::uint64_t expires_epoch) {
    if (command_id.empty() || (type != "REQUEST_STATUS" && type != "SET_INDICATOR_STATE")) return CommandStatus::kRejectedExpired;
    if (expires_epoch <= now_epoch) return CommandStatus::kRejectedExpired;
    if (!processed_.insert(std::move(command_id)).second) return CommandStatus::kRejectedDuplicate;
    return CommandStatus::kReceived;
  }
 private:
  std::set<std::string> processed_;
};

class TelemetryBatcher {
 public:
  explicit TelemetryBatcher(std::size_t target = 10) : target_(target) {}
  bool add(SimulatedSample sample) { if (samples_.size() >= target_) return false; samples_.push_back(std::move(sample)); return true; }
  bool ready() const { return samples_.size() == target_; }
  std::vector<SimulatedSample> take_for_qos1() { auto result = samples_; samples_.clear(); return result; }
 private:
  std::size_t target_;
  std::vector<SimulatedSample> samples_;
};

template <typename T>
class LocalQueue {
 public:
  explicit LocalQueue(std::size_t capacity) : capacity_(capacity) {}
  bool push(T value) { if (values_.size() >= capacity_) return false; values_.push_back(std::move(value)); return true; }
  std::optional<T> front() const { return values_.empty() ? std::nullopt : std::optional<T>{values_.front()}; }
  void acknowledge() { if (!values_.empty()) values_.pop_front(); }
  std::size_t size() const { return values_.size(); }
 private:
  std::size_t capacity_;
  std::deque<T> values_;
};

class OtaStateMachine {
 public:
  OtaState state() const { return state_; }
  bool accept_manifest(bool model_matches, bool newer_or_recovery, bool unexpired, bool signed_manifest) {
    if (!(model_matches && newer_or_recovery && unexpired && signed_manifest)) return false;
    state_ = OtaState::kEligible; return true;
  }
  void downloaded(bool size_and_sha256_valid, bool signature_valid) { state_ = size_and_sha256_valid && signature_valid ? OtaState::kVerified : OtaState::kRollback; }
  void begin_boot_validation() { if (state_ == OtaState::kVerified) state_ = OtaState::kPendingBootValidation; }
  void validate_boot(bool critical_initialization_ok) { state_ = critical_initialization_ok ? OtaState::kValid : OtaState::kRollback; }
 private:
  OtaState state_{OtaState::kIdle};
};
}  // namespace algaguard
