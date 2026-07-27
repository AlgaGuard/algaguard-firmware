#include <cstring>

#include <unity.h>

#include "algaguard/host_two_slot.hpp"

using namespace algaguard::host_test;

FaultInjector faults;
FakeStore store;

Transaction transaction() { return Transaction(store, faults); }
Record rec(const char* id) { return {1, 0, false, id, "host-uuid", "PENDING", {}, {}, {}}; }

void setUp() { faults.reset(); store = FakeStore{}; }
void tearDown() {}

void assertSafe(const TransactionResult& result) {
  TEST_ASSERT_NULL(std::strstr(result.diagnostic.c_str(), "0x11"));
  TEST_ASSERT_NULL(std::strstr(result.diagnostic.c_str(), "0x22"));
  TEST_ASSERT_NULL(std::strstr(result.diagnostic.c_str(), "0x33"));
}
void seed(Transaction& tx) { TEST_ASSERT_TRUE(tx.enrollInitial(rec("old")).success); }
void assertOldActive(Transaction& tx) {
  LoadStatus status;
  const auto loaded = tx.loadActive(&status);
  TEST_ASSERT_EQUAL(static_cast<int>(LoadStatus::READY), static_cast<int>(status));
  TEST_ASSERT_EQUAL_STRING("old", loaded->device_id.c_str());
  TEST_ASSERT_EQUAL(1, loaded->generation);
}
void enrollmentFailure(FaultPoint point) {
  auto tx = transaction();
  faults.fail_on(point);
  const auto result = tx.enrollInitial(rec("first"));
  TEST_ASSERT_FALSE(result.success);
  TEST_ASSERT_EQUAL(static_cast<int>(reasonFor(point)), static_cast<int>(result.reason));
  TEST_ASSERT_FALSE(store.readActivePointer().has_value());
  LoadStatus status;
  TEST_ASSERT_FALSE(tx.loadActive(&status).has_value());
  TEST_ASSERT_EQUAL(static_cast<int>(LoadStatus::UNPROVISIONED), static_cast<int>(status));
  assertSafe(result);
}
void committedEnrollmentInterruption(FaultPoint point) {
  auto tx = transaction();
  faults.fail_on(point);
  const auto result = tx.enrollInitial(rec("first"));
  TEST_ASSERT_FALSE(result.success);
  TEST_ASSERT_EQUAL(static_cast<int>(reasonFor(point)), static_cast<int>(result.reason));
  TEST_ASSERT_FALSE(store.readActivePointer().has_value());
  const auto reboot = store.snapshot();
  FakeStore restored(reboot);
  Transaction after_reboot(restored, faults);
  LoadStatus status;
  const auto loaded = after_reboot.loadActive(&status);
  TEST_ASSERT_EQUAL(static_cast<int>(LoadStatus::READY), static_cast<int>(status));
  TEST_ASSERT_EQUAL_STRING("first", loaded->device_id.c_str());
  TEST_ASSERT_TRUE(loaded->committed);
  assertSafe(result);
}
void replacementFailure(FaultPoint point) {
  auto tx = transaction();
  seed(tx);
  faults.fail_on(point);
  const auto result = tx.replaceActive(rec("new"));
  TEST_ASSERT_FALSE(result.success);
  TEST_ASSERT_EQUAL(static_cast<int>(reasonFor(point)), static_cast<int>(result.reason));
  assertOldActive(tx);
  assertSafe(result);
}

void test_01_before_inactive_slot_initialization() { enrollmentFailure(FaultPoint::BEFORE_INACTIVE_SLOT_INIT); }
void test_02_private_key_write_failure() { enrollmentFailure(FaultPoint::WRITE_PRIVATE_KEY); }
void test_03_certificate_write_failure() { enrollmentFailure(FaultPoint::WRITE_CERTIFICATE); }
void test_04_ca_chain_write_failure() { enrollmentFailure(FaultPoint::WRITE_CA_CHAIN); }
void test_05_metadata_write_failure() { enrollmentFailure(FaultPoint::WRITE_METADATA); }
void test_06_readback_failure() { enrollmentFailure(FaultPoint::READBACK); }
void test_07_readback_mismatch() { enrollmentFailure(FaultPoint::READBACK_MISMATCH); }
void test_08_committed_marker_write_failure() { enrollmentFailure(FaultPoint::WRITE_COMMITTED_MARKER); }

