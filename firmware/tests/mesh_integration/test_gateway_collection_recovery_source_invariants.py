#!/usr/bin/env python3
from pathlib import Path
import re
import unittest


FIRMWARE = Path(__file__).resolve().parents[2]
APP = FIRMWARE / "app"
RESULT_RUNTIME = (APP / "src/app_gateway_result_runtime.inc").read_text()
COLLECTION_RUNTIME = (APP / "src/app_gateway_collection_runtime.inc").read_text()
RECOVERY_HEADER = (APP / "src/app_gateway_collection_recovery.h").read_text()
RECOVERY_SOURCE = (APP / "src/app_gateway_collection_recovery.c").read_text()
DELIVERY = (APP / "src/app_mesh_report_delivery.inc").read_text()
GATEWAY_COMMAND_HEADER = (FIRMWARE / "include/gateway_command.h").read_text()
GATEWAY_COMMAND_SOURCE = (FIRMWARE / "src/gateway_command.c").read_text()
GATEWAY_COMMAND_TEST = (FIRMWARE / "tests/test_gateway_command.c").read_text()
ANCHOR_INIT = (APP / "src/app_anchor_init.inc").read_text()
APP_CMAKE = (APP / "CMakeLists.txt").read_text()
FIRMWARE_CMAKE = (FIRMWARE / "CMakeLists.txt").read_text()


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    if match is None:
        raise AssertionError(f"missing function {name}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(f"unterminated function {name}")


class GatewayCollectionRecoverySourceInvariants(unittest.TestCase):
    def test_collection_history_has_no_durable_snapshot_or_hot_nvs_path(self):
        for retired_symbol in (
            "GATEWAY_COLLECTION_STATE_SNAPSHOT_VERSION",
            "GATEWAY_COLLECTION_STATE_PERSISTENCE_VERSION",
            "gateway_collection_state_snapshot",
            "gateway_collection_export_snapshot",
            "gateway_collection_restore_snapshot",
            "persistence_version",
            "persistence_valid",
        ):
            with self.subTest(retired_symbol=retired_symbol):
                self.assertNotIn(retired_symbol, GATEWAY_COMMAND_HEADER)
                self.assertNotIn(retired_symbol, GATEWAY_COMMAND_SOURCE)
                self.assertNotIn(retired_symbol, GATEWAY_COMMAND_TEST)

        for retired_owner in (
            "gateway_collection_persistence_dirty",
            "gateway_collection_clear_pending",
            "gateway_collection_restore_pending",
            "gateway_eack_custody_clear_pending",
            "gateway_persist_collection_state",
            "gateway_clear_eack_custody_or_defer",
            "gateway_restore_collection_runtime",
        ):
            with self.subTest(retired_owner=retired_owner):
                self.assertNotIn(retired_owner, RESULT_RUNTIME)
                self.assertNotIn(retired_owner, COLLECTION_RUNTIME)

        self.assertNotIn("app_durable_state", COLLECTION_RUNTIME)
        self.assertNotIn("nvs_", COLLECTION_RUNTIME)
        self.assertNotIn("app_durable_state", RECOVERY_HEADER)
        self.assertNotIn("app_durable_state", RECOVERY_SOURCE)
        self.assertNotIn("nvs_", RECOVERY_SOURCE)
        self.assertIn(
            "RAM-only recovery owner exceeded its bounded gateway budget",
            RECOVERY_HEADER,
        )
        self.assertNotIn("#if 0", GATEWAY_COMMAND_TEST)

        collection_clear = function_body(
            RESULT_RUNTIME, "gateway_finalize_collection_clear"
        )
        self.assertNotIn("app_durable_state", collection_clear)
        self.assertNotIn("nvs_", collection_clear)

    def test_recovery_is_raw_validated_then_host_receipt_gated(self):
        preflight = function_body(
            RESULT_RUNTIME, "gateway_preflight_result_semantic_delivery"
        )
        bundle_validate = preflight.index("gateway_validate_raw_result_bundle(")
        bundle_recovery = preflight.index(
            "app_gateway_collection_recovery_preflight(", bundle_validate
        )
        command_validate = preflight.index("gateway_validate_raw_command_result(")
        command_recovery = preflight.index(
            "app_gateway_collection_recovery_preflight(", bundle_recovery + 1
        )
        self.assertLess(bundle_validate, bundle_recovery)
        self.assertLess(command_validate, command_recovery)

        recovery = function_body(
            RESULT_RUNTIME, "gateway_collection_recovery_after_host_receipt"
        )
        for required in (
            "app_gateway_control_sequence_next(&recovery_attempt_id)",
            "app_gateway_collection_recovery_begin(",
            "mesh_try_send_c5_flood_resume(",
            "C5_CONTACT_PURPOSE_COLLECTION_EACK_FLOOD",
            "gateway_collection_recovery.host_custody_pending",
        ):
            with self.subTest(required=required):
                self.assertIn(required, recovery)
        self.assertIn("return -EAGAIN;", recovery)
        self.assertLess(
            recovery.index("app_gateway_collection_recovery_begin("),
            recovery.index("mesh_try_send_c5_flood_resume("),
        )
        self.assertNotIn(
            "app_gateway_collection_recovery_reset(&gateway_collection_recovery)",
            recovery,
        )
        self.assertIn("eack->gateway_epoch == current_gateway_epoch", RECOVERY_SOURCE)

        complete = function_body(DELIVERY, "mesh_complete_gateway_host_delivery_locked")
        recovery_finalize = complete.index(
            "gateway_collection_recovery_after_host_receipt("
        )
        ble_retire = complete.index("gateway_ble_finish_host_delivery(")
        self.assertLess(recovery_finalize, ble_retire)
        self.assertIn(
            "semantic_ret != APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECOVERY",
            complete,
        )
        self.assertIn("APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECOVERY", complete)
        self.assertLess(
            complete.index("gateway_ble_finish_host_delivery("),
            complete.index("gateway_collection_recovery_finish_host_delivery("),
        )

        reserve = DELIVERY.index("gateway_collection_recovery_reserve_host_custody(")
        stream_commit = DELIVERY.index("gateway_ble_commit_stream_reservation_projection(")
        self.assertLess(reserve, stream_commit)
        self.assertIn("gateway_collection_recovery_cancel_host_custody(", DELIVERY)
        self.assertIn("gateway_collection_recovery.host_custody_pending", COLLECTION_RUNTIME)

    def test_production_publisher_reserves_transport_identity(self):
        self.assertIn(
            ".reserve_event_seq = gateway_reserve_command_event_sequence",
            ANCHOR_INIT,
        )

    def test_restored_publication_is_marked_replay_before_exposure(self):
        replay = function_body(
            RESULT_RUNTIME, "gateway_replay_pending_assignment_publication"
        )
        prepare = replay.index(
            "app_gateway_assignment_publisher_prepare_table("
        )
        replay_mark = replay.index(
            "app_gateway_assignment_publisher_mark_prepared_replay(", prepare
        )
        commit = replay.index(
            "app_gateway_assignment_publisher_commit_prepared_batch(",
            replay_mark,
        )
        self.assertLess(prepare, replay_mark)
        self.assertLess(replay_mark, commit)
        self.assertIn("cold_boot_replay = !gateway_membership_publication_live_owner", replay)
        replay_gate = replay.rfind("if (cold_boot_replay)", prepare, replay_mark)
        self.assertGreaterEqual(replay_gate, 0)
        self.assertLess(replay_gate, replay_mark)

    def test_recovery_owner_is_in_each_build_and_test_gate(self):
        self.assertIn("src/app_gateway_collection_recovery.c", APP_CMAKE)
        self.assertIn("test_app_gateway_collection_recovery", FIRMWARE_CMAKE)
        self.assertIn("app_gateway_collection_recovery", FIRMWARE_CMAKE)


if __name__ == "__main__":
    unittest.main()
