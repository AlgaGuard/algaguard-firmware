#include "algaguard/config.hpp"
#include "algaguard/credentials.hpp"
#include "algaguard/display.hpp"
#include "algaguard/hardware.hpp"
#include "algaguard/startup.hpp"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace {
constexpr char kTag[] = "algaguard";
constexpr std::uint32_t kExpectedFlashBytes = 16U * 1024U * 1024U;
constexpr std::uint32_t kExpectedPsramBytes = 8U * 1024U * 1024U;
constexpr algaguard::CredentialLimits credential_limits{};

algaguard::DeterministicSimulator simulator{0xA16A6A4DU};
algaguard::LocalQueue<algaguard::SimulatedSample> queue{120};
algaguard::TelemetryBatcher batcher{10};
algaguard::MenuController menu;
algaguard::DebouncedButton up_button;
algaguard::DebouncedButton down_button;
algaguard::DebouncedButton select_button;
algaguard::DebouncedButton back_button;

static_assert(ALGAGUARD_MQTT_MAX_BATCH_SAMPLES <= 120,
              "MQTT batch exceeds released contract");
static_assert(algaguard::hardware::kSda == 8 && algaguard::hardware::kScl == 9,
              "OLED pins changed without hardware approval");

struct RuntimeBoardProfile {
  std::uint32_t flash_bytes{};
  std::uint32_t psram_bytes{};
  std::uint16_t chip_revision{};
  std::uint8_t chip_cores{};
  const char* running_partition{"unknown"};
  const char* firmware_version{"unknown"};
  bool psram_available{};
  bool expected_n16r8{};
};

i2c_master_bus_handle_t oled_bus{};
i2c_master_dev_handle_t oled_device{};

esp_err_t oled_command(std::uint8_t command) {
  const std::array<std::uint8_t, 2> payload{0x00, command};
  return i2c_master_transmit(oled_device, payload.data(), payload.size(), 100);
}

esp_err_t oled_framebuffer(const std::array<std::uint8_t, 1024>& framebuffer) {
  for (std::uint8_t page = 0; page < 8; ++page) {
    esp_err_t result = oled_command(static_cast<std::uint8_t>(0xB0U | page));
    if (result != ESP_OK) return result;
    if ((result = oled_command(0x00)) != ESP_OK) return result;
    if ((result = oled_command(0x10)) != ESP_OK) return result;
    std::array<std::uint8_t, 129> payload{};
    payload[0] = 0x40;
    std::copy_n(framebuffer.data() + page * 128, 128, payload.data() + 1);
    result =
        i2c_master_transmit(oled_device, payload.data(), payload.size(), 100);
    if (result != ESP_OK) return result;
  }
  return ESP_OK;
}

std::array<std::uint8_t, 5> glyph(char raw) {
  const char value =
      static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
  if (value >= '0' && value <= '9') {
    constexpr std::array<std::array<std::uint8_t, 5>, 10> digits{{
        {{7, 5, 5, 5, 7}}, {{2, 6, 2, 2, 7}}, {{7, 1, 7, 4, 7}},
        {{7, 1, 7, 1, 7}}, {{5, 5, 7, 1, 1}}, {{7, 4, 7, 1, 7}},
        {{7, 4, 7, 5, 7}}, {{7, 1, 1, 2, 2}}, {{7, 5, 7, 5, 7}},
        {{7, 5, 7, 1, 7}},
    }};
    return digits[static_cast<std::size_t>(value - '0')];
  }
  if (value >= 'A' && value <= 'Z') {
    constexpr std::array<std::array<std::uint8_t, 5>, 26> letters{{
        {{2, 5, 7, 5, 5}}, {{6, 5, 6, 5, 6}}, {{3, 4, 4, 4, 3}},
        {{6, 5, 5, 5, 6}}, {{7, 4, 6, 4, 7}}, {{7, 4, 6, 4, 4}},
        {{3, 4, 5, 5, 3}}, {{5, 5, 7, 5, 5}}, {{7, 2, 2, 2, 7}},
        {{1, 1, 1, 5, 2}}, {{5, 5, 6, 5, 5}}, {{4, 4, 4, 4, 7}},
        {{5, 7, 7, 5, 5}}, {{5, 7, 7, 7, 5}}, {{2, 5, 5, 5, 2}},
        {{6, 5, 6, 4, 4}}, {{2, 5, 5, 7, 3}}, {{6, 5, 6, 5, 5}},
        {{3, 4, 2, 1, 6}}, {{7, 2, 2, 2, 2}}, {{5, 5, 5, 5, 7}},
        {{5, 5, 5, 5, 2}}, {{5, 5, 7, 7, 5}}, {{5, 5, 2, 5, 5}},
        {{5, 5, 2, 2, 2}}, {{7, 1, 2, 4, 7}},
    }};
    return letters[static_cast<std::size_t>(value - 'A')];
  }
  if (value == '-') return {{0, 0, 7, 0, 0}};
  if (value == '_') return {{0, 0, 0, 0, 7}};
  if (value == ':') return {{0, 2, 0, 2, 0}};
  if (value == '.') return {{0, 0, 0, 0, 2}};
  return {{0, 0, 0, 0, 0}};
}

