#include <cstring>
#include <unity.h>
#include "algaguard/identity_oled_renderer.hpp"
using namespace algaguard::host_test;void setUp(){}void tearDown(){} IdentityOledRenderer R;
void test_114_unprovisioned_main(){auto x=R.render(identityStatusView(DevelopmentIdentityState::UNPROVISIONED),IdentityScreenMode::MAIN_STATUS);TEST_ASSERT_EQUAL(2,x.count);TEST_ASSERT_TRUE(x.lines[0].text.size()<=21);}
void test_115_active_main_warning(){auto x=R.render(identityStatusView(DevelopmentIdentityState::DEV_IDENTITY_ACTIVE),IdentityScreenMode::MAIN_STATUS);TEST_ASSERT_TRUE(x.warningVisible);TEST_ASSERT_EQUAL_STRING("INSECURE DEV KEY",x.lines[2].text.c_str());}
void test_116_active_diag(){auto x=R.render(identityStatusView(DevelopmentIdentityState::DEV_IDENTITY_ACTIVE,2,"abc","OK"),IdentityScreenMode::SECURITY_DIAGNOSTICS);TEST_ASSERT_EQUAL(4,x.count);TEST_ASSERT_TRUE(x.warningVisible);}
void test_117_terminal(){for(auto s:{DevelopmentIdentityState::DEV_IDENTITY_EXPIRED,DevelopmentIdentityState::DEV_IDENTITY_CORRUPT,DevelopmentIdentityState::DEV_IDENTITY_RESET_REQUIRED})TEST_ASSERT_EQUAL(2,R.render(identityStatusView(s),IdentityScreenMode::MAIN_STATUS).count);}
void test_118_clip(){auto v=identityStatusView(DevelopmentIdentityState::DEV_CSR_READY,1,"abcdefghijklmnop","OK");auto x=R.render(v,IdentityScreenMode::SECURITY_DIAGNOSTICS);TEST_ASSERT_TRUE(x.truncated);TEST_ASSERT_TRUE(x.lines[3].text.size()<=21);}
void test_119_secret(){IdentityStatusViewModel v;v.primaryText="sessionToken";v.secondaryText="ACTIVE";auto x=R.render(v,IdentityScreenMode::MAIN_STATUS);TEST_ASSERT_EQUAL_STRING("[REDACTED]",x.lines[0].text.c_str());}
int main(int,char**){UNITY_BEGIN();RUN_TEST(test_114_unprovisioned_main);RUN_TEST(test_115_active_main_warning);RUN_TEST(test_116_active_diag);RUN_TEST(test_117_terminal);RUN_TEST(test_118_clip);RUN_TEST(test_119_secret);return UNITY_END();}
