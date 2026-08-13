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

// Real-sensor pins (DS18B20/TDS/pH/SD -- BH1750 and DS3231 share kSda/kScl
// above, no dedicated pins of their own).
constexpr int kDs18b20Data = 17;
constexpr int kTdsAdcPin = 1;   // ADC1_CH0
constexpr int kPhAdcPin = 2;    // ADC1_CH1
constexpr int kSdCs = 10;
constexpr int kSdMosi = 11;
constexpr int kSdSck = 12;
constexpr int kSdMiso = 13;

static_assert(kSda != 19 && kScl != 20, "Native USB pins are reserved");
static_assert(kLedRed != 38 && kLedGreen != 38 && kLedBlue != 38, "Onboard RGB pin is reserved");

// GPIO26-37 are claimed by the shared flash+octal-PSRAM SPI bus on this
// module (confirmed via sdkconfig CONFIG_SPIRAM_CLK_IO=30/CONFIG_SPIRAM_CS_IO=26,
// plus the octal data/DQS lines at 33-37) -- wider than the 35-37 this repo's
// docs previously called out. Every new pin must fall outside that range and
// outside the other reserved pins (USB 19/20, onboard RGB 38, UART0 43/44,
// strapping 0/3/45/46).
static_assert(kDs18b20Data < 26 || kDs18b20Data > 37, "GPIO reserved for flash/PSRAM");
static_assert(kTdsAdcPin < 26 || kTdsAdcPin > 37, "GPIO reserved for flash/PSRAM");
static_assert(kPhAdcPin < 26 || kPhAdcPin > 37, "GPIO reserved for flash/PSRAM");
static_assert(kSdCs < 26 || kSdCs > 37, "GPIO reserved for flash/PSRAM");
static_assert(kSdMosi < 26 || kSdMosi > 37, "GPIO reserved for flash/PSRAM");
static_assert(kSdSck < 26 || kSdSck > 37, "GPIO reserved for flash/PSRAM");
static_assert(kSdMiso < 26 || kSdMiso > 37, "GPIO reserved for flash/PSRAM");
static_assert(kDs18b20Data != 19 && kDs18b20Data != 20 && kDs18b20Data != 38 &&
                  kDs18b20Data != 43 && kDs18b20Data != 44,
              "GPIO reserved for USB/RGB/UART0");
}  // namespace algaguard::hardware
