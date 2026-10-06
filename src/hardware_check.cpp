// Bench hardware check for a freshly soldered board (env:
// esp32-s3-dev-hardware-check). Tests each peripheral on its own and prints
// PASS/FAIL with the pins to inspect, so a bad joint can be found before the
// full firmware is flashed. Never formats the SD card and never writes the
// RTC; the only SD write is one test file that is deleted again.
#if defined(ALGAGUARD_HARDWARE_CHECK)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "algaguard/bh1750.hpp"
#include "algaguard/display.hpp"
#include "algaguard/ds18b20.hpp"
#include "algaguard/ds3231.hpp"
#include "algaguard/hardware.hpp"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

namespace hw = algaguard::hardware;

namespace {

struct Result {
  std::string name;
  bool ok;
};
std::vector<Result> results;

void report(const char* name, bool ok, const std::string& detail,
            const char* hint = nullptr) {
  std::printf("[%s] %-10s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
  if (!ok && hint != nullptr) std::printf("              check: %s\n", hint);
  results.push_back({name, ok});
}

void info(const char* name, const std::string& detail) {
  std::printf("[INFO] %-10s %s\n", name, detail.c_str());
}

void heading(const char* text) { std::printf("\n=== %s ===\n", text); }

void wait_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

std::string format(const char* pattern, double value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), pattern, value);
  return buffer;
}

// ---------------------------------------------------------------- OLED --
i2c_master_bus_handle_t bus{};
i2c_master_dev_handle_t oled{};

bool oled_command(std::uint8_t command) {
  const std::array<std::uint8_t, 2> payload{0x00, command};
  return i2c_master_transmit(oled, payload.data(), payload.size(), 100) ==
         ESP_OK;
}

void oled_show(const std::array<std::string, 4>& lines) {
  if (oled == nullptr) return;
  const auto frame = algaguard::compose_screen({lines});
  for (std::uint8_t page = 0; page < 8; ++page) {
    oled_command(static_cast<std::uint8_t>(0xB0U | page));
    oled_command(0x00);
    oled_command(0x10);
    std::array<std::uint8_t, 129> payload{};
    payload[0] = 0x40;
    std::copy_n(frame.data() + page * 128, 128, payload.data() + 1);
    i2c_master_transmit(oled, payload.data(), payload.size(), 100);
  }
}

// ----------------------------------------------------------------- ADC --
adc_oneshot_unit_handle_t adc{};
struct AnalogInput {
  const char* name;
  int pin;
  adc_channel_t channel{};
  adc_cali_handle_t cali{};
};
std::array<AnalogInput, 2> analog{{{"TDS", hw::kTdsAdcPin}, {"pH", hw::kPhAdcPin}}};

// Average and spread of 32 samples, in millivolts. A large spread means the
// pin is floating (wire not connected to the sensor board's output).
std::optional<std::pair<double, double>> read_millivolts(const AnalogInput& input) {
  if (adc == nullptr || input.cali == nullptr) return std::nullopt;
  double sum = 0;
  double sum_squares = 0;
  constexpr int samples = 32;
  for (int index = 0; index < samples; ++index) {
    int raw = 0;
    int mv = 0;
    if (adc_oneshot_read(adc, input.channel, &raw) != ESP_OK ||
        adc_cali_raw_to_voltage(input.cali, raw, &mv) != ESP_OK)
      return std::nullopt;
    sum += mv;
    sum_squares += static_cast<double>(mv) * mv;
    vTaskDelay(1);
  }
  const double mean = sum / samples;
  return std::make_pair(mean, std::sqrt(std::max(0.0, sum_squares / samples - mean * mean)));
}

// ------------------------------------------------------------- sensors --
algaguard::Ds18b20Sensor ds18b20{static_cast<gpio_num_t>(hw::kDs18b20Data)};
algaguard::Bh1750Sensor bh1750;
bool bh1750_ready = false;
algaguard::Ds3231Rtc ds3231;
bool ds3231_ready = false;
bool ds18b20_present = false;

