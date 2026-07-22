#include <unity.h>
#include "algaguard/domain.hpp"

void setUp() {}
void tearDown() {}

void test_deterministic_simulated_data() {
  algaguard::DeterministicSimulator one{42};
  algaguard::DeterministicSimulator two{42};
  const auto first = one.next(1, 1000);
  const auto second = two.next(1, 1000);
  TEST_ASSERT_EQUAL_FLOAT(static_cast<float>(first.ph), static_cast<float>(second.ph));
  TEST_ASSERT_TRUE(first.simulated);
  TEST_ASSERT_EQUAL_STRING("1", first.sequence.c_str());
}

void test_queue_retains_until_application_ack() {
  algaguard::LocalQueue<int> queue{2};
  TEST_ASSERT_TRUE(queue.push(1));
  TEST_ASSERT_EQUAL_UINT32(1, queue.size());
  queue.acknowledge();
  TEST_ASSERT_EQUAL_UINT32(0, queue.size());
}

void test_button_debounce_and_menu() {
  algaguard::DebouncedButton button{35};
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::ButtonEvent::kNone), static_cast<int>(button.update(true, 1)));
  button.update(true, 40);
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::ButtonEvent::kNone), static_cast<int>(button.update(false, 80)));
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::ButtonEvent::kShortPress), static_cast<int>(button.update(false, 120)));
  algaguard::MenuController menu;
  const auto home = menu.page();
  menu.down();
  TEST_ASSERT_NOT_EQUAL(static_cast<int>(home), static_cast<int>(menu.page()));
  menu.back();
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::MenuPage::kHome), static_cast<int>(menu.page()));
}

void test_ota_requires_verified_manifest_and_rolls_back() {
  algaguard::OtaStateMachine ota;
  TEST_ASSERT_FALSE(ota.accept_manifest(true, true, true, false));
  TEST_ASSERT_TRUE(ota.accept_manifest(true, true, true, true));
  ota.downloaded(true, true);
  ota.begin_boot_validation();
  ota.validate_boot(false);
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::OtaState::kRollback), static_cast<int>(ota.state()));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_deterministic_simulated_data);
  RUN_TEST(test_queue_retains_until_application_ack);
  RUN_TEST(test_button_debounce_and_menu);
  RUN_TEST(test_ota_requires_verified_manifest_and_rolls_back);
  return UNITY_END();
}
