#include <unity.h>

#include "algaguard/display.hpp"
#include "algaguard/display_fx.hpp"

void setUp() {}
void tearDown() {}

namespace {
unsigned popcount(const algaguard::Framebuffer& framebuffer) {
  unsigned count = 0;
  for (const auto byte : framebuffer)
    for (std::uint8_t bit = 0; bit < 8; ++bit)
      count += (byte >> bit) & 1U;
  return count;
}
}  // namespace

void test_compose_screen_draws_border_and_divider() {
  const algaguard::DiagnosticScreen screen{{{"ALGAGUARD", "LINE ONE", "LINE TWO", "LINE THREE"}}};
  const auto framebuffer = algaguard::compose_screen(screen);
  TEST_ASSERT_TRUE(framebuffer[0] & (1U << 0));    // top-left border pixel (x=0,y=0)
  TEST_ASSERT_TRUE(framebuffer[7 * 128 + 127] & (1U << 7));  // bottom-right border pixel (x=127,y=63)
  bool dividerFound = false;
  for (std::uint8_t x = 3; x < 125; ++x)
    if (framebuffer[(16U / 8U) * 128U + x] & (1U << (16U % 8U))) dividerFound = true;
  TEST_ASSERT_TRUE(dividerFound);
  TEST_ASSERT_TRUE(popcount(framebuffer) > 0);
}

void test_compose_screen_is_a_pure_function_of_its_input() {
  const algaguard::DiagnosticScreen screen{{{"A", "B", "C", "D"}}};
  const auto first = algaguard::compose_screen(screen);
  const auto second = algaguard::compose_screen(screen);
  TEST_ASSERT_EQUAL_MEMORY(first.data(), second.data(), first.size());
}

void test_wipe_transition_endpoints_match_inputs_exactly() {
  algaguard::Framebuffer from{};
  algaguard::Framebuffer to{};
  from.fill(0xAA);
  to.fill(0x55);
  const auto atStart = algaguard::wipe_transition(from, to, 0, 4);
  TEST_ASSERT_EQUAL_MEMORY(from.data(), atStart.data(), from.size());
  const auto atEnd = algaguard::wipe_transition(from, to, 4, 4);
  TEST_ASSERT_EQUAL_MEMORY(to.data(), atEnd.data(), to.size());
  const auto pastEnd = algaguard::wipe_transition(from, to, 9, 4);
  TEST_ASSERT_EQUAL_MEMORY(to.data(), pastEnd.data(), to.size());
  const auto zeroSteps = algaguard::wipe_transition(from, to, 0, 0);
  TEST_ASSERT_EQUAL_MEMORY(to.data(), zeroSteps.data(), to.size());
}

void test_wipe_transition_sweeps_left_to_right() {
  algaguard::Framebuffer from{};
  algaguard::Framebuffer to{};
  from.fill(0x00);
  to.fill(0xFF);
  const auto midway = algaguard::wipe_transition(from, to, 2, 4);
  // Threshold at step 2/4 is column 64: left half should read as `to`, right half as `from`.
  TEST_ASSERT_EQUAL_UINT8(0xFF, midway[0]);    // page 0, column 0
  TEST_ASSERT_EQUAL_UINT8(0xFF, midway[63]);   // page 0, column 63
  TEST_ASSERT_EQUAL_UINT8(0x00, midway[64]);   // page 0, column 64
  TEST_ASSERT_EQUAL_UINT8(0x00, midway[127]);  // page 0, column 127
}

void test_splash_frame_reveals_progressively_and_terminates_full() {
  const auto blank = algaguard::splash_frame(0, 8);
  TEST_ASSERT_EQUAL_UINT(0, popcount(blank));
  const auto early = algaguard::splash_frame(2, 8);
  const auto late = algaguard::splash_frame(6, 8);
  const auto full = algaguard::splash_frame(8, 8);
  TEST_ASSERT_TRUE(popcount(early) < popcount(late));
  TEST_ASSERT_TRUE(popcount(late) <= popcount(full));
  TEST_ASSERT_TRUE(popcount(full) > 0);
  // Guard against div-by-zero total_steps: should not crash and should equal the full frame.
  const auto guarded = algaguard::splash_frame(0, 0);
  TEST_ASSERT_TRUE(guarded.size() == full.size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_compose_screen_draws_border_and_divider);
  RUN_TEST(test_compose_screen_is_a_pure_function_of_its_input);
  RUN_TEST(test_wipe_transition_endpoints_match_inputs_exactly);
  RUN_TEST(test_wipe_transition_sweeps_left_to_right);
  RUN_TEST(test_splash_frame_reveals_progressively_and_terminates_full);
  return UNITY_END();
}
