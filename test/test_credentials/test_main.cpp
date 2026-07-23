#include <unity.h>

#include <cstring>

#include "algaguard/credentials.hpp"

void setUp() {}
void tearDown() {}

namespace {
constexpr char kCertificate[] =
    "-----BEGIN CERTIFICATE-----\nPUBLIC-CERTIFICATE\n-----END CERTIFICATE-----";

algaguard::PublicCredentialBundle bundle() {
  return {"credential-1",
          kCertificate,
          {kCertificate},
          {"AG-000001", {"urn:algaguard:device:10000000-0000-4000-8000-000000000001"}},
          100,
          200,
          false,
          false};
}

class FakeLocalKeyProvider final : public algaguard::LocalPrivateKeyProvider {
 public:
  std::optional<algaguard::PrivateKeyHandle> generate(algaguard::KeyAlgorithm algorithm) override {
    algorithm_ = algorithm;
    return algaguard::PrivateKeyHandle{7, 1};
  }
  std::optional<algaguard::CsrSubmission> create_csr(const algaguard::PrivateKeyHandle& key,
                                                      const algaguard::DeviceBinding& binding) override {
    if (key.slot != 7) return std::nullopt;
    return algaguard::CsrSubmission{"-----BEGIN CERTIFICATE REQUEST-----\nPUBLIC-CSR\n-----END CERTIFICATE REQUEST-----",
                                    binding, algorithm_};
  }
  bool destroy(const algaguard::PrivateKeyHandle& key) override { return key.slot == 7; }

 private:
  algaguard::KeyAlgorithm algorithm_{algaguard::KeyAlgorithm::kEcP256};
};

class FakeTlsLoader final : public algaguard::MqttTlsCredentialLoader {
 public:
  bool load(const algaguard::PrivateKeyHandle& key, const algaguard::PublicCredentialBundle& public_bundle,
            const algaguard::BrokerEndpoint& endpoint) override {
    loaded = key.slot == 7 && public_bundle.credential_id == "credential-1" && endpoint.port == 8883 &&
             endpoint.server_name == "mqtt.localhost";
    return loaded;
  }
  bool loaded{false};
};
}  // namespace

void test_limits_and_public_chain_are_bounded() {
  const algaguard::CredentialLimits limits;
  TEST_ASSERT_TRUE(limits.bounded());
  const auto parsed = algaguard::parse_public_certificate_chain(kCertificate, limits);
  TEST_ASSERT_TRUE(parsed.has_value());
  TEST_ASSERT_EQUAL_UINT32(1, parsed->size());
  TEST_ASSERT_FALSE(algaguard::parse_public_certificate_chain(
                        "-----BEGIN PRIVATE KEY-----\nsecret\n-----END PRIVATE KEY-----", limits)
                        .has_value());
}

void test_certificate_binding_requires_exact_cn_and_single_uuid_san() {
  const algaguard::DeviceBinding expected{"AG-000001", "10000000-0000-4000-8000-000000000001"};
  auto credential = bundle();
  TEST_ASSERT_TRUE(algaguard::exact_certificate_binding(expected, credential.identity));
  credential.identity.common_name = "AG-000002";
  TEST_ASSERT_FALSE(algaguard::exact_certificate_binding(expected, credential.identity));
  credential = bundle();
  credential.identity.san_uris.push_back("urn:algaguard:device:other");
  TEST_ASSERT_FALSE(algaguard::exact_certificate_binding(expected, credential.identity));
}

void test_local_key_handle_creates_public_csr_and_loads_mqtt_without_export() {
  FakeLocalKeyProvider keys;
  const algaguard::DeviceBinding binding{"AG-000001", "10000000-0000-4000-8000-000000000001"};
  const auto handle = keys.generate(algaguard::KeyAlgorithm::kEcP256);
  TEST_ASSERT_TRUE(handle.has_value());
  const auto csr = keys.create_csr(*handle, binding);
  TEST_ASSERT_TRUE(csr.has_value());
  TEST_ASSERT_EQUAL_STRING(binding.device_id.c_str(), csr->binding.device_id.c_str());
  TEST_ASSERT_EQUAL_STRING(binding.device_uuid.c_str(), csr->binding.device_uuid.c_str());
  TEST_ASSERT_NULL(strstr(csr->pem.c_str(), "PRIVATE KEY"));
  FakeTlsLoader loader;
  TEST_ASSERT_TRUE(loader.load(*handle, bundle(), {"localhost", 8883, "mqtt.localhost", 60, 3600}));
  TEST_ASSERT_TRUE(loader.loaded);
}

