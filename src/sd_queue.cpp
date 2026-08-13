#include "algaguard/sd_queue.hpp"

#if defined(ESP_PLATFORM)

#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

namespace algaguard {
namespace {
constexpr const char* kMountPoint = "/sdcard";
constexpr const char* kQueuePath = "/sdcard/queue.bin";
constexpr const char* kQueueTempPath = "/sdcard/queue.bin.tmp";
constexpr const char* kCursorPath = "/sdcard/queue.cursor";
constexpr std::uint64_t kCompactionThresholdRecords = 64;
constexpr std::size_t kRecordSize = sizeof(SdQueueRecord);

std::uint64_t readCursorFile() {
  FILE* file = std::fopen(kCursorPath, "rb");
  if (file == nullptr) return 0;
  std::uint64_t value = 0;
  const std::size_t read = std::fread(&value, sizeof(value), 1, file);
  std::fclose(file);
  return read == 1 ? value : 0;
}

bool writeCursorFile(std::uint64_t value) {
  FILE* file = std::fopen(kCursorPath, "wb");
  if (file == nullptr) return false;
  const bool ok = std::fwrite(&value, sizeof(value), 1, file) == 1;
  std::fflush(file);
  if (ok) fsync(fileno(file));
  std::fclose(file);
  return ok;
}

std::uint64_t recordCount() {
  FILE* file = std::fopen(kQueuePath, "rb");
  if (file == nullptr) return 0;
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fclose(file);
  return size > 0 ? static_cast<std::uint64_t>(size) / kRecordSize : 0;
}

}  // namespace

esp_err_t SdQueue::configure(gpio_num_t mosi, gpio_num_t miso, gpio_num_t sck,
                             gpio_num_t cs) {
  spi_bus_config_t busConfig{};
  busConfig.mosi_io_num = mosi;
  busConfig.miso_io_num = miso;
  busConfig.sclk_io_num = sck;
  busConfig.quadwp_io_num = -1;
  busConfig.quadhd_io_num = -1;
  esp_err_t result = spi_bus_initialize(SPI2_HOST, &busConfig, SPI_DMA_CH_AUTO);
  if (result != ESP_OK) return result;

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SPI2_HOST;
  sdspi_device_config_t slotConfig = SDSPI_DEVICE_CONFIG_DEFAULT();
  slotConfig.gpio_cs = cs;
  slotConfig.host_id = SPI2_HOST;

  esp_vfs_fat_sdmmc_mount_config_t mountConfig{};
  mountConfig.format_if_mount_failed = true;
  mountConfig.max_files = 2;

  sdmmc_card_t* card = nullptr;
  result = esp_vfs_fat_sdspi_mount(kMountPoint, &host, &slotConfig,
                                   &mountConfig, &card);
  if (result != ESP_OK) return result;

  mounted_ = true;
  cursor_ = readCursorFile();
  return ESP_OK;
}

bool SdQueue::append(const SdQueueRecord& record) {
  if (!mounted_) return false;
  if (recordCount() - cursor_ >= ALGAGUARD_QUEUE_MAX_SAMPLES) {
    // Drop the single oldest unread record to make room, rather than
    // refusing the new one -- prioritizes recent-trend continuity across a
    // long outage over preserving the earliest part of it.
    ++cursor_;
    if (!writeCursorFile(cursor_)) return false;
  }
  FILE* file = std::fopen(kQueuePath, "ab");
  if (file == nullptr) return false;
  const auto bytes = encode(record);
  const bool ok = std::fwrite(bytes.data(), bytes.size(), 1, file) == 1;
  std::fflush(file);
  if (ok) fsync(fileno(file));
  std::fclose(file);
  return ok;
}

std::optional<SdQueueRecord> SdQueue::peekOldestUnread() {
  if (!mounted_) return std::nullopt;
  FILE* file = std::fopen(kQueuePath, "rb");
  if (file == nullptr) return std::nullopt;
  std::fseek(file, static_cast<long>(cursor_ * kRecordSize), SEEK_SET);
  std::uint8_t buffer[kRecordSize];
  const std::size_t read = std::fread(buffer, 1, kRecordSize, file);
  std::fclose(file);
  if (read != kRecordSize) return std::nullopt;
  auto decoded = decode(buffer, read);
  if (!decoded) {
    // A torn write (power loss mid-append) or genuine corruption at this
    // offset -- truncate the file there and treat the queue as drained up
    // to this point rather than getting stuck on an unreadable record.
    truncate(kQueuePath, static_cast<long>(cursor_ * kRecordSize));
    return std::nullopt;
  }
  return decoded;
}

bool SdQueue::acknowledgeOldestUnread() {
  if (!mounted_) return false;
  ++cursor_;
  if (!writeCursorFile(cursor_)) return false;
  return compactIfNeeded();
}

bool SdQueue::compactIfNeeded() {
  if (cursor_ < kCompactionThresholdRecords) return true;
  FILE* source = std::fopen(kQueuePath, "rb");
  if (source == nullptr) return false;
  std::fseek(source, static_cast<long>(cursor_ * kRecordSize), SEEK_SET);
  FILE* dest = std::fopen(kQueueTempPath, "wb");
  if (dest == nullptr) {
    std::fclose(source);
    return false;
  }
  std::uint8_t buffer[kRecordSize];
  std::size_t read = 0;
  bool ok = true;
  while ((read = std::fread(buffer, 1, kRecordSize, source)) == kRecordSize) {
    if (std::fwrite(buffer, 1, kRecordSize, dest) != kRecordSize) {
      ok = false;
      break;
    }
  }
  std::fflush(dest);
  if (ok) fsync(fileno(dest));
  std::fclose(source);
  std::fclose(dest);
  if (!ok) {
    std::remove(kQueueTempPath);
    return false;
  }
  if (std::rename(kQueueTempPath, kQueuePath) != 0) return false;
  cursor_ = 0;
  return writeCursorFile(cursor_);
}

}  // namespace algaguard

#endif
