#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
BLE = (ROOT / "app/src/app_gateway_ble.c").read_text(encoding="utf-8")
REPORT = read_composed_source(ROOT / "app/src/app_mesh_report.c")
RETRY_HEADER = (ROOT / "app/src/app_gateway_eack_retry.h").read_text(
    encoding="utf-8"
)
RETRY = (ROOT / "app/src/app_gateway_eack_retry.c").read_text(
    encoding="utf-8"
)
COLLECTION = (ROOT / "app/src/app_gateway_collection_eack.c").read_text(
    encoding="utf-8"
)
GATEWAY_COMMAND = (ROOT / "src/gateway_command.c").read_text(encoding="utf-8")
MESH_RELAY = read_composed_source(ROOT / "src/mesh_relay.c")


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


class GatewayEackRetrySourceInvariantTests(unittest.TestCase):
    def test_total_send_failure_uses_fresh_random_exponential_retry(self):
        retry = function_body(BLE, "gateway_schedule_collection_eack_retry")
        worker = function_body(BLE, "gateway_collection_eack_work_handler")

        self.assertIn("gateway_send_collection_eack", worker)
        self.assertIn("gateway_schedule_collection_eack_retry", worker)
        self.assertIn("app_gateway_eack_retry_note_failure", retry)
        self.assertIn("app_gateway_eack_retry_note_failed_channel9_target", retry)
        self.assertIn("sys_rand32_get()", retry)
        self.assertNotIn("RELAY_BUSY_RETRY_MIN_MS", retry)

    def test_collection_and_persistence_workers_share_mesh_route_owner(self):
        schedule_round = function_body(
            BLE, "gateway_schedule_collection_eack_round"
        )
        schedule_retry = function_body(
            BLE, "gateway_schedule_collection_eack_retry"
        )
        complete_round = function_body(
            BLE, "gateway_complete_collection_eack_round"
        )
        schedule_persistence = function_body(
            BLE, "gateway_schedule_persistence_retry"
        )

        for body in (
            schedule_round,
            schedule_retry,
            complete_round,
            schedule_persistence,
        ):
            self.assertIn("mesh_route_work_reschedule", body)
            self.assertNotIn("k_work_reschedule", body)

        self.assertNotIn(
            "k_work_reschedule(&gateway_collection_eack_work", BLE
        )
        self.assertNotIn(
            "k_work_reschedule(&gateway_persistence_retry_work", BLE
        )

    def test_c5_pre_rf_deferral_remains_owned_by_eack_retry(self):
        callback = function_body(BLE, "gateway_eack_send_c5_flood")
        send = function_body(BLE, "gateway_send_collection_eack")
        resume = function_body(REPORT, "mesh_try_send_c5_flood_resume")
        priority = function_body(REPORT, "mesh_c5_flood_purpose_is_priority")

        self.assertIn("mesh_try_send_c5_flood_resume", callback)
        self.assertNotIn("mesh_send_c5_flood(", callback)
        self.assertIn("progress->complete", callback)
        self.assertIn("progress->next_opportunity", callback)
        self.assertIn("flood_result.sent_count", callback)
        self.assertIn("app_mesh_flood_repeat_limit()", callback)
        self.assertIn("force_c5_recovery = true", callback)
        self.assertIn("return -EAGAIN", callback)
        self.assertIn("app_mesh_flood_send_bounded_resume", resume)
        self.assertIn("progress->complete", resume)
        self.assertIn("result->sent_count", resume)
        self.assertIn("C5_CONTACT_PURPOSE_COLLECTION_EACK_FLOOD", priority)
        self.assertIn("struct app_mesh_flood_progress c5_flood_progress", RETRY_HEADER)
        self.assertIn("app_gateway_eack_retry_freeze", send)
        self.assertIn("gateway_schedule_collection_eack_retry", BLE)

    def test_collection_round_advances_only_after_send_success(self):
        body = function_body(BLE, "gateway_collection_eack_work_handler")
        send = body.index("gateway_send_collection_eack")
        failure = body.index("if (ret < 0)", send)
        failure_return = body.index("return;", failure)
        complete = body.index("gateway_complete_collection_eack_round")
        advance = function_body(
            BLE, "gateway_complete_collection_eack_round"
        )

        self.assertLess(send, failure)
        self.assertLess(failure, failure_return)
        self.assertLess(failure_return, complete)
        self.assertIn("gateway_collection_advance_retry_round", advance)

    def test_success_clears_custody_only_after_durable_round_advance(self):
        body = function_body(BLE, "gateway_complete_collection_eack_round")
        advance = body.index("gateway_collection_advance_retry_round")
        persist = body.index("gateway_persist_collection_state", advance)
        failure = body.index("if (ret < 0)", persist)
        failure_return = body.index("return;", failure)
        commit = body.index("app_gateway_eack_retry_commit_success", failure_return)
        clear = body.index("app_mesh_persistence_clear_gateway_eack_custody", commit)

        self.assertLess(advance, persist)
        self.assertLess(persist, failure)
        self.assertLess(failure, failure_return)
        self.assertLess(failure_return, commit)
        self.assertLess(commit, clear)
        retry_success = function_body(
            RETRY, "app_gateway_eack_retry_note_success"
        )
        self.assertIn("identity_equal", retry_success)
        commit_success = function_body(
            RETRY, "app_gateway_eack_retry_commit_success"
        )
        self.assertIn("retry_key(&state->identity)", commit_success)

    def test_retry_identity_includes_collection_and_protocol_round(self):
        body = function_body(RETRY, "collection_identity")

        for field in (
            "gateway_id",
            "command_seq",
            "collection_epoch_id",
            "retry_round",
            "eack_sequence",
        ):
            self.assertIn(field, body)

    def test_total_failure_policy_preserves_ch9_then_c5_sender(self):
        body = function_body(BLE, "gateway_collection_eack_work_handler")

        self.assertIn('gateway_send_collection_eack("collection-eack-round"', body)
        self.assertNotIn("gateway_eack_send_channel9", body)
        self.assertNotIn("gateway_eack_send_c5_flood", body)

    def test_result_and_bundle_accept_before_post_stream_retry_drive(self):
        real_gateway = BLE[
            BLE.index("static struct gateway_command_pending gateway_command_pending_state") :
        ]
        finalize = function_body(
            real_gateway, "gateway_finalize_semantic_delivery"
        )

        drive = function_body(BLE, "gateway_drive_collection_eack_after_record")
        pending = drive.index("gateway_collection_eack_retry_state.active")
        send = drive.index("gateway_send_collection_eack", pending)
        failure = drive.index("if (ret < 0)", send)
        retry = drive.index("gateway_schedule_collection_eack_retry", failure)
        success = drive.index("gateway_complete_collection_eack_round", retry)

        self.assertLess(pending, send)
        self.assertLess(send, failure)
        self.assertLess(failure, retry)
        self.assertLess(retry, success)
        self.assertIn("} else {", drive[retry:success])

        for function_name in (
            "gateway_note_collection_result",
            "gateway_note_collection_bundle",
        ):
            with self.subTest(function_name=function_name):
                body = function_body(BLE, function_name)
                self.assertIn("gateway_accept_collection_record", body)
                self.assertNotIn("gateway_drive_collection_eack_after_record", body)
        self.assertIn("gateway_drive_collection_eack_after_record", finalize)

    def test_each_successful_periodic_or_immediate_send_completes_one_round(self):
        for function_name in (
            "gateway_collection_eack_work_handler",
            "gateway_drive_collection_eack_after_record",
        ):
            with self.subTest(function_name=function_name):
                body = function_body(BLE, function_name)
                self.assertIn("gateway_complete_collection_eack_round", body)

    def test_failed_payload_is_frozen_and_reused_without_rebuilding(self):
        send = function_body(BLE, "gateway_send_collection_eack")
        retry_restore = send.index("app_gateway_eack_retry_restore")
        prepare = send.index("app_gateway_collection_eack_prepare", retry_restore)
        freeze = send.index("app_gateway_eack_retry_freeze", prepare)
        persist = send.index(
            "app_mesh_persistence_save_gateway_eack_custody", freeze
        )
        orchestration_send = send.index("app_gateway_collection_eack_send")
        failure = send.index("if (ret < 0)", orchestration_send)
        collection_send = function_body(
            COLLECTION, "app_gateway_collection_eack_send"
        )
        collection_prepare = function_body(
            COLLECTION, "app_gateway_collection_eack_prepare"
        )

        self.assertLess(retry_restore, prepare)
        self.assertLess(prepare, freeze)
        self.assertLess(freeze, persist)
        self.assertLess(persist, orchestration_send)
        self.assertLess(orchestration_send, failure)
        self.assertIn("use_prebuilt_eack", send)
        self.assertIn("app_gateway_collection_eack_prepare", collection_send)
        self.assertIn("use_prebuilt_eack", collection_prepare)
        self.assertIn("validate_prebuilt_eack", collection_prepare)

    def test_results_during_frozen_retry_are_deferred_to_later_round(self):
        body = function_body(BLE, "gateway_accept_collection_record")
        active = body.index("gateway_collection_eack_retry_state.active")
        changed = body.index("if (collection_changed)", active)
        dirty = body.index("gateway_collection_eack_round_dirty = true", changed)
        pending_return = body.index("return 0;", dirty)

        self.assertLess(active, changed)
        self.assertLess(changed, dirty)
        self.assertLess(dirty, pending_return)

        complete = function_body(BLE, "gateway_complete_collection_eack_round")
        self.assertIn("used_frozen_snapshot && gateway_collection_eack_round_dirty", complete)
        self.assertIn(
            "mesh_route_work_reschedule(&gateway_collection_eack_work, 0u)",
            complete,
        )

    def test_duplicate_after_final_commit_rearms_a_durable_eack(self):
        real_gateway = BLE[
            BLE.index("static struct gateway_command_pending gateway_command_pending_state") :
        ]
        accept = function_body(BLE, "gateway_accept_collection_record")
        finalize = function_body(
            real_gateway, "gateway_finalize_semantic_delivery"
        )
        result = function_body(BLE, "gateway_note_collection_result")
        bundle = function_body(BLE, "gateway_note_collection_bundle")

        reopen = accept.index("!gateway_collection_state.eack_pending")
        mark = accept.index("gateway_collection_state.eack_pending = true", reopen)
        persist = accept.index("gateway_persist_collection_state", mark)
        self.assertLess(reopen, mark)
        self.assertLess(mark, persist)
        self.assertIn("gateway_drive_collection_eack_after_record", finalize)
        self.assertIn("duplicate ?", result)
        self.assertIn("!duplicate", result)
        self.assertIn("duplicate_count", bundle)
        self.assertIn("accepted_count != 0u", bundle)

    def test_eack_builders_leave_outer_flood_retries_disabled(self):
        for body in (
            function_body(GATEWAY_COMMAND, "gateway_collection_prepare_eack_outbound"),
            function_body(
                GATEWAY_COMMAND,
                "gateway_collection_prepare_missing_eack_outbound",
            ),
        ):
            self.assertIn("memset(out, 0, sizeof(*out))", body)
            self.assertNotIn("flood_retry_count =", body)

    def test_packet_sequence_is_independent_of_saturating_backoff_round(self):
        builders = (
            function_body(GATEWAY_COMMAND, "gateway_collection_prepare_eack_outbound"),
            function_body(
                GATEWAY_COMMAND,
                "gateway_collection_prepare_missing_eack_outbound",
            ),
        )
        for body in builders:
            self.assertIn("collection->eack_sequence", body)
            self.assertNotIn("collection->retry_round + 1u", body)

        key = function_body(RETRY, "retry_key")
        self.assertIn("identity->eack_sequence", key)

    def test_receiver_binds_header_to_decoded_eack_identity(self):
        protocol = (ROOT / "src/protocol.c").read_text(encoding="utf-8")
        validate = function_body(
            protocol, "gateway_collection_eack_packet_validate"
        )
        relay_validate = function_body(
            MESH_RELAY, "collection_eack_broadcast_valid"
        )

        self.assertIn("packet->session_id != decoded.command_seq", validate)
        self.assertIn("packet->seq != decoded.packet_sequence", validate)
        self.assertIn("packet->src_id != decoded.gateway_id", validate)
        self.assertIn("packet->dst_id != 0u", validate)
        self.assertIn("gateway_collection_eack_packet_validate", relay_validate)

    def test_boot_resumes_only_persisted_pending_eack_identity(self):
        decoded_marker = BLE.index("struct gateway_collection_eack decoded_eack;")
        real_init = BLE.rfind(
            "void gateway_command_result_tracking_init", 0, decoded_marker
        )
        self.assertGreaterEqual(real_init, 0)
        init = function_body(
            BLE[real_init:], "gateway_command_result_tracking_init"
        )

        pending = init.index("!gateway_collection_state.eack_pending")
        restore = init.index(
            "app_mesh_persistence_restore_gateway_eack_custody", pending
        )
        import_custody = init.index("app_gateway_eack_retry_import_custody", restore)
        exact_validate = init.index(
            "gateway_collection_eack_packet_validate", import_custody
        )
        resume = init.index("collection-eack-reset-resume", exact_validate)
        final_gap = init.index("collection-eack-final-reset-gap", resume)

        self.assertLess(pending, restore)
        self.assertLess(restore, import_custody)
        self.assertLess(import_custody, exact_validate)
        self.assertLess(exact_validate, resume)
        self.assertLess(resume, final_gap)

    def test_eack_dedup_is_exact_round_not_whole_session(self):
        body = function_body(MESH_RELAY, "duplicate_matches_packet")

        self.assertNotIn("MSG_GATEWAY_COLLECTION_EACK", body)
        self.assertIn("entry->seq == packet->seq", body)

    def test_later_invocations_exclude_exact_failed_ch9_lanes(self):
        build = function_body(BLE, "gateway_send_collection_eack")
        send = function_body(COLLECTION, "app_gateway_collection_eack_send")

        self.assertIn("excluded_channel9_next_hop_ids", build)
        self.assertIn("failed_channel9_next_hop_ids", build)
        self.assertIn("return_target_excluded", send)
        self.assertIn("eligible_return_target_count", send)


if __name__ == "__main__":
    unittest.main()
