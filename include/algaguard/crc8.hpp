#pragma once

#include <cstddef>
#include <cstdint>

namespace algaguard {

// Dallas/Maxim 1-Wire CRC8 (polynomial 0x31, reflected 0x8C) -- used to
// validate DS18B20 ROM codes and scratchpad reads.
inline std::uint8_t crc8_dallas(const std::uint8_t* data, std::size_t length) {
  std::uint8_t crc = 0;
  for (std::size_t index = 0; index < length; ++index) {
    std::uint8_t inputByte = data[index];
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint8_t mix = (crc ^ inputByte) & 0x01U;
      crc >>= 1;
      if (mix) crc ^= 0x8CU;
      inputByte >>= 1;
    }
  }
  return crc;
}

}  // namespace algaguard