void test_09_initial_reboot_after_committed_marker_recovers_committed_slot() {
  auto tx = transaction();
  faults.fail_on(FaultPoint::AFTER_COMMITTED_MARKER);
  const auto result = tx.enrollInitial(rec("first"));
  TEST_ASSERT_FALSE(result.success);
  const auto reboot = store.snapshot();
  FakeStore restored(reboot);
  Transaction after_reboot(restored, faults);
  LoadStatus status;
  const auto loaded = after_reboot.loadActive(&status);
  TEST_ASSERT_EQUAL(static_cast<int>(LoadStatus::READY), static_cast<int>(status));
  TEST_ASSERT_EQUAL_STRING("first", loaded->device_id.c_str());
  TEST_ASSERT_TRUE(loaded->committed);
  assertSafe(result);
}
void test_10_active_slot_write_failure() { committedEnrollmentInterruption(FaultPoint::WRITE_ACTIVE_SLOT); }
void test_11_active_generation_write_failure() { committedEnrollmentInterruption(FaultPoint::WRITE_ACTIVE_GENERATION); }
void test_12_pointer_completion_failure() { committedEnrollmentInterruption(FaultPoint::COMPLETE_POINTER_UPDATE); }

void test_13_replacement_before_inactive_slot_preserves_old() { replacementFailure(FaultPoint::BEFORE_INACTIVE_SLOT_INIT); }
void test_14_replacement_key_write_preserves_old() { replacementFailure(FaultPoint::WRITE_PRIVATE_KEY); }
void test_15_replacement_certificate_write_preserves_old() { replacementFailure(FaultPoint::WRITE_CERTIFICATE); }
void test_16_replacement_ca_write_preserves_old() { replacementFailure(FaultPoint::WRITE_CA_CHAIN); }
void test_17_replacement_metadata_write_preserves_old() { replacementFailure(FaultPoint::WRITE_METADATA); }
void test_18_replacement_readback_preserves_old() { replacementFailure(FaultPoint::READBACK); }
void test_19_replacement_readback_mismatch_preserves_old() { replacementFailure(FaultPoint::READBACK_MISMATCH); }
void test_20_replacement_commit_marker_preserves_old() { replacementFailure(FaultPoint::WRITE_COMMITTED_MARKER); }

void test_21_replacement_interruption_after_commit_reboots_to_valid_old_pointer() {
  auto tx = transaction();
  seed(tx);
  faults.fail_on(FaultPoint::AFTER_COMMITTED_MARKER);
  const auto result = tx.replaceActive(rec("new"));
  TEST_ASSERT_FALSE(result.success);
  const auto reboot = store.snapshot();
  FakeStore restored(reboot);
  Transaction after_reboot(restored, faults);
  assertOldActive(after_reboot);
  const auto pending = restored.readSlot(Slot::SLOT_1);
  TEST_ASSERT_TRUE(pending->committed);
  TEST_ASSERT_EQUAL_STRING("new", pending->device_id.c_str());
  assertSafe(result);
}
void test_22_replacement_active_slot_failure_preserves_old() { replacementFailure(FaultPoint::WRITE_ACTIVE_SLOT); }
void test_23_replacement_active_generation_failure_preserves_old() { replacementFailure(FaultPoint::WRITE_ACTIVE_GENERATION); }
void test_24_replacement_pointer_completion_failure_preserves_old() { replacementFailure(FaultPoint::COMPLETE_POINTER_UPDATE); }

