#include "algaguard/hardware.hpp"
#include "algaguard/credentials.hpp"
#include "algaguard/modules.hpp"

#include "driver/gpio.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

namespace {
constexpr char kTag[] = "algaguard";
algaguard::DeterministicSimulator simulator{0xA16A6A4DU};
algaguard::LocalQueue<algaguard::SimulatedSample> queue{120};
constexpr algaguard::CredentialLimits credential_limits{};
static_assert(ALGAGUARD_MQTT_MAX_BATCH_SAMPLES <= 120, "MQTT batch exceeds released contract");

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

void sampling_task(void*) {
  std::uint64_t sequence = 1;
  while (true) {
    queue.push(simulator.next(sequence++, static_cast<std::uint64_t>(xTaskGetTickCount()) * portTICK_PERIOD_MS));
    ESP_LOGI(kTag, "simulated_sample queued=%u quality=SIMULATED", static_cast<unsigned>(queue.size()));
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
}  // namespace

extern "C" void app_main() {
  ESP_ERROR_CHECK(nvs_flash_init());
  configure_gpio();
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
  gpio_set_level(static_cast<gpio_num_t>(algaguard::hardware::kLedBlue), 1);
  xTaskCreate(sampling_task, "simulated_sampling", 4096, nullptr, 5, nullptr);
}
