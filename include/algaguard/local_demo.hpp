#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "algaguard/display.hpp"
#include "algaguard/nutrient_index.hpp"

namespace algaguard {

inline constexpr const char* kLocalDemoClassification =
    "DEVELOPMENT_ONLY_LOCAL_MOCK_SENSORS";

inline constexpr bool local_demo_profile_allowed(bool development,
                                                  bool production,
                                                  bool release,
                                                  bool enabled) {
  return development && !production && !release && enabled;
}

struct LocalDemoReading {
  std::uint64_t sequence{};
  double temperatureC{};
  double ph{};
  double lightLux{};
  double nutrientPercent{};
};

class LocalDemoGenerator {
 public:
  explicit LocalDemoGenerator(std::uint32_t seed = 37) : seed_(seed) {}

  LocalDemoReading next(std::uint64_t sequence) const {
    const double step = static_cast<double>((sequence + seed_) % 10000U);
    const double slow = std::sin(step / 17.0);
    const double slower = std::sin(step / 43.0 + seed_);
    const double temperatureC = rounded(24.0 + slow * 0.8 + slower * 0.2, 2);
    const double ph = rounded(7.1 + slow * 0.12, 2);
    // Fake TDS wave (not itself a wire field) feeding the same Nutrient
    // Strength Index formula the real-sensor path uses, so mock and real
    // readings are shaped identically for every downstream consumer.
    const double fakeTdsPpm = 700.0 + slow * 120.0 + slower * 40.0;
    return {sequence, temperatureC, ph,
            rounded(900.0 + slow * 110.0 + slower * 35.0, 0),
            rounded(nutrient_percent(fakeTdsPpm, ph, temperatureC), 1)};
  }

 private:
  static double rounded(double value, int decimals) {
    const double scale = decimals == 0 ? 1.0 : decimals == 1 ? 10.0 : 100.0;
    return std::round(value * scale) / scale;
  }
  std::uint32_t seed_;
};

// kHome intentionally dropped: it was only ever a navigation-anchor
// placeholder ("you're at the top level"), a role the real main menu screen
// now fills directly.
enum class LocalDemoPage : std::uint8_t {
  kTemperaturePh,
  kLight,
  kNutrients,
  kDeviceStatus,
  kNetwork,
  kAbout,
};

enum class LocalDemoNetworkState : std::uint8_t {
  kReady,
  kConfirmForget,
  kForgotten,
  kForgetFailed,
};

enum class LocalDemoAction : std::uint8_t { kNone, kForgetSavedWifi };

// Tracks just the Network screen's forget-WiFi confirmation sub-flow. Page
// selection/cycling now lives in the main menu (main.cpp's MainMenuNav) --
// this class no longer owns a "current page" concept.
class LocalDemoNetworkFlow {
 public:
  LocalDemoAction select() {
    if (networkState_ == LocalDemoNetworkState::kReady ||
        networkState_ == LocalDemoNetworkState::kForgotten ||
        networkState_ == LocalDemoNetworkState::kForgetFailed) {
      networkState_ = LocalDemoNetworkState::kConfirmForget;
      return LocalDemoAction::kNone;
    }
    return LocalDemoAction::kForgetSavedWifi;
  }
  void setForgetResult(bool succeeded) {
    networkState_ = succeeded ? LocalDemoNetworkState::kForgotten
                              : LocalDemoNetworkState::kForgetFailed;
  }
  void reset() { networkState_ = LocalDemoNetworkState::kReady; }
  LocalDemoNetworkState networkState() const { return networkState_; }

 private:
  LocalDemoNetworkState networkState_{LocalDemoNetworkState::kReady};
};

inline std::string local_demo_value(const char* label, double value,
                                    unsigned decimals, const char* unit = "") {
  char output[32]{};
  std::snprintf(output, sizeof(output), "%s %.*f%s", label,
                static_cast<int>(decimals), value, unit);
  return output;
}

inline DiagnosticScreen local_demo_screen(LocalDemoPage page,
                                          const LocalDemoReading& reading,
                                          bool advertisingActive,
                                          bool wifiConnected = false,
                                          bool cloudConnected = false,
                                          bool developmentWifiStored = false,
                                          LocalDemoNetworkState networkState =
                                              LocalDemoNetworkState::kReady) {
  const char* const wifiState =
      wifiConnected ? "WIFI CONNECTED" : "WIFI SETUP REQUIRED";
  const char* const cloudState =
      cloudConnected ? "CLOUD CONNECTED" : "CLOUD NOT READY";
  switch (page) {
    case LocalDemoPage::kTemperaturePh:
      return {{{"DEMO TEMP PH",
                local_demo_value("TEMP", reading.temperatureC, 2, " C"),
                local_demo_value("PH", reading.ph, 2), "DEV GENERATED"}}};
    case LocalDemoPage::kLight:
      return {{{"DEMO LIGHT",
                local_demo_value("LIGHT", reading.lightLux, 0, " LUX"),
                "DEV GENERATED", cloudState}}};
    case LocalDemoPage::kNutrients:
      return {{{"DEMO NUTRIENTS",
                local_demo_value("NSI", reading.nutrientPercent, 1, " PCT"),
                "DEV GENERATED", cloudState}}};
    case LocalDemoPage::kDeviceStatus:
      return {{{advertisingActive ? "BLE ADV ACTIVE" : "BLE ADV INIT",
                wifiState, cloudState, "DEV GENERATED"}}};
    case LocalDemoPage::kNetwork:
      if (networkState == LocalDemoNetworkState::kConfirmForget)
        return {{{"FORGET WIFI?", "SELECT CONFIRM", "BACK CANCEL", "NO VALUES SHOWN"}}};
      if (networkState == LocalDemoNetworkState::kForgotten)
        return {{{"WIFI FORGOTTEN", "RESTART SETUP", "NO VALUES SHOWN", "BACK HOME"}}};
      if (networkState == LocalDemoNetworkState::kForgetFailed)
        return {{{"FORGET FAILED", "WIFI UNCHANGED", "BACK CANCEL", "NO VALUES SHOWN"}}};
      return {{{"NETWORK", wifiState,
                developmentWifiStored ? "DEV NVS ENABLED" : "RAM ONLY",
                "SELECT FORGET"}}};
    case LocalDemoPage::kAbout:
      return {{{"AlgaGuard", "FW 0.2.0 DEMO", "DEV BOARD", "NO SECRETS"}}};
  }
  return {{{"AlgaGuard", "DEVICE DATA MODE", cloudState, "NO SECRETS"}}};
}

inline constexpr bool local_demo_never_uses_network() { return true; }
inline constexpr bool local_demo_persists_samples() { return false; }

}  // namespace algaguard
