#pragma once

#include <algorithm>

namespace algaguard {

// TDS: DFRobot Gravity-TDS-sensor reference formula (temperature-compensated
// cubic fit on the probe's analog voltage). This is the standard published
// formula for this exact class of hobbyist TDS module -- a starting point,
// not a validated instrument for this specific probe/water chemistry.
inline double tds_ppm_from_voltage(double voltageV, double temperatureC) {
  const double compensationCoefficient = 1.0 + 0.02 * (temperatureC - 25.0);
  const double compensatedVoltage = voltageV / compensationCoefficient;
  const double v2 = compensatedVoltage * compensatedVoltage;
  const double v3 = v2 * compensatedVoltage;
  const double tds =
      (133.42 * v3 - 255.86 * v2 + 857.39 * compensatedVoltage) * 0.5;
  return std::max(0.0, tds);
}

// pH: linear conversion of the voltage seen on the pH ADC pin.
//
// Wiring: the PH-4502C runs on 5 V and its Po output reaches GPIO2 through a
// 10k / 18k divider (Po -> 10k -> GPIO2 -> 18k -> GND), so GPIO2 sees 0.643 x
// Po and never exceeds ~3.2 V. The board's offset trimmer (the one next to
// the BNC socket) is left as it is: the pH 7 point below is a ONE-POINT FIELD
// CALIBRATION (2026-10-07) -- the probe settled at 1669 mV on GPIO2 (Po
// 2.596 V) in tap water, which is taken as pH 7.0 (Sri Lankan tap water is
// specified at pH 6.5-8.5 and is typically near neutral). The slope is the
// board's typical 0.18 V per pH unit at Po. Replace both kPhBoard* values
// with a pH 4 / pH 7 buffer calibration before trusting absolute pH.
inline constexpr double kPhDividerRatio = 18.0 / (10.0 + 18.0);
inline constexpr double kPhBoardNeutralVoltage = 2.596;  // Po at pH 7
inline constexpr double kPhBoardVoltsPerUnit = 0.18;    // Po slope per pH unit
inline constexpr double kPhNeutralVoltage =
    kPhBoardNeutralVoltage * kPhDividerRatio;  // ~1.607 V on GPIO2
inline constexpr double kPhVoltsPerUnit =
    kPhBoardVoltsPerUnit * kPhDividerRatio;  // ~0.116 V per pH unit on GPIO2

inline double ph_from_voltage(double voltageV) {
  return std::clamp(
      7.0 + (kPhNeutralVoltage - voltageV) / kPhVoltsPerUnit, 0.0, 14.0);
}

}  // namespace algaguard