void draw_text(std::array<std::uint8_t, 1024>& framebuffer,
               std::string_view text, std::uint8_t origin_x,
               std::uint8_t origin_y) {
  std::uint8_t x = origin_x;
  for (const char character : text) {
    if (x > 123) break;
    const auto rows = glyph(character);
    for (std::uint8_t row = 0; row < rows.size(); ++row) {
      for (std::uint8_t column = 0; column < 3; ++column) {
        if ((rows[row] & (1U << (2U - column))) == 0) continue;
        const auto pixel_x = static_cast<std::uint8_t>(x + column);
        const auto pixel_y = static_cast<std::uint8_t>(origin_y + row);
        framebuffer[(pixel_y / 8U) * 128U + pixel_x] |=
            static_cast<std::uint8_t>(1U << (pixel_y % 8U));
      }
    }
    x = static_cast<std::uint8_t>(x + 4U);
  }
}

esp_err_t render_screen(const algaguard::DiagnosticScreen& screen) {
  std::array<std::uint8_t, 1024> framebuffer{};
  for (std::uint8_t index = 0; index < screen.lines.size(); ++index)
    draw_text(framebuffer, screen.lines[index], 0,
              static_cast<std::uint8_t>(index * 8U));
  return oled_framebuffer(framebuffer);
}

void configure_gpio() {
  const std::uint64_t output_mask =
      (1ULL << algaguard::hardware::kLedRed) |
      (1ULL << algaguard::hardware::kLedGreen) |
      (1ULL << algaguard::hardware::kLedBlue);
  gpio_config_t outputs{};
  outputs.pin_bit_mask = output_mask;
  outputs.mode = GPIO_MODE_OUTPUT;
  outputs.pull_up_en = GPIO_PULLUP_DISABLE;
  outputs.pull_down_en = GPIO_PULLDOWN_DISABLE;
  outputs.intr_type = GPIO_INTR_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&outputs));

  const std::uint64_t input_mask =
      (1ULL << algaguard::hardware::kButtonUp) |
      (1ULL << algaguard::hardware::kButtonDown) |
      (1ULL << algaguard::hardware::kButtonSelect) |
      (1ULL << algaguard::hardware::kButtonBack);
  gpio_config_t inputs{};
  inputs.pin_bit_mask = input_mask;
  inputs.mode = GPIO_MODE_INPUT;
  inputs.pull_up_en = GPIO_PULLUP_ENABLE;
  inputs.pull_down_en = GPIO_PULLDOWN_DISABLE;
  inputs.intr_type = GPIO_INTR_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&inputs));
}

esp_err_t configure_oled_i2c() {
  if (oled_device != nullptr) return ESP_OK;
  i2c_master_bus_config_t bus_config{};
  bus_config.i2c_port = I2C_NUM_0;
  bus_config.sda_io_num =
      static_cast<gpio_num_t>(algaguard::hardware::kSda);
  bus_config.scl_io_num =
      static_cast<gpio_num_t>(algaguard::hardware::kScl);
  bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_config.glitch_ignore_cnt = 7;
  bus_config.flags.enable_internal_pullup = true;
  esp_err_t result = i2c_new_master_bus(&bus_config, &oled_bus);
  if (result != ESP_OK) return result;

  i2c_device_config_t device_config{};
  device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  device_config.device_address = algaguard::hardware::kOledAddress;
  device_config.scl_speed_hz = 400000;
  result =
      i2c_master_bus_add_device(oled_bus, &device_config, &oled_device);
  if (result != ESP_OK) {
    i2c_del_master_bus(oled_bus);
    oled_bus = nullptr;
    return result;
  }
  for (const std::uint8_t command :
       {0xAE, 0x20, 0x00, 0x40, 0xA1, 0xC8, 0x81, 0x7F, 0xA6, 0xA8,
        0x3F, 0xD3, 0x00, 0xD5, 0x80, 0xD9, 0xF1, 0xDA, 0x12, 0xDB,
        0x40, 0x8D, 0x14, 0xAF}) {
    result = oled_command(command);
    if (result != ESP_OK) return result;
  }
  return ESP_OK;
}

