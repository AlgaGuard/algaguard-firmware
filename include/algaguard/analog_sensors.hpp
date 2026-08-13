#pragma once

#include <cstdint>
#include <optional>

#include "algaguard/analog_conversion.hpp"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

namespace algaguard {

// TDS and pH are both plain analog reads, deliberately kept on ADC1 rather
// than ADC2: ADC2 shares hardware with the Wi-Fi radio on the ESP32-S3 and
// gives unreliable readings once Wi-Fi is active, which this device always
// has enabled. Takes GPIO numbers (not raw ADC channel indices) and
// resolves them via adc_oneshot_io_to_channel() -- GPIO-to-channel mapping
// is chip-specific (e.g. GPIO1 is channel 0, not channel 1, on the S3's
// ADC1) and must not be hand-computed from the pin number.
class AnalogSensors {
 public:
  esp_err_t configure(gpio_num_t tdsPin, gpio_num_t phPin) {
    adc_unit_t tdsUnit{};
    adc_unit_t phUnit{};
    esp_err_t result = adc_oneshot_io_to_channel(tdsPin, &tdsUnit, &tdsChannel_);
    if (result != ESP_OK || tdsUnit != ADC_UNIT_1) return ESP_ERR_INVALID_ARG;
    result = adc_oneshot_io_to_channel(phPin, &phUnit, &phChannel_);
    if (result != ESP_OK || phUnit != ADC_UNIT_1) return ESP_ERR_INVALID_ARG;

    adc_oneshot_unit_init_cfg_t unitConfig{};
    unitConfig.unit_id = ADC_UNIT_1;
    result = adc_oneshot_new_unit(&unitConfig, &unit_);
    if (result != ESP_OK) return result;

    adc_oneshot_chan_cfg_t channelConfig{};
    channelConfig.atten = ADC_ATTEN_DB_12;
    channelConfig.bitwidth = ADC_BITWIDTH_DEFAULT;
    result = adc_oneshot_config_channel(unit_, tdsChannel_, &channelConfig);
    if (result != ESP_OK) return result;
    result = adc_oneshot_config_channel(unit_, phChannel_, &channelConfig);
    if (result != ESP_OK) return result;

    adc_cali_curve_fitting_config_t tdsCaliConfig{};
    tdsCaliConfig.unit_id = ADC_UNIT_1;
    tdsCaliConfig.chan = tdsChannel_;
    tdsCaliConfig.atten = ADC_ATTEN_DB_12;
    tdsCaliConfig.bitwidth = ADC_BITWIDTH_DEFAULT;
    result = adc_cali_create_scheme_curve_fitting(&tdsCaliConfig, &tdsCali_);
    if (result != ESP_OK) return result;

    adc_cali_curve_fitting_config_t phCaliConfig{};
    phCaliConfig.unit_id = ADC_UNIT_1;
    phCaliConfig.chan = phChannel_;
    phCaliConfig.atten = ADC_ATTEN_DB_12;
    phCaliConfig.bitwidth = ADC_BITWIDTH_DEFAULT;
    return adc_cali_create_scheme_curve_fitting(&phCaliConfig, &phCali_);
  }

  std::optional<double> readTdsPpm(double temperatureC) const {
    const auto voltage = readVoltage(tdsChannel_, tdsCali_);
    if (!voltage) return std::nullopt;
    return tds_ppm_from_voltage(*voltage, temperatureC);
  }

  std::optional<double> readPh() const {
    const auto voltage = readVoltage(phChannel_, phCali_);
    if (!voltage) return std::nullopt;
    return ph_from_voltage(*voltage);
  }

 private:
  std::optional<double> readVoltage(adc_channel_t channel,
                                    adc_cali_handle_t cali) const {
    if (unit_ == nullptr || cali == nullptr) return std::nullopt;
    int raw = 0;
    if (adc_oneshot_read(unit_, channel, &raw) != ESP_OK) return std::nullopt;
    int millivolts = 0;
    if (adc_cali_raw_to_voltage(cali, raw, &millivolts) != ESP_OK)
      return std::nullopt;
    return millivolts / 1000.0;
  }

  adc_oneshot_unit_handle_t unit_{};
  adc_cali_handle_t tdsCali_{};
  adc_cali_handle_t phCali_{};
  adc_channel_t tdsChannel_{};
  adc_channel_t phChannel_{};
};

}  // namespace algaguard
