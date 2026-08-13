#pragma once

#include <array>
#include <cstdint>
#include <ctime>
#include <optional>

#include "driver/i2c_master.h"

namespace algaguard {

// DS3231 real-time clock, added onto an already-open I2C bus. Role: this
// device has no battery-backed RTC of its own, and SNTP (the primary clock
// source, synced on every boot -- see ensureTrustedClock()) needs network
// access. The DS3231 supplements that: disciplined from SNTP whenever it
// succeeds, and used as a fallback wall-clock (for timestamping SD-buffered
// samples) on a boot where SNTP hasn't succeeded yet. It never replaces
// SNTP as the primary source.
class Ds3231Rtc {
 public:
  static constexpr std::uint8_t kAddress = 0x68;

  esp_err_t configure(i2c_master_bus_handle_t bus) {
    i2c_device_config_t deviceConfig{};
    deviceConfig.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    deviceConfig.device_address = kAddress;
    deviceConfig.scl_speed_hz = 100000;
    return i2c_master_bus_add_device(bus, &deviceConfig, &device_);
  }

  // True means the oscillator was interrupted (power loss with no backup,
  // or never set) -- any prior write is not trustworthy and readUtc() must
  // not be relied on until a fresh writeUtc() clears this flag.
  bool oscillatorStopped() const {
    std::uint8_t status = 0;
    if (!readRegister(0x0F, status)) return true;
    return (status & 0x80U) != 0;
  }

  std::optional<std::time_t> readUtc() const {
    std::array<std::uint8_t, 7> registers{};
    if (!readRegisters(0x00, registers)) return std::nullopt;
    std::tm parsed{};
    parsed.tm_sec = bcdToDecimal(registers[0] & 0x7FU);
    parsed.tm_min = bcdToDecimal(registers[1] & 0x7FU);
    parsed.tm_hour = bcdToDecimal(registers[2] & 0x3FU);  // 24-hour mode
    parsed.tm_mday = bcdToDecimal(registers[4] & 0x3FU);
    parsed.tm_mon = bcdToDecimal(registers[5] & 0x1FU) - 1;
    parsed.tm_year = bcdToDecimal(registers[6]) + 100;  // years since 1900
    const std::time_t epoch = timegm(&parsed);
    return epoch > 0 ? std::optional<std::time_t>{epoch} : std::nullopt;
  }

  bool writeUtc(std::time_t utc) {
    std::tm parsed{};
    gmtime_r(&utc, &parsed);
    const std::array<std::uint8_t, 8> payload{
        0x00,
        decimalToBcd(static_cast<int>(parsed.tm_sec)),
        decimalToBcd(parsed.tm_min),
        decimalToBcd(parsed.tm_hour),
        decimalToBcd(parsed.tm_wday + 1),
        decimalToBcd(parsed.tm_mday),
        decimalToBcd(parsed.tm_mon + 1),
        decimalToBcd(parsed.tm_year % 100),
    };
    if (i2c_master_transmit(device_, payload.data(), payload.size(), 100) !=
        ESP_OK)
      return false;
    // Clear OSF (bit 7 of the status register) now that we've set a trusted
    // time, leaving the other status bits untouched.
    std::uint8_t status = 0;
    if (!readRegister(0x0F, status)) return false;
    const std::array<std::uint8_t, 2> clearOsf{
        0x0F, static_cast<std::uint8_t>(status & 0x7FU)};
    return i2c_master_transmit(device_, clearOsf.data(), clearOsf.size(),
                               100) == ESP_OK;
  }

 private:
  static int bcdToDecimal(std::uint8_t bcd) {
    return ((bcd >> 4) * 10) + (bcd & 0x0FU);
  }
  static std::uint8_t decimalToBcd(int decimal) {
    return static_cast<std::uint8_t>(((decimal / 10) << 4) | (decimal % 10));
  }

  bool readRegister(std::uint8_t reg, std::uint8_t& value) const {
    return i2c_master_transmit_receive(device_, &reg, 1, &value, 1, 100) ==
           ESP_OK;
  }
  template <std::size_t N>
  bool readRegisters(std::uint8_t startReg, std::array<std::uint8_t, N>& out) const {
    return i2c_master_transmit_receive(device_, &startReg, 1, out.data(),
                                       out.size(), 100) == ESP_OK;
  }

  i2c_master_dev_handle_t device_{};
};

}  // namespace algaguard