void apply_leds(algaguard::StartupState state,
                bool remote_indicator = false) {
  const auto leds = algaguard::startup_led_state(state, remote_indicator);
  gpio_set_level(static_cast<gpio_num_t>(algaguard::hardware::kLedRed),
                 leds.red);
  gpio_set_level(static_cast<gpio_num_t>(algaguard::hardware::kLedGreen),
                 leds.green);
  gpio_set_level(static_cast<gpio_num_t>(algaguard::hardware::kLedBlue),
                 leds.blue);
}

class EspFoundationServices final : public algaguard::StartupServices {
 public:
  explicit EspFoundationServices(RuntimeBoardProfile& board) : board_(board) {}

  algaguard::StartupResult platform_init() override {
    const auto config_result =
        algaguard::validate_firmware_config(algaguard::active_firmware_config());
    if (config_result != algaguard::ConfigError::kNone)
      return {algaguard::OperationStatus::kFatalFailure,
              algaguard::StartupReason::kFatalModuleFailure};
    if (!credential_limits.bounded())
      return {algaguard::OperationStatus::kFatalFailure,
              algaguard::StartupReason::kFatalModuleFailure};
    profile_mismatch_ = !board_.expected_n16r8;
    return {algaguard::OperationStatus::kSuccess};
  }

  algaguard::StartupResult storage_init() override {
    const esp_err_t result = nvs_flash_init();
    if (result != ESP_OK)
      return {algaguard::OperationStatus::kRecoverableFailure,
              algaguard::StartupReason::kStorageUnavailable};
    return {algaguard::OperationStatus::kSuccess};
  }

  algaguard::StartupResult display_init() override {
    if (configure_oled_i2c() != ESP_OK)
      return {algaguard::OperationStatus::kRecoverableFailure,
              algaguard::StartupReason::kModuleUnavailable};
    const auto boot =
        algaguard::boot_screen(algaguard::active_firmware_config());
    if (render_screen(boot) != ESP_OK)
      return {algaguard::OperationStatus::kRecoverableFailure,
              algaguard::StartupReason::kModuleUnavailable};
    return {algaguard::OperationStatus::kSuccess};
  }

  algaguard::StartupResult input_init() override {
    configure_gpio();
    return {algaguard::OperationStatus::kSuccess};
  }

  algaguard::StartupResult load_provisioning_state() override {
    if (profile_mismatch_)
      return {algaguard::OperationStatus::kRecoverableFailure,
              algaguard::StartupReason::kBoardProfileMismatch};
    return {algaguard::OperationStatus::kSuccess,
            algaguard::StartupReason::kNone, false, false, false};
  }

  algaguard::StartupResult start_ble_provisioning() override {
    return not_implemented("BLE_PROVISIONING");
  }
  algaguard::StartupResult poll_wifi() override {
    return not_implemented("WIFI");
  }
  algaguard::StartupResult poll_time_sync() override {
    return not_implemented("TIME_SYNC");
  }
  algaguard::StartupResult check_credentials() override {
    return {algaguard::OperationStatus::kMissing};
  }
  algaguard::StartupResult poll_credential_bootstrap() override {
    return not_implemented("CREDENTIAL_BOOTSTRAP");
  }
  algaguard::StartupResult poll_mqtt() override {
    return not_implemented("MQTT_MTLS");
  }
  algaguard::StartupResult validate_pending_ota() override {
    return not_implemented("OTA_VALIDATION");
  }
  algaguard::StartupResult controlled_reset() override {
    return not_implemented("CONTROLLED_RESET_STORAGE_ERASE");
  }

 private:
  algaguard::StartupResult not_implemented(const char* module) {
    ESP_LOGW(kTag, "module=%s result=NOT_IMPLEMENTED", module);
    return {algaguard::OperationStatus::kNotImplemented,
            algaguard::StartupReason::kModuleNotImplemented};
  }

  RuntimeBoardProfile& board_;
  bool profile_mismatch_{};
};

RuntimeBoardProfile board_profile;
EspFoundationServices services{board_profile};
algaguard::StartupStateMachine startup{services};

void render_startup_state() {
  static auto last_state = static_cast<algaguard::StartupState>(255);
  if (last_state == startup.state()) return;
  last_state = startup.state();
  const auto screen = algaguard::state_screen(startup.state(), startup.reason());
  const esp_err_t result = render_screen(screen);
  if (result != ESP_OK)
    ESP_LOGE(kTag, "display_update_failed code=%s",
             esp_err_to_name(result));
  ESP_LOGI(kTag, "startup_state=%s reason=%u",
           algaguard::startup_state_name(startup.state()).data(),
           static_cast<unsigned>(startup.reason()));
}

