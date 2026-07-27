#include <filesystem>

#include <unity.h>

#include "algaguard/host_crypto_record_adapter.hpp"

using namespace algaguard::host_test;

constexpr char kDeviceId[] = "AG-TEST-0001";
constexpr char kDeviceUuid[] = "11111111-2222-4333-8444-555555555555";

void setUp() {}
void tearDown() {}

void buildFixture(EphemeralHostCryptoFixture& fixture) {
  TEST_ASSERT_TRUE(fixture.generateDeviceKey());
  TEST_ASSERT_TRUE(fixture.generateCsr(kDeviceId, kDeviceUuid));
  TEST_ASSERT_TRUE(fixture.createTestCa());
  TEST_ASSERT_TRUE(fixture.issueClientCertificate(kDeviceId, kDeviceUuid));
}
HostCredentialRecord adapted(EphemeralHostCryptoFixture& fixture) {
  const auto record = adaptFixtureCredential(fixture, kDeviceId, kDeviceUuid);
  TEST_ASSERT_TRUE(record.has_value());
  return *record;
}
Transaction cryptoTransaction(FakeStore& store, FaultInjector& faults) {
  return Transaction(store, faults, validateCryptoBackedHostRecord);
}
void assertActive(Transaction& transaction, const std::string& fingerprint, std::uint32_t generation) {
  LoadStatus status;
  const auto record = transaction.loadActive(&status);
  TEST_ASSERT_EQUAL(static_cast<int>(LoadStatus::READY), static_cast<int>(status));
  TEST_ASSERT_EQUAL(generation, record->generation);
  TEST_ASSERT_EQUAL(static_cast<int>(HostCredentialValidation::VALID), static_cast<int>(validateHostCredentialRecord(*record)));
  const auto active_fingerprint = hostCertificateFingerprint(*record);
  TEST_ASSERT_TRUE(!active_fingerprint.empty());
  if (fingerprint != "safe") TEST_ASSERT_EQUAL_STRING(fingerprint.c_str(), active_fingerprint.c_str());
}

