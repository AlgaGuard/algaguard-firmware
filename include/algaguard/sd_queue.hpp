#pragma once

#include <cstdint>
#include <optional>

#include "algaguard/sd_queue_record.hpp"
#include "driver/gpio.h"
#include "driver/spi_common.h"

namespace algaguard {

#if !defined(ALGAGUARD_QUEUE_MAX_SAMPLES)
#define ALGAGUARD_QUEUE_MAX_SAMPLES 1440
#endif

// Offline buffer/replay queue on a microSD card, mounted FATFS-over-SPI.
// Used only while the device can't reach the cloud (no Wi-Fi, or Wi-Fi but
// no MQTT session) -- samples are appended here instead of published live,
// then drained/replayed once connectivity returns. See sd_queue_record.hpp
// for the durable record format this is built on.
class SdQueue {
 public:
  esp_err_t configure(gpio_num_t mosi, gpio_num_t miso, gpio_num_t sck,
                      gpio_num_t cs);
  bool mounted() const { return mounted_; }

  // Appends one sample. If the queue is already at its cap
  // (ALGAGUARD_QUEUE_MAX_SAMPLES unread records), the single oldest unread
  // record is dropped first to make room -- recent-trend continuity is
  // prioritized over the earliest part of a long outage.
  bool append(const SdQueueRecord& record);

  // Non-destructive look at the oldest not-yet-replayed record, if any.
  std::optional<SdQueueRecord> peekOldestUnread();

  // Call only after that record has been both published *and*
  // acknowledged -- advances the durable read cursor past it. A crash
  // between publish and this call means that one record is replayed again
  // on the next drain; the backend already de-duplicates on messageId.
  bool acknowledgeOldestUnread();

  bool empty() { return !peekOldestUnread().has_value(); }

 private:
  bool compactIfNeeded();

  bool mounted_{};
  std::uint64_t cursor_{};
};

}  // namespace algaguard
