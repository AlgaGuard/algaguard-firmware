#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

#include "algaguard/npk_estimator_weights.hpp"

namespace algaguard {

// Nitrate, phosphate and potassium estimated from pH and temperature by the
// models in npk_estimator_weights.hpp. These are model estimates, not
// measurements; phosphate and potassium are experimental (see that header).
struct NpkEstimate {
  std::optional<double> nitrateMgL;
  std::optional<double> phosphateMgL;
  std::optional<double> potassiumMgL;
};

namespace npk_detail {

inline double relu(double value) { return value > 0.0 ? value : 0.0; }

template <std::size_t Hidden1, std::size_t Hidden2>
double mlp_1_to_1(double input, double inputMean, double inputScale,
                  const double (&w1)[Hidden1], const double (&b1)[Hidden1],
                  const double (&w2)[Hidden1 * Hidden2],
                  const double (&b2)[Hidden2], const double (&w3)[Hidden2],
                  double b3, double outputMean, double outputScale) {
  const double x = (input - inputMean) / inputScale;
  double layer1[Hidden1];
  for (std::size_t j = 0; j < Hidden1; ++j) layer1[j] = relu(x * w1[j] + b1[j]);
  double output = b3;
  for (std::size_t k = 0; k < Hidden2; ++k) {
    double sum = b2[k];
    for (std::size_t j = 0; j < Hidden1; ++j) sum += layer1[j] * w2[j * Hidden2 + k];
    output += relu(sum) * w3[k];
  }
  return output * outputScale + outputMean;
}

inline bool in_range(double value, double low, double high) {
  return std::isfinite(value) && value >= low && value <= high;
}

}  // namespace npk_detail

inline double estimate_nitrate_mg_l(double ph) {
  using namespace npk_weights;
  return npk_detail::mlp_1_to_1(ph, kNitrateInputMean, kNitrateInputScale,
                                kNitrateW1, kNitrateB1, kNitrateW2, kNitrateB2,
                                kNitrateW3, kNitrateB3, kNitrateOutputMean,
                                kNitrateOutputScale);
}

inline double estimate_phosphate_mg_l(double ph) {
  using namespace npk_weights;
  return npk_detail::mlp_1_to_1(ph, kPhosphateInputMean, kPhosphateInputScale,
                                kPhosphateW1, kPhosphateB1, kPhosphateW2,
                                kPhosphateB2, kPhosphateW3, kPhosphateB3,
                                kPhosphateOutputMean, kPhosphateOutputScale);
}

inline double estimate_potassium_mg_l(double temperatureC) {
  using namespace npk_weights;
  const double z = (temperatureC - kPotassiumInputMean) / kPotassiumInputScale;
  return kPotassiumIntercept + kPotassiumCoef[0] + kPotassiumCoef[1] * z +
         kPotassiumCoef[2] * z * z + kPotassiumCoef[3] * z * z * z;
}

// Readings outside the range the models were trained on are not
// extrapolated -- the corresponding estimate is left empty, and a negative
// prediction (possible at the high-pH edge) is reported as 0 mg/L.
inline NpkEstimate estimate_npk(double ph, double temperatureC) {
  using namespace npk_weights;
  NpkEstimate estimate;
  if (npk_detail::in_range(ph, kPhMin, kPhMax)) {
    estimate.nitrateMgL = std::max(0.0, estimate_nitrate_mg_l(ph));
    estimate.phosphateMgL = std::max(0.0, estimate_phosphate_mg_l(ph));
  }
  if (npk_detail::in_range(temperatureC, kTempMinC, kTempMaxC))
    estimate.potassiumMgL = std::max(0.0, estimate_potassium_mg_l(temperatureC));
  return estimate;
}

}  // namespace algaguard
