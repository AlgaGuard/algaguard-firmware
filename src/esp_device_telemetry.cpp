#include "algaguard/esp_device_telemetry.hpp"

#if defined(ESP_PLATFORM) && defined(ALGAGUARD_ENABLE_DEVICE_MQTT_TELEMETRY)

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "esp_log.h"
#include "esp_random.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace algaguard {
namespace {
constexpr char kTag[] = "algaguard-mqtt";
constexpr std::size_t kMaximumInboundBytes = 4096;
constexpr std::uint64_t kPublishIntervalMs = 5000;

std::optional<std::string> jsonString(std::string_view input,
                                      std::string_view key) {
  const std::string marker = "\"" + std::string{key} + "\"";
  auto position = input.find(marker);
  if (position == std::string_view::npos ||
      input.find(marker, position + marker.size()) != std::string_view::npos)
    return std::nullopt;
  position = input.find(':', position + marker.size());
  if (position == std::string_view::npos) return std::nullopt;
  position = input.find('"', position + 1);
  if (position == std::string_view::npos) return std::nullopt;
  const auto end = input.find('"', position + 1);
  if (end == std::string_view::npos || end - position - 1 > 128)
    return std::nullopt;
  const auto value = input.substr(position + 1, end - position - 1);
  if (value.find('\\') != std::string_view::npos) return std::nullopt;
  return std::string{value};
}

std::string randomUuid() {
  std::array<std::uint8_t, 16> bytes{};
  esp_fill_random(bytes.data(), bytes.size());
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);
  char output[37]{};
  std::snprintf(output, sizeof(output),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
                bytes[12], bytes[13], bytes[14], bytes[15]);
  return output;
}

std::optional<std::string> utcNow() {
  std::time_t now{};
  std::time(&now);
  if (now < 1704067200) return std::nullopt;
  std::tm utc{};
  gmtime_r(&now, &utc);
  char output[21]{};
  if (std::strftime(output, sizeof(output), "%Y-%m-%dT%H:%M:%SZ", &utc) != 20)
    return std::nullopt;
  return std::string{output};
}

std::optional<std::time_t> parseUtc(std::string_view value) {
  if (value.size() != 20 || value[4] != '-' || value[7] != '-' ||
      value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
      value[19] != 'Z')
    return std::nullopt;
  std::tm parsed{};
  if (std::sscanf(std::string{value}.c_str(), "%4d-%2d-%2dT%2d:%2d:%2dZ",
                  &parsed.tm_year, &parsed.tm_mon, &parsed.tm_mday,
                  &parsed.tm_hour, &parsed.tm_min, &parsed.tm_sec) != 6)
    return std::nullopt;
  parsed.tm_year -= 1900;
  parsed.tm_mon -= 1;
  const auto epoch = timegm(&parsed);
  return epoch > 0 ? std::optional<std::time_t>{epoch} : std::nullopt;
}

}  // namespace

struct EspDeviceTelemetryRuntime::Impl {
  esp_mqtt_client_handle_t client{};
  std::string deviceId;
  BrokerEndpoint endpoint;
  SoftwareTlsIdentity identity;
  std::string uri;
  std::string telemetryTopic;
  std::string ackTopic;
  std::string commandTopic;
  std::string commandResultTopic;
  DeviceTelemetryPublishWindow window;
  StaticSemaphore_t mutexStorage{};
  SemaphoreHandle_t mutex{xSemaphoreCreateMutexStatic(&mutexStorage)};
  std::atomic_bool connected{};
  std::atomic_bool profileInstalled{};
  std::atomic_bool unpairPending{};
  std::string unpairCommandId;
  std::time_t unpairExpiresAt{};
  std::uint64_t lastPublishMs{};

  ~Impl() {
    if (client != nullptr) {
      (void)esp_mqtt_client_stop(client);
      (void)esp_mqtt_client_destroy(client);
    }
    std::fill(identity.client_private_key.begin(),
              identity.client_private_key.end(), 0);
  }

  bool lock(TickType_t wait = pdMS_TO_TICKS(250)) {
    return mutex != nullptr && xSemaphoreTake(mutex, wait) == pdTRUE;
  }
  void unlock() { xSemaphoreGive(mutex); }

  int publish(const std::string& topic, std::string_view payload) {
    if (client == nullptr || !connected.load(std::memory_order_acquire)) return -1;
    return esp_mqtt_client_enqueue(client, topic.c_str(), payload.data(),
                                   static_cast<int>(payload.size()), 1, 0, true);
  }

