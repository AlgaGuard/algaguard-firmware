#include <unity.h>

#include "algaguard/secure_identity.hpp"

void setUp() {}
void tearDown() {}

namespace {
algaguard::SecureCredentialRecord valid_record() {
  return {{"10000000-0000-4000-8000-000000000001", "credential-1", "abc", 200, 1},
          "-----BEGIN CERTIFICATE-----\nPUBLIC\n-----END CERTIFICATE-----",
          {"-----BEGIN CERTIFICATE-----\nCA\n-----END CERTIFICATE-----"}, {1, 3072, 0, true}};
}
}

void test_profiles_separate_development_software_key_from_deferred_ds_plan() {
  algaguard::EfuseInventory blank{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(algaguard::SecurityStatus::kReady), static_cast<int>(algaguard::security_status_for(blank, algaguard::SecurityProfile::kDevSoftwareKey)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(algaguard::SecurityStatus::kReady), static_cast<int>(algaguard::security_status_for(blank, algaguard::SecurityProfile::kSecurityCi)));
  blank.key_blocks = {algaguard::EfusePurpose::kDsHmacDownstream, algaguard::EfusePurpose::kNvsHmacUpstream, algaguard::EfusePurpose::kFlashEncryption, algaguard::EfusePurpose::kSecureBootDigest, algaguard::EfusePurpose::kReserved, algaguard::EfusePurpose::kReserved};
  TEST_ASSERT_TRUE(algaguard::valid_production_efuse_plan(blank));
}

void test_journal_is_atomic_and_never_accepts_private_material() {
  algaguard::CredentialJournal journal;
  auto record = valid_record();
  TEST_ASSERT_TRUE(journal.stage(record));
  TEST_ASSERT_FALSE(journal.active().has_value());
  TEST_ASSERT_TRUE(journal.commit());
  TEST_ASSERT_EQUAL_UINT32(1, journal.generation());
  record.certificate_pem = "-----BEGIN PRIVATE KEY-----";
  TEST_ASSERT_FALSE(journal.stage(record));
}

void test_opaque_context_requires_rsa_3072_and_ds_key_zero() {
  TEST_ASSERT_TRUE((algaguard::OpaqueDsContext{9, 3072, 0, true}.usable()));
  TEST_ASSERT_FALSE((algaguard::OpaqueDsContext{9, 2048, 0, true}.usable()));
  TEST_ASSERT_FALSE((algaguard::OpaqueDsContext{9, 3072, 1, true}.usable()));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_profiles_separate_development_software_key_from_deferred_ds_plan);
  RUN_TEST(test_journal_is_atomic_and_never_accepts_private_material);
  RUN_TEST(test_opaque_context_requires_rsa_3072_and_ds_key_zero);
  return UNITY_END();
}