constexpr std::array<int, 4> kButtons{hw::kButtonUp, hw::kButtonDown,
                                      hw::kButtonSelect, hw::kButtonBack};
constexpr std::array<const char*, 4> kButtonNames{"UP", "DOWN", "SELECT", "BACK"};

// ------------------------------------------------------------------ steps --
void check_chip() {
  heading("1. ESP32-S3 module");
  esp_chip_info_t chip{};
  esp_chip_info(&chip);
  std::uint32_t flash = 0;
  esp_flash_get_size(nullptr, &flash);
  const bool psram = esp_psram_is_initialized();
  char detail[96];
  std::snprintf(detail, sizeof(detail), "rev %u, %u cores, flash %lu MB, PSRAM %u MB",
                chip.revision, chip.cores,
                static_cast<unsigned long>(flash / (1024 * 1024)),
                psram ? static_cast<unsigned>(esp_psram_get_size() / (1024 * 1024)) : 0U);
  report("CHIP", chip.model == CHIP_ESP32S3 && flash >= 16U * 1024 * 1024 && psram,
         detail, "expected an ESP32-S3 N16R8 (16 MB flash, 8 MB PSRAM)");
}

// Solder bridges between neighbouring header pins (on the DevKitC-1 header
// 4-5-6-7-15-16-17-18-8-3-46-9-10-11-12-13-14 run side by side). Each pin is
// driven LOW in turn while the others sit on pull-ups; a pin that follows it
// is joined to it. LED pins only drive (an LED to GND loads a pulled-up pin
// and would look shorted) and the analog sensor outputs are never driven.
// Voltage on an ADC1-capable pin (GPIO1-10). Returns nullopt for other pins.
std::optional<int> measure_millivolts(int gpio) {
  adc_unit_t unit_id{};
  adc_channel_t channel{};
  if (adc_oneshot_io_to_channel(gpio, &unit_id, &channel) != ESP_OK || unit_id != ADC_UNIT_1)
    return std::nullopt;
  adc_oneshot_unit_handle_t unit{};
  adc_oneshot_unit_init_cfg_t unit_config{};
  unit_config.unit_id = ADC_UNIT_1;
  if (adc_oneshot_new_unit(&unit_config, &unit) != ESP_OK) return std::nullopt;
  adc_oneshot_chan_cfg_t channel_config{};
  channel_config.atten = ADC_ATTEN_DB_12;
  channel_config.bitwidth = ADC_BITWIDTH_DEFAULT;
  adc_oneshot_config_channel(unit, channel, &channel_config);
  adc_cali_curve_fitting_config_t cali_config{};
  cali_config.unit_id = ADC_UNIT_1;
  cali_config.chan = channel;
  cali_config.atten = ADC_ATTEN_DB_12;
  cali_config.bitwidth = ADC_BITWIDTH_DEFAULT;
  adc_cali_handle_t cali{};
  std::optional<int> result;
  if (adc_cali_create_scheme_curve_fitting(&cali_config, &cali) == ESP_OK) {
    int raw = 0;
    int mv = 0;
    if (adc_oneshot_read(unit, channel, &raw) == ESP_OK &&
        adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK)
      result = mv;
    adc_cali_delete_scheme_curve_fitting(cali);
  }
  adc_oneshot_del_unit(unit);
  return result;
}

