#pragma once

#include <cstdint>
#include <optional>

namespace algaguard {

// Telemetry sequence numbers that keep increasing across reboots. The cloud
// de-duplicates on (device, sequence), so restarting the count at 1 after a
// power cut made every new reading a "duplicate" until the old high-water
// mark was passed -- hours of silently dropped data.
//
// Numbers are reserved in blocks so flash is written once per block, not per
// sample: the stored value is the last number of the current block. After a
// reboot counting resumes at the next block, skipping the unused rest of the
// old one (gaps are harmless; repeats are not).
class PersistentSequence {
 public:
  static constexpr std::uint64_t kBlockSize = 1000;
  // Older firmware restarted at 1 on every boot, so start well above any
  // number it can have sent.
  static constexpr std::uint64_t kFirstSequence = 1'000'000;

  // `stored` is the reservation read from flash (nullopt on first boot).
  // Returns the new reservation, which the caller must persist.
  std::uint64_t begin(std::optional<std::uint64_t> stored) {
    next_ = stored ? *stored + 1 : kFirstSequence;
    reserved_ = next_ + kBlockSize - 1;
    return reserved_;
  }

  // The next sequence number. When it starts a new block, `persist` receives
  // the new reservation, which must be written to flash.
  std::uint64_t next(std::optional<std::uint64_t>& persist) {
    persist.reset();
    if (next_ > reserved_) {
      reserved_ += kBlockSize;
      persist = reserved_;
    }
    return next_++;
  }

 private:
  std::uint64_t next_{kFirstSequence};
  std::uint64_t reserved_{kFirstSequence + kBlockSize - 1};
};

}  // namespace algaguard
