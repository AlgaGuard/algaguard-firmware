#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace algaguard::host_test {

// Host-test policy only: a fourth generation is rejected before any write.
inline constexpr std::uint32_t kMaximumGeneration = 3;
inline constexpr std::size_t kMaximumHostBlobSize = 4096;

enum class Slot : std::uint8_t { SLOT_0, SLOT_1 };
enum class FaultPoint : std::uint8_t {
  BEFORE_INACTIVE_SLOT_INIT,
  WRITE_PRIVATE_KEY,
  WRITE_CERTIFICATE,
  WRITE_CA_CHAIN,
  WRITE_METADATA,
  READBACK,
  READBACK_MISMATCH,
  WRITE_COMMITTED_MARKER,
  AFTER_COMMITTED_MARKER,
  WRITE_ACTIVE_SLOT,
  WRITE_ACTIVE_GENERATION,
  COMPLETE_POINTER_UPDATE,
  CLEAN_OLD_SLOT,
};
enum class LoadStatus : std::uint8_t {
  READY,
  UNPROVISIONED,
  DEVELOPMENT_IDENTITY_CORRUPT,
  AMBIGUOUS_ACTIVE_IDENTITY,
};
enum class SlotValidity : std::uint8_t {
  SLOT_VALID,
  SLOT_UNCOMMITTED,
  SLOT_SCHEMA_UNSUPPORTED,
  SLOT_GENERATION_INVALID,
  SLOT_IDENTITY_INVALID,
  SLOT_RECORD_OVERSIZED,
  SLOT_INTEGRITY_INVALID,
};
enum class ReasonCode : std::uint8_t {
  NONE,
  BEFORE_INACTIVE_SLOT_INIT,
  WRITE_PRIVATE_KEY,
  WRITE_CERTIFICATE,
  WRITE_CA_CHAIN,
  WRITE_METADATA,
  READBACK,
  READBACK_MISMATCH,
  WRITE_COMMITTED_MARKER,
  AFTER_COMMITTED_MARKER,
  WRITE_ACTIVE_SLOT,
  WRITE_ACTIVE_GENERATION,
  COMPLETE_POINTER_UPDATE,
  CLEAN_OLD_SLOT,
  VALIDATION_FAILED,
  GENERATION_OVERFLOW,
};

struct Record {
  std::uint32_t schema_version{1};
  std::uint32_t generation{};
  bool committed{};
  std::string device_id;
  std::string device_uuid;
  std::string integrity_tag;
  std::vector<std::uint8_t> key_blob;
  std::vector<std::uint8_t> certificate_blob;
  std::vector<std::uint8_t> ca_chain_blob;
};
struct Pointer { Slot slot{}; std::uint32_t generation{}; };
struct PointerDraft { std::optional<Slot> slot; std::optional<std::uint32_t> generation; };
struct Snapshot {
  std::array<std::optional<Record>, 2> slots;
  std::optional<Pointer> active;
  PointerDraft draft;
};
struct TransactionResult {
  bool success{};
  ReasonCode reason{ReasonCode::NONE};
  std::string diagnostic;
};

inline const char* reasonName(ReasonCode reason) {
  switch (reason) {
    case ReasonCode::NONE: return "NONE";
    case ReasonCode::BEFORE_INACTIVE_SLOT_INIT: return "BEFORE_INACTIVE_SLOT_INIT";
    case ReasonCode::WRITE_PRIVATE_KEY: return "WRITE_PRIVATE_KEY";
    case ReasonCode::WRITE_CERTIFICATE: return "WRITE_CERTIFICATE";
    case ReasonCode::WRITE_CA_CHAIN: return "WRITE_CA_CHAIN";
    case ReasonCode::WRITE_METADATA: return "WRITE_METADATA";
    case ReasonCode::READBACK: return "READBACK";
    case ReasonCode::READBACK_MISMATCH: return "READBACK_MISMATCH";
    case ReasonCode::WRITE_COMMITTED_MARKER: return "WRITE_COMMITTED_MARKER";
    case ReasonCode::AFTER_COMMITTED_MARKER: return "AFTER_COMMITTED_MARKER";
    case ReasonCode::WRITE_ACTIVE_SLOT: return "WRITE_ACTIVE_SLOT";
    case ReasonCode::WRITE_ACTIVE_GENERATION: return "WRITE_ACTIVE_GENERATION";
    case ReasonCode::COMPLETE_POINTER_UPDATE: return "COMPLETE_POINTER_UPDATE";
    case ReasonCode::CLEAN_OLD_SLOT: return "CLEAN_OLD_SLOT";
    case ReasonCode::VALIDATION_FAILED: return "VALIDATION_FAILED";
    case ReasonCode::GENERATION_OVERFLOW: return "GENERATION_OVERFLOW";
  }
  return "UNKNOWN";
}

