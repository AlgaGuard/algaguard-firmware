#include <filesystem>
#include <unity.h>
#include "algaguard/host_tls_identity.hpp"
using namespace algaguard::host_test;
constexpr char I[]="AG-TEST-0001", U[]="11111111-2222-4333-8444-555555555555";
void setUp(){} void tearDown(){}
HostCredentialRecord committed(EphemeralHostCryptoFixture& f){TEST_ASSERT_TRUE(f.generateDeviceKey());TEST_ASSERT_TRUE(f.generateCsr(I,U));TEST_ASSERT_TRUE(f.createTestCa());TEST_ASSERT_TRUE(f.issueClientCertificate(I,U));auto r=*adaptFixtureCredential(f,I,U);r.record.committed=true;return r;}
void test_98_valid_persisted_record_builds_identity(){EphemeralHostCryptoFixture f;auto r=committed(f);TEST_ASSERT_TRUE(buildHostTlsIdentity(r,I,U).has_value());}
void test_99_identity_reports_profile_warning(){EphemeralHostCryptoFixture f;auto r=committed(f);auto x=buildHostTlsIdentity(r,I,U);TEST_ASSERT_EQUAL_STRING("DEV_SOFTWARE_KEY",x->securityProfile().c_str());TEST_ASSERT_EQUAL_STRING("SOFTWARE_PRIVATE_KEY_IN_USE",x->warningCode().c_str());}
void test_100_ds_absent(){EphemeralHostCryptoFixture f;auto r=committed(f);TEST_ASSERT_FALSE(buildHostTlsIdentity(r,I,U)->dsIdentityPresent());}
void test_101_first_reboot(){EphemeralHostCryptoFixture f;auto r=committed(f);FakeStore s;FaultInjector z;Transaction t(s,z,validateCryptoBackedHostRecord);TEST_ASSERT_TRUE(t.enrollInitial(r.record).success);FakeStore q(s.snapshot());auto a=q.readSlot(Slot::SLOT_0);HostCredentialRecord x{*a,r.certificate_fingerprint,r.certificate_serial};TEST_ASSERT_TRUE(buildHostTlsIdentity(x,I,U).has_value());}
void test_102_second_reboot(){test_101_first_reboot();}
void test_103_replacement_metadata(){EphemeralHostCryptoFixture a,b;auto x=committed(a),y=committed(b);FakeStore s;FaultInjector z;Transaction t(s,z,validateCryptoBackedHostRecord);TEST_ASSERT_TRUE(t.enrollInitial(x.record).success);TEST_ASSERT_TRUE(t.replaceActive(y.record).success);HostCredentialRecord r{*s.readSlot(Slot::SLOT_1),y.certificate_fingerprint,y.certificate_serial};TEST_ASSERT_EQUAL_STRING(y.certificate_fingerprint.c_str(),buildHostTlsIdentity(r,I,U)->certificateFingerprint().c_str());}
void test_104_mismatch_fails(){EphemeralHostCryptoFixture a,b;auto x=committed(a),y=committed(b);x.record.certificate_blob=y.record.certificate_blob;TEST_ASSERT_FALSE(buildHostTlsIdentity(x,I,U).has_value());}
void test_105_ca_identity_fail(){EphemeralHostCryptoFixture f;auto r=committed(f);r.record.device_uuid="bad";TEST_ASSERT_FALSE(buildHostTlsIdentity(r,I,U).has_value());}
void test_106_corrupt_oversized_fail(){EphemeralHostCryptoFixture f;auto r=committed(f);r.record.key_blob[0]=0;TEST_ASSERT_FALSE(buildHostTlsIdentity(r,I,U).has_value());r=committed(f);r.record.key_blob.resize(kMaximumHostBlobSize+1);TEST_ASSERT_FALSE(buildHostTlsIdentity(r,I,U).has_value());}
void test_107_teardown_zeroizes(){EphemeralHostCryptoFixture f;auto r=committed(f);auto x=buildHostTlsIdentity(r,I,U);TEST_ASSERT_TRUE(x->privateKeySizeForTest()>0);x->clear();TEST_ASSERT_EQUAL(0,x->privateKeySizeForTest());TEST_ASSERT_TRUE(f.cleanup());}
int main(int,char**){UNITY_BEGIN();RUN_TEST(test_98_valid_persisted_record_builds_identity);RUN_TEST(test_99_identity_reports_profile_warning);RUN_TEST(test_100_ds_absent);RUN_TEST(test_101_first_reboot);RUN_TEST(test_102_second_reboot);RUN_TEST(test_103_replacement_metadata);RUN_TEST(test_104_mismatch_fails);RUN_TEST(test_105_ca_identity_fail);RUN_TEST(test_106_corrupt_oversized_fail);RUN_TEST(test_107_teardown_zeroizes);return UNITY_END();}
