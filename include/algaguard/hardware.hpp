#pragma once
#include <cstdint>

namespace algaguard::hardware {
// Current shared ESP32-S3 physical OLED module address. The physical-test
// profile probes this exact seven-bit address; it never scans the whole bus.
constexpr std::uint8_t kOledAddress = 0x3C;
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
