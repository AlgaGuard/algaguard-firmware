#include "algaguard/config.hpp"
#include "algaguard/ble_provisioning_transport.hpp"
#include "algaguard/esp_idf_wifi_connection_adapter.hpp"
#include "algaguard/credentials.hpp"
#include "algaguard/display.hpp"
#include "algaguard/hardware.hpp"
#include "algaguard/physical_test_harness.hpp"
#include "algaguard/physical_provisioning_runtime_bridge.hpp"
#include "algaguard/startup.hpp"
#include "algaguard/wifi_connection_runtime.hpp"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/uart.h"
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
#include <cstdio>
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
algaguard::EspIdfBleProvisioningTransport ble_provisioning_transport;
algaguard::EspIdfWifiConnectionAdapter wifi_connection_adapter;
algaguard::WifiConnectionRuntime wifi_connection_runtime{wifi_connection_adapter};
#if defined(ALGAGUARD_PHYSICAL_TEST_MODE)
algaguard::PhysicalTestSessionInstaller physical_session_installer;
algaguard::PhysicalProvisioningRuntimeBridge physical_runtime_bridge;
algaguard::PhysicalSessionControlProtocol physical_session_protocol;
bool physical_session_console_ready{};
void initialize_physical_session_console();
void poll_physical_session_console();
#endif

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
  bool chip_is_esp32s3{};
  bool partition_layout_valid{};
  bool oled_initialized{};
  [[maybe_unused]] bool physical_test_core_ready{};
};

i2c_master_bus_handle_t oled_bus{};
i2c_master_dev_handle_t oled_device{};
std::uint8_t oled_active_address{algaguard::hardware::kOledAddress};
bool oled_address_detected{};

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

#if defined(ALGAGUARD_PHYSICAL_TEST_MODE)
  // The address was measured over the USB-only physical-test harness. This is
  // deliberately an exact probe, never a general I2C bus scan.
  if (i2c_master_probe(oled_bus, algaguard::hardware::kOledAddress, 100) != ESP_OK) {
    i2c_del_master_bus(oled_bus);
    oled_bus = nullptr;
    return ESP_ERR_NOT_FOUND;
  }
  oled_address_detected = true;
#endif

  i2c_device_config_t device_config{};
  device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  device_config.device_address = oled_active_address;
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
#if defined(ALGAGUARD_PHYSICAL_TEST_MODE)
  ESP_LOGI(kTag, "OLED_ADDRESS_OK address=0x%02X", static_cast<unsigned>(oled_active_address));
#endif
  return ESP_OK;
}