void test_25_cleanup_failure_leaves_new_identity_active() {
  auto tx = transaction();
  seed(tx);
  faults.fail_on(FaultPoint::CLEAN_OLD_SLOT);
  const auto result = tx.replaceActive(rec("new"));
  TEST_ASSERT_FALSE(result.success);
  LoadStatus status;
  const auto loaded = tx.loadActive(&status);
  TEST_ASSERT_EQUAL_STRING("new", loaded->device_id.c_str());
  TEST_ASSERT_EQUAL(2, loaded->generation);
  TEST_ASSERT_TRUE(store.readSlot(Slot::SLOT_0).has_value());
  assertSafe(result);
}

void test_26_successful_replacement_from_slot_0_activates_slot_1() {
  auto tx = transaction(); seed(tx); TEST_ASSERT_TRUE(tx.replaceActive(rec("one")).success);
  TEST_ASSERT_EQUAL(1, static_cast<int>(store.readActivePointer()->slot));
}
void test_27_successful_replacement_from_slot_1_activates_slot_0() {
  auto tx = transaction(); seed(tx); TEST_ASSERT_TRUE(tx.replaceActive(rec("one")).success);
  TEST_ASSERT_TRUE(tx.replaceActive(rec("two")).success);
  TEST_ASSERT_EQUAL(0, static_cast<int>(store.readActivePointer()->slot));
}
void test_28_repeated_successful_replacements_alternate_slots() {
  auto tx = transaction(); seed(tx);
  TEST_ASSERT_TRUE(tx.replaceActive(rec("one")).success);
  TEST_ASSERT_EQUAL(1, static_cast<int>(store.readActivePointer()->slot));
  TEST_ASSERT_TRUE(tx.replaceActive(rec("two")).success);
  TEST_ASSERT_EQUAL(0, static_cast<int>(store.readActivePointer()->slot));
}
void test_29_generation_increments_monotonically() {
  auto tx = transaction(); seed(tx);
  TEST_ASSERT_EQUAL(1, store.readActivePointer()->generation);
  TEST_ASSERT_TRUE(tx.replaceActive(rec("one")).success);
  TEST_ASSERT_EQUAL(2, store.readActivePointer()->generation);
  TEST_ASSERT_TRUE(tx.replaceActive(rec("two")).success);
  TEST_ASSERT_EQUAL(3, store.readActivePointer()->generation);
}
void test_30_generation_overflow_is_detected_before_write() {
  auto tx = transaction(); seed(tx); TEST_ASSERT_TRUE(tx.replaceActive(rec("one")).success);
  TEST_ASSERT_TRUE(tx.replaceActive(rec("two")).success);
  const auto before = store.snapshot();
  const auto result = tx.replaceActive(rec("overflow"));
  TEST_ASSERT_FALSE(result.success);
  TEST_ASSERT_EQUAL(static_cast<int>(ReasonCode::GENERATION_OVERFLOW), static_cast<int>(result.reason));
  const auto after = store.snapshot();
  TEST_ASSERT_EQUAL(before.active->generation, after.active->generation);
  TEST_ASSERT_EQUAL_STRING(before.slots[0]->device_id.c_str(), after.slots[0]->device_id.c_str());
  assertSafe(result);
}
void test_31_generation_overflow_fails_closed_and_preserves_old() {
  auto tx = transaction(); seed(tx); TEST_ASSERT_TRUE(tx.replaceActive(rec("one")).success);
  TEST_ASSERT_TRUE(tx.replaceActive(rec("two")).success);
  TEST_ASSERT_FALSE(tx.replaceActive(rec("overflow")).success);
  LoadStatus status;
  const auto loaded = tx.loadActive(&status);
  TEST_ASSERT_EQUAL_STRING("two", loaded->device_id.c_str());
  TEST_ASSERT_EQUAL(kMaximumGeneration, loaded->generation);
}

