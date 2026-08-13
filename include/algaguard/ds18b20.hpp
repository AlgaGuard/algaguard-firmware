#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "algaguard/crc8.hpp"
#include "algaguard/onewire_bus.hpp"
#include "freertos/FreeRTOS.h"

namespace algaguard {

// Single-drop DS18B20 driver (skip-ROM addressing -- this device wires
// exactly one probe; a second probe on the same bus would need ROM search,
// out of scope here). Non-blocking usage pattern: call startConversion(),
// then some time later (>=750ms for 12-bit resolution) call readCelsius()
// for that result while immediately calling startConversion() again for the
// next reading -- see sampling_task in main.cpp for the actual pipelining.
class Ds18b20Sensor {
 public:
  explicit Ds18b20Sensor(gpio_num_t pin) : bus_(pin) {}

  esp_err_t configure() {
    const esp_err_t result = bus_.configure();
    if (result != ESP_OK) return result;
    portENTER_CRITICAL(&lock_);
    bool ok = bus_.reset();
    if (ok) {
      bus_.writeByte(0xCC);  // Skip ROM
      bus_.writeByte(0x4E);  // Write Scratchpad
      bus_.writeByte(0x00);  // TH (unused, alarm search not used)
      bus_.writeByte(0x00);  // TL (unused)
      bus_.writeByte(0x7F);  // 12-bit resolution
    }
    portEXIT_CRITICAL(&lock_);
    return ok ? ESP_OK : ESP_ERR_NOT_FOUND;
  }

  // Returns false if no presence pulse was detected (probe unwired/absent).
  bool startConversion() {
    portENTER_CRITICAL(&lock_);
    const bool present = bus_.reset();
    if (present) {
      bus_.writeByte(0xCC);  // Skip ROM
      bus_.writeByte(0x44);  // Convert T
    }
    portEXIT_CRITICAL(&lock_);
    return present;
  }

  // Reads back whatever conversion last completed. Returns nullopt if the
  // probe didn't respond or the scratchpad failed its CRC8 check.
  std::optional<double> readCelsius() {
    std::array<std::uint8_t, 9> scratchpad{};
    portENTER_CRITICAL(&lock_);
    const bool present = bus_.reset();
    if (present) {
      bus_.writeByte(0xCC);  // Skip ROM
      bus_.writeByte(0xBE);  // Read Scratchpad
      bus_.readBytes(scratchpad.data(), scratchpad.size());
    }
    portEXIT_CRITICAL(&lock_);
    if (!present) return std::nullopt;
    if (crc8_dallas(scratchpad.data(), 8) != scratchpad[8]) return std::nullopt;
    const std::int16_t raw = static_cast<std::int16_t>(
        (static_cast<std::uint16_t>(scratchpad[1]) << 8) | scratchpad[0]);
    return raw / 16.0;
  }

 private:
  onewire::Bus bus_;
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
};

}  // namespace algaguard