  void commandResult(std::string_view commandId, std::string_view status,
                     std::string_view code = {}) {
    const auto now = utcNow();
    if (!now) return;
    std::string body =
        "{\"schema\":\"urn:algaguard:schema:mqtt:command-result:v1\","
        "\"schemaVersion\":\"1.0.0\",\"messageId\":\"" + randomUuid() +
        "\",\"deviceId\":\"" + deviceId + "\",\"sentAt\":\"" + *now +
        "\",\"payload\":{\"commandId\":\"" + std::string{commandId} +
        "\",\"status\":\"" + std::string{status} +
        "\",\"reportedAt\":\"" + *now + "\"";
    if (!code.empty())
      body += ",\"error\":{\"code\":\"" + std::string{code} +
              "\",\"message\":\"Command was rejected\"," 
              "\"retryable\":false}";
    body += "}}";
    (void)publish(commandResultTopic, body);
    std::fill(body.begin(), body.end(), '\0');
  }

  void receiveCommand(std::string_view payload) {
    const auto schema = jsonString(payload, "schema");
    const auto returnedDevice = jsonString(payload, "deviceId");
    const auto schemaVersion = jsonString(payload, "schemaVersion");
    const auto commandId = jsonString(payload, "commandId");
    const auto commandType = jsonString(payload, "commandType");
    const auto configurationId = jsonString(payload, "configurationId");
    const auto profileId = jsonString(payload, "profileId");
    const auto profileVersion = jsonString(payload, "profileVersion");
    const auto expiresAt = jsonString(payload, "expiresAt");
    if (schema &&
        *schema == "urn:algaguard:schema:mqtt:physical-unpair-command:v1" &&
        schemaVersion && *schemaVersion == "1.0.0" && returnedDevice &&
        *returnedDevice == deviceId && commandId && valid_uuid(*commandId) &&
        commandType && *commandType == "REQUEST_PHYSICAL_UNPAIR" &&
        expiresAt) {
      const auto expiry = parseUtc(*expiresAt);
      std::time_t current{};
      std::time(&current);
      if (!expiry || current <= 0 || *expiry <= current ||
          *expiry > current + 300 || !lock()) {
        commandResult(*commandId, "REJECTED", "INVALID_OR_EXPIRED_UNPAIR");
        return;
      }
      if (unpairPending.load(std::memory_order_acquire)) {
        unlock();
        commandResult(*commandId, "REJECTED", "UNPAIR_ALREADY_PENDING");
        return;
      }
      unpairCommandId = *commandId;
      unpairExpiresAt = *expiry;
      unpairPending.store(true, std::memory_order_release);
      unlock();
      commandResult(*commandId, "IN_PROGRESS");
      ESP_LOGI(kTag, "PHYSICAL_UNPAIR_WAITING_FOR_LOCAL_CONFIRMATION");
      return;
    }
    if (!schema || *schema != "urn:algaguard:schema:mqtt:command:v1" ||
        !schemaVersion || *schemaVersion != "1.0.0" || !returnedDevice ||
        *returnedDevice != deviceId || !commandId ||
        !valid_uuid(*commandId) || !commandType ||
        *commandType != "APPLY_PROFILE_CONFIGURATION" || !configurationId ||
        !valid_uuid(*configurationId) || !profileId || !profileVersion) {
      if (commandId && valid_uuid(*commandId))
        commandResult(*commandId, "REJECTED", "INVALID_PROFILE_CONFIGURATION");
      ESP_LOGW(kTag, "MQTT_PROFILE_CONFIGURATION_REJECTED reason=INVALID_CONTRACT");
      return;
    }
    if (!lock()) return;
    const bool installed = window.installProfile({*profileId, *profileVersion});
    profileInstalled.store(installed, std::memory_order_release);
    unlock();
    if (!installed) {
      commandResult(*commandId, "REJECTED", "INVALID_PROFILE_CONFIGURATION");
      ESP_LOGW(kTag, "MQTT_PROFILE_CONFIGURATION_REJECTED reason=INVALID_REFERENCE");
      return;
    }
    commandResult(*commandId, "SUCCEEDED");
    ESP_LOGI(kTag, "MQTT_PROFILE_CONFIGURATION_ACTIVE source=AUTHORIZED_COMMAND");
  }

  bool finishUnpair(std::string_view status, std::string_view code = {}) {
    if (!lock()) return false;
    if (!unpairPending.load(std::memory_order_acquire) ||
        unpairCommandId.empty()) {
      unlock();
      return false;
    }
    const auto commandId = unpairCommandId;
    std::fill(unpairCommandId.begin(), unpairCommandId.end(), '\0');
    unpairCommandId.clear();
    unpairExpiresAt = 0;
    unpairPending.store(false, std::memory_order_release);
    unlock();
    commandResult(commandId, status, code);
    return true;
  }