inline ReasonCode reasonFor(FaultPoint point) {
  return static_cast<ReasonCode>(static_cast<std::uint8_t>(point) + 1);
}

class Injector {
 public:
  virtual ~Injector() = default;
  virtual bool hit(FaultPoint point) = 0;
};

class FaultInjector final : public Injector {
 public:
  void fail_on(FaultPoint point, std::uint32_t encounter = 1) {
    point_ = point;
    remaining_ = encounter == 0 ? 1 : encounter;
    armed_ = true;
  }
  void reset() { armed_ = false; remaining_ = 0; }
  bool hit(FaultPoint point) override {
    if (!armed_ || point_ != point) return false;
    if (--remaining_ != 0) return false;
    armed_ = false;
    return true;
  }

 private:
  FaultPoint point_{};
  std::uint32_t remaining_{};
  bool armed_{};
};

// This is the production/no-test injection behavior: every stage proceeds.
class NoopInjector final : public Injector {
 public:
  bool hit(FaultPoint) override { return false; }
};

class Store {
 public:
  virtual ~Store() = default;
  virtual std::optional<Record> readSlot(Slot slot) = 0;
  virtual bool writeSlot(Slot slot, Record record) = 0;
  virtual void eraseSlot(Slot slot) = 0;
  virtual std::optional<Pointer> readActivePointer() = 0;
  virtual bool writeActiveSlot(Slot slot) = 0;
  virtual bool writeActiveGeneration(std::uint32_t generation) = 0;
  virtual bool completePointerUpdate() = 0;
  virtual Snapshot snapshot() = 0;
};

class FakeStore final : public Store {
 public:
  FakeStore() = default;
  explicit FakeStore(Snapshot snapshot) : data_(std::move(snapshot)) {}
  std::optional<Record> readSlot(Slot slot) override {
    const auto index = static_cast<int>(slot);
    return index >= 0 && index < static_cast<int>(data_.slots.size()) ? data_.slots[index] : std::nullopt;
  }
  bool writeSlot(Slot slot, Record record) override {
    const auto index = static_cast<int>(slot);
    if (index < 0 || index >= static_cast<int>(data_.slots.size())) return false;
    data_.slots[index] = std::move(record);
    return true;
  }
  void eraseSlot(Slot slot) override {
    const auto index = static_cast<int>(slot);
    if (index >= 0 && index < static_cast<int>(data_.slots.size())) data_.slots[index].reset();
  }
  std::optional<Pointer> readActivePointer() override { return data_.active; }
  bool writeActiveSlot(Slot slot) override { data_.draft.slot = slot; return true; }
  bool writeActiveGeneration(std::uint32_t generation) override { data_.draft.generation = generation; return true; }
  bool completePointerUpdate() override {
    if (!data_.draft.slot || !data_.draft.generation) return false;
    data_.active = Pointer{*data_.draft.slot, *data_.draft.generation};
    data_.draft = {};
    return true;
  }
  Snapshot snapshot() override { return data_; }
  void restore(Snapshot snapshot) { data_ = std::move(snapshot); }
  void corruptActivePointer() { data_.active = Pointer{Slot::SLOT_0, 999}; }
  void corruptActiveSlotPointer() { data_.active = Pointer{static_cast<Slot>(99), 1}; }
  void corruptActiveGeneration() { if (data_.active) data_.active->generation = 999; }
  void clearActivePointer() { data_.active.reset(); }
  void markSlotUncommitted(Slot slot) { mutate(slot, [](Record& r) { r.committed = false; }); }
  void corruptSchemaVersion(Slot slot) { mutate(slot, [](Record& r) { r.schema_version = 99; }); }
  void corruptIntegrityTag(Slot slot) { mutate(slot, [](Record& r) { r.integrity_tag = "CORRUPT"; }); }
  void clearDeviceId(Slot slot) { mutate(slot, [](Record& r) { r.device_id.clear(); }); }
  void clearDeviceUuid(Slot slot) { mutate(slot, [](Record& r) { r.device_uuid.clear(); }); }
  void oversizeKeyBlob(Slot slot) { mutate(slot, [](Record& r) { r.key_blob.assign(kMaximumHostBlobSize + 1, 0x44); }); }
  void oversizeCertificateBlob(Slot slot) { mutate(slot, [](Record& r) { r.certificate_blob.assign(kMaximumHostBlobSize + 1, 0x55); }); }
  void oversizeCaChainBlob(Slot slot) { mutate(slot, [](Record& r) { r.ca_chain_blob.assign(kMaximumHostBlobSize + 1, 0x66); }); }
  void corruptGeneration(Slot slot) { mutate(slot, [](Record& r) { r.generation = 0; }); }
  void corruptCommittedMarker(Slot slot) { markSlotUncommitted(slot); }
  void eraseBothSlots() { eraseSlot(Slot::SLOT_0); eraseSlot(Slot::SLOT_1); }

