#include <unity.h>

#include "algaguard/local_demo.hpp"

void setUp() {}
void tearDown() {}

void test_local_demo_guard_model() {
  TEST_ASSERT_TRUE(algaguard::local_demo_profile_allowed(true, false, false, true));
  TEST_ASSERT_FALSE(algaguard::local_demo_profile_allowed(false, false, false, true));
  TEST_ASSERT_FALSE(algaguard::local_demo_profile_allowed(true, true, false, true));
  TEST_ASSERT_TRUE(algaguard::local_demo_never_uses_network());
  TEST_ASSERT_FALSE(algaguard::local_demo_persists_samples());
}

void test_local_demo_generator_is_deterministic_and_bounded() {
  algaguard::LocalDemoGenerator first{7};
  algaguard::LocalDemoGenerator second{7};
  for (std::uint64_t sequence = 1; sequence <= 1000; ++sequence) {
    const auto a = first.next(sequence);
    const auto b = second.next(sequence);
    TEST_ASSERT_FLOAT_WITHIN(0.0001F, static_cast<float>(a.temperatureC),
                             static_cast<float>(b.temperatureC));
    TEST_ASSERT_TRUE(a.ph >= 0 && a.ph <= 14);
    TEST_ASSERT_TRUE(a.lightLux >= 0);
    TEST_ASSERT_TRUE(a.nitrateMgL >= 0);
    TEST_ASSERT_TRUE(a.phosphateMgL >= 0);
    TEST_ASSERT_TRUE(a.potassiumMgL >= 0);
  }
}

void test_local_demo_menu_wraps_and_home_is_safe() {
  algaguard::LocalDemoMenu menu;
  TEST_ASSERT_EQUAL(0, menu.index());
  menu.previous();
  TEST_ASSERT_EQUAL(6, menu.index());
  menu.next();
  TEST_ASSERT_EQUAL(0, menu.index());
  TEST_ASSERT_EQUAL(algaguard::LocalDemoAction::kNone, menu.select());
  TEST_ASSERT_EQUAL(1, menu.index());
  menu.home();
  TEST_ASSERT_EQUAL(0, menu.index());
}

void test_local_demo_all_pages_are_safe_and_explicit() {
  const auto reading = algaguard::LocalDemoGenerator{}.next(1);
  algaguard::LocalDemoMenu menu;
  bool sawLocal = false;
  bool sawOffline = false;
  bool sawNotConfigured = false;
  for (unsigned page = 0; page < 7; ++page) {
    const auto screen = algaguard::local_demo_screen(menu.page(), reading, true);
    TEST_ASSERT_TRUE(screen.safe());
    for (const auto& line : screen.lines) {
      sawLocal = sawLocal || line.find("DEV GENERATED") != std::string::npos;
      sawOffline = sawOffline || line.find("CLOUD NOT READY") != std::string::npos;
      sawNotConfigured = sawNotConfigured || line.find("WIFI SETUP REQUIRED") != std::string::npos;
      TEST_ASSERT_EQUAL(std::string::npos, line.find("password"));
      TEST_ASSERT_EQUAL(std::string::npos, line.find("session"));
    }
    menu.next();
  }
  TEST_ASSERT_TRUE(sawLocal);
  TEST_ASSERT_TRUE(sawOffline);
  TEST_ASSERT_TRUE(sawNotConfigured);
}

void test_local_demo_network_forget_requires_confirmation() {
  algaguard::LocalDemoMenu menu;
  for (unsigned page = 0; page < 5; ++page) menu.next();
  TEST_ASSERT_EQUAL(algaguard::LocalDemoPage::kNetwork, menu.page());
  TEST_ASSERT_EQUAL(algaguard::LocalDemoAction::kNone, menu.select());
  TEST_ASSERT_EQUAL(algaguard::LocalDemoNetworkState::kConfirmForget,
                    menu.networkState());
  TEST_ASSERT_EQUAL(algaguard::LocalDemoAction::kForgetSavedWifi,
                    menu.select());
  menu.setForgetResult(true);
  TEST_ASSERT_EQUAL(algaguard::LocalDemoNetworkState::kForgotten,
                    menu.networkState());
  const auto screen = algaguard::local_demo_screen(
      menu.page(), algaguard::LocalDemoGenerator{}.next(1), true, false,
      false, true, menu.networkState());
  TEST_ASSERT_TRUE(screen.safe());
  TEST_ASSERT_EQUAL(std::string::npos, screen.lines[0].find("password"));
  menu.home();
  TEST_ASSERT_EQUAL(algaguard::LocalDemoPage::kHome, menu.page());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_local_demo_guard_model);
  RUN_TEST(test_local_demo_generator_is_deterministic_and_bounded);
  RUN_TEST(test_local_demo_menu_wraps_and_home_is_safe);
  RUN_TEST(test_local_demo_all_pages_are_safe_and_explicit);
  RUN_TEST(test_local_demo_network_forget_requires_confirmation);
  return UNITY_END();
}