  void expireUnpair() {
    if (!unpairPending.load(std::memory_order_acquire)) return;
    std::time_t current{};
    std::time(&current);
    if (current > 0 && current >= unpairExpiresAt)
      (void)finishUnpair("EXPIRED", "PHYSICAL_CONFIRMATION_EXPIRED");
  }

  void receiveAck(std::string_view payload) {
    const auto schema = jsonString(payload, "schema");
    const auto schemaVersion = jsonString(payload, "schemaVersion");
    const auto returnedDevice = jsonString(payload, "deviceId");
    const auto batchId = jsonString(payload, "batchId");
    const auto status = jsonString(payload, "status");
    if (!schema || *schema != "urn:algaguard:schema:mqtt:telemetry-ack:v1" ||
        !schemaVersion || *schemaVersion != "1.0.0" || !returnedDevice ||
        *returnedDevice != deviceId || !batchId || !valid_uuid(*batchId) ||
        !status)
      return;
    if (!lock()) return;
    const bool acknowledged = window.acknowledge(*batchId, *status);
    unlock();
    if (acknowledged)
      ESP_LOGI(kTag, "DEVICE_TELEMETRY_ACK status=%s source=DEVICE_LOCAL_SIMULATION",
               status->c_str());
  }

  void receive(esp_mqtt_event_handle_t event) {
    if (event->topic == nullptr || event->data == nullptr ||
        event->current_data_offset != 0 || event->data_len != event->total_data_len ||
        event->data_len <= 0 ||
        static_cast<std::size_t>(event->data_len) > kMaximumInboundBytes)
      return;
    const std::string_view topic{event->topic,
                                 static_cast<std::size_t>(event->topic_len)};
    const std::string_view payload{event->data,
                                   static_cast<std::size_t>(event->data_len)};
    if (topic == commandTopic) receiveCommand(payload);
    else if (topic == ackTopic) receiveAck(payload);
  }

  static void event(void* argument, esp_event_base_t, std::int32_t eventId,
                    void* eventData) {
    auto* self = static_cast<Impl*>(argument);
    auto* mqttEvent = static_cast<esp_mqtt_event_handle_t>(eventData);
    switch (static_cast<esp_mqtt_event_id_t>(eventId)) {
      case MQTT_EVENT_CONNECTED:
        self->connected.store(true, std::memory_order_release);
        (void)esp_mqtt_client_subscribe(self->client, self->ackTopic.c_str(), 1);
        (void)esp_mqtt_client_subscribe(self->client, self->commandTopic.c_str(), 1);
        ESP_LOGI(kTag, "DEVICE_MQTT_CONNECTED mtls=true source=DEVICE_LOCAL_SIMULATION");
        break;
      case MQTT_EVENT_DISCONNECTED:
        self->connected.store(false, std::memory_order_release);
        ESP_LOGW(kTag, "DEVICE_MQTT_DISCONNECTED retry=bounded");
        break;
      case MQTT_EVENT_DATA:
        self->receive(mqttEvent);
        break;
      case MQTT_EVENT_ERROR:
        ESP_LOGW(kTag, "DEVICE_MQTT_ERROR category=TRANSPORT_OR_TLS");
        break;
      default:
        break;
    }
  }
};

EspDeviceTelemetryRuntime::EspDeviceTelemetryRuntime()
    : impl_(std::make_unique<Impl>()) {}
EspDeviceTelemetryRuntime::~EspDeviceTelemetryRuntime() = default;

