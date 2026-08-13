#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace algaguard {

// BH1750FVI ambient light sensor, added onto an already-open I2C bus (this
// device shares the OLED's bus rather than owning a separate one).
class Bh1750Sensor {
 public:
  static constexpr std::uint8_t kDefaultAddress = 0x23;

  esp_err_t configure(i2c_master_bus_handle_t bus,
                      std::uint8_t address = kDefaultAddress) {
    i2c_device_config_t deviceConfig{};
    deviceConfig.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    deviceConfig.device_address = address;
    deviceConfig.scl_speed_hz = 100000;
    esp_err_t result = i2c_master_bus_add_device(bus, &deviceConfig, &device_);
    if (result != ESP_OK) return result;
    const std::uint8_t powerOn = 0x01;
    result = i2c_master_transmit(device_, &powerOn, 1, 100);
    if (result != ESP_OK) return result;
    const std::uint8_t continuousHighRes = 0x10;
    result = i2c_master_transmit(device_, &continuousHighRes, 1, 100);
    if (result != ESP_OK) return result;
    // One-time settle for the first continuous-mode measurement; the sensor
    // free-runs after this, so no further per-read delay is needed.
    vTaskDelay(pdMS_TO_TICKS(180));
    return ESP_OK;
  }

  std::optional<double> readLux() const {
    if (device_ == nullptr) return std::nullopt;
    std::array<std::uint8_t, 2> raw{};
    if (i2c_master_receive(device_, raw.data(), raw.size(), 100) != ESP_OK)
      return std::nullopt;
    const std::uint16_t value =
        (static_cast<std::uint16_t>(raw[0]) << 8) | raw[1];
    return value / 1.2;
  }

 private:
  i2c_master_dev_handle_t device_{};
};

}  // namespace algaguard
