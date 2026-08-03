#include <cstring>
#include <unity.h>
#include "algaguard/identity_status_view_model.hpp"
using namespace algaguard::host_test; void setUp(){}void tearDown(){}
void test_108_unprovisioned(){auto v=identityStatusView(DevelopmentIdentityState::UNPROVISIONED);TEST_ASSERT_EQUAL_STRING("UNPROVISIONED",v.secondaryText.c_str());}
void test_109_progress(){TEST_ASSERT_EQUAL_STRING("GENERATING",identityStatusView(DevelopmentIdentityState::DEV_KEY_GENERATING).secondaryText.c_str());TEST_ASSERT_EQUAL_STRING("READY",identityStatusView(DevelopmentIdentityState::DEV_CSR_READY).secondaryText.c_str());TEST_ASSERT_EQUAL_STRING("PENDING",identityStatusView(DevelopmentIdentityState::DEV_CERT_PENDING).secondaryText.c_str());}
void test_110_active(){auto v=identityStatusView(DevelopmentIdentityState::DEV_IDENTITY_ACTIVE);TEST_ASSERT_TRUE(v.warningPersistent);TEST_ASSERT_EQUAL_STRING("INSECURE DEV KEY",v.warningText.c_str());}
void test_111_terminal(){TEST_ASSERT_EQUAL_STRING("EXPIRED",identityStatusView(DevelopmentIdentityState::DEV_IDENTITY_EXPIRED).secondaryText.c_str());TEST_ASSERT_EQUAL_STRING("CORRUPT",identityStatusView(DevelopmentIdentityState::DEV_IDENTITY_CORRUPT).secondaryText.c_str());TEST_ASSERT_EQUAL_STRING("RESET REQUIRED",identityStatusView(DevelopmentIdentityState::DEV_IDENTITY_RESET_REQUIRED).secondaryText.c_str());}
void test_112_safe_diag(){auto v=identityStatusView(DevelopmentIdentityState::DEV_CSR_READY,2,"abcdef","OK");TEST_ASSERT_NOT_NULL(strstr(v.diagnosticsText.c_str(),"gen=2"));}
void test_113_redact_truncate(){TEST_ASSERT_EQUAL_STRING("[REDACTED]",redactSecretLike("sessionToken").c_str());TEST_ASSERT_EQUAL_STRING("abc...",truncateSafe("abcdefgh",6).c_str());}
int main(int,char**){UNITY_BEGIN();RUN_TEST(test_108_unprovisioned);RUN_TEST(test_109_progress);RUN_TEST(test_110_active);RUN_TEST(test_111_terminal);RUN_TEST(test_112_safe_diag);RUN_TEST(test_113_redact_truncate);return UNITY_END();}
