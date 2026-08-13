#include <unity.h>

#include "algaguard/sd_queue_record.hpp"

namespace {
void assertNear(double expected, double actual) {
  TEST_ASSERT_FLOAT_WITHIN(0.0001F, static_cast<float>(expected),
                           static_cast<float>(actual));
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_encode_decode_round_trips() {
  algaguard::SdQueueRecord record{};
  record.sequence = 42;
  record.observedAtUnix = 1754467200;
  record.temperatureC = 24.5;
  record.ph = 7.2;
  record.lightLux = 850.0;
  record.nutrientPercent = 63.4;

  const auto bytes = algaguard::encode(record);
  const auto decoded = algaguard::decode(bytes.data(), bytes.size());
  TEST_ASSERT_TRUE(decoded.has_value());
  TEST_ASSERT_EQUAL_UINT64(record.sequence, decoded->sequence);
  TEST_ASSERT_EQUAL_INT64(record.observedAtUnix, decoded->observedAtUnix);
  assertNear(record.temperatureC, decoded->temperatureC);
  assertNear(record.ph, decoded->ph);
  assertNear(record.lightLux, decoded->lightLux);
  assertNear(record.nutrientPercent, decoded->nutrientPercent);
}

void test_decode_rejects_single_bit_corruption() {
  algaguard::SdQueueRecord record{};
  record.sequence = 1;
  auto bytes = algaguard::encode(record);
  bytes[10] ^= 0x01;  // flip a bit inside the payload, away from the CRC field
  TEST_ASSERT_FALSE(algaguard::decode(bytes.data(), bytes.size()).has_value());
}

void test_decode_rejects_truncated_record() {
  // Simulates a power-loss mid-write: only part of the record made it to
  // disk. Must be rejected, not misread as a short/different record.
  algaguard::SdQueueRecord record{};
  auto bytes = algaguard::encode(record);
  TEST_ASSERT_FALSE(algaguard::decode(bytes.data(), bytes.size() / 2).has_value());
}

void test_decode_rejects_wrong_magic() {
  algaguard::SdQueueRecord record{};
  auto bytes = algaguard::encode(record);
  bytes[0] ^= 0xFF;  // corrupt the leading magic byte
  TEST_ASSERT_FALSE(algaguard::decode(bytes.data(), bytes.size()).has_value());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_encode_decode_round_trips);
  RUN_TEST(test_decode_rejects_single_bit_corruption);
  RUN_TEST(test_decode_rejects_truncated_record);
  RUN_TEST(test_decode_rejects_wrong_magic);
  return UNITY_END();
}