void test_32_reboot_with_valid_active_slot_reloads_same_identity() {
  auto tx = transaction(); seed(tx); FakeStore restored(store.snapshot()); Transaction rebooted(restored, faults); assertOldActive(rebooted);
}
void test_33_reboot_after_failed_replacement_reloads_old_identity() {
  auto tx = transaction(); seed(tx); faults.fail_on(FaultPoint::WRITE_CERTIFICATE);
  TEST_ASSERT_FALSE(tx.replaceActive(rec("new")).success);
  FakeStore restored(store.snapshot()); Transaction rebooted(restored, faults); assertOldActive(rebooted);
}
void test_34_reboot_after_successful_replacement_reloads_new_identity() {
  auto tx = transaction(); seed(tx); TEST_ASSERT_TRUE(tx.replaceActive(rec("new")).success);
  FakeStore restored(store.snapshot()); Transaction rebooted(restored, faults);
  LoadStatus status; const auto loaded = rebooted.loadActive(&status);
  TEST_ASSERT_EQUAL_STRING("new", loaded->device_id.c_str()); TEST_ASSERT_EQUAL(2, loaded->generation);
}
void test_35_reboot_after_committed_marker_before_pointer_uses_documented_rule() {
  auto tx = transaction(); seed(tx); faults.fail_on(FaultPoint::AFTER_COMMITTED_MARKER);
  TEST_ASSERT_FALSE(tx.replaceActive(rec("new")).success);
  FakeStore restored(store.snapshot()); Transaction rebooted(restored, faults);
  // A valid active pointer wins; the newer committed record remains stored for deterministic inspection.
  assertOldActive(rebooted);
  TEST_ASSERT_TRUE(restored.readSlot(Slot::SLOT_1)->committed);
}

Record validRecord(const char* id, std::uint32_t generation) {
  auto record = rec(id);
  record.generation = generation;
  record.committed = true;
  record.integrity_tag = "OK";
  record.key_blob = {0x11};
  record.certificate_blob = {0x22};
  record.ca_chain_blob = {0x33};
  return record;
}
void activate(Slot slot, std::uint32_t generation) {
  TEST_ASSERT_TRUE(store.writeActiveSlot(slot));
  TEST_ASSERT_TRUE(store.writeActiveGeneration(generation));
  TEST_ASSERT_TRUE(store.completePointerUpdate());
}
void twoValid(std::uint32_t first = 1, std::uint32_t second = 2) {
  TEST_ASSERT_TRUE(store.writeSlot(Slot::SLOT_0, validRecord("zero", first)));
  TEST_ASSERT_TRUE(store.writeSlot(Slot::SLOT_1, validRecord("one", second)));
}
void assertRecovered(const char* id, std::uint32_t generation) {
  auto tx = transaction();
  LoadStatus status;
  const auto loaded = tx.loadActive(&status);
  TEST_ASSERT_EQUAL(static_cast<int>(LoadStatus::READY), static_cast<int>(status));
  TEST_ASSERT_EQUAL_STRING(id, loaded->device_id.c_str());
  TEST_ASSERT_EQUAL(generation, loaded->generation);
}
void assertCorrupt() {
  auto tx = transaction(); LoadStatus status;
  TEST_ASSERT_FALSE(tx.loadActive(&status).has_value());
  TEST_ASSERT_EQUAL(static_cast<int>(LoadStatus::DEVELOPMENT_IDENTITY_CORRUPT), static_cast<int>(status));
}

