#pragma once
#include <cstdint>

namespace algaguard::hardware {
// The replacement OLED is labelled with its 8-bit I2C write address (0x78).
// ESP-IDF code uses the corresponding 7-bit address and adds the R/W bit.
constexpr std::uint8_t kOledWriteAddress = 0x78;
constexpr std::uint8_t kOledAddress = kOledWriteAddress >> 1U;
static_assert((kOledAddress << 1U) == kOledWriteAddress, "OLED address conversion must preserve the write address");
constexpr int kSda = 8;
constexpr int kScl = 9;
constexpr int kButtonUp = 4;
constexpr int kButtonDown = 5;
constexpr int kButtonSelect = 6;
constexpr int kButtonBack = 7;
constexpr int kLedRed = 14;
constexpr int kLedGreen = 15;
constexpr int kLedBlue = 16;
constexpr bool kUsbPowerOnly = true;
static_assert(kSda != 19 && kScl != 20, "Native USB pins are reserved");
static_assert(kLedRed != 38 && kLedGreen != 38 && kLedBlue != 38, "Onboard RGB pin is reserved");
}  // namespace algaguard::hardware
