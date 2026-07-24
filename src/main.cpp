#include "algaguard/hardware.hpp"
#include "algaguard/credentials.hpp"
#include "algaguard/modules.hpp"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <array>

namespace {
constexpr char kTag[] = "algaguard";
algaguard::DeterministicSimulator simulator{0xA16A6A4DU};
algaguard::LocalQueue<algaguard::SimulatedSample> queue{120};
algaguard::TelemetryBatcher batcher{10};
algaguard::MenuController menu;
algaguard::DebouncedButton up_button;
algaguard::DebouncedButton down_button;
algaguard::DebouncedButton select_button;
algaguard::DebouncedButton back_button;
constexpr algaguard::CredentialLimits credential_limits{};
static_assert(ALGAGUARD_MQTT_MAX_BATCH_SAMPLES <= 120, "MQTT batch exceeds released contract");

esp_err_t oled_command(std::uint8_t command) {
  i2c_cmd_handle_t transaction = i2c_cmd_link_create();
  i2c_master_start(transaction);
  i2c_master_write_byte(transaction, (algaguard::hardware::kOledAddress << 1U) | I2C_MASTER_WRITE, true);
  i2c_master_write_byte(transaction, 0x00, true);
  i2c_master_write_byte(transaction, command, true);
  i2c_master_stop(transaction);
  const esp_err_t result = i2c_master_cmd_begin(I2C_NUM_0, transaction, pdMS_TO_TICKS(100));
  i2c_cmd_link_delete(transaction);
  return result;
}

esp_err_t clear_oled() {
  for (std::uint8_t page = 0; page < 8; ++page) {
    esp_err_t result = oled_command(0xB0U | page);
    if (result != ESP_OK) return result;
    result = oled_command(0x00);
    if (result != ESP_OK) return result;
    result = oled_command(0x10);
    if (result != ESP_OK) return result;
    i2c_cmd_handle_t transaction = i2c_cmd_link_create();
    i2c_master_start(transaction);
    i2c_master_write_byte(transaction, (algaguard::hardware::kOledAddress << 1U) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(transaction, 0x40, true);
    const std::array<std::uint8_t, 128> blank{};
    i2c_master_write(transaction, blank.data(), blank.size(), true);
    i2c_master_stop(transaction);
    result = i2c_master_cmd_begin(I2C_NUM_0, transaction, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(transaction);
    if (result != ESP_OK) return result;
  }
  return ESP_OK;
}

void configure_gpio() {
  const std::uint64_t output_mask = (1ULL << algaguard::hardware::kLedRed) | (1ULL << algaguard::hardware::kLedGreen) |
                                    (1ULL << algaguard::hardware::kLedBlue);
  gpio_config_t outputs{.pin_bit_mask = output_mask, .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_DISABLE,
                        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE};
  ESP_ERROR_CHECK(gpio_config(&outputs));
  const std::uint64_t input_mask = (1ULL << algaguard::hardware::kButtonUp) | (1ULL << algaguard::hardware::kButtonDown) |
                                   (1ULL << algaguard::hardware::kButtonSelect) | (1ULL << algaguard::hardware::kButtonBack);
  gpio_config_t inputs{.pin_bit_mask = input_mask, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
                       .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE};
  ESP_ERROR_CHECK(gpio_config(&inputs));
}

void configure_oled_i2c() {
  i2c_config_t config{.mode = I2C_MODE_MASTER,
                      .sda_io_num = static_cast<gpio_num_t>(algaguard::hardware::kSda),
                      .scl_io_num = static_cast<gpio_num_t>(algaguard::hardware::kScl),
                      .sda_pullup_en = GPIO_PULLUP_ENABLE,
                      .scl_pullup_en = GPIO_PULLUP_ENABLE,
                      .master = {.clk_speed = 400000},
                      .clk_flags = 0};
  ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &config));
  ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0, config.mode, 0, 0, 0));
  for (const std::uint8_t command : {0xAE, 0x20, 0x00, 0x40, 0xA1, 0xC8, 0x81, 0x7F, 0xA6, 0xA8, 0x3F,
                                      0xD3, 0x00, 0xD5, 0x80, 0xD9, 0xF1, 0xDA, 0x12, 0xDB, 0x40, 0x8D,
                                      0x14, 0xAF}) {
    ESP_ERROR_CHECK(oled_command(command));
  }
  ESP_ERROR_CHECK(clear_oled());
}