 private:
  template <typename Mutation>
  void mutate(Slot slot, Mutation mutation) {
    const auto index = static_cast<int>(slot);
    if (index >= 0 && index < static_cast<int>(data_.slots.size()) && data_.slots[index]) mutation(*data_.slots[index]);
  }
  Snapshot data_{};
};

inline SlotValidity slotValidity(const Record& record) {
  if (!record.committed) return SlotValidity::SLOT_UNCOMMITTED;
  if (record.schema_version != 1) return SlotValidity::SLOT_SCHEMA_UNSUPPORTED;
  if (record.generation == 0 || record.generation > kMaximumGeneration) return SlotValidity::SLOT_GENERATION_INVALID;
  if (record.device_id.empty() || record.device_uuid.empty()) return SlotValidity::SLOT_IDENTITY_INVALID;
  if (record.key_blob.empty() || record.certificate_blob.empty() || record.ca_chain_blob.empty() ||
      record.key_blob.size() > kMaximumHostBlobSize || record.certificate_blob.size() > kMaximumHostBlobSize ||
      record.ca_chain_blob.size() > kMaximumHostBlobSize) return SlotValidity::SLOT_RECORD_OVERSIZED;
  return record.integrity_tag == "OK" ? SlotValidity::SLOT_VALID : SlotValidity::SLOT_INTEGRITY_INVALID;
}
inline bool valid(const Record& record) { return slotValidity(record) == SlotValidity::SLOT_VALID; }
using Validator = bool (*)(const Record&);

class Transaction {
 public:
  Transaction(Store& store, Injector& injector, Validator validator = valid)
      : store_(store), injector_(injector), validator_(validator) {}

  // Recovery rule: a valid active pointer wins. Without one, the highest valid
  // committed generation wins; an equal-generation tie fails closed.
  std::optional<Record> loadActive(LoadStatus* status = nullptr) {
    if (const auto pointer = store_.readActivePointer()) {
      const auto record = store_.readSlot(pointer->slot);
      if (record && validator_(*record) && record->generation == pointer->generation) {
        if (status) *status = LoadStatus::READY;
        return record;
      }
    }
    std::optional<Record> best;
    bool invalid_committed = false;
    for (const auto slot : {Slot::SLOT_0, Slot::SLOT_1}) {
      const auto record = store_.readSlot(slot);
      if (!record || !record->committed) continue;
      if (!validator_(*record)) { invalid_committed = true; continue; }
      if (best && best->generation == record->generation) {
        if (status) *status = LoadStatus::AMBIGUOUS_ACTIVE_IDENTITY;
        return std::nullopt;
      }
      if (!best || best->generation < record->generation) best = record;
    }
    if (status) *status = best ? LoadStatus::READY
                                : invalid_committed ? LoadStatus::DEVELOPMENT_IDENTITY_CORRUPT
                                                    : LoadStatus::UNPROVISIONED;
    return best;
  }

  TransactionResult enrollInitial(Record record) { return write(Slot::SLOT_0, std::move(record), std::nullopt); }
  TransactionResult replaceActive(Record record) {
    const auto active = store_.readActivePointer();
    if (!active) return failure(ReasonCode::VALIDATION_FAILED, Slot::SLOT_0, 0, record, false);
    const auto target = active->slot == Slot::SLOT_0 ? Slot::SLOT_1 : Slot::SLOT_0;
    return write(target, std::move(record), active->slot);
  }

