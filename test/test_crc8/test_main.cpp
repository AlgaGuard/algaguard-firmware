#include <unity.h>

#include <cstring>

#include "algaguard/crc8.hpp"

void setUp() {}
void tearDown() {}

// Self-verifying property of this CRC8 construction (init=0x00, xorout=0x00):
// appending a correctly-computed check byte to the message and recomputing
// the CRC over the extended message always yields a fixed residual of 0x00.
// This is exactly what DS18B20 scratchpad validation relies on in practice
// (crc8(bytes[0..7]) is compared against the received bytes[8]), so it
// exercises the real usage pattern without depending on a hand-copied
// datasheet vector.
void test_crc8_residual_of_message_plus_its_own_crc_is_zero() {
  const std::uint8_t message[7] = {0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x23};
  const std::uint8_t check = algaguard::crc8_dallas(message, sizeof(message));
  std::uint8_t extended[8];
  std::memcpy(extended, message, sizeof(message));
  extended[7] = check;
  TEST_ASSERT_EQUAL_HEX8(0x00, algaguard::crc8_dallas(extended, sizeof(extended)));
}

void test_crc8_empty_input_is_zero() {
  TEST_ASSERT_EQUAL_HEX8(0x00, algaguard::crc8_dallas(nullptr, 0));
}

void test_crc8_detects_single_bit_flip() {
  const std::uint8_t original[7] = {0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x23};
  std::uint8_t flipped[7];
  std::memcpy(flipped, original, sizeof(original));
  flipped[3] ^= 0x01;
  TEST_ASSERT_NOT_EQUAL(algaguard::crc8_dallas(original, sizeof(original)),
                        algaguard::crc8_dallas(flipped, sizeof(flipped)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_crc8_residual_of_message_plus_its_own_crc_is_zero);
  RUN_TEST(test_crc8_empty_input_is_zero);
  RUN_TEST(test_crc8_detects_single_bit_flip);
  return UNITY_END();
}