bool EspDeviceTelemetryRuntime::start(std::string deviceId,
                                      BrokerEndpoint endpoint,
                                      SoftwareTlsIdentity identity) {
  if (impl_->client != nullptr || !valid_telemetry_device_id(deviceId) ||
      endpoint.host.empty() || endpoint.host != endpoint.server_name ||
      endpoint.port == 0 || identity.status != SecurityStatus::kReady ||
      identity.ca_certificate.empty() || identity.client_certificate.empty() ||
      identity.client_private_key.empty())
    return false;
  impl_->deviceId = std::move(deviceId);
  impl_->endpoint = std::move(endpoint);
  impl_->identity = std::move(identity);
  impl_->uri = "mqtts://" + impl_->endpoint.host + ":" +
               std::to_string(impl_->endpoint.port);
  const std::string root = "algaguard/v1/devices/" + impl_->deviceId;
  impl_->telemetryTopic = root + "/telemetry";
  impl_->ackTopic = root + "/telemetry/ack";
  impl_->commandTopic = root + "/commands";
  impl_->commandResultTopic = root + "/command-results";

  esp_mqtt_client_config_t config{};
  config.broker.address.uri = impl_->uri.c_str();
  config.broker.verification.certificate =
      reinterpret_cast<const char*>(impl_->identity.ca_certificate.data());
  config.broker.verification.common_name = impl_->endpoint.server_name.c_str();
  config.credentials.client_id = impl_->deviceId.c_str();
  config.credentials.authentication.certificate = reinterpret_cast<const char*>(
      impl_->identity.client_certificate.data());
  config.credentials.authentication.key = reinterpret_cast<const char*>(
      impl_->identity.client_private_key.data());
  config.credentials.authentication.key_len =
      impl_->identity.client_private_key.size();
  // esp-mqtt sends PINGREQ at keepalive/2 (mqtt_client.c: process_keepalive()).
  // A provisioned keepalive of 60s therefore pings every 30s, which lines up
  // almost exactly with the ~28-30s idle/NAT timeout several carrier-grade
  // and consumer NATs enforce -- the ping and the timeout race, and the ping
  // occasionally loses, producing the "transport_read(): EOF, errno=119"
  // disconnect/reconnect cycle observed on live hardware. Clamping keepalive
  // well below that gives every ping real margin, regardless of what was
  // baked into an already-paired device's stored credentials.
  constexpr std::uint32_t kMaxKeepaliveSeconds = 20;
  config.session.keepalive = std::min(impl_->endpoint.keepalive_seconds,
                                       kMaxKeepaliveSeconds);
  config.session.disable_clean_session = true;
  config.network.reconnect_timeout_ms = 2000;
  config.network.timeout_ms = 10000;
  impl_->client = esp_mqtt_client_init(&config);
  if (impl_->client == nullptr) return false;
  if (esp_mqtt_client_register_event(impl_->client, MQTT_EVENT_ANY,
                                     &Impl::event, impl_.get()) != ESP_OK ||
      esp_mqtt_client_start(impl_->client) != ESP_OK) {
    esp_mqtt_client_destroy(impl_->client);
    impl_->client = nullptr;
    return false;
  }
  return true;
}

void EspDeviceTelemetryRuntime::poll(const LocalDemoReading& reading,
                                     std::uint64_t uptimeMs) {
  impl_->expireUnpair();
  // Profile assignment routes threshold notifications; it must never gate
  // whether real sensor readings reach the platform, so profileInstalled is
  // deliberately not part of this condition.
  if (!impl_->connected.load(std::memory_order_acquire) || !impl_->lock())
    return;
  if (impl_->window.exhausted(uptimeMs)) {
    impl_->window.clearPending();
    ESP_LOGW(kTag, "DEVICE_TELEMETRY_ACK_TIMEOUT retries=3 dropped=1");
  }
  if (impl_->window.retryDue(uptimeMs)) {
    const std::string payload{impl_->window.pendingPayload()};
    if (impl_->window.retry(uptimeMs)) (void)impl_->publish(impl_->telemetryTopic, payload);
    impl_->unlock();
    return;
  }
  if (impl_->window.pending() || uptimeMs < impl_->lastPublishMs + kPublishIntervalMs) {
    impl_->unlock();
    return;
  }
  const auto now = utcNow();
  if (!now) {
    impl_->unlock();
    return;
  }
  const std::string messageId = randomUuid();
  const std::string batchId = randomUuid();
  const auto payload = build_device_simulated_telemetry(
      impl_->deviceId, impl_->window.profile(), reading, *now, messageId,
      batchId, uptimeMs);
  if (payload && impl_->window.begin(batchId, *payload, uptimeMs)) {
    impl_->lastPublishMs = uptimeMs;
    (void)impl_->publish(impl_->telemetryTopic, *payload);
    ESP_LOGI(kTag,
             "DEVICE_TELEMETRY_PUBLISHED qos=1 samples=1 source=DEVICE_LOCAL_SIMULATION");
  }
  impl_->unlock();
}

bool EspDeviceTelemetryRuntime::connected() const {
  return impl_->connected.load(std::memory_order_acquire);
}
bool EspDeviceTelemetryRuntime::started() const { return impl_->client != nullptr; }
bool EspDeviceTelemetryRuntime::profileInstalled() const {
  return impl_->profileInstalled.load(std::memory_order_acquire);
}

bool EspDeviceTelemetryRuntime::physicalUnpairPending() const {
  return impl_->unpairPending.load(std::memory_order_acquire);
}

bool EspDeviceTelemetryRuntime::confirmPhysicalUnpair(bool localStateCleared) {
  return localStateCleared
             ? impl_->finishUnpair("SUCCEEDED")
             : impl_->finishUnpair("FAILED", "LOCAL_STATE_CLEAR_FAILED");
}

bool EspDeviceTelemetryRuntime::cancelPhysicalUnpair() {
  return impl_->finishUnpair("REJECTED", "PHYSICAL_CONFIRMATION_CANCELLED");
}

}  // namespace algaguard

#endif