void startup_task(void*) {
  while (true) {
    startup.tick();
    if (startup.state() >= algaguard::StartupState::kProvisioningStateLoad)
      apply_leds(startup.state());
    if (startup.state() >= algaguard::StartupState::kInputInit)
      render_startup_state();
    if (startup.state() == algaguard::StartupState::kDegraded) {
      const auto delay = startup.retry_delay_ms();
      ESP_LOGW(kTag, "startup_retry delay_ms=%u attempt=%u reason=%u", delay,
               startup.retry_attempt(), static_cast<unsigned>(startup.reason()));
      vTaskDelay(pdMS_TO_TICKS(delay));
      startup.retry();
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void sampling_task(void*) {
  std::uint64_t sequence = 1;
  while (true) {
    if (startup.state() != algaguard::StartupState::kOnline) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    const auto sample = simulator.next(
        sequence++,
        static_cast<std::uint64_t>(xTaskGetTickCount()) * portTICK_PERIOD_MS);
    queue.push(sample);
    batcher.add(sample);
    if (batcher.ready()) {
      const auto qos1_batch = batcher.take_for_qos1();
      ESP_LOGI(kTag,
               "telemetry qos=1 samples=%u quality=SIMULATED ack=required",
               static_cast<unsigned>(qos1_batch.size()));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void input_task(void*) {
  while (true) {
    if (startup.state() < algaguard::StartupState::kProvisioningStateLoad) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    const auto now =
        static_cast<std::uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (up_button.update(
            gpio_get_level(
                static_cast<gpio_num_t>(algaguard::hardware::kButtonUp)) == 0,
            now) == algaguard::ButtonEvent::kShortPress)
      menu.up();
    if (down_button.update(
            gpio_get_level(
                static_cast<gpio_num_t>(algaguard::hardware::kButtonDown)) == 0,
            now) == algaguard::ButtonEvent::kShortPress)
      menu.down();
    if (select_button.update(
            gpio_get_level(static_cast<gpio_num_t>(
                algaguard::hardware::kButtonSelect)) == 0,
            now) == algaguard::ButtonEvent::kShortPress) {
      menu.select();
      if (menu.reset_confirmed()) startup.confirmed_reset(true);
    }
    if (back_button.update(
            gpio_get_level(
                static_cast<gpio_num_t>(algaguard::hardware::kButtonBack)) == 0,
            now) == algaguard::ButtonEvent::kShortPress)
      menu.back();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
}  // namespace

extern "C" void app_main() {
  esp_chip_info_t chip{};
  esp_chip_info(&chip);
  board_profile.chip_revision = chip.revision;
  board_profile.chip_cores = chip.cores;
  ESP_ERROR_CHECK(esp_flash_get_size(nullptr, &board_profile.flash_bytes));
  board_profile.psram_available = esp_psram_is_initialized();
  board_profile.psram_bytes = board_profile.psram_available
                                  ? static_cast<std::uint32_t>(
                                        esp_psram_get_size())
                                  : 0;
  board_profile.expected_n16r8 =
      board_profile.flash_bytes >= kExpectedFlashBytes &&
      board_profile.psram_bytes >= kExpectedPsramBytes;
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running != nullptr) board_profile.running_partition = running->label;
  const esp_app_desc_t* application = esp_app_get_description();
  if (application != nullptr)
    board_profile.firmware_version = application->version;

  ESP_LOGI(
      kTag,
      "board_report chip=ESP32-S3 revision=%u cores=%u flash_bytes=%u "
      "flash_mode=qio flash_frequency_mhz=80 psram_available=%s "
      "psram_bytes=%u partition=%s firmware=%s environment=%s "
      "build=%s expected_n16r8=%s",
      board_profile.chip_revision, board_profile.chip_cores,
      board_profile.flash_bytes,
      board_profile.psram_available ? "true" : "false",
      board_profile.psram_bytes, board_profile.running_partition,
      board_profile.firmware_version,
      algaguard::active_firmware_config().environment_id.data(),
      algaguard::active_firmware_config().build_environment.data(),
      board_profile.expected_n16r8 ? "true" : "false");
  if (!board_profile.expected_n16r8)
    ESP_LOGE(kTag,
             "reason=BOARD_PROFILE_MISMATCH expected_flash=16777216 "
             "expected_psram=8388608");

  xTaskCreate(startup_task, "startup_state", 6144, nullptr, 8, nullptr);
  xTaskCreate(input_task, "buttons", 4096, nullptr, 5, nullptr);
  xTaskCreate(sampling_task, "simulated_sampling", 4096, nullptr, 4, nullptr);
}
