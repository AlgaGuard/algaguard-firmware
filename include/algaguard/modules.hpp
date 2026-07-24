#pragma once
#include <string>
#include "algaguard/domain.hpp"

namespace algaguard {
struct BoardDiagnostics { std::uint32_t flash_bytes{}; std::uint32_t psram_bytes{}; bool expected_n16r8{}; };
struct Display { static constexpr int width = 128; static constexpr int height = 64; };
struct QrRenderer { std::string compact_payload; std::string fallback_code; };
struct LedIndicators { bool red{}; bool green{}; bool blue{}; };
struct BleProvisioning { ProvisioningState state{ProvisioningState::kUnprovisioned}; };
struct WifiManager { bool connected{}; };
struct TimeManager { std::string quality{"UNSYNCED"}; };
struct MqttClient { bool tls_required{true}; int qos{1}; };
struct ProfileCache { std::string profile_id; std::string profile_version; };
struct StatusHealthPublisher { bool retained_online{}; };
struct SecureNvs { bool encrypted_credentials_only{true}; };
struct SdQueueAdapter { bool enabled{false}; };
struct HttpsOtaClient { bool signature_verification_required{true}; bool inactive_partition_only{true}; };
}  // namespace algaguard
