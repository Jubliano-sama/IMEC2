#!/usr/bin/env python3
from pathlib import Path
import re
import unittest


FIRMWARE = Path(__file__).resolve().parents[2]
APP = FIRMWARE / "app"
SOURCE = (APP / "src/app_durable_state.c").read_text()
HEADER = (APP / "src/app_durable_state.h").read_text()
ANCHOR_COMMANDS = (APP / "src/app_anchor_commands.inc").read_text()
ANCHOR_INIT = (APP / "src/app_anchor_init.inc").read_text()
CMAKE = (FIRMWARE / "CMakeLists.txt").read_text()


def braced_body(source: str, declaration: str) -> str:
    match = re.search(declaration + r"\s*\{", source, re.DOTALL)
    if match is None:
        raise AssertionError(f"missing declaration matching {declaration}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(f"unterminated declaration matching {declaration}")


def function_body(source: str, name: str) -> str:
    return braced_body(source, rf"\b{name}\s*\([^;]*?\)")


class AnchorAssignmentPersistenceSourceInvariants(unittest.TestCase):
    def test_public_type_is_semantic_and_fixed_width(self) -> None:
        assignment = braced_body(
            HEADER, r"struct\s+app_durable_state_anchor_assignment"
        )

        self.assertRegex(
            HEADER,
            r"APP_DURABLE_STATE_ANCHOR_ASSIGNMENT_PAYLOAD_SIZE\s+168u",
        )
        self.assertIn("APP_DURABLE_STATE_ANCHOR_ASSIGNMENT = 5", HEADER)
        self.assertIn(
            "struct discovery_assignment_table_commitment table_commitment;",
            assignment,
        )
        self.assertIn(
            "struct discovery_assignment_table_commitment "
            "pending_table_commitment;",
            assignment,
        )
        for field in (
            "epoch",
            "table_command_seq",
            "pending_epoch",
            "pending_table_command_seq",
            "retired_epochs",
            "table_packet_seq",
            "response_spread_ms",
            "slot",
            "slot_count",
            "provisioned",
            "retired_epoch_count",
            "ordered_epoch_valid",
            "ack_pending",
            "pending_slot",
            "pending_slot_count",
            "pending_valid",
        ):
            self.assertRegex(assignment, rf"\b{field}\b")

        for forbidden in (
            "local_id",
            "gateway_id",
            "version",
            "size",
            "checksum",
            "valid",
            "ack_retry_round",
        ):
            self.assertNotRegex(assignment, rf"\b{forbidden}\s*;")
        self.assertIn("retry state", HEADER)
        for api in (
            "app_durable_state_save_anchor_assignment",
            "app_durable_state_restore_anchor_assignment",
            "app_durable_state_delete_anchor_assignment",
        ):
            self.assertIn(api, HEADER)

    def test_codec_is_explicit_and_rejects_noncanonical_bytes(self) -> None:
        encode = function_body(SOURCE, "durable_encode_anchor_assignment")
        decode = function_body(SOURCE, "durable_decode_anchor_assignment")

        self.assertIn("APP_DURABLE_STATE_ID_ANCHOR_ASSIGNMENT", SOURCE)
        self.assertIn("APP_DURABLE_STATE_ANCHOR_ASSIGNMENT_RECORD_SIZE", encode)
        self.assertIn("APP_DURABLE_STATE_ANCHOR_ASSIGNMENT_PAYLOAD_SIZE", encode)
        self.assertNotRegex(
            encode,
            r"memcpy\s*\(\s*record\s*,\s*assignment",
        )
        self.assertNotRegex(
            decode,
            r"memcpy\s*\(\s*assignment\s*,\s*record",
        )
        for offset in (
            "ASSIGNMENT_GATEWAY_ID_OFFSET",
            "ASSIGNMENT_TABLE_COMMITMENT_OFFSET",
            "ASSIGNMENT_PENDING_COMMITMENT_OFFSET",
            "ASSIGNMENT_EPOCH_OFFSET",
            "ASSIGNMENT_TABLE_COMMAND_SEQ_OFFSET",
            "ASSIGNMENT_PENDING_EPOCH_OFFSET",
            "ASSIGNMENT_PENDING_TABLE_COMMAND_SEQ_OFFSET",
            "ASSIGNMENT_RETIRED_EPOCHS_OFFSET",
            "ASSIGNMENT_TABLE_PACKET_SEQ_OFFSET",
            "ASSIGNMENT_RESPONSE_SPREAD_MS_OFFSET",
            "ASSIGNMENT_SLOT_OFFSET",
            "ASSIGNMENT_PENDING_VALID_OFFSET",
            "ASSIGNMENT_PENDING_RESPONSE_LANE_OFFSET",
            "ASSIGNMENT_PENDING_RESPONSE_LANE_COUNT_OFFSET",
            "ASSIGNMENT_RESERVED_OFFSET",
        ):
            self.assertIn(offset, encode + decode)
        self.assertEqual(decode.count("ASSIGNMENT_RESERVED_OFFSET"), 1)
        self.assertIn("durable_crc16(", encode)
        self.assertIn("durable_crc16(", decode)
        self.assertIn("durable_validate_anchor_assignment(assignment)", decode)
        self.assertIn("return -EPROTO", decode)
        self.assertIn("return -EBADMSG", decode)
        self.assertIn("return -EACCES", decode)
        self.assertNotIn("struct app_mesh_discovery_assignment_snapshot", SOURCE)
        self.assertNotIn("struct app_mesh_discovery_assignment_snapshot", HEADER)

    def test_save_skips_exact_replays_and_every_mutation_is_read_back(self) -> None:
        save = function_body(
            SOURCE, "app_durable_state_save_anchor_assignment"
        )
        delete = function_body(
            SOURCE, "app_durable_state_delete_anchor_assignment"
        )

        first_read = save.index("durable_owner.backend.read(")
        equality = save.index("durable_anchor_assignments_equal(", first_read)
        encode = save.index("durable_encode_anchor_assignment(", equality)
        write = save.index("durable_owner.backend.write(", encode)
        readback = save.index("durable_owner.backend.read(", write)
        verify = save.index("durable_anchor_assignments_equal(", readback)
        self.assertLess(first_read, equality)
        self.assertLess(equality, encode)
        self.assertLess(encode, write)
        self.assertLess(write, readback)
        self.assertLess(readback, verify)
        self.assertEqual(save.count("durable_owner.backend.write("), 1)
        self.assertEqual(save.count("durable_owner.backend.read("), 2)

        delete_read = delete.index("durable_owner.backend.read(")
        delete_decode = delete.index(
            "durable_decode_anchor_assignment(", delete_read
        )
        erase = delete.index("durable_owner.backend.erase(", delete_decode)
        delete_readback = delete.index("durable_owner.backend.read(", erase)
        missing = delete.index("read_len == -ENOENT", delete_readback)
        success = delete.index("ret = 0", missing)
        self.assertLess(delete_read, delete_decode)
        self.assertLess(delete_decode, erase)
        self.assertLess(erase, delete_readback)
        self.assertLess(delete_readback, missing)
        self.assertLess(missing, success)
        self.assertEqual(delete.count("durable_owner.backend.read("), 2)

    def test_bounded_ram_cache_has_one_typed_durable_owner(self) -> None:
        cache_read = function_body(
            ANCHOR_COMMANDS, "anchor_restore_discovery_assignment_ram"
        )
        to_durable = function_body(
            ANCHOR_COMMANDS, "anchor_assignment_snapshot_to_durable"
        )
        from_durable = function_body(
            ANCHOR_COMMANDS, "anchor_assignment_durable_to_snapshot"
        )
        save = function_body(
            ANCHOR_COMMANDS, "anchor_save_discovery_assignment_semantic"
        )
        retry_ram = function_body(
            ANCHOR_COMMANDS, "anchor_save_discovery_assignment_retry_ram"
        )
        clear = function_body(
            ANCHOR_COMMANDS, "anchor_clear_discovery_assignment_semantic"
        )
        restore_durable = function_body(
            ANCHOR_COMMANDS, "anchor_restore_discovery_assignment_durable"
        )

        self.assertIn("anchor_ram_discovery_assignment_snapshot", cache_read)
        self.assertNotIn("app_durable_state_", cache_read)
        self.assertNotIn("ack_retry_round", to_durable)
        self.assertIn("snapshot->local_id = DEVICE_ID", from_durable)
        self.assertIn("snapshot->gateway_id = GATEWAY_ID", from_durable)
        self.assertIn(
            "APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION", from_durable
        )
        self.assertIn("snapshot->valid = 1u", from_durable)
        self.assertIn("snapshot->ack_retry_round = 0u", from_durable)

        convert = save.index("anchor_assignment_snapshot_to_durable(")
        durable_save = save.index(
            "app_durable_state_save_anchor_assignment(", convert
        )
        cache_publish = save.index(
            "anchor_ram_discovery_assignment_snapshot = *snapshot",
            durable_save,
        )
        self.assertLess(convert, durable_save)
        self.assertLess(durable_save, cache_publish)
        self.assertNotIn("app_durable_state_", retry_ram)
        self.assertIn(
            "anchor_ram_discovery_assignment_snapshot = *snapshot", retry_ram
        )

        durable_delete = clear.index(
            "app_durable_state_delete_anchor_assignment("
        )
        cache_clear = clear.index(
            "memset(&anchor_ram_discovery_assignment_snapshot", durable_delete
        )
        self.assertLess(durable_delete, cache_clear)

        durable_restore = restore_durable.index(
            "app_durable_state_restore_anchor_assignment("
        )
        convert_restore = restore_durable.index(
            "anchor_assignment_durable_to_snapshot(", durable_restore
        )
        live_restore = restore_durable.index(
            "anchor_restore_live_discovery_assignment_snapshot(",
            convert_restore,
        )
        restored_cache_publish = restore_durable.index(
            "anchor_ram_discovery_assignment_snapshot = snapshot",
            live_restore,
        )
        self.assertLess(durable_restore, convert_restore)
        self.assertLess(convert_restore, live_restore)
        self.assertLess(live_restore, restored_cache_publish)

        self.assertEqual(
            ANCHOR_COMMANDS.count(
                "app_durable_state_save_anchor_assignment("
            ),
            1,
        )
        self.assertEqual(
            ANCHOR_COMMANDS.count(
                "app_durable_state_restore_anchor_assignment("
            ),
            1,
        )
        self.assertEqual(
            ANCHOR_COMMANDS.count(
                "app_durable_state_delete_anchor_assignment("
            ),
            1,
        )

    def test_startup_resume_restores_durable_state_before_cache_use(self) -> None:
        start = function_body(ANCHOR_INIT, "app_anchor_start_anchor_role")
        resume = function_body(
            ANCHOR_COMMANDS,
            "anchor_resume_pending_discovery_assignment_ack",
        )

        work_init = start.index("anchor_discovery_claim_work_handler")
        startup_resume = start.index(
            "anchor_resume_pending_discovery_assignment_ack(false)",
            work_init,
        )
        durable_restore = resume.index(
            "anchor_restore_discovery_assignment_durable()"
        )
        cache_read = resume.index(
            "anchor_restore_discovery_assignment_ram(&snapshot)",
            durable_restore,
        )
        schedule = resume.index(
            "anchor_schedule_discovery_response(", cache_read
        )
        self.assertLess(work_init, startup_resume)
        self.assertLess(durable_restore, cache_read)
        self.assertLess(cache_read, schedule)

    def test_pending_and_promotion_persist_before_exposure(self) -> None:
        apply = function_body(
            ANCHOR_COMMANDS, "anchor_apply_discovery_assignment_command"
        )
        promote = function_body(
            ANCHOR_COMMANDS,
            "anchor_promote_discovery_assignment_locked_impl",
        )

        self.assertRegex(
            apply,
            r"if\s*\(table_decision\s*!=\s*"
            r"APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY\)\s*\{\s*"
            r"table_decision\s*=\s*"
            r"local_anchor_discovery_assignment_note_table\(",
        )
        pending_saves = [
            match.start()
            for match in re.finditer(
                r"anchor_save_discovery_assignment_semantic\(&snapshot\)",
                apply,
            )
        ]
        self.assertEqual(len(pending_saves), 2)
        persist_then_apply = re.findall(
            r"ret\s*=\s*"
            r"anchor_save_discovery_assignment_semantic\(&snapshot\);\s*"
            r"if\s*\(ret\s*<\s*0\)\s*\{.*?\}\s*"
            r"table_decision\s*=\s*"
            r"local_anchor_discovery_assignment_note_table\(",
            apply,
            re.DOTALL,
        )
        self.assertEqual(len(persist_then_apply), 2)

        listed_save, unlisted_save = pending_saves
        listed_live_apply = apply.index(
            "local_anchor_discovery_assignment_note_table(", listed_save
        )
        self.assertLess(listed_save, listed_live_apply)

        unlisted_live_apply = apply.index(
            "local_anchor_discovery_assignment_note_table(", unlisted_save
        )
        unlisted_schedule = apply.index(
            "anchor_schedule_late_discovery_claim(", unlisted_live_apply
        )
        self.assertLess(unlisted_save, unlisted_live_apply)
        self.assertLess(unlisted_live_apply, unlisted_schedule)

        promotion_save = promote.index(
            "anchor_save_discovery_assignment_semantic(&snapshot)"
        )
        live_commit = promote.index(
            "local_anchor_commit_discovery_assignment(", promotion_save
        )
        self.assertLess(promotion_save, live_commit)

    def test_retry_round_is_not_a_flash_mutation(self) -> None:
        to_durable = function_body(
            ANCHOR_COMMANDS, "anchor_assignment_snapshot_to_durable"
        )
        retry_ram = function_body(
            ANCHOR_COMMANDS, "anchor_save_discovery_assignment_retry_ram"
        )
        retry = function_body(
            ANCHOR_COMMANDS,
            "anchor_persist_discovery_assignment_ack_retry_round",
        )
        replay = function_body(
            ANCHOR_COMMANDS,
            "anchor_resume_pending_discovery_assignment_ack",
        )

        self.assertNotIn("ack_retry_round", to_durable)
        self.assertNotIn("app_durable_state_", retry_ram)
        self.assertNotIn("anchor_save_discovery_assignment_semantic", retry)
        self.assertNotIn("app_durable_state_save_anchor_assignment", retry)
        self.assertNotIn("app_durable_state_delete_anchor_assignment", retry)
        self.assertNotIn("anchor_save_discovery_assignment_semantic", replay)
        self.assertNotIn("app_durable_state_save_anchor_assignment", replay)
        self.assertNotIn("app_durable_state_delete_anchor_assignment", replay)
        self.assertIn("anchor_save_discovery_assignment_retry_ram", retry)
        self.assertIn("anchor_save_discovery_assignment_retry_ram", replay)

    def test_sparse_identity_slot_has_a_durable_compact_timing_lane(self) -> None:
        to_durable = function_body(
            ANCHOR_COMMANDS, "anchor_assignment_snapshot_to_durable"
        )
        from_durable = function_body(
            ANCHOR_COMMANDS, "anchor_assignment_durable_to_snapshot"
        )
        delay = function_body(
            ANCHOR_COMMANDS, "anchor_discovery_response_delay_ms"
        )
        apply = function_body(
            ANCHOR_COMMANDS, "anchor_apply_discovery_assignment_command"
        )
        resume = function_body(
            ANCHOR_COMMANDS,
            "anchor_resume_pending_discovery_assignment_ack",
        )

        self.assertIn("pending_response_lane", to_durable)
        self.assertIn("pending_response_lane_count", to_durable)
        self.assertIn("pending_response_lane", from_durable)
        self.assertIn("pending_response_lane_count", from_durable)
        self.assertIn("pending->response_lane", delay)
        self.assertIn("pending->response_lane_count", delay)
        self.assertIn("discovery_assignment_response_lane(", apply)
        self.assertIn("snapshot.pending_response_lane = response_lane", apply)
        self.assertIn("snapshot.pending_response_lane_count == 0u", resume)

    def test_ambiguous_promotion_retains_exact_owner(self) -> None:
        promotion = function_body(
            ANCHOR_COMMANDS,
            "anchor_discovery_assignment_service_ack_promotion",
        )

        retry_classification = promotion.index("ret == -ETIMEDOUT")
        reschedule = promotion.index(
            "anchor_discovery_claim_reschedule_locked(",
            retry_classification,
        )
        fail_closed = promotion.index("fail_closed = true", reschedule)
        stop_watchdog = promotion.index(
            "app_watchdog_stop_feeding()", fail_closed
        )
        self.assertLess(retry_classification, reschedule)
        self.assertLess(reschedule, fail_closed)
        self.assertLess(fail_closed, stop_watchdog)
        self.assertEqual(
            promotion.count("anchor_discovery_claim_pending.active = false"),
            1,
        )
        self.assertIn("if (ret == 0)", promotion)

    def test_semantic_changes_have_no_frequency_admission_gate(self) -> None:
        apply = function_body(
            ANCHOR_COMMANDS, "anchor_apply_discovery_assignment_command"
        )

        self.assertNotIn("ANCHOR_ASSIGNMENT_COMMISSIONING_BURST", ANCHOR_COMMANDS)
        self.assertNotIn("ANCHOR_ASSIGNMENT_REASSIGN_INTERVAL_MS", ANCHOR_COMMANDS)
        self.assertNotIn(
            "anchor_admit_discovery_assignment_semantic_change", ANCHOR_COMMANDS
        )
        self.assertNotIn("assignment change admission rate-limited", ANCHOR_COMMANDS)
        self.assertIn("anchor_save_discovery_assignment_semantic(&snapshot)", apply)

    def test_native_and_source_guards_are_registered(self) -> None:
        self.assertIn("test_app_durable_anchor_assignment", CMAKE)
        self.assertIn("app_durable_anchor_assignment", CMAKE)
        self.assertIn(
            "test_anchor_assignment_persistence_source_invariants.py", CMAKE
        )
        self.assertIn("anchor_assignment_persistence_source_invariants", CMAKE)


if __name__ == "__main__":
    unittest.main()