void test_logs_redact_private_and_public_pem_bodies() {
  TEST_ASSERT_EQUAL_STRING("[REDACTED_PRIVATE_MATERIAL]",
                           algaguard::redact_credential_log("-----BEGIN PRIVATE KEY-----").c_str());
  TEST_ASSERT_EQUAL_STRING("[REDACTED_PEM_BODY]", algaguard::redact_credential_log(kCertificate).c_str());
  TEST_ASSERT_EQUAL_STRING("credential-1", algaguard::redact_credential_log("credential-1").c_str());
}

void test_rotation_proves_new_connection_before_old_revocation() {
  algaguard::CredentialRotationStateMachine rotation{"old"};
  TEST_ASSERT_TRUE(rotation.begin(100, 30));
  TEST_ASSERT_TRUE(rotation.stage("new"));
  TEST_ASSERT_FALSE(rotation.acknowledge("new"));
  TEST_ASSERT_EQUAL_STRING("old", rotation.active_credential_id().c_str());
  TEST_ASSERT_TRUE(rotation.prove_connection("new", 120));
  TEST_ASSERT_TRUE(rotation.acknowledge("new"));
  TEST_ASSERT_EQUAL_STRING("new", rotation.active_credential_id().c_str());
  TEST_ASSERT_TRUE(rotation.old_revocation_required());
}

void test_failed_rotation_recovers_old_and_compromise_clears_it() {
  algaguard::CredentialRotationStateMachine rotation{"old"};
  TEST_ASSERT_TRUE(rotation.begin(100, 30));
  TEST_ASSERT_TRUE(rotation.stage("new"));
  TEST_ASSERT_FALSE(rotation.prove_connection("new", 131));
  rotation.fail_or_timeout();
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::CredentialState::kRecovery), static_cast<int>(rotation.state()));
  TEST_ASSERT_EQUAL_STRING("old", rotation.active_credential_id().c_str());
  TEST_ASSERT_TRUE(rotation.recover_with_working_credential());
  rotation.revoke(true);
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::CredentialState::kCompromised), static_cast<int>(rotation.state()));
  TEST_ASSERT_TRUE(rotation.active_credential_id().empty());
}

void test_expiry_revocation_and_unsynchronized_time_fail_safe() {
  const algaguard::CredentialLimits limits;
  auto credential = bundle();
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::ValidityDecision::kWaitForTime),
                    static_cast<int>(algaguard::credential_validity(
                        credential, algaguard::TimeQuality::kUnsynchronized, 150, 0, limits)));
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::ValidityDecision::kAllow),
                    static_cast<int>(algaguard::credential_validity(
                        credential, algaguard::TimeQuality::kSynchronized, 150, 150, limits)));
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::ValidityDecision::kExpired),
                    static_cast<int>(algaguard::credential_validity(
                        credential, algaguard::TimeQuality::kSynchronized, 200, 190, limits)));
  credential.revoked = true;
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::ValidityDecision::kRevoked),
                    static_cast<int>(algaguard::credential_validity(
                        credential, algaguard::TimeQuality::kSynchronized, 150, 150, limits)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_limits_and_public_chain_are_bounded);
  RUN_TEST(test_certificate_binding_requires_exact_cn_and_single_uuid_san);
  RUN_TEST(test_local_key_handle_creates_public_csr_and_loads_mqtt_without_export);
  RUN_TEST(test_logs_redact_private_and_public_pem_bodies);
  RUN_TEST(test_rotation_proves_new_connection_before_old_revocation);
  RUN_TEST(test_failed_rotation_recovers_old_and_compromise_clears_it);
  RUN_TEST(test_expiry_revocation_and_unsynchronized_time_fail_safe);
  return UNITY_END();
}