 private:
  TransactionResult write(Slot target, Record record, std::optional<Slot> old_slot) {
    const auto active = store_.readActivePointer();
    const auto next_generation = active ? active->generation + 1 : 1;
    if (active && active->generation >= kMaximumGeneration)
      return failure(ReasonCode::GENERATION_OVERFLOW, target, active->generation, record, false);
    if (injector_.hit(FaultPoint::BEFORE_INACTIVE_SLOT_INIT))
      return failure(reasonFor(FaultPoint::BEFORE_INACTIVE_SLOT_INIT), target, next_generation, record, false);

    record.generation = next_generation;
    record.committed = false;
    record.integrity_tag = "PENDING";
    if (!store_.writeSlot(target, record)) return failure(ReasonCode::WRITE_METADATA, target, next_generation, record, false);
    if (injector_.hit(FaultPoint::WRITE_PRIVATE_KEY)) return fault(FaultPoint::WRITE_PRIVATE_KEY, target, record);
    if (record.key_blob.empty()) record.key_blob = {0x11};
    store_.writeSlot(target, record);
    if (injector_.hit(FaultPoint::WRITE_CERTIFICATE)) return fault(FaultPoint::WRITE_CERTIFICATE, target, record);
    if (record.certificate_blob.empty()) record.certificate_blob = {0x22};
    store_.writeSlot(target, record);
    if (injector_.hit(FaultPoint::WRITE_CA_CHAIN)) return fault(FaultPoint::WRITE_CA_CHAIN, target, record);
    if (record.ca_chain_blob.empty()) record.ca_chain_blob = {0x33};
    store_.writeSlot(target, record);
    if (injector_.hit(FaultPoint::WRITE_METADATA)) return fault(FaultPoint::WRITE_METADATA, target, record);
    record.integrity_tag = "OK";
    store_.writeSlot(target, record);

    if (injector_.hit(FaultPoint::READBACK)) return fault(FaultPoint::READBACK, target, record);
    const auto readback = store_.readSlot(target);
    if (injector_.hit(FaultPoint::READBACK_MISMATCH) || !readback || readback->device_id != record.device_id)
      return fault(FaultPoint::READBACK_MISMATCH, target, record);
    if (!validator_(Record{record.schema_version, record.generation, true, record.device_id, record.device_uuid,
                           record.integrity_tag, record.key_blob, record.certificate_blob, record.ca_chain_blob}))
      return failure(ReasonCode::VALIDATION_FAILED, target, record.generation, record, false);

    if (injector_.hit(FaultPoint::WRITE_COMMITTED_MARKER)) return fault(FaultPoint::WRITE_COMMITTED_MARKER, target, record);
    record.committed = true;
    store_.writeSlot(target, record);
    if (injector_.hit(FaultPoint::AFTER_COMMITTED_MARKER)) return fault(FaultPoint::AFTER_COMMITTED_MARKER, target, record);
    if (injector_.hit(FaultPoint::WRITE_ACTIVE_SLOT)) return fault(FaultPoint::WRITE_ACTIVE_SLOT, target, record);
    store_.writeActiveSlot(target);
    if (injector_.hit(FaultPoint::WRITE_ACTIVE_GENERATION)) return fault(FaultPoint::WRITE_ACTIVE_GENERATION, target, record);
    store_.writeActiveGeneration(record.generation);
    if (injector_.hit(FaultPoint::COMPLETE_POINTER_UPDATE)) return fault(FaultPoint::COMPLETE_POINTER_UPDATE, target, record);
    if (!store_.completePointerUpdate()) return failure(ReasonCode::COMPLETE_POINTER_UPDATE, target, record.generation, record, true);
    if (old_slot && injector_.hit(FaultPoint::CLEAN_OLD_SLOT)) return fault(FaultPoint::CLEAN_OLD_SLOT, target, record);
    if (old_slot) store_.eraseSlot(*old_slot);
    return {true, ReasonCode::NONE, diagnostic(ReasonCode::NONE, target, record.generation, record, true)};
  }

  TransactionResult fault(FaultPoint point, Slot slot, const Record& record) {
    return failure(reasonFor(point), slot, record.generation, record, record.committed);
  }
  TransactionResult failure(ReasonCode reason, Slot slot, std::uint32_t generation, const Record& record, bool committed) {
    return {false, reason, diagnostic(reason, slot, generation, record, committed)};
  }
  static std::string diagnostic(ReasonCode reason, Slot slot, std::uint32_t generation,
                                const Record& record, bool committed) {
    return "reason=" + std::string(reasonName(reason)) + " slot=" + std::to_string(static_cast<int>(slot)) +
           " generation=" + std::to_string(generation) + " committed=" + (committed ? "true" : "false") +
           " record_id=" + record.device_id;
  }

  Store& store_;
  Injector& injector_;
  Validator validator_;
};

}  // namespace algaguard::host_test
