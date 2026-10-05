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
    TEST_ASSERT_TRUE(a.nutrientPercent >= 0 && a.nutrientPercent <= 100);
  }
}

// Page selection/cycling now lives in main.cpp's MainMenuNav (the real
// on-screen main menu) rather than in this header -- LocalDemoPage here is
// just a content-page identifier passed straight into local_demo_screen().
void test_local_demo_all_pages_are_safe_and_explicit() {
  const auto reading = algaguard::LocalDemoGenerator{}.next(1);
  constexpr std::array<algaguard::LocalDemoPage, 7> pages{{
      algaguard::LocalDemoPage::kTemperaturePh,
      algaguard::LocalDemoPage::kLight,
      algaguard::LocalDemoPage::kNutrients,
      algaguard::LocalDemoPage::kNpkEstimates,
      algaguard::LocalDemoPage::kDeviceStatus,
      algaguard::LocalDemoPage::kNetwork,
      algaguard::LocalDemoPage::kAbout,
  }};
  bool sawLocal = false;
  bool sawOffline = false;
  bool sawNotConfigured = false;
  for (const auto page : pages) {
    const auto screen = algaguard::local_demo_screen(page, reading, true);
    TEST_ASSERT_TRUE(screen.safe());
    for (const auto& line : screen.lines) {
      sawLocal = sawLocal || line.find("DEV GENERATED") != std::string::npos;
      sawOffline = sawOffline || line.find("CLOUD NOT READY") != std::string::npos;
      sawNotConfigured = sawNotConfigured || line.find("WIFI SETUP REQUIRED") != std::string::npos;
      TEST_ASSERT_EQUAL(std::string::npos, line.find("password"));
      TEST_ASSERT_EQUAL(std::string::npos, line.find("session"));
    }
  }
  TEST_ASSERT_TRUE(sawLocal);
  TEST_ASSERT_TRUE(sawOffline);
  TEST_ASSERT_TRUE(sawNotConfigured);
}

void test_npk_page_shows_estimates_and_marks_p_and_k_experimental() {
  algaguard::LocalDemoReading reading{1, 24.2, 7.1, 900.0, 63.4};
  const auto npk = algaguard::estimate_npk(reading.ph, reading.temperatureC);
  const auto screen = algaguard::local_demo_screen(
      algaguard::LocalDemoPage::kNpkEstimates, reading, true);
  TEST_ASSERT_TRUE(screen.safe());
  TEST_ASSERT_EQUAL_STRING("NPK EST", screen.lines[0].c_str());
  TEST_ASSERT_EQUAL_STRING(
      algaguard::npk_estimate_line("N", npk.nitrateMgL, false).c_str(),
      screen.lines[1].c_str());
  TEST_ASSERT_EQUAL(std::string::npos, screen.lines[1].find("EXPERIMENTAL"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, screen.lines[2].find("EXPERIMENTAL"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, screen.lines[3].find("EXPERIMENTAL"));
  // Every line fits the 30 characters the OLED font can draw across.
  for (const auto& line : screen.lines) TEST_ASSERT_TRUE(line.size() <= 30);

  reading.ph = 6.0;  // below the model's training range
  const auto outOfRange = algaguard::local_demo_screen(
      algaguard::LocalDemoPage::kNpkEstimates, reading, true);
  TEST_ASSERT_EQUAL_STRING("N OUT OF RANGE", outOfRange.lines[1].c_str());
  TEST_ASSERT_EQUAL_STRING("P OUT OF RANGE", outOfRange.lines[2].c_str());
}

void test_local_demo_network_forget_requires_confirmation() {
  algaguard::LocalDemoNetworkFlow flow;
  TEST_ASSERT_EQUAL(algaguard::LocalDemoNetworkState::kReady, flow.networkState());
  TEST_ASSERT_EQUAL(algaguard::LocalDemoAction::kNone, flow.select());
  TEST_ASSERT_EQUAL(algaguard::LocalDemoNetworkState::kConfirmForget,
                    flow.networkState());
  TEST_ASSERT_EQUAL(algaguard::LocalDemoAction::kForgetSavedWifi,
                    flow.select());
  flow.setForgetResult(true);
  TEST_ASSERT_EQUAL(algaguard::LocalDemoNetworkState::kForgotten,
                    flow.networkState());
  const auto screen = algaguard::local_demo_screen(
      algaguard::LocalDemoPage::kNetwork, algaguard::LocalDemoGenerator{}.next(1),
      true, false, false, true, flow.networkState());
  TEST_ASSERT_TRUE(screen.safe());
  TEST_ASSERT_EQUAL(std::string::npos, screen.lines[0].find("password"));
  flow.reset();
  TEST_ASSERT_EQUAL(algaguard::LocalDemoNetworkState::kReady, flow.networkState());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_local_demo_guard_model);
  RUN_TEST(test_local_demo_generator_is_deterministic_and_bounded);
  RUN_TEST(test_local_demo_all_pages_are_safe_and_explicit);
  RUN_TEST(test_npk_page_shows_estimates_and_marks_p_and_k_experimental);
  RUN_TEST(test_local_demo_network_forget_requires_confirmation);
  return UNITY_END();
}