void test_36_valid_active_pointer_to_slot_zero_wins() { twoValid(); activate(Slot::SLOT_0, 1); assertRecovered("zero", 1); }
void test_37_valid_active_pointer_to_slot_one_wins() { twoValid(); activate(Slot::SLOT_1, 2); assertRecovered("one", 2); }
void test_38_corrupt_active_slot_pointer_falls_back_to_highest() { twoValid(); activate(Slot::SLOT_0, 1); store.corruptActiveSlotPointer(); assertRecovered("one", 2); }
void test_39_corrupt_active_generation_falls_back_to_highest() { twoValid(); activate(Slot::SLOT_0, 1); store.corruptActiveGeneration(); assertRecovered("one", 2); }
void test_40_stale_active_pointer_falls_back_to_newer_slot() { twoValid(2, 3); activate(Slot::SLOT_0, 1); assertRecovered("one", 3); }
void test_41_pointer_to_uncommitted_slot_falls_back_to_valid_slot() { twoValid(); activate(Slot::SLOT_0, 1); store.markSlotUncommitted(Slot::SLOT_0); assertRecovered("one", 2); }
void test_42_missing_pointer_with_one_valid_slot_recovers_it() { TEST_ASSERT_TRUE(store.writeSlot(Slot::SLOT_1, validRecord("one", 2))); assertRecovered("one", 2); }
void test_43_missing_pointer_with_two_valid_generations_selects_higher() { twoValid(); assertRecovered("one", 2); }
void test_44_missing_pointer_with_equal_generations_fails_closed() { twoValid(2, 2); auto tx = transaction(); LoadStatus status; TEST_ASSERT_FALSE(tx.loadActive(&status).has_value()); TEST_ASSERT_EQUAL(static_cast<int>(LoadStatus::AMBIGUOUS_ACTIVE_IDENTITY), static_cast<int>(status)); }
void test_45_pointer_to_corrupt_slot_falls_back_to_other_valid_slot() { twoValid(); activate(Slot::SLOT_0, 1); store.corruptIntegrityTag(Slot::SLOT_0); assertRecovered("one", 2); }

void test_46_slot_zero_corrupt_slot_one_valid_recovers_alternate() { twoValid(); store.corruptIntegrityTag(Slot::SLOT_0); assertRecovered("one", 2); }
void test_47_slot_one_corrupt_slot_zero_valid_recovers_alternate() { twoValid(); store.corruptIntegrityTag(Slot::SLOT_1); assertRecovered("zero", 1); }
void test_48_both_corrupt_slots_fail_closed() { twoValid(); store.corruptIntegrityTag(Slot::SLOT_0); store.corruptSchemaVersion(Slot::SLOT_1); assertCorrupt(); }
void test_49_missing_committed_marker_is_invalid() { twoValid(); store.markSlotUncommitted(Slot::SLOT_0); TEST_ASSERT_EQUAL(static_cast<int>(SlotValidity::SLOT_UNCOMMITTED), static_cast<int>(slotValidity(*store.readSlot(Slot::SLOT_0)))); assertRecovered("one", 2); }
void test_50_unsupported_schema_is_invalid() { twoValid(); store.corruptSchemaVersion(Slot::SLOT_0); TEST_ASSERT_EQUAL(static_cast<int>(SlotValidity::SLOT_SCHEMA_UNSUPPORTED), static_cast<int>(slotValidity(*store.readSlot(Slot::SLOT_0)))); assertRecovered("one", 2); }
void test_51_missing_device_id_is_invalid() { twoValid(); store.clearDeviceId(Slot::SLOT_0); TEST_ASSERT_EQUAL(static_cast<int>(SlotValidity::SLOT_IDENTITY_INVALID), static_cast<int>(slotValidity(*store.readSlot(Slot::SLOT_0)))); assertRecovered("one", 2); }
void test_52_missing_device_uuid_is_invalid() { twoValid(); store.clearDeviceUuid(Slot::SLOT_0); TEST_ASSERT_EQUAL(static_cast<int>(SlotValidity::SLOT_IDENTITY_INVALID), static_cast<int>(slotValidity(*store.readSlot(Slot::SLOT_0)))); assertRecovered("one", 2); }
void test_53_oversized_key_blob_is_invalid() { twoValid(); store.oversizeKeyBlob(Slot::SLOT_0); TEST_ASSERT_EQUAL(static_cast<int>(SlotValidity::SLOT_RECORD_OVERSIZED), static_cast<int>(slotValidity(*store.readSlot(Slot::SLOT_0)))); assertRecovered("one", 2); }
void test_54_oversized_certificate_blob_is_invalid() { twoValid(); store.oversizeCertificateBlob(Slot::SLOT_0); TEST_ASSERT_EQUAL(static_cast<int>(SlotValidity::SLOT_RECORD_OVERSIZED), static_cast<int>(slotValidity(*store.readSlot(Slot::SLOT_0)))); assertRecovered("one", 2); }
void test_55_oversized_ca_chain_blob_is_invalid() { twoValid(); store.oversizeCaChainBlob(Slot::SLOT_0); TEST_ASSERT_EQUAL(static_cast<int>(SlotValidity::SLOT_RECORD_OVERSIZED), static_cast<int>(slotValidity(*store.readSlot(Slot::SLOT_0)))); assertRecovered("one", 2); }
void test_56_corrupt_integrity_tag_is_invalid() { twoValid(); store.corruptIntegrityTag(Slot::SLOT_0); TEST_ASSERT_EQUAL(static_cast<int>(SlotValidity::SLOT_INTEGRITY_INVALID), static_cast<int>(slotValidity(*store.readSlot(Slot::SLOT_0)))); assertRecovered("one", 2); }
void test_57_erased_active_slot_recovers_valid_alternate() { twoValid(); activate(Slot::SLOT_0, 1); store.eraseSlot(Slot::SLOT_0); assertRecovered("one", 2); }
void test_58_both_slots_erased_are_unprovisioned() { twoValid(); store.eraseBothSlots(); auto tx = transaction(); LoadStatus status; TEST_ASSERT_FALSE(tx.loadActive(&status).has_value()); TEST_ASSERT_EQUAL(static_cast<int>(LoadStatus::UNPROVISIONED), static_cast<int>(status)); }
void test_59_invalid_generation_slot_recovers_valid_alternate() { twoValid(); store.corruptGeneration(Slot::SLOT_0); TEST_ASSERT_EQUAL(static_cast<int>(SlotValidity::SLOT_GENERATION_INVALID), static_cast<int>(slotValidity(*store.readSlot(Slot::SLOT_0)))); assertRecovered("one", 2); }

