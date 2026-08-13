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

// pH: two-point linear calibration against the probe's analog voltage.
// kPhNeutralVoltage/kPhVoltsPerUnit MUST be re-measured for the specific
// PH4502C board + probe in use (calibrate against pH 4/7/10 buffer
// solutions) -- the values below are placeholders only, not a calibration.
inline constexpr double kPhNeutralVoltage = 1.65;  // voltage at pH 7 buffer
inline constexpr double kPhVoltsPerUnit = 0.18;    // slope, volts per pH unit

inline double ph_from_voltage(double voltageV) {
  return std::clamp(
      7.0 + (kPhNeutralVoltage - voltageV) / kPhVoltsPerUnit, 0.0, 14.0);
}

}  // namespace algaguard