void check_solder_bridges() {
  heading("0. Solder bridges between pins");
  struct Pin {
    int gpio;
    const char* name;
    bool receiver;
  };
  const std::array<Pin, 17> pins{{{4, "UP", true},
                                  {5, "DOWN", true},
                                  {6, "SELECT", true},
                                  {7, "BACK", true},
                                  {8, "SDA", true},
                                  {9, "SCL", true},
                                  {10, "SD CS", true},
                                  {11, "SD MOSI", true},
                                  {12, "SD SCK", true},
                                  {13, "SD MISO", true},
                                  {14, "LED R", false},
                                  {15, "LED G", false},
                                  {16, "LED B", false},
                                  {17, "DS18B20", true},
                                  // unused pins sitting between the ones above
                                  // on the header, so a blob across them shows
                                  {18, "unused", true},
                                  {3, "unused", true},
                                  {46, "unused", true}}};
  const auto as_input = [](int gpio) {
    gpio_reset_pin(static_cast<gpio_num_t>(gpio));
    gpio_set_direction(static_cast<gpio_num_t>(gpio), GPIO_MODE_INPUT);
    gpio_set_pull_mode(static_cast<gpio_num_t>(gpio), GPIO_PULLUP_ONLY);
  };
  for (const auto& pin : pins) as_input(pin.gpio);
  wait_ms(5);
  bool clean = true;
  std::array<bool, pins.size()> low_at_rest{};
  for (std::size_t index = 0; index < pins.size(); ++index)
    low_at_rest[index] =
        pins[index].receiver && gpio_get_level(static_cast<gpio_num_t>(pins[index].gpio)) == 0;
  for (std::size_t index = 0; index < pins.size(); ++index)
    if (const auto& pin = pins[index]; low_at_rest[index]) {
      std::printf("              GPIO%d (%s) is LOW at rest: shorted to GND%s\n", pin.gpio,
                  pin.name, pin.gpio <= 7 ? ", or the button is being held" : "");
      clean = false;
    }
  for (const auto& driver : pins) {
    const auto gpio = static_cast<gpio_num_t>(driver.gpio);
    gpio_set_pull_mode(gpio, GPIO_FLOATING);
    gpio_set_direction(gpio, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(gpio, 0);
    esp_rom_delay_us(500);
    if (gpio_get_level(gpio) != 0) {
      std::printf("              GPIO%d (%s) cannot be pulled LOW: shorted to 3.3V/5V\n",
                  driver.gpio, driver.name);
      clean = false;
    }
    for (const auto& other : pins) {
      if (&other == &driver || !other.receiver ||
          low_at_rest[static_cast<std::size_t>(&other - pins.data())])
        continue;
      if (gpio_get_level(static_cast<gpio_num_t>(other.gpio)) == 0) {
        std::printf("              GPIO%d (%s) and GPIO%d (%s) are connected\n",
                    driver.gpio, driver.name, other.gpio, other.name);
        // With its pull-up off, the voltage tells a dead short (~0 V) from a
        // link through a resistor (the other side's pull-ups hold it up).
        gpio_set_pull_mode(static_cast<gpio_num_t>(other.gpio), GPIO_FLOATING);
        if (const auto mv = measure_millivolts(other.gpio)) {
          if (*mv < 250)
            std::printf("                GPIO%d sits at %d mV: a direct short (solder bridge or "
                        "two wires touching)\n",
                        other.gpio, *mv);
          else
            std::printf("                GPIO%d sits at %d mV: joined THROUGH A RESISTOR, e.g. a "
                        "pull-up resistor soldered to this line instead of 3.3V\n",
                        other.gpio, *mv);
        }
        as_input(other.gpio);
        clean = false;
      }
    }
    as_input(driver.gpio);
    esp_rom_delay_us(500);
  }
  for (const auto& pin : pins) gpio_reset_pin(static_cast<gpio_num_t>(pin.gpio));
  report("PINS", clean, clean ? "no shorts between the signal pins" : "short circuit found (see above)",
         "reflow the joints named above and remove any solder blob between the pads");
}

void check_i2c() {
  heading("2. I2C bus (SDA GPIO8, SCL GPIO9): OLED, BH1750, DS3231");
  // Both lines idle HIGH through pull-ups. A line stuck LOW is a short to
  // GND or a module wired backwards, and nothing on the bus will answer.
  gpio_config_t probe{};
  probe.pin_bit_mask = (1ULL << hw::kSda) | (1ULL << hw::kScl);
  probe.mode = GPIO_MODE_INPUT;
  probe.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&probe);
  wait_ms(5);
  const bool sda_high = gpio_get_level(static_cast<gpio_num_t>(hw::kSda)) == 1;
  const bool scl_high = gpio_get_level(static_cast<gpio_num_t>(hw::kScl)) == 1;
  report("I2C LINES", sda_high && scl_high,
         std::string("SDA ") + (sda_high ? "high" : "LOW") + ", SCL " +
             (scl_high ? "high" : "LOW"),
         "a line held LOW: look for a solder bridge to GND, or SDA/SCL swapped on a module");

  i2c_master_bus_config_t config{};
  config.i2c_port = I2C_NUM_0;
  config.sda_io_num = static_cast<gpio_num_t>(hw::kSda);
  config.scl_io_num = static_cast<gpio_num_t>(hw::kScl);
  config.clk_source = I2C_CLK_SRC_DEFAULT;
  config.glitch_ignore_cnt = 7;
  config.flags.enable_internal_pullup = true;
  if (i2c_new_master_bus(&config, &bus) != ESP_OK) {
    report("I2C BUS", false, "could not start the I2C driver");
    return;
  }
  esp_log_level_set("i2c.master", ESP_LOG_NONE);
  std::vector<std::uint8_t> found;
  for (std::uint8_t address = 0x08; address < 0x78; ++address)
    if (i2c_master_probe(bus, address, 50) == ESP_OK) found.push_back(address);
  std::string list;
  for (auto address : found) {
    char text[8];
    std::snprintf(text, sizeof(text), " 0x%02X", address);
    list += text;
  }
  info("I2C SCAN", found.empty() ? "no devices answered" : "answered:" + list);
  const auto has = [&](std::uint8_t address) {
    return std::find(found.begin(), found.end(), address) != found.end();
  };

  // OLED
  const std::uint8_t oled_address = has(0x3C) ? 0x3C : has(0x3D) ? 0x3D : 0;
  char oled_detail[32];
  std::snprintf(oled_detail, sizeof(oled_detail), "found at 0x%02X", oled_address);
  report("OLED", oled_address != 0,
         oled_address ? oled_detail : "not found at 0x3C/0x3D",
         "OLED VCC 3.3V, GND, SDA->GPIO8, SCL->GPIO9");
  if (oled_address != 0) {
    i2c_device_config_t device{};
    device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device.device_address = oled_address;
    device.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(bus, &device, &oled) == ESP_OK) {
      for (const std::uint8_t command :
           {0xAE, 0x20, 0x00, 0x40, 0xA1, 0xC8, 0x81, 0x7F, 0xA6, 0xA8, 0x3F,
            0xD3, 0x00, 0xD5, 0x80, 0xD9, 0xF1, 0xDA, 0x12, 0xDB, 0x40, 0x8D,
            0x14, 0xAF})
        oled_command(command);
      std::printf("              look: the whole OLED should light up white for 2 s\n");
      oled_command(0xA5);  // every pixel on, independent of display RAM
      wait_ms(2000);
      oled_command(0xA4);
      oled_show({"HW CHECK", "OLED OK", "RUNNING TESTS", "SEE SERIAL LOG"});
    }
  }

  // BH1750 light sensor (ADDR pin low -> 0x23, high -> 0x5C)
  const std::uint8_t light_address = has(0x23) ? 0x23 : has(0x5C) ? 0x5C : 0;
  if (light_address != 0 && bh1750.configure(bus, light_address) == ESP_OK) {
    const auto lux = bh1750.readLux();
    bh1750_ready = lux.has_value();
    report("BH1750", bh1750_ready,
           lux ? format("%.1f lux", *lux) : "found but reading failed",
           "BH1750 VCC 3.3V and GND");
  } else {
    report("BH1750", false, "not found at 0x23/0x5C",
           "BH1750 VCC 3.3V, GND, SDA->GPIO8, SCL->GPIO9, ADDR to GND");
  }

  // DS3231 real-time clock
  if (has(0x68) && ds3231.configure(bus) == ESP_OK) {
    const auto first = ds3231.readUtc();
    wait_ms(1500);
    const auto second = ds3231.readUtc();
    ds3231_ready = first.has_value() && second.has_value();
    const bool ticking = ds3231_ready && *second > *first;
    report("DS3231", ticking,
           ticking ? "found, clock is ticking" : "found but the clock is not advancing",
           "DS3231 VCC 3.3V and GND; the module's crystal side");
    if (ds3231.oscillatorStopped())
      info("DS3231", "time not set yet or the coin cell is missing/flat (normal on a new "
                     "module; the firmware sets it from NTP)");
  } else {
    report("DS3231", false, "not found at 0x68",
           "DS3231 VCC 3.3V, GND, SDA->GPIO8, SCL->GPIO9");
  }
}

