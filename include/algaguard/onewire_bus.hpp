#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "esp_rom_sys.h"

namespace algaguard::onewire {

// Hand-rolled bit-banged 1-Wire bus primitive (single drop, no ROM search --
// this device wires exactly one DS18B20). Matches this repo's convention of
// hand-written drivers against raw ESP-IDF rather than pulling in a managed
// component for a protocol this small. Every multi-bit transaction must be
// called from inside a critical section by the caller (the strict
// microsecond-level timing cannot tolerate FreeRTOS preemption mid-byte).
class Bus {
 public:
  explicit Bus(gpio_num_t pin) : pin_(pin) {}

  esp_err_t configure() {
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << pin_;
    config.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    // No internal pull-up: the documented external 4.7kOhm pull-up to 3.3V
    // is required for reliable 1-Wire timing and is assumed present.
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    release();
    return gpio_config(&config);
  }

  // Reset pulse + presence detect. Returns true if a device responded.
  bool reset() const {
    drive_low();
    esp_rom_delay_us(480);
    release();
    esp_rom_delay_us(70);
    const bool presence = gpio_get_level(pin_) == 0;
    esp_rom_delay_us(410);
    return presence;
  }

  void writeBit(bool bit) const {
    drive_low();
    esp_rom_delay_us(bit ? 6 : 60);
    release();
    esp_rom_delay_us(bit ? 64 : 10);
  }

  bool readBit() const {
    drive_low();
    esp_rom_delay_us(6);
    release();
    esp_rom_delay_us(9);
    const bool value = gpio_get_level(pin_) != 0;
    esp_rom_delay_us(55);
    return value;
  }

  void writeByte(std::uint8_t value) const {
    for (int bit = 0; bit < 8; ++bit) {
      writeBit((value & 0x01U) != 0);
      value >>= 1;
    }
  }

  std::uint8_t readByte() const {
    std::uint8_t value = 0;
    for (int bit = 0; bit < 8; ++bit) value |= (readBit() ? 1U : 0U) << bit;
    return value;
  }

  void writeBytes(const std::uint8_t* data, std::size_t length) const {
    for (std::size_t index = 0; index < length; ++index) writeByte(data[index]);
  }

  void readBytes(std::uint8_t* data, std::size_t length) const {
    for (std::size_t index = 0; index < length; ++index) data[index] = readByte();
  }

 private:
  void drive_low() const {
    gpio_set_direction(pin_, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(pin_, 0);
  }
  void release() const {
    gpio_set_level(pin_, 1);
    gpio_set_direction(pin_, GPIO_MODE_INPUT_OUTPUT_OD);
  }

  gpio_num_t pin_;
};

}  // namespace algaguard::onewire
