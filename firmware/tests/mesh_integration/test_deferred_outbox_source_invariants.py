#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
PERSISTENCE = read_composed_source(ROOT / "app" / "src" / "app_mesh_persistence.c")
REPORT = read_composed_source(ROOT / "app" / "src" / "app_mesh_report.c")
PREEMPTION = read_composed_source(ROOT / "app" / "src" / "app_mesh_preemption.c")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    if match is None:
        raise AssertionError(f"function not found: {name}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {name}")


class DeferredOutboxSourceInvariantTests(unittest.TestCase):
    def test_deferred_slot_is_distinct_and_reserved_in_capacity_budget(self):
        active = re.search(r"APP_MESH_NVS_OUTBOX_ID\s+(0x[0-9A-Fa-f]+)u", PERSISTENCE)
        deferred = re.search(
            r"APP_MESH_NVS_DEFERRED_OUTBOX_ID\s+(0x[0-9A-Fa-f]+)u", PERSISTENCE
        )
        self.assertIsNotNone(active)
        self.assertIsNotNone(deferred)
        self.assertNotEqual(active.group(1), deferred.group(1))
        self.assertGreaterEqual(
            PERSISTENCE.count(
                "APP_MESH_NVS_ENTRY_BYTES(sizeof(struct mesh_relay_outbox_snapshot))"
            ),
            2,
        )

    def test_deferred_save_never_targets_active_slot(self):
        save = function_body(PERSISTENCE, "app_mesh_persistence_save_deferred_outbox")
        self.assertIn("APP_MESH_NVS_DEFERRED_OUTBOX_ID", save)
        self.assertNotIn("APP_MESH_NVS_OUTBOX_ID", save)

    def test_presence_accessor_is_cache_only(self):
        present = function_body(
            PERSISTENCE, "app_mesh_persistence_deferred_outbox_present"
        )
        self.assertNotIn("app_mesh_persistence_init", present)
        self.assertNotIn("read_deferred_outbox", present)
        self.assertIn("deferred_outbox_presence", present)
        self.assertIn("mesh_nvs_ready", present)

    def test_presence_cache_is_updated_only_by_custody_transitions(self):
        read = function_body(PERSISTENCE, "read_deferred_outbox")
        write = function_body(PERSISTENCE, "mesh_persistence_write")
        clear = function_body(PERSISTENCE, "clear_deferred_outbox_locked")
        self.assertIn("atomic_set(&deferred_outbox_presence, 0)", read)
        self.assertIn("atomic_set(&deferred_outbox_presence, 1)", read)
        self.assertIn("id == APP_MESH_NVS_DEFERRED_OUTBOX_ID", write)
        self.assertIn("atomic_set(&deferred_outbox_presence, 1)", write)
        self.assertIn("atomic_set(&deferred_outbox_presence, 0)", clear)

    def test_deferred_transactions_use_nonblocking_try_lock(self):
        functions = (
            "app_mesh_persistence_deferred_outbox_present",
            "app_mesh_persistence_clear_deferred_outbox",
            "app_mesh_persistence_save_deferred_outbox",
            "app_mesh_persistence_restore_deferred_outbox",
            "app_mesh_persistence_complete_deferred_outbox",
            "app_mesh_persistence_clear_deferred_outbox_if_matches",
        )
        for name in functions:
            body = function_body(PERSISTENCE, name)
            self.assertIn("deferred_outbox_try_lock", body, name)
            self.assertIn("deferred_outbox_unlock", body, name)
            self.assertNotIn("while (", body, name)
        clear_locked = function_body(PERSISTENCE, "clear_deferred_outbox_locked")
        self.assertIn("atomic_set(&deferred_outbox_presence, 0)", clear_locked)
        self.assertIn("deferred_outbox_busy", PERSISTENCE)

    def test_restore_promotes_before_retiring_deferred_copy(self):
        restore = function_body(
            PERSISTENCE, "app_mesh_persistence_restore_deferred_outbox"
        )
        validate = restore.index("mesh_relay_restore_outbox_snapshot")
        promote = restore.index("APP_MESH_NVS_OUTBOX_ID")
        verify = restore.index("verify_outbox_snapshot")
        clear = restore.rindex("clear_deferred_outbox_locked")
        self.assertLess(validate, promote)
        self.assertLess(promote, verify)
        self.assertLess(promote, clear)

    def test_restore_does_not_clear_transient_read_errors(self):
        restore = function_body(
            PERSISTENCE, "app_mesh_persistence_restore_deferred_outbox"
        )
        read_error = restore.index("if (read_ret < 0)")
        read_error_end = restore.index("return read_ret;", read_error)
        error_branch = restore[read_error:read_error_end]
        corrupt_branch = error_branch[error_branch.index("if (read_ret == -EBADMSG)") :]
        self.assertIn("read_ret == -EBADMSG", corrupt_branch)
        self.assertIn("clear_deferred_outbox_locked()", corrupt_branch)
        self.assertNotIn("return ret;", error_branch[: error_branch.index(corrupt_branch)])

    def test_save_retries_same_owner_and_rejects_conflicting_owner(self):
        save = function_body(PERSISTENCE, "app_mesh_persistence_save_deferred_outbox")
        self.assertIn("deferred_outbox_snapshots_match", save)
        self.assertRegex(save, r"\?\s*0\s*:\s*-EBUSY")
        self.assertIn("if (ret == -EBADMSG)", save)
        self.assertIn("} else if (ret < 0)", save)

    def test_real_persistence_fixture_uses_deferred_and_current_dependencies(self):
        fixture = (ROOT / "app/tests/mesh_persistence/CMakeLists.txt").read_text()
        source = (ROOT / "app/tests/mesh_persistence/src/main.c").read_text()
        self.assertIn("../../src/app_mesh_local_delivery.c", fixture)
        self.assertIn("../../../src/mesh_route_path.c", fixture)
        self.assertIn("../../../src/operation_policy.c", fixture)
        self.assertIn(".save_deferred_outbox = save_deferred_outbox_for_preempt", source)
        self.assertIn("app_mesh_persistence_test_fail_deferred_read", source)
        self.assertIn("test_deferred_outbox_contention_is_retryable", source)
        self.assertIn("test_deferred_outbox_same_snapshot_is_idempotent", source)

    def test_anchor_preemption_registers_deferred_owner(self):
        preempt = function_body(REPORT, "mesh_preempt_for_click_event")
        self.assertIn(".save_deferred_outbox = mesh_preempt_save_outbox", preempt)
        schedule = function_body(REPORT, "mesh_schedule_tx_timeout")
        self.assertIn("app_mesh_tx_timeout_work_needed", schedule)
        self.assertIn("deferred_pending", schedule)

    def test_ack_reconciles_deferred_before_active_clear(self):
        delivery = function_body(REPORT, "mesh_handle_result_actions")
        cleanup = delivery.index("app_mesh_persistence_clear_deferred_outbox_if_matches")
        active_clear = delivery.index("app_mesh_persistence_clear_outbox", cleanup)
        self.assertLess(cleanup, active_clear)

    def test_scheduler_predicate_keeps_deferred_owner(self):
        predicate = function_body(PREEMPTION, "app_mesh_tx_timeout_work_needed")
        self.assertIn("deferred_outbox_pending", predicate)
        self.assertIn("return relay_active", predicate)


if __name__ == "__main__":
    unittest.main()
