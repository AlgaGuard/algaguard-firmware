#pragma once

#include <algorithm>
#include <cmath>

namespace algaguard {

// There is only a TDS (total dissolved solids) probe on this device, not
// separate nitrate/phosphate/potassium ion-selective electrodes, so a single
// composite Nutrient Strength Index stands in for the three separate NPK
// values the platform previously reported. This is a soft-sensing heuristic
// (TDS as the primary dissolved-nutrient proxy, pH and temperature as
// availability/uptake correction factors) -- not a validated instrument.
// The reference/optimal-band constants below are starting points and MUST be
// checked against real calibration data before the output is trusted.
inline constexpr double kNutrientReferencePpm = 1200.0;
inline constexpr double kNutrientPhOptimalLow = 6.5;
inline constexpr double kNutrientPhOptimalHigh = 8.5;
inline constexpr double kNutrientPhTolerance = 1.5;
inline constexpr double kNutrientTempOptimalLow = 20.0;
inline constexpr double kNutrientTempOptimalHigh = 30.0;
inline constexpr double kNutrientTempTolerance = 5.0;

// 1.0 inside [low, high], linearly decaying to 0.0 at `tolerance` beyond
// either edge, clamped at 0.0 further out.
inline double triangular_suitability(double value, double low, double high,
                                     double tolerance) {
  if (value >= low && value <= high) return 1.0;
  const double distance = value < low ? low - value : value - high;
  if (tolerance <= 0.0) return 0.0;
  return std::clamp(1.0 - distance / tolerance, 0.0, 1.0);
}

inline double nutrient_percent(double tdsPpm, double ph, double tempC) {
  const double tdsNormalized =
      std::clamp(tdsPpm / kNutrientReferencePpm, 0.0, 1.0);
  const double phFactor = triangular_suitability(
      ph, kNutrientPhOptimalLow, kNutrientPhOptimalHigh, kNutrientPhTolerance);
  const double tempFactor = triangular_suitability(
      tempC, kNutrientTempOptimalLow, kNutrientTempOptimalHigh,
      kNutrientTempTolerance);
  return std::clamp(tdsNormalized * phFactor * tempFactor * 100.0, 0.0, 100.0);
}

}  // namespace algaguard