void check_ds18b20() {
  heading("3. DS18B20 water temperature (DATA GPIO17)");
  ds18b20_present = ds18b20.configure() == ESP_OK;
  if (!ds18b20_present) {
    report("DS18B20", false, "no presence pulse",
           "DATA->GPIO17, 4.7k resistor from DATA to 3.3V, VDD 3.3V, GND");
    return;
  }
  ds18b20.startConversion();
  wait_ms(800);
  const auto celsius = ds18b20.readCelsius();
  if (!celsius) {
    report("DS18B20", false, "answers but data is corrupt (CRC)",
           "a weak joint or missing 4.7k pull-up on DATA (GPIO17)");
  } else if (*celsius == 85.0) {
    report("DS18B20", false, "reads 85.0 C (power-on value: conversion did not run)",
           "VDD (red) wire to 3.3V");
  } else {
    report("DS18B20", *celsius > -20 && *celsius < 80, format("%.2f C", *celsius),
           "value out of range: check the probe wiring");
  }
}

void check_analog() {
  heading("4. Analog sensors: TDS (GPIO1), pH (GPIO2)");
  adc_oneshot_unit_init_cfg_t unit{};
  unit.unit_id = ADC_UNIT_1;
  if (adc_oneshot_new_unit(&unit, &adc) != ESP_OK) {
    report("ADC", false, "could not start ADC1");
    return;
  }
  for (auto& input : analog) {
    adc_unit_t unit_id{};
    adc_oneshot_io_to_channel(input.pin, &unit_id, &input.channel);
    adc_oneshot_chan_cfg_t channel{};
    channel.atten = ADC_ATTEN_DB_12;
    channel.bitwidth = ADC_BITWIDTH_DEFAULT;
    adc_oneshot_config_channel(adc, input.channel, &channel);
    adc_cali_curve_fitting_config_t cali{};
    cali.unit_id = ADC_UNIT_1;
    cali.chan = input.channel;
    cali.atten = ADC_ATTEN_DB_12;
    cali.bitwidth = ADC_BITWIDTH_DEFAULT;
    adc_cali_create_scheme_curve_fitting(&cali, &input.cali);
    const auto reading = read_millivolts(input);
    if (!reading) {
      report(input.name, false, "ADC read failed");
      continue;
    }
    const auto [mean, spread] = *reading;
    char detail[96];
    std::snprintf(detail, sizeof(detail), "%.0f mV (noise %.0f mV)", mean, spread);
    if (spread > 60) {
      report(input.name, false, std::string(detail) + " - pin is floating",
             input.pin == hw::kTdsAdcPin
                 ? "TDS board signal (A) -> GPIO1, and the board's VCC/GND"
                 : "pH board signal (Po) -> GPIO2, and the board's VCC/GND");
    } else if (mean > 3200) {
      report(input.name, false, std::string(detail) + " - stuck at 3.3V",
             "signal wire touching 3.3V, or the board powered from 5V");
    } else {
      report(input.name, true, detail);
    }
  }
  info("ANALOG", "TDS reads ~0 mV in air and rises in water; a pH board in pH-7 water "
                 "is usually around 1500-2500 mV");
}