void test_86_fixture_adapts_into_bounded_credential_record() {
  EphemeralHostCryptoFixture fixture; buildFixture(fixture); const auto credential = adapted(fixture);
  TEST_ASSERT_FALSE(credential.record.committed);
  TEST_ASSERT_TRUE(credential.record.key_blob.size() <= kMaximumHostBlobSize);
  TEST_ASSERT_TRUE(credential.record.certificate_blob.size() <= kMaximumHostBlobSize);
  TEST_ASSERT_TRUE(credential.record.ca_chain_blob.size() <= kMaximumHostBlobSize);
  TEST_ASSERT_TRUE(!credential.certificate_fingerprint.empty());
}
void test_87_crypto_backed_record_validation_succeeds() {
  EphemeralHostCryptoFixture fixture; buildFixture(fixture); const auto credential = adapted(fixture);
  TEST_ASSERT_EQUAL(static_cast<int>(HostCredentialValidation::VALID), static_cast<int>(validateHostCredentialRecord(credential.record)));
}
void test_88_initial_enrollment_with_real_fixture_succeeds() {
  EphemeralHostCryptoFixture fixture; buildFixture(fixture); const auto credential = adapted(fixture);
  FakeStore store; FaultInjector faults; auto transaction = cryptoTransaction(store, faults);
  TEST_ASSERT_TRUE(transaction.enrollInitial(credential.record).success);
  TEST_ASSERT_EQUAL(0, static_cast<int>(store.readActivePointer()->slot));
  assertActive(transaction, credential.certificate_fingerprint, 1);
}
void test_89_first_snapshot_reboot_reload_validates() {
  FakeStore store; FaultInjector faults; Snapshot snapshot;
  { EphemeralHostCryptoFixture fixture; buildFixture(fixture); auto transaction = cryptoTransaction(store, faults); TEST_ASSERT_TRUE(transaction.enrollInitial(adapted(fixture).record).success); snapshot = store.snapshot(); TEST_ASSERT_TRUE(fixture.cleanup()); }
  FakeStore restored(snapshot); auto rebooted = cryptoTransaction(restored, faults); assertActive(rebooted, "safe", 1);
}
void test_90_second_snapshot_reboot_reload_validates() {
  FakeStore store; FaultInjector faults; Snapshot first;
  { EphemeralHostCryptoFixture fixture; buildFixture(fixture); auto transaction = cryptoTransaction(store, faults); TEST_ASSERT_TRUE(transaction.enrollInitial(adapted(fixture).record).success); first = store.snapshot(); }
  FakeStore reboot_one(first); auto first_transaction = cryptoTransaction(reboot_one, faults); assertActive(first_transaction, "safe", 1);
  FakeStore reboot_two(reboot_one.snapshot()); auto second_transaction = cryptoTransaction(reboot_two, faults); assertActive(second_transaction, "safe", 1);
}
void test_91_atomic_replacement_with_second_fixture_succeeds() {
  EphemeralHostCryptoFixture first; EphemeralHostCryptoFixture second; buildFixture(first); buildFixture(second);
  const auto old = adapted(first); const auto replacement = adapted(second); FakeStore store; FaultInjector faults; auto transaction = cryptoTransaction(store, faults);
  TEST_ASSERT_TRUE(transaction.enrollInitial(old.record).success);
  TEST_ASSERT_TRUE(transaction.replaceActive(replacement.record).success);
  TEST_ASSERT_EQUAL(1, static_cast<int>(store.readActivePointer()->slot));
  assertActive(transaction, replacement.certificate_fingerprint, 2);
  FakeStore rebooted(store.snapshot()); auto after_reboot = cryptoTransaction(rebooted, faults); assertActive(after_reboot, replacement.certificate_fingerprint, 2);
}
void test_92_failed_replacement_preserves_old_identity() {
  EphemeralHostCryptoFixture first; EphemeralHostCryptoFixture second; buildFixture(first); buildFixture(second);
  const auto old = adapted(first); const auto replacement = adapted(second); FakeStore store; FaultInjector faults; auto transaction = cryptoTransaction(store, faults);
  TEST_ASSERT_TRUE(transaction.enrollInitial(old.record).success); faults.fail_on(FaultPoint::WRITE_CERTIFICATE);
  TEST_ASSERT_FALSE(transaction.replaceActive(replacement.record).success); assertActive(transaction, old.certificate_fingerprint, 1);
}
void test_93_mismatched_key_certificate_is_rejected() {
  EphemeralHostCryptoFixture first; EphemeralHostCryptoFixture second; buildFixture(first); buildFixture(second);
  auto credential = adapted(first); credential.record.certificate_blob = adapted(second).record.certificate_blob;
  TEST_ASSERT_EQUAL(static_cast<int>(HostCredentialValidation::KEY_MISMATCH), static_cast<int>(validateHostCredentialRecord(credential.record)));
}
void test_94_wrong_ca_is_rejected() {
  EphemeralHostCryptoFixture first; EphemeralHostCryptoFixture second; buildFixture(first); buildFixture(second);
  auto credential = adapted(first); credential.record.ca_chain_blob = adapted(second).record.ca_chain_blob;
  TEST_ASSERT_EQUAL(static_cast<int>(HostCredentialValidation::CHAIN_INVALID), static_cast<int>(validateHostCredentialRecord(credential.record)));
}
void test_95_wrong_device_identity_is_rejected() {
  EphemeralHostCryptoFixture fixture; buildFixture(fixture); auto credential = adapted(fixture); credential.record.device_id = "AG-OTHER";
  TEST_ASSERT_EQUAL(static_cast<int>(HostCredentialValidation::IDENTITY_INVALID), static_cast<int>(validateHostCredentialRecord(credential.record)));
}
void test_96_corrupt_and_oversized_credential_blobs_are_rejected() {
  EphemeralHostCryptoFixture fixture; buildFixture(fixture); const auto original = adapted(fixture).record;
  auto corrupt_key = original; corrupt_key.key_blob[0] = 0; TEST_ASSERT_NOT_EQUAL(static_cast<int>(HostCredentialValidation::VALID), static_cast<int>(validateHostCredentialRecord(corrupt_key)));
  auto corrupt_certificate = original; corrupt_certificate.certificate_blob[0] = 0; TEST_ASSERT_NOT_EQUAL(static_cast<int>(HostCredentialValidation::VALID), static_cast<int>(validateHostCredentialRecord(corrupt_certificate)));
  auto corrupt_ca = original; corrupt_ca.ca_chain_blob[0] = 0; TEST_ASSERT_NOT_EQUAL(static_cast<int>(HostCredentialValidation::VALID), static_cast<int>(validateHostCredentialRecord(corrupt_ca)));
  auto oversized = original; oversized.key_blob.resize(kMaximumHostBlobSize + 1); TEST_ASSERT_EQUAL(static_cast<int>(HostCredentialValidation::RECORD_INVALID), static_cast<int>(validateHostCredentialRecord(oversized)));
  oversized = original; oversized.certificate_blob.resize(kMaximumHostBlobSize + 1); TEST_ASSERT_EQUAL(static_cast<int>(HostCredentialValidation::RECORD_INVALID), static_cast<int>(validateHostCredentialRecord(oversized)));
  oversized = original; oversized.ca_chain_blob.resize(kMaximumHostBlobSize + 1); TEST_ASSERT_EQUAL(static_cast<int>(HostCredentialValidation::RECORD_INVALID), static_cast<int>(validateHostCredentialRecord(oversized)));
}
void test_97_teardown_removes_temporary_private_material() {
  EphemeralHostCryptoFixture fixture; buildFixture(fixture); auto credential = adapted(fixture); const auto directory = fixture.temporaryDirectory();
  FakeStore store; TEST_ASSERT_TRUE(store.writeSlot(Slot::SLOT_0, credential.record)); store.eraseBothSlots();
  zeroizeHostCredentialRecord(credential.record);
  TEST_ASSERT_TRUE(credential.record.key_blob.empty()); TEST_ASSERT_TRUE(credential.record.certificate_blob.empty()); TEST_ASSERT_TRUE(credential.record.ca_chain_blob.empty());
  TEST_ASSERT_TRUE(fixture.cleanup()); TEST_ASSERT_FALSE(std::filesystem::exists(directory));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_86_fixture_adapts_into_bounded_credential_record);
  RUN_TEST(test_87_crypto_backed_record_validation_succeeds);
  RUN_TEST(test_88_initial_enrollment_with_real_fixture_succeeds);
  RUN_TEST(test_89_first_snapshot_reboot_reload_validates);
  RUN_TEST(test_90_second_snapshot_reboot_reload_validates);
  RUN_TEST(test_91_atomic_replacement_with_second_fixture_succeeds);
  RUN_TEST(test_92_failed_replacement_preserves_old_identity);
  RUN_TEST(test_93_mismatched_key_certificate_is_rejected);
  RUN_TEST(test_94_wrong_ca_is_rejected);
  RUN_TEST(test_95_wrong_device_identity_is_rejected);
  RUN_TEST(test_96_corrupt_and_oversized_credential_blobs_are_rejected);
  RUN_TEST(test_97_teardown_removes_temporary_private_material);
  return UNITY_END();
}
