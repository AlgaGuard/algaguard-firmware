#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>

namespace algaguard {

// Fixed-size, packed binary record for the SD offline-buffer queue -- one
// record per buffered telemetry sample. Fixed size (rather than a
// line-delimited text format) is deliberate: it makes a torn write from a
// power loss trivially detectable (any partial trailing record fails the
// magic/CRC check and gets truncated away) without needing to reason about
// where a line boundary was.
#pragma pack(push, 1)
struct SdQueueRecord {
  std::uint32_t magic{kMagic};
  std::uint16_t recordVersion{1};
  std::uint16_t reserved{};
  std::uint64_t sequence{};
  std::int64_t observedAtUnix{};
  double temperatureC{};
  double ph{};
  double lightLux{};
  double nutrientPercent{};
  std::uint32_t crc32{};

  static constexpr std::uint32_t kMagic = 0x41475351U;  // "AGSQ"
};
#pragma pack(pop)

inline std::uint32_t crc32_ieee(const std::uint8_t* data, std::size_t length) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = -(crc & 1U);
      crc = (crc >> 1) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}

using SdQueueRecordBytes = std::array<std::uint8_t, sizeof(SdQueueRecord)>;

inline SdQueueRecordBytes encode(const SdQueueRecord& record) {
  SdQueueRecord withCrc = record;
  withCrc.magic = SdQueueRecord::kMagic;
  withCrc.crc32 = 0;
  withCrc.crc32 = crc32_ieee(
      reinterpret_cast<const std::uint8_t*>(&withCrc),
      sizeof(SdQueueRecord) - sizeof(SdQueueRecord::crc32));
  SdQueueRecordBytes bytes{};
  std::memcpy(bytes.data(), &withCrc, sizeof(SdQueueRecord));
  return bytes;
}

inline std::optional<SdQueueRecord> decode(const std::uint8_t* buffer,
                                           std::size_t length) {
  if (length != sizeof(SdQueueRecord)) return std::nullopt;
  SdQueueRecord record{};
  std::memcpy(&record, buffer, sizeof(SdQueueRecord));
  if (record.magic != SdQueueRecord::kMagic) return std::nullopt;
  const std::uint32_t expectedCrc = crc32_ieee(
      buffer, sizeof(SdQueueRecord) - sizeof(SdQueueRecord::crc32));
  if (expectedCrc != record.crc32) return std::nullopt;
  return record;
}

}  // namespace algaguard