void test_60_generation_zero_is_rejected() { const auto record = validRecord("zero", 0); TEST_ASSERT_EQUAL(static_cast<int>(SlotValidity::SLOT_GENERATION_INVALID), static_cast<int>(slotValidity(record))); }
void test_61_generation_one_is_valid() { const auto record = validRecord("one", 1); TEST_ASSERT_EQUAL(static_cast<int>(SlotValidity::SLOT_VALID), static_cast<int>(slotValidity(record))); }
void test_62_generation_maximum_is_valid() { const auto record = validRecord("max", kMaximumGeneration); TEST_ASSERT_EQUAL(static_cast<int>(SlotValidity::SLOT_VALID), static_cast<int>(slotValidity(record))); }
void test_63_generation_above_maximum_is_rejected() { const auto record = validRecord("above", kMaximumGeneration + 1); TEST_ASSERT_EQUAL(static_cast<int>(SlotValidity::SLOT_GENERATION_INVALID), static_cast<int>(slotValidity(record))); }
void test_64_higher_valid_generation_wins_recovery() { twoValid(1, kMaximumGeneration); assertRecovered("one", kMaximumGeneration); }
void test_65_equal_generation_after_reboot_fails_closed() { twoValid(2, 2); FakeStore restored(store.snapshot()); Transaction rebooted(restored, faults); LoadStatus status; TEST_ASSERT_FALSE(rebooted.loadActive(&status).has_value()); TEST_ASSERT_EQUAL(static_cast<int>(LoadStatus::AMBIGUOUS_ACTIVE_IDENTITY), static_cast<int>(status)); }