void apply_leds(algaguard::LedPriority priority, bool remote_indicator = false) {
  const auto state = algaguard::led_state(priority, remote_indicator);
  gpio_set_level(static_cast<gpio_num_t>(algaguard::hardware::kLedRed), state.red);
  gpio_set_level(static_cast<gpio_num_t>(algaguard::hardware::kLedGreen), state.green);
  gpio_set_level(static_cast<gpio_num_t>(algaguard::hardware::kLedBlue), state.blue);
}

void render_oled_state() {
  // The SSD1306 remains on the approved 0x3C I2C bus. Rendering is deliberately
  // non-secret: setup shows device ID/QR/fallback code; cloud/OTA screens show status only.
  static int last_page = -1;
  if (last_page == static_cast<int>(menu.page())) return;
  last_page = static_cast<int>(menu.page());
  ESP_LOGI(kTag, "oled page=%d device=AG-000001 simulated=true", static_cast<int>(menu.page()));
}

void sampling_task(void*) {
  std::uint64_t sequence = 1;
  while (true) {
    const auto sample = simulator.next(sequence++, static_cast<std::uint64_t>(xTaskGetTickCount()) * portTICK_PERIOD_MS);
    queue.push(sample);
    batcher.add(sample);
    if (batcher.ready()) {
      const auto qos1_batch = batcher.take_for_qos1();
      ESP_LOGI(kTag, "telemetry qos=1 samples=%u quality=SIMULATED ack=required",
               static_cast<unsigned>(qos1_batch.size()));
    }
    ESP_LOGI(kTag, "simulated_sample queued=%u quality=SIMULATED", static_cast<unsigned>(queue.size()));
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void input_task(void*) {
  while (true) {
    const auto now = static_cast<std::uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (up_button.update(gpio_get_level(static_cast<gpio_num_t>(algaguard::hardware::kButtonUp)) == 0, now) ==
        algaguard::ButtonEvent::kShortPress) menu.up();
    if (down_button.update(gpio_get_level(static_cast<gpio_num_t>(algaguard::hardware::kButtonDown)) == 0, now) ==
        algaguard::ButtonEvent::kShortPress) menu.down();
    if (select_button.update(gpio_get_level(static_cast<gpio_num_t>(algaguard::hardware::kButtonSelect)) == 0, now) ==
        algaguard::ButtonEvent::kShortPress) menu.select();
    if (back_button.update(gpio_get_level(static_cast<gpio_num_t>(algaguard::hardware::kButtonBack)) == 0, now) ==
        algaguard::ButtonEvent::kShortPress) menu.back();
    render_oled_state();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
}  // namespace

extern "C" void app_main() {
  ESP_ERROR_CHECK(nvs_flash_init());
  configure_gpio();
  configure_oled_i2c();
  esp_chip_info_t chip{};
  esp_chip_info(&chip);
  std::uint32_t flash_bytes = 0;
  ESP_ERROR_CHECK(esp_flash_get_size(nullptr, &flash_bytes));
  const std::uint32_t psram_bytes = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  const bool expected_n16r8 = flash_bytes >= 16U * 1024U * 1024U && psram_bytes >= 8U * 1024U * 1024U;
  ESP_LOGI(kTag, "AlgaGuard USB-only boot cores=%d flash=%u psram=%u expected_n16r8=%s oled=0x%02X", chip.cores,
           flash_bytes, psram_bytes, expected_n16r8 ? "true" : "false", algaguard::hardware::kOledAddress);
  if (!credential_limits.bounded()) {
    ESP_LOGE(kTag, "credential_transport_limits_invalid");
    return;
  }
  apply_leds(algaguard::LedPriority::kSetupOrOta);
  xTaskCreate(sampling_task, "simulated_sampling", 4096, nullptr, 5, nullptr);
  xTaskCreate(input_task, "buttons_oled", 4096, nullptr, 5, nullptr);
}