void check_sd() {
  heading("5. SD card (CS 10, MOSI 11, SCK 12, MISO 13)");
  spi_bus_config_t spi{};
  spi.mosi_io_num = hw::kSdMosi;
  spi.miso_io_num = hw::kSdMiso;
  spi.sclk_io_num = hw::kSdSck;
  spi.quadwp_io_num = -1;
  spi.quadhd_io_num = -1;
  if (spi_bus_initialize(SPI2_HOST, &spi, SPI_DMA_CH_AUTO) != ESP_OK) {
    report("SD CARD", false, "could not start the SPI bus");
    return;
  }
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SPI2_HOST;
  sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot.gpio_cs = static_cast<gpio_num_t>(hw::kSdCs);
  slot.host_id = SPI2_HOST;
  esp_vfs_fat_sdmmc_mount_config_t mount{};
  mount.format_if_mount_failed = false;  // never erase the user's card
  mount.max_files = 2;
  sdmmc_card_t* card = nullptr;
  const esp_err_t result =
      esp_vfs_fat_sdspi_mount("/sdcheck", &host, &slot, &mount, &card);
  if (result != ESP_OK) {
    report("SD CARD", false, std::string("mount failed: ") + esp_err_to_name(result),
           result == ESP_FAIL
               ? "card answered but is not FAT32-formatted (format it on the PC)"
               : "card inserted? CS->GPIO10, MOSI->11, SCK->12, MISO->13, VCC 3.3V, GND");
    spi_bus_free(SPI2_HOST);
    return;
  }
  const double megabytes = static_cast<double>(card->csd.capacity) *
                           card->csd.sector_size / (1024.0 * 1024.0);
  const char* path = "/sdcheck/HWCHECK.TXT";
  bool ok = false;
  if (FILE* file = std::fopen(path, "w")) {
    const char* text = "AlgaGuard hardware check";
    std::fputs(text, file);
    std::fclose(file);
    if (FILE* back = std::fopen(path, "r")) {
      char buffer[64]{};
      std::fgets(buffer, sizeof(buffer), back);
      std::fclose(back);
      ok = std::strcmp(buffer, text) == 0;
    }
    std::remove(path);
  }
  report("SD CARD", ok,
         format("%.0f MB card, ", megabytes) + (ok ? "write/read OK" : "write/read FAILED"),
         "MISO (GPIO13) joint, or a write-protected/faulty card");
  esp_vfs_fat_sdcard_unmount("/sdcheck", card);
  spi_bus_free(SPI2_HOST);
}