void test_66_corrupt_pointer_survives_snapshot_and_recovers() { twoValid(); activate(Slot::SLOT_0, 1); store.corruptActivePointer(); FakeStore restored(store.snapshot()); Transaction rebooted(restored, faults); LoadStatus status; const auto loaded = rebooted.loadActive(&status); TEST_ASSERT_EQUAL_STRING("one", loaded->device_id.c_str()); }
void test_67_corrupt_slot_survives_snapshot_and_alternate_recovers() { twoValid(); store.corruptIntegrityTag(Slot::SLOT_0); FakeStore restored(store.snapshot()); Transaction rebooted(restored, faults); LoadStatus status; const auto loaded = rebooted.loadActive(&status); TEST_ASSERT_EQUAL_STRING("one", loaded->device_id.c_str()); }
void test_68_both_corrupt_slots_survive_snapshot_and_fail_closed() { twoValid(); store.corruptIntegrityTag(Slot::SLOT_0); store.corruptSchemaVersion(Slot::SLOT_1); FakeStore restored(store.snapshot()); Transaction rebooted(restored, faults); LoadStatus status; TEST_ASSERT_FALSE(rebooted.loadActive(&status).has_value()); TEST_ASSERT_EQUAL(static_cast<int>(LoadStatus::DEVELOPMENT_IDENTITY_CORRUPT), static_cast<int>(status)); }
void test_69_unsupported_schema_survives_snapshot_and_is_rejected() { twoValid(); store.corruptSchemaVersion(Slot::SLOT_0); FakeStore restored(store.snapshot()); TEST_ASSERT_EQUAL(static_cast<int>(SlotValidity::SLOT_SCHEMA_UNSUPPORTED), static_cast<int>(slotValidity(*restored.readSlot(Slot::SLOT_0)))); }
void test_70_oversized_record_survives_snapshot_and_is_rejected() { twoValid(); store.oversizeKeyBlob(Slot::SLOT_0); FakeStore restored(store.snapshot()); TEST_ASSERT_EQUAL(static_cast<int>(SlotValidity::SLOT_RECORD_OVERSIZED), static_cast<int>(slotValidity(*restored.readSlot(Slot::SLOT_0)))); }
void test_71_unprovisioned_snapshot_restores_as_unprovisioned() { FakeStore restored(store.snapshot()); Transaction rebooted(restored, faults); LoadStatus status; TEST_ASSERT_FALSE(rebooted.loadActive(&status).has_value()); TEST_ASSERT_EQUAL(static_cast<int>(LoadStatus::UNPROVISIONED), static_cast<int>(status)); }

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_01_before_inactive_slot_initialization); RUN_TEST(test_02_private_key_write_failure);
  RUN_TEST(test_03_certificate_write_failure); RUN_TEST(test_04_ca_chain_write_failure);
  RUN_TEST(test_05_metadata_write_failure); RUN_TEST(test_06_readback_failure);
  RUN_TEST(test_07_readback_mismatch); RUN_TEST(test_08_committed_marker_write_failure);
  RUN_TEST(test_09_initial_reboot_after_committed_marker_recovers_committed_slot);
  RUN_TEST(test_10_active_slot_write_failure); RUN_TEST(test_11_active_generation_write_failure);
  RUN_TEST(test_12_pointer_completion_failure); RUN_TEST(test_13_replacement_before_inactive_slot_preserves_old);
  RUN_TEST(test_14_replacement_key_write_preserves_old); RUN_TEST(test_15_replacement_certificate_write_preserves_old);
  RUN_TEST(test_16_replacement_ca_write_preserves_old); RUN_TEST(test_17_replacement_metadata_write_preserves_old);
  RUN_TEST(test_18_replacement_readback_preserves_old); RUN_TEST(test_19_replacement_readback_mismatch_preserves_old);
  RUN_TEST(test_20_replacement_commit_marker_preserves_old);
  RUN_TEST(test_21_replacement_interruption_after_commit_reboots_to_valid_old_pointer);
  RUN_TEST(test_22_replacement_active_slot_failure_preserves_old);
  RUN_TEST(test_23_replacement_active_generation_failure_preserves_old);
  RUN_TEST(test_24_replacement_pointer_completion_failure_preserves_old);
  RUN_TEST(test_25_cleanup_failure_leaves_new_identity_active);
  RUN_TEST(test_26_successful_replacement_from_slot_0_activates_slot_1);
  RUN_TEST(test_27_successful_replacement_from_slot_1_activates_slot_0);
  RUN_TEST(test_28_repeated_successful_replacements_alternate_slots);
  RUN_TEST(test_29_generation_increments_monotonically);
  RUN_TEST(test_30_generation_overflow_is_detected_before_write);
  RUN_TEST(test_31_generation_overflow_fails_closed_and_preserves_old);
  RUN_TEST(test_32_reboot_with_valid_active_slot_reloads_same_identity);
  RUN_TEST(test_33_reboot_after_failed_replacement_reloads_old_identity);
  RUN_TEST(test_34_reboot_after_successful_replacement_reloads_new_identity);
  RUN_TEST(test_35_reboot_after_committed_marker_before_pointer_uses_documented_rule);
  RUN_TEST(test_36_valid_active_pointer_to_slot_zero_wins); RUN_TEST(test_37_valid_active_pointer_to_slot_one_wins);
  RUN_TEST(test_38_corrupt_active_slot_pointer_falls_back_to_highest); RUN_TEST(test_39_corrupt_active_generation_falls_back_to_highest);
  RUN_TEST(test_40_stale_active_pointer_falls_back_to_newer_slot); RUN_TEST(test_41_pointer_to_uncommitted_slot_falls_back_to_valid_slot);
  RUN_TEST(test_42_missing_pointer_with_one_valid_slot_recovers_it); RUN_TEST(test_43_missing_pointer_with_two_valid_generations_selects_higher);
  RUN_TEST(test_44_missing_pointer_with_equal_generations_fails_closed); RUN_TEST(test_45_pointer_to_corrupt_slot_falls_back_to_other_valid_slot);
  RUN_TEST(test_46_slot_zero_corrupt_slot_one_valid_recovers_alternate); RUN_TEST(test_47_slot_one_corrupt_slot_zero_valid_recovers_alternate);
  RUN_TEST(test_48_both_corrupt_slots_fail_closed); RUN_TEST(test_49_missing_committed_marker_is_invalid);
  RUN_TEST(test_50_unsupported_schema_is_invalid); RUN_TEST(test_51_missing_device_id_is_invalid);
  RUN_TEST(test_52_missing_device_uuid_is_invalid); RUN_TEST(test_53_oversized_key_blob_is_invalid);
  RUN_TEST(test_54_oversized_certificate_blob_is_invalid); RUN_TEST(test_55_oversized_ca_chain_blob_is_invalid);
  RUN_TEST(test_56_corrupt_integrity_tag_is_invalid); RUN_TEST(test_57_erased_active_slot_recovers_valid_alternate);
  RUN_TEST(test_58_both_slots_erased_are_unprovisioned); RUN_TEST(test_59_invalid_generation_slot_recovers_valid_alternate);
  RUN_TEST(test_60_generation_zero_is_rejected); RUN_TEST(test_61_generation_one_is_valid);
  RUN_TEST(test_62_generation_maximum_is_valid); RUN_TEST(test_63_generation_above_maximum_is_rejected);
  RUN_TEST(test_64_higher_valid_generation_wins_recovery); RUN_TEST(test_65_equal_generation_after_reboot_fails_closed);
  RUN_TEST(test_66_corrupt_pointer_survives_snapshot_and_recovers); RUN_TEST(test_67_corrupt_slot_survives_snapshot_and_alternate_recovers);
  RUN_TEST(test_68_both_corrupt_slots_survive_snapshot_and_fail_closed); RUN_TEST(test_69_unsupported_schema_survives_snapshot_and_is_rejected);
  RUN_TEST(test_70_oversized_record_survives_snapshot_and_is_rejected); RUN_TEST(test_71_unprovisioned_snapshot_restores_as_unprovisioned);
  return UNITY_END();
}