void apply_leds(algaguard::StartupState state,
                bool remote_indicator = false) {
#if defined(ALGAGUARD_PHYSICAL_TEST_MODE)
  if (state != algaguard::StartupState::kFault) {
    const auto advertising = ble_provisioning_transport.advertisingRuntimeStatus();
    algaguard::PhysicalTestState physical_state =
        advertising.stage == algaguard::BleAdvertisingStage::kAdvStartOk
            ? algaguard::PhysicalTestState::kBleAdvertisingActive
            : advertising.stage == algaguard::BleAdvertisingStage::kAdvStartFailed
                  ? algaguard::PhysicalTestState::kBleAdvertisingFailed
                  : algaguard::PhysicalTestState::kBleAdvertisingInitializing;
    switch (wifi_connection_runtime.state()) {
      case algaguard::WifiConnectionState::kConnecting:
      case algaguard::WifiConnectionState::kRetryWait:
        physical_state = algaguard::PhysicalTestState::kWifiConnecting;
        break;
      case algaguard::WifiConnectionState::kConnected:
        physical_state = algaguard::PhysicalTestState::kWifiConnected;
        break;
      case algaguard::WifiConnectionState::kAuthFailed:
        physical_state = algaguard::PhysicalTestState::kAuthFailed;
        break;
      case algaguard::WifiConnectionState::kNetworkNotFound:
        physical_state = algaguard::PhysicalTestState::kNetworkNotFound;
        break;
      case algaguard::WifiConnectionState::kTimedOut:
        physical_state = algaguard::PhysicalTestState::kTimedOut;
        break;
      case algaguard::WifiConnectionState::kCancelled:
        physical_state = algaguard::PhysicalTestState::kCancelled;
        break;
      default:
        break;
    }
    const auto pattern = algaguard::physical_test_led_pattern(physical_state);
    const bool illuminated = !pattern.blink || (xTaskGetTickCount() / 10U) % 2U == 0;
    gpio_set_level(static_cast<gpio_num_t>(algaguard::hardware::kLedRed),
                   pattern.red && illuminated);
    gpio_set_level(static_cast<gpio_num_t>(algaguard::hardware::kLedGreen),
                   pattern.green && illuminated);
    gpio_set_level(static_cast<gpio_num_t>(algaguard::hardware::kLedBlue),
                   pattern.blue && illuminated);
    return;
  }
#endif
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
#if defined(ALGAGUARD_PHYSICAL_TEST_MODE)
    const auto preflight = algaguard::physical_board_preflight(
        {board_.chip_is_esp32s3, board_.flash_bytes, board_.psram_available,
         board_.psram_bytes, board_.partition_layout_valid, true, board_.oled_initialized});
    board_.physical_test_core_ready = preflight.safeForReadiness;
    if (!board_.physical_test_core_ready)
      return {algaguard::OperationStatus::kFatalFailure,
              algaguard::StartupReason::kBoardProfileMismatch};
#endif
    return {algaguard::OperationStatus::kSuccess};
  }

  algaguard::StartupResult storage_init() override {
    const esp_err_t result = nvs_flash_init();
    if (result != ESP_OK)
      return {algaguard::OperationStatus::kRecoverableFailure,
              algaguard::StartupReason::kStorageUnavailable};
#if defined(ALGAGUARD_PHYSICAL_TEST_MODE)
    ble_provisioning_transport.recordAdvertisingStage(
        algaguard::BleAdvertisingStage::kNvsReady, result);
#endif
    wifi_connection_adapter.setRuntime(&wifi_connection_runtime);
    if (!wifi_connection_adapter.init() || !wifi_connection_adapter.start())
      return {algaguard::OperationStatus::kRecoverableFailure,
              algaguard::StartupReason::kModuleUnavailable};
#if defined(ALGAGUARD_PHYSICAL_TEST_MODE)
    initialize_physical_session_console();
    ESP_LOGI(kTag, "WIFI_RUNTIME_READY_NOT_CONNECTED persistence=false");
    ESP_LOGI(kTag, "%s", algaguard::physical_wifi_gate_state_code(
                 algaguard::physical_wifi_connect_gate.state()).data());
    ESP_LOGI(kTag, "%s", algaguard::physical_session_state_code(physical_session_installer.state()).data());
#endif
    return {algaguard::OperationStatus::kSuccess};
  }

  algaguard::StartupResult display_init() override {
    board_.oled_initialized = configure_oled_i2c() == ESP_OK;
    if (!board_.oled_initialized)
      return {algaguard::OperationStatus::kRecoverableFailure,
              algaguard::StartupReason::kModuleUnavailable};
#if defined(ALGAGUARD_PHYSICAL_TEST_MODE)
    const auto preflight = algaguard::physical_board_preflight(
        {board_.chip_is_esp32s3, board_.flash_bytes, board_.psram_available,
         board_.psram_bytes, board_.partition_layout_valid, true, board_.oled_initialized});
    if (!preflight.displayValidated)
      return {algaguard::OperationStatus::kRecoverableFailure,
              algaguard::StartupReason::kModuleUnavailable};
    const auto screen =
        algaguard::physical_test_screen(algaguard::PhysicalTestState::kBleAdvertisingInitializing);
    ESP_LOGW(kTag, "INSECURE DEVELOPMENT PHYSICAL TEST MODE preflight=%u",
             static_cast<unsigned>(preflight.reason));
    if (render_screen(screen) != ESP_OK)
      return {algaguard::OperationStatus::kRecoverableFailure,
              algaguard::StartupReason::kModuleUnavailable};
    return {algaguard::OperationStatus::kSuccess};
#else
    const auto boot =
        algaguard::boot_screen(algaguard::active_firmware_config());
    auto screen = boot;
#if defined(ALGAGUARD_SECURITY_PROFILE_DEV_SOFTWARE_KEY)
    screen.lines[2] = "INSECURE DEV KEY";
    screen.lines[3] = "DEV SOFTWARE KEY";
    ESP_LOGW(kTag, "security_profile=DEV_SOFTWARE_KEY warning=SOFTWARE_PRIVATE_KEY_IN_USE");
#endif
    if (render_screen(screen) != ESP_OK)
      return {algaguard::OperationStatus::kRecoverableFailure,
              algaguard::StartupReason::kModuleUnavailable};
    return {algaguard::OperationStatus::kSuccess};
#endif
  }

  algaguard::StartupResult input_init() override {
    configure_gpio();
    return {algaguard::OperationStatus::kSuccess};
  }

  algaguard::StartupResult load_provisioning_state() override {
    if (profile_mismatch_)
      return {algaguard::OperationStatus::kRecoverableFailure,
              algaguard::StartupReason::kBoardProfileMismatch};
#if defined(ALGAGUARD_PHYSICAL_TEST_MODE)
    if (!board_.physical_test_core_ready)
      return {algaguard::OperationStatus::kFatalFailure,
              algaguard::StartupReason::kBoardProfileMismatch};
#endif
    return {algaguard::OperationStatus::kSuccess,
            algaguard::StartupReason::kNone, false, false, false};
  }

  algaguard::StartupResult start_ble_provisioning() override {
    if (!ble_provisioning_transport.startGattService())
      return {algaguard::OperationStatus::kRecoverableFailure,
              algaguard::StartupReason::kModuleUnavailable};
    return {algaguard::OperationStatus::kSuccess};
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
  static auto last_advertising_stage = static_cast<algaguard::BleAdvertisingStage>(255);
  static std::int32_t last_advertising_code{};
#if defined(ALGAGUARD_PHYSICAL_TEST_MODE)
  const auto advertising = ble_provisioning_transport.advertisingRuntimeStatus();
  if (last_state == startup.state() && last_advertising_stage == advertising.stage &&
      last_advertising_code == advertising.returnCode)
    return;
  last_advertising_stage = advertising.stage;
  last_advertising_code = advertising.returnCode;
#else
  if (last_state == startup.state()) return;
#endif
  last_state = startup.state();
#if defined(ALGAGUARD_PHYSICAL_TEST_MODE)
  auto physical_state =
      advertising.stage == algaguard::BleAdvertisingStage::kAdvStartOk
          ? algaguard::PhysicalTestState::kBleAdvertisingActive
          : advertising.stage == algaguard::BleAdvertisingStage::kAdvStartFailed
                ? algaguard::PhysicalTestState::kBleAdvertisingFailed
                : algaguard::PhysicalTestState::kBleAdvertisingInitializing;
  if (advertising.stage != algaguard::BleAdvertisingStage::kAdvStartFailed) {
    const auto gateState = algaguard::physical_wifi_connect_gate.state();
    if (gateState == algaguard::PhysicalWifiConnectGateState::kArmed)
      physical_state = algaguard::PhysicalTestState::kConnectTestArmed;
    else if (gateState == algaguard::PhysicalWifiConnectGateState::kConsumed)
      physical_state = algaguard::PhysicalTestState::kConnectTestConsumed;
    else if (gateState == algaguard::PhysicalWifiConnectGateState::kExpired)
      physical_state = algaguard::PhysicalTestState::kConnectTestExpired;
    else if (physical_runtime_bridge.handoffInstalled())
      physical_state = algaguard::PhysicalTestState::kWifiHandoffReady;
    else if (physical_session_installer.state() == algaguard::PhysicalSessionInstallerState::kArmed)
      physical_state = algaguard::PhysicalTestState::kSessionArmed;
    else if (physical_session_installer.state() == algaguard::PhysicalSessionInstallerState::kRejected)
      physical_state = algaguard::PhysicalTestState::kSessionInstallRejected;
  }
  auto visible_screen = algaguard::physical_test_screen(physical_state, advertising.returnCode);
#else
  auto visible_screen = algaguard::state_screen(startup.state(), startup.reason());
#endif
#if defined(ALGAGUARD_SECURITY_PROFILE_DEV_SOFTWARE_KEY)
  visible_screen.lines[3] = "INSECURE DEV KEY";
#endif
  const esp_err_t result = render_screen(visible_screen);
  if (result != ESP_OK)
    ESP_LOGE(kTag, "display_update_failed code=%s",
             esp_err_to_name(result));
  ESP_LOGI(kTag, "startup_state=%s reason=%u",
           algaguard::startup_state_name(startup.state()).data(),
           static_cast<unsigned>(startup.reason()));
}