void check_leds() {
  heading("6. RGB LED (R GPIO14, G GPIO15, B GPIO16)");
  gpio_config_t outputs{};
  outputs.pin_bit_mask =
      (1ULL << hw::kLedRed) | (1ULL << hw::kLedGreen) | (1ULL << hw::kLedBlue);
  outputs.mode = GPIO_MODE_OUTPUT;
  gpio_config(&outputs);
  const std::array<std::pair<int, const char*>, 3> leds{
      {{hw::kLedRed, "RED"}, {hw::kLedGreen, "GREEN"}, {hw::kLedBlue, "BLUE"}}};
  for (int round = 0; round < 2; ++round)
    for (const auto& [pin, name] : leds) {
      if (round == 0) std::printf("              look: LED should be %s now\n", name);
      oled_show({"LED TEST", std::string("LED ") + name + " ON", "", ""});
      gpio_set_level(static_cast<gpio_num_t>(pin), 1);
      wait_ms(1200);
      gpio_set_level(static_cast<gpio_num_t>(pin), 0);
    }
  info("LED", "you saw RED, GREEN, BLUE twice in that order? If a colour was wrong or "
              "missing, check that leg's resistor and joint (R14 G15 B16). If the LED "
              "was lit when it should be off, it is common-anode and wired to 3.3V");
}

