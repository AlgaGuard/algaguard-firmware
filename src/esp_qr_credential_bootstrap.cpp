#include "algaguard/esp_qr_credential_bootstrap.hpp"

#if defined(ESP_PLATFORM) && defined(ALGAGUARD_ENABLE_QR_ONBOARDING)

#include <algorithm>
#include <array>
#include <charconv>
#include <ctime>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"

namespace algaguard {
namespace {
constexpr std::size_t kMaximumResponseBytes = 24U * 1024U;
constexpr std::time_t kMinimumTrustedUnixTime = 1704067200;  // 2024-01-01 UTC
constexpr TickType_t kClockSyncTimeout = pdMS_TO_TICKS(15000);

void wipe(std::string& value) {
  std::fill(value.begin(), value.end(), '\0');
  value.clear();
}

bool ensureTrustedClock() {
  std::time_t now{};
  std::time(&now);
  if (now >= kMinimumTrustedUnixTime) return true;

  const esp_sntp_config_t config =
      ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  if (esp_netif_sntp_init(&config) != ESP_OK) {
    ESP_LOGW("algaguard", "QR_CREDENTIAL_CLOCK_SYNC_FAILED stage=init");
    return false;
  }
  const esp_err_t synchronized = esp_netif_sntp_sync_wait(kClockSyncTimeout);
  std::time(&now);
  esp_netif_sntp_deinit();
  const bool ready = synchronized == ESP_OK && now >= kMinimumTrustedUnixTime;
  if (!ready)
    ESP_LOGW("algaguard", "QR_CREDENTIAL_CLOCK_SYNC_FAILED stage=wait");
  return ready;
}

std::string jsonEscape(std::string_view value) {
  std::string output;
  output.reserve(value.size() + 32);
  for (const unsigned char byte : value) {
    switch (byte) {
      case '\\': output += "\\\\"; break;
      case '"': output += "\\\""; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default:
        if (byte < 0x20) {
          char encoded[7]{};
          std::snprintf(encoded, sizeof(encoded), "\\u%04x", byte);
          output += encoded;
        } else {
          output.push_back(static_cast<char>(byte));
        }
    }
  }
  return output;
}

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
  std::string output;
  for (++position; position < input.size(); ++position) {
    const char value = input[position];
    if (value == '"') return output;
    if (value != '\\') {
      if (static_cast<unsigned char>(value) < 0x20) return std::nullopt;
      output.push_back(value);
      continue;
    }
    if (++position >= input.size()) return std::nullopt;
    switch (input[position]) {
      case '"': output.push_back('"'); break;
      case '\\': output.push_back('\\'); break;
      case '/': output.push_back('/'); break;
      case 'b': output.push_back('\b'); break;
      case 'f': output.push_back('\f'); break;
      case 'n': output.push_back('\n'); break;
      case 'r': output.push_back('\r'); break;
      case 't': output.push_back('\t'); break;
      default: return std::nullopt;
    }
  }
  return std::nullopt;
}

std::optional<std::vector<std::string>> jsonStringArray(
    std::string_view input, std::string_view key) {
  const std::string marker = "\"" + std::string{key} + "\"";
  auto position = input.find(marker);
  if (position == std::string_view::npos ||
      input.find(marker, position + marker.size()) != std::string_view::npos)
    return std::nullopt;
  position = input.find('[', position + marker.size());
  if (position == std::string_view::npos) return std::nullopt;
  const auto end = input.find(']', position + 1);
  if (end == std::string_view::npos) return std::nullopt;
  std::vector<std::string> result;
  std::string fragment{"{\"value\":"};
  fragment.append(input.substr(position + 1, end - position - 1));
  fragment.push_back('}');
  auto value = jsonString(fragment, "value");
  if (!value || value->find("-----BEGIN CERTIFICATE-----") == std::string::npos)
    return std::nullopt;
  result.push_back(std::move(*value));
  return result;
}

bool uuid(std::string_view value) {
  if (value.size() != 36) return false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (value[index] != '-') return false;
    } else if (!((value[index] >= '0' && value[index] <= '9') ||
                 (value[index] >= 'a' && value[index] <= 'f') ||
                 (value[index] >= 'A' && value[index] <= 'F'))) return false;
  }
  return true;
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

std::optional<std::uint64_t> isoEpoch(std::string_view value) {
  int year{}, month{}, day{}, hour{}, minute{}, second{};
  if (value.size() < 20 || std::sscanf(std::string{value.substr(0, 20)}.c_str(),
      "%4d-%2d-%2dT%2d:%2d:%2dZ", &year, &month, &day, &hour, &minute,
      &second) != 6 || year < 2020 || month < 1 || month > 12 || day < 1 ||
      day > 31 || hour > 23 || minute > 59 || second > 60) return std::nullopt;
  auto daysFromCivil = [](int y, unsigned m, unsigned d) -> std::int64_t {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153U * (m + (m > 2 ? -3 : 9)) + 2U) / 5U + d - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return static_cast<std::int64_t>(era) * 146097 + doe - 719468;
  };
  const auto days = daysFromCivil(year, static_cast<unsigned>(month),
                                  static_cast<unsigned>(day));
  if (days < 0) return std::nullopt;
  return static_cast<std::uint64_t>(days) * 86400ULL +
         static_cast<std::uint64_t>(hour) * 3600ULL +
         static_cast<std::uint64_t>(minute) * 60ULL +
         static_cast<std::uint64_t>(second);
}

