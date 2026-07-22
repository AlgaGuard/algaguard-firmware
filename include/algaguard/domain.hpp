#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace algaguard {
enum class ProvisioningState { kUnprovisioned, kBleAdvertising, kWifiConnecting, kBootstrap, kProvisioned, kFault };
enum class MenuPage { kSetupQr, kHome, kTelemetry, kNetwork, kDeviceInformation, kOta, kResetConfirmation };
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
  void back() { index_ = 1; }
 private:
  std::array<MenuPage, 7> pages_{MenuPage::kSetupQr, MenuPage::kHome, MenuPage::kTelemetry, MenuPage::kNetwork,
                                MenuPage::kDeviceInformation, MenuPage::kOta, MenuPage::kResetConfirmation};
  std::size_t index_{1};
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
