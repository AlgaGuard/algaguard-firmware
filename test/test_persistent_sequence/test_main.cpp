#include <cstdint>
#include <optional>
#include <unity.h>

#include "algaguard/persistent_sequence.hpp"

namespace {
using algaguard::PersistentSequence;

void test_first_boot_starts_above_old_firmware_numbers() {
  PersistentSequence sequence;
  const auto reserved = sequence.begin(std::nullopt);
  TEST_ASSERT_EQUAL_UINT64(PersistentSequence::kFirstSequence +
                               PersistentSequence::kBlockSize - 1,
                           reserved);
  std::optional<std::uint64_t> persist;
  TEST_ASSERT_EQUAL_UINT64(PersistentSequence::kFirstSequence, sequence.next(persist));
  TEST_ASSERT_FALSE(persist.has_value());
}

void test_reboot_never_repeats_a_number() {
  PersistentSequence first;
  const auto stored = first.begin(std::nullopt);
  std::optional<std::uint64_t> persist;
  std::uint64_t last = 0;
  for (int index = 0; index < 10; ++index) last = first.next(persist);

  PersistentSequence afterReboot;
  afterReboot.begin(stored);
  TEST_ASSERT_TRUE(afterReboot.next(persist) > last);
}

void test_flash_is_written_once_per_block() {
  PersistentSequence sequence;
  sequence.begin(std::nullopt);
  std::optional<std::uint64_t> persist;
  int writes = 0;
  std::uint64_t last = 0;
  for (std::uint64_t index = 0; index < 3 * PersistentSequence::kBlockSize; ++index) {
    const auto value = sequence.next(persist);
    if (index > 0) TEST_ASSERT_EQUAL_UINT64(last + 1, value);
    last = value;
    if (persist) {
      ++writes;
      // A reservation always covers the number just handed out.
      TEST_ASSERT_TRUE(*persist >= value);
    }
  }
  TEST_ASSERT_EQUAL_INT(2, writes);
}
}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_first_boot_starts_above_old_firmware_numbers);
  RUN_TEST(test_reboot_never_repeats_a_number);
  RUN_TEST(test_flash_is_written_once_per_block);
  return UNITY_END();
}
