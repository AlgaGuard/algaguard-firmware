#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace algaguard {

enum class FirmwareEnvironment : std::uint8_t {
  kDevelopment = 1,
  kCampus = 2,
  kProduction = 3,
};

enum class ConfigError : std::uint8_t {
  kNone,
  kUnsupportedSchema,
  kInvalidEnvironment,
  kInactiveEnvironment,
  kInvalidDeviceId,
  kInvalidBootstrapUrl,
  kInvalidMqttHost,
  kInvalidMqttPort,
  kInvalidOtaUrl,
  kMissingTrustAnchor,
  kInvalidBleService,
  kMissingBuildMetadata,
  kForbiddenSecretField,
  kPrivateMaterialPresent,
};

struct FirmwareConfig {
  std::uint16_t schema_version;
  FirmwareEnvironment environment;
  std::string_view environment_id;
  std::string_view device_id;
  std::string_view bootstrap_api_url;
  std::string_view mqtt_host;
  std::uint16_t mqtt_tls_port;
  std::string_view ota_api_url;
  std::string_view public_trust_anchor_id;
  std::string_view ble_service_uuid;
  std::uint16_t ble_protocol_version;
  std::string_view firmware_version;
  std::string_view build_environment;
  bool active;
};

inline constexpr FirmwareConfig kDevelopmentConfig{
    1,
    FirmwareEnvironment::kDevelopment,
    "development",
    "AG-000001",
    "https://dev.algaguard.bosilu.dev/api/v1/device-credentials/bootstrap",
    "mqtt-dev.algaguard.bosilu.dev",
    8883,
    "https://dev.algaguard.bosilu.dev/api/v1",
    "platform-provisioned-public-ca",
    "0000a1a0-0000-1000-8000-00805f9b34fb",
    1,
    "0.2.0-foundation",
    "platformio-espidf-development",
    true,
};

inline constexpr FirmwareConfig kCampusPlaceholderConfig{
    1,
    FirmwareEnvironment::kCampus,
    "campus",
    "AG-000001",
    "https://campus.invalid/api/v1/device-credentials/bootstrap",
    "mqtt.campus.invalid",
    8883,
    "https://campus.invalid/api/v1",
    "campus-public-ca-not-configured",
    "0000a1a0-0000-1000-8000-00805f9b34fb",
    1,
    "0.2.0-foundation",
    "platformio-espidf-campus-placeholder",
    false,
};

inline constexpr FirmwareConfig kProductionPlaceholderConfig{
    1,
    FirmwareEnvironment::kProduction,
    "production",
    "AG-000001",
    "https://production.invalid/api/v1/device-credentials/bootstrap",
    "mqtt.production.invalid",
    8883,
    "https://production.invalid/api/v1",
    "production-public-ca-not-configured",
    "0000a1a0-0000-1000-8000-00805f9b34fb",
    1,
    "0.2.0-foundation",
    "platformio-espidf-production-placeholder",
    false,
};

inline bool valid_device_id(std::string_view value) {
  if (value.size() != 9 || value.substr(0, 3) != "AG-") return false;
  return std::all_of(value.begin() + 3, value.end(), [](char character) {
    return std::isdigit(static_cast<unsigned char>(character)) != 0;
  });
}

inline bool valid_https_url(std::string_view value) {
  return value.size() > 8 && value.substr(0, 8) == "https://" &&
         value.find("localhost") == std::string_view::npos &&
         value.find("127.0.0.1") == std::string_view::npos;
}

inline bool valid_mqtt_host(std::string_view value) {
  return !value.empty() && value.find("://") == std::string_view::npos &&
         value.find("localhost") == std::string_view::npos &&
         value.find("127.0.0.1") == std::string_view::npos;
}

inline bool forbidden_configuration_field(std::string_view field) {
  std::string normalized{field};
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](char value) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
  });
  for (std::string_view forbidden :
       {"password", "private_key", "privatekey", "client_secret", "mqtt_username", "mqtt_password"}) {
    if (normalized.find(forbidden) != std::string::npos) return true;
  }
  return false;
}

inline bool contains_private_material(std::string_view value) {
  return value.find("PRIVATE KEY") != std::string_view::npos ||
         value.find("BEGIN OPENSSH") != std::string_view::npos;
}

inline ConfigError validate_firmware_config(
    const FirmwareConfig& config,
    const std::vector<std::string_view>& external_fields = {}) {
  if (config.schema_version != 1) return ConfigError::kUnsupportedSchema;
  if (config.environment < FirmwareEnvironment::kDevelopment ||
      config.environment > FirmwareEnvironment::kProduction)
    return ConfigError::kInvalidEnvironment;
  if (!config.active) return ConfigError::kInactiveEnvironment;
  if (!valid_device_id(config.device_id)) return ConfigError::kInvalidDeviceId;
  if (!valid_https_url(config.bootstrap_api_url)) return ConfigError::kInvalidBootstrapUrl;
  if (!valid_mqtt_host(config.mqtt_host)) return ConfigError::kInvalidMqttHost;
  if (config.mqtt_tls_port == 0) return ConfigError::kInvalidMqttPort;
  if (!valid_https_url(config.ota_api_url)) return ConfigError::kInvalidOtaUrl;
  if (config.public_trust_anchor_id.empty()) return ConfigError::kMissingTrustAnchor;
  if (config.ble_service_uuid.size() != 36 || config.ble_protocol_version == 0)
    return ConfigError::kInvalidBleService;
  if (config.firmware_version.empty() || config.build_environment.empty())
    return ConfigError::kMissingBuildMetadata;
  for (const auto field : external_fields)
    if (forbidden_configuration_field(field)) return ConfigError::kForbiddenSecretField;
  for (const auto value :
       {config.bootstrap_api_url, config.mqtt_host, config.ota_api_url,
        config.public_trust_anchor_id, config.build_environment})
    if (contains_private_material(value)) return ConfigError::kPrivateMaterialPresent;
  return ConfigError::kNone;
}

inline constexpr const FirmwareConfig& active_firmware_config() {
  return kDevelopmentConfig;
}

}  // namespace algaguard