#if defined(ALGAGUARD_PHYSICAL_TEST_MODE)
void initialize_physical_session_console() {
  if (physical_session_console_ready) return;
  if (!uart_is_driver_installed(UART_NUM_0) &&
      uart_driver_install(UART_NUM_0, 1024, 0, 0, nullptr, 0) != ESP_OK) {
    ESP_LOGW(kTag, "SESSION_INSTALL_REJECTED console_ready=false");
    return;
  }
  physical_session_console_ready = true;
  ESP_LOGI(kTag, "SESSION_INSTALLER_READY console=COM16 protocol=%u",
           static_cast<unsigned>(algaguard::kPhysicalSessionProtocolVersion));
}

void poll_physical_session_console() {
  if (!physical_session_console_ready) return;
  std::array<std::uint8_t, 64> received{};
  const auto count = uart_read_bytes(UART_NUM_0, received.data(), received.size(), 0);
  if (count <= 0) return;
  const auto acknowledgement = physical_session_protocol.ingest(
      ble_provisioning_transport, physical_session_installer, received.data(),
      static_cast<std::size_t>(count), static_cast<std::uint64_t>(xTaskGetTickCount()),
      physical_runtime_bridge.handoffInstalled(),
      wifi_connection_runtime.state() == algaguard::WifiConnectionState::kConnecting ||
          wifi_connection_runtime.state() == algaguard::WifiConnectionState::kRetryWait);
  if (!physical_session_protocol.awaitingFrame()) {
    const auto code = algaguard::physical_session_ack_code(acknowledgement);
    if (acknowledgement == algaguard::PhysicalSessionControlAck::kOledAddressQuery) {
      char address[20]{};
      const auto written = oled_address_detected
                               ? std::snprintf(address, sizeof(address), "OLED_ADDRESS_0x%02X\n",
                                               static_cast<unsigned>(oled_active_address))
                               : std::snprintf(address, sizeof(address), "OLED_ADDRESS_NONE\n");
      if (written > 0)
        (void)uart_write_bytes(UART_NUM_0, address,
                               static_cast<std::size_t>(written));
    } else {
      (void)uart_write_bytes(UART_NUM_0, code.data(), code.size());
      (void)uart_write_bytes(UART_NUM_0, "\n", 1);
    }
    const auto gate = algaguard::physical_wifi_gate_state_code(
        algaguard::physical_wifi_connect_gate.state());
    const auto connectionActive =
        wifi_connection_runtime.state() == algaguard::WifiConnectionState::kConnecting ||
        wifi_connection_runtime.state() == algaguard::WifiConnectionState::kRetryWait;
    ESP_LOGI(kTag, "%.*s gate=%.*s activeSessionPresent=%s handoffPresent=%s wifiRuntimeReady=true "
             "connectAttemptActive=%s secretsCleared=%s", static_cast<int>(code.size()), code.data(),
             static_cast<int>(gate.size()), gate.data(),
             physical_session_installer.armed() ? "true" : "false",
             physical_runtime_bridge.handoffInstalled() ? "true" : "false",
             connectionActive ? "true" : "false",
             physical_session_installer.secretsCleared() && wifi_connection_runtime.secretsCleared()
                 ? "true" : "false");
  }
}
#endif