void check_buttons() {
  heading("7. Buttons (UP 4, DOWN 5, SELECT 6, BACK 7)");
  gpio_config_t inputs{};
  for (int pin : kButtons) inputs.pin_bit_mask |= 1ULL << pin;
  inputs.mode = GPIO_MODE_INPUT;
  inputs.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&inputs);
  wait_ms(10);
  for (std::size_t index = 0; index < kButtons.size(); ++index)
    if (gpio_get_level(static_cast<gpio_num_t>(kButtons[index])) == 0)
      std::printf("              warning: %s reads PRESSED without being touched -> "
                  "shorted to GND or wired across the wrong legs\n",
                  kButtonNames[index]);
  std::printf("              press each button once now (UP, DOWN, SELECT, BACK) - 30 s\n");
  std::array<bool, 4> seen{};
  std::array<int, 4> last{1, 1, 1, 1};
  const TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(30000);
  while (xTaskGetTickCount() < end &&
         !(seen[0] && seen[1] && seen[2] && seen[3])) {
    for (std::size_t index = 0; index < kButtons.size(); ++index) {
      const int level = gpio_get_level(static_cast<gpio_num_t>(kButtons[index]));
      if (level == 0 && last[index] == 1) {
        seen[index] = true;
        std::printf("              %s pressed\n", kButtonNames[index]);
      }
      last[index] = level;
    }
    std::string pending;
    for (std::size_t index = 0; index < kButtons.size(); ++index)
      if (!seen[index]) pending += std::string(kButtonNames[index]) + " ";
    oled_show({"BUTTONS", "PRESS EACH ONCE", "LEFT: " + pending, ""});
    wait_ms(20);
  }
  for (std::size_t index = 0; index < kButtons.size(); ++index)
    report(kButtonNames[index], seen[index], seen[index] ? "press detected" : "no press detected",
           "one leg to the GPIO, the other to GND (use diagonal legs on a 4-leg switch)");
}

void summary() {
  heading("SUMMARY");
  int failed = 0;
  std::string failures;
  for (const auto& result : results) {
    std::printf("  %-10s %s\n", result.name.c_str(), result.ok ? "PASS" : "FAIL");
    if (!result.ok) {
      ++failed;
      if (failures.size() < 26) failures += result.name + " ";
    }
  }
  std::printf("\n%s\n", failed == 0 ? "ALL CHECKS PASSED - the board is ready for the real firmware"
                                    : "SOME CHECKS FAILED - fix the joints listed above and press "
                                      "RESET to run again");
  oled_show({failed == 0 ? "ALL PASS" : "FAILED",
             failed == 0 ? "READY FOR FIRMWARE" : failures,
             format("%.0f CHECKS", static_cast<double>(results.size())),
             "LIVE VALUES ON SERIAL"});
}

void live_values() {
  heading("LIVE VALUES (every 2 s, press RESET to rerun the checks)");
  std::array<int, 4> last{1, 1, 1, 1};
  TickType_t next = 0;
  while (true) {
    for (std::size_t index = 0; index < kButtons.size(); ++index) {
      const int level = gpio_get_level(static_cast<gpio_num_t>(kButtons[index]));
      if (level == 0 && last[index] == 1) std::printf("  button %s\n", kButtonNames[index]);
      last[index] = level;
    }
    if (xTaskGetTickCount() >= next) {
      next = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
      std::string line = "  ";
      if (ds18b20_present) {
        const auto celsius = ds18b20.readCelsius();
        ds18b20.startConversion();
        line += celsius ? format("water %.2f C  ", *celsius) : "water ERR  ";
      }
      if (bh1750_ready) {
        const auto lux = bh1750.readLux();
        line += lux ? format("light %.0f lux  ", *lux) : "light ERR  ";
      }
      for (const auto& input : analog)
        if (const auto reading = read_millivolts(input))
          line += std::string(input.name) + format(" %.0f mV  ", reading->first);
      std::printf("%s\n", line.c_str());
    }
    wait_ms(20);
  }
}

}  // namespace

void algaguard_run_hardware_check() {
  wait_ms(1500);  // let the serial monitor attach after reset
  std::printf("\n\nAlgaGuard hardware check - one part at a time\n");
  check_chip();
  check_solder_bridges();
  check_i2c();
  check_ds18b20();
  check_analog();
  check_sd();
  check_leds();
  check_buttons();
  summary();
  live_values();
}

#endif  // ALGAGUARD_HARDWARE_CHECK