struct HttpResponse {
  std::string body;
  bool overflow{};
};

esp_err_t onHttpEvent(esp_http_client_event_t* event) {
  auto* response = static_cast<HttpResponse*>(event->user_data);
  if (event->event_id == HTTP_EVENT_ON_DATA && response != nullptr &&
      event->data != nullptr && event->data_len > 0) {
    const auto incoming = static_cast<std::size_t>(event->data_len);
    if (response->body.size() + incoming > kMaximumResponseBytes) {
      response->overflow = true;
      return ESP_FAIL;
    }
    response->body.append(static_cast<const char*>(event->data), incoming);
  }
  return ESP_OK;
}

std::optional<std::string> post(std::string_view url, std::string& body,
                                std::string_view authorization = {}) {
  if (!ensureTrustedClock()) {
    wipe(body);
    return std::nullopt;
  }
  HttpResponse response;
  esp_http_client_config_t config{};
  const std::string stableUrl{url};
  config.url = stableUrl.c_str();
  config.event_handler = onHttpEvent;
  config.user_data = &response;
  config.timeout_ms = 15000;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.disable_auto_redirect = true;
  auto client = esp_http_client_init(&config);
  if (client == nullptr) { wipe(body); return std::nullopt; }
  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "Accept", "application/json");
  if (!authorization.empty()) {
    const std::string header = "Bearer " + std::string{authorization};
    esp_http_client_set_header(client, "Authorization", header.c_str());
  }
  esp_http_client_set_post_field(client, body.data(), body.size());
  const esp_err_t performed = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  wipe(body);
  if (performed != ESP_OK || response.overflow || status != 201) {
    wipe(response.body);
    return std::nullopt;
  }
  return response.body;
}
}  // namespace

std::optional<QrBootstrapAuthorization>
EspQrCredentialBootstrapTransport::exchange(std::string_view sessionToken,
                                             std::string_view deviceId) {
  std::string body =
      "{\"schema\":\"urn:algaguard:schema:onboarding:bootstrap-token-exchange-request:v1\","
      "\"schemaVersion\":\"1.0.0\",\"sessionToken\":\"" +
      jsonEscape(sessionToken) + "\",\"deviceId\":\"" +
      jsonEscape(deviceId) + "\"}";
  auto response = post(baseUrl_ + "/exchange", body);
  if (!response) return std::nullopt;
  auto token = jsonString(*response, "bootstrapToken");
  auto returnedDevice = jsonString(*response, "deviceId");
  auto deviceUuid = jsonString(*response, "deviceUuid");
  const bool valid = token && token->size() >= 32 && token->size() <= 128 &&
                     returnedDevice && *returnedDevice == deviceId &&
                     deviceUuid && uuid(*deviceUuid);
  wipe(*response);
  if (!valid) {
    if (token) wipe(*token);
    return std::nullopt;
  }
  QrBootstrapAuthorization result;
  result.deviceUuid = std::move(*deviceUuid);
  result.bootstrapToken = std::move(*token);
  return result;
}

std::optional<PublicCredentialBundle>
EspQrCredentialBootstrapTransport::issue(std::string_view bootstrapToken,
                                         const CsrSubmission& csr) {
  std::string body =
      "{\"schema\":\"urn:algaguard:schema:onboarding:credential-csr-submission:v1\","
      "\"schemaVersion\":\"1.0.0\",\"deviceUuid\":\"" +
      jsonEscape(csr.binding.device_uuid) + "\",\"deviceId\":\"" +
      jsonEscape(csr.binding.device_id) +
      "\",\"purpose\":\"INITIAL\",\"rotationId\":null,\"idempotencyKey\":\"" +
      randomUuid() + "\",\"keyAlgorithm\":\"" +
      (csr.algorithm == KeyAlgorithm::kEcP256 ? "EC_P256" : "RSA_3072") +
      "\",\"csrPem\":\"" +
      jsonEscape(csr.pem) + "\"}";
  auto response = post(baseUrl_ + "/issue", body, bootstrapToken);
  if (!response) return std::nullopt;
  auto credentialId = jsonString(*response, "credentialId");
  auto deviceUuid = jsonString(*response, "deviceUuid");
  auto deviceId = jsonString(*response, "deviceId");
  auto certificate = jsonString(*response, "certificatePem");
  auto notBefore = jsonString(*response, "notBefore");
  auto notAfter = jsonString(*response, "notAfter");
  auto chain = jsonStringArray(*response, "caChainPem");
  auto beforeEpoch = notBefore ? isoEpoch(*notBefore) : std::nullopt;
  auto afterEpoch = notAfter ? isoEpoch(*notAfter) : std::nullopt;
  const bool valid = credentialId && uuid(*credentialId) && deviceUuid &&
      *deviceUuid == csr.binding.device_uuid && deviceId &&
      *deviceId == csr.binding.device_id && certificate && chain &&
      beforeEpoch && afterEpoch && *afterEpoch > *beforeEpoch &&
      certificate->find("PRIVATE KEY") == std::string::npos;
  wipe(*response);
  if (!valid) return std::nullopt;
  return PublicCredentialBundle{
      std::move(*credentialId), std::move(*certificate), std::move(*chain),
      CertificateIdentity{csr.binding.device_id,
                          {"urn:algaguard:device:" + csr.binding.device_uuid}},
      *beforeEpoch, *afterEpoch, false, false};
}

}  // namespace algaguard

#endif