void startup_task(void*) {
#if defined(ALGAGUARD_PHYSICAL_TEST_MODE)
  auto last_advertising_status = ble_provisioning_transport.advertisingRuntimeStatus();
#endif
  while (true) {
    startup.tick();
    ble_provisioning_transport.pollProvisioningTransport(
        static_cast<std::uint64_t>(xTaskGetTickCount()));
    wifi_connection_runtime.poll(static_cast<std::uint64_t>(xTaskGetTickCount()));
#if defined(ALGAGUARD_PHYSICAL_TEST_MODE)
    poll_physical_session_console();
    if (ble_provisioning_transport.latestSafeStatus().view().find("ACCEPTED") != std::string_view::npos &&
        physical_runtime_bridge.onAccepted(ble_provisioning_transport, wifi_connection_runtime,
                                           physical_session_installer)) {
      physical_session_installer.markProcessingStarted();
      ESP_LOGI(kTag, "WIFI_HANDOFF_INSTALLED connectExecutionEnabled=%s",
               physical_runtime_bridge.connectExecutionEnabled() ? "true" : "false");
    }
    const auto advertising = ble_provisioning_transport.advertisingRuntimeStatus();
    if (advertising.stage != last_advertising_status.stage ||
        advertising.returnCode != last_advertising_status.returnCode ||
        advertising.retryCount != last_advertising_status.retryCount ||
        advertising.advertisingActive != last_advertising_status.advertisingActive) {
      const auto diagnostic = algaguard::ble_advertising_safe_diagnostic(advertising);
      ESP_LOGI(kTag, "%s", diagnostic.c_str());
      last_advertising_status = advertising;
    }
    if (startup.state() == algaguard::StartupState::kUnprovisioned &&
        board_profile.physical_test_core_ready)
      (void)startup.begin_provisioning();
#endif
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
  board_profile.chip_is_esp32s3 = chip.model == CHIP_ESP32S3;
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
  board_profile.partition_layout_valid =
      running != nullptr && esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                     ESP_PARTITION_SUBTYPE_DATA_OTA,
                                                     nullptr) != nullptr;
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

#if defined(ALGAGUARD_PHYSICAL_TEST_MODE)
  // Keep the read-only physical diagnostic channel available even when a
  // display preflight fails, so a wiring/address fault can be diagnosed safely.
  initialize_physical_session_console();
#endif
  xTaskCreate(startup_task, "startup_state", 6144, nullptr, 8, nullptr);
  xTaskCreate(input_task, "buttons", 4096, nullptr, 5, nullptr);
  xTaskCreate(sampling_task, "simulated_sampling", 4096, nullptr, 4, nullptr);
}
