#include <filesystem>

#include <unity.h>

#include "algaguard/host_crypto_fixture.hpp"

using algaguard::host_test::EphemeralHostCryptoFixture;

constexpr char kDeviceId[] = "AG-TEST-0001";
constexpr char kDeviceUuid[] = "11111111-2222-4333-8444-555555555555";

void setUp() {}
void tearDown() {}

void ready(EphemeralHostCryptoFixture& fixture) {
  TEST_ASSERT_TRUE(fixture.generateDeviceKey());
  TEST_ASSERT_TRUE(fixture.generateCsr(kDeviceId, kDeviceUuid));
  TEST_ASSERT_TRUE(fixture.createTestCa());
  TEST_ASSERT_TRUE(fixture.issueClientCertificate(kDeviceId, kDeviceUuid));
}

void test_72_rsa_3072_key_generation_succeeds() {
  EphemeralHostCryptoFixture fixture;
  TEST_ASSERT_TRUE(fixture.generateDeviceKey());
  TEST_ASSERT_TRUE(fixture.cleanup());
}

void test_73_generated_public_key_reports_3072_bits() {
  EphemeralHostCryptoFixture fixture;
  TEST_ASSERT_TRUE(fixture.generateDeviceKey());
  TEST_ASSERT_EQUAL(3072, fixture.metadata().key_bits);
  TEST_ASSERT_EQUAL_STRING("RSA", fixture.metadata().key_type.c_str());
  TEST_ASSERT_TRUE(fixture.metadata().public_fingerprint.size() > 20);
}

void test_74_csr_generation_succeeds() {
  EphemeralHostCryptoFixture fixture;
  TEST_ASSERT_TRUE(fixture.generateDeviceKey());
  TEST_ASSERT_TRUE(fixture.generateCsr(kDeviceId, kDeviceUuid));
}

void test_75_csr_signature_validates() {
  EphemeralHostCryptoFixture fixture;
  TEST_ASSERT_TRUE(fixture.generateDeviceKey());
  TEST_ASSERT_TRUE(fixture.generateCsr(kDeviceId, kDeviceUuid));
  TEST_ASSERT_TRUE(fixture.validateCsrSignature());
}

void test_76_csr_contains_expected_device_id() {
  EphemeralHostCryptoFixture fixture;
  TEST_ASSERT_TRUE(fixture.generateDeviceKey());
  TEST_ASSERT_TRUE(fixture.generateCsr(kDeviceId, kDeviceUuid));
  TEST_ASSERT_TRUE(fixture.validateCsrIdentity(kDeviceId, kDeviceUuid));
}

void test_77_csr_contains_expected_device_uuid() {
  EphemeralHostCryptoFixture fixture;
  TEST_ASSERT_TRUE(fixture.generateDeviceKey());
  TEST_ASSERT_TRUE(fixture.generateCsr(kDeviceId, kDeviceUuid));
  TEST_ASSERT_TRUE(fixture.validateCsrIdentity(kDeviceId, kDeviceUuid));
}

void test_78_test_ca_generation_succeeds() {
  EphemeralHostCryptoFixture fixture;
  TEST_ASSERT_TRUE(fixture.createTestCa());
}

void test_79_matching_client_certificate_validates() {
  EphemeralHostCryptoFixture fixture;
  ready(fixture);
  TEST_ASSERT_TRUE(fixture.validateCertificateChain());
  TEST_ASSERT_TRUE(fixture.validateCertificateValidity());
}

void test_80_key_certificate_match_succeeds() {
  EphemeralHostCryptoFixture fixture;
  ready(fixture);
  TEST_ASSERT_TRUE(fixture.validateKeyCertificateMatch());
}

void test_81_client_auth_and_digital_signature_usages_validate() {
  EphemeralHostCryptoFixture fixture;
  ready(fixture);
  TEST_ASSERT_TRUE(fixture.validateClientAuthUsage());
}

void test_82_wrong_device_identity_is_rejected() {
  EphemeralHostCryptoFixture fixture;
  ready(fixture);
  TEST_ASSERT_FALSE(fixture.validateCertificateIdentity("AG-TEST-OTHER", kDeviceUuid));
}

void test_83_wrong_key_is_rejected() {
  EphemeralHostCryptoFixture fixture;
  ready(fixture);
  TEST_ASSERT_TRUE(fixture.validateWrongKeyRejected());
}

void test_84_wrong_ca_and_expired_certificate_are_rejected() {
  EphemeralHostCryptoFixture fixture;
  ready(fixture);
  TEST_ASSERT_TRUE(fixture.validateWrongCaRejected());
  TEST_ASSERT_TRUE(fixture.validateExpiredCertificateRejected());
}

void test_85_temporary_fixture_cleanup_removes_private_material() {
  EphemeralHostCryptoFixture fixture;
  TEST_ASSERT_TRUE(fixture.generateDeviceKey());
  const auto directory = fixture.temporaryDirectory();
  TEST_ASSERT_TRUE(std::filesystem::exists(directory));
  TEST_ASSERT_TRUE(fixture.cleanup());
  TEST_ASSERT_FALSE(std::filesystem::exists(directory));
  TEST_ASSERT_TRUE(fixture.metadata().cleanup_succeeded);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_72_rsa_3072_key_generation_succeeds);
  RUN_TEST(test_73_generated_public_key_reports_3072_bits);
  RUN_TEST(test_74_csr_generation_succeeds);
  RUN_TEST(test_75_csr_signature_validates);
  RUN_TEST(test_76_csr_contains_expected_device_id);
  RUN_TEST(test_77_csr_contains_expected_device_uuid);
  RUN_TEST(test_78_test_ca_generation_succeeds);
  RUN_TEST(test_79_matching_client_certificate_validates);
  RUN_TEST(test_80_key_certificate_match_succeeds);
  RUN_TEST(test_81_client_auth_and_digital_signature_usages_validate);
  RUN_TEST(test_82_wrong_device_identity_is_rejected);
  RUN_TEST(test_83_wrong_key_is_rejected);
  RUN_TEST(test_84_wrong_ca_and_expired_certificate_are_rejected);
  RUN_TEST(test_85_temporary_fixture_cleanup_removes_private_material);
  return UNITY_END();
}
