#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
APP = read_composed_source(ROOT / "app" / "src" / "app_mesh_report.c")
RELAY = read_composed_source(ROOT / "src" / "mesh_relay.c")
GATEWAY_BLE = (ROOT / "app" / "src" / "app_gateway_ble.c").read_text()
PERSISTENCE_H = (ROOT / "app" / "src" / "app_mesh_persistence.h").read_text()
PERSISTENCE_C = (ROOT / "app" / "src" / "app_mesh_persistence.c").read_text()
STREAM_C = (ROOT / "app" / "src" / "app_gateway_ble_stream.c").read_text()


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


def last_function_body(source: str, name: str) -> str:
    matches = list(re.finditer(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL))
    if not matches:
        raise AssertionError(f"function not found: {name}")
    start = matches[-1].end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {name}")


class ClickBleCustodySourceInvariantTests(unittest.TestCase):
    def test_click_reports_use_the_gateway_ble_custody_gate(self):
        app_gate = function_body(
            APP, "mesh_gateway_delivery_requires_semantic_acceptance"
        )
        relay_gate = function_body(RELAY, "gateway_delivery_requires_commit")

        for gate in (app_gate, relay_gate):
            self.assertIn("MSG_CLICK_REPORT", gate)
            self.assertIn("FLAG_GATEWAY_ACK_REQUIRED", gate)
            self.assertIn("packet->dst_id", gate)

    def test_reserve_commit_and_ack_order_is_single_path(self):
        drain = function_body(APP, "mesh_drain_rx_queue_locked")

        reserve = drain.index("gateway_ble_reserve_stream_packet")
        semantic = drain.index("mesh_gateway_accept_semantic_delivery", reserve)
        new_accept = drain.index("APP_GATEWAY_SEMANTIC_ACCEPT_NEW", semantic)
        stream_commit = drain.index("gateway_ble_commit_stream_reservation", new_accept)
        relay_commit = drain.index("mesh_relay_commit_gateway_delivery", stream_commit)
        ack_actions = drain.index("mesh_handle_result_actions", relay_commit)

        self.assertLess(reserve, semantic)
        self.assertLess(semantic, new_accept)
        self.assertLess(new_accept, stream_commit)
        self.assertLess(stream_commit, relay_commit)
        self.assertLess(relay_commit, ack_actions)
        self.assertEqual(drain.count("gateway_ble_commit_stream_reservation"), 1)
        self.assertIn("gateway_semantic_delivery_processed = true", drain)

    def test_click_journal_commit_precedes_ble_commit_and_gateway_ack(self):
        drain = function_body(APP, "mesh_drain_rx_queue_locked")

        journal_commit = drain.index(
            "app_mesh_persistence_save_gateway_click_journal"
        )
        stream_commit = drain.index("gateway_ble_commit_stream_reservation")
        relay_commit = drain.index("mesh_relay_commit_gateway_delivery")

        self.assertLess(journal_commit, stream_commit)
        self.assertLess(stream_commit, relay_commit)

    def test_click_semantic_acceptance_reuses_pending_journal_identity(self):
        accept = function_body(APP, "mesh_gateway_accept_semantic_delivery")
        click_case = accept.index("case MSG_CLICK_REPORT:")
        journal_match = accept.index(
            "app_mesh_persistence_gateway_click_journal_matches", click_case
        )
        duplicate = accept.index(
            "APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE", journal_match
        )

        self.assertLess(journal_match, duplicate)

    def test_gateway_click_journal_identity_excludes_mutable_transport_fields(self):
        identity = function_body(PERSISTENCE_C, "gateway_click_packet_matches")

        self.assertIn("left->flags == right->flags", identity)
        self.assertIn("left->src_id == right->src_id", identity)
        self.assertIn("left->dst_id == right->dst_id", identity)
        self.assertIn("left->session_id == right->session_id", identity)
        self.assertIn("left->seq == right->seq", identity)
        self.assertIn("left->payload_len == right->payload_len", identity)
        self.assertNotIn("left->ttl", identity)
        self.assertNotIn("left->message_age_ms", identity)

    def test_gateway_ble_clears_click_journal_only_after_notify_completion(self):
        tx_complete = function_body(GATEWAY_BLE, "gateway_ble_tx_complete")
        mark_sent = tx_complete.index("gateway_ble_stream_mark_sent")
        clear = tx_complete.index(
            "app_mesh_persistence_clear_gateway_click_journal_if_matches"
        )

        self.assertLess(mark_sent, clear)

    def test_gateway_ble_startup_restores_click_journal_before_transport_start(self):
        init = function_body(GATEWAY_BLE, "gateway_ble_init")
        stream_init = init.index("gateway_ble_stream_init")
        restore = init.index("gateway_restore_click_journal_runtime")
        bt_enable = init.index("bt_enable")

        self.assertLess(stream_init, restore)
        self.assertLess(restore, bt_enable)

    def test_click_journal_restore_reserves_a_free_stream_slot_before_borrowing_staging(self):
        restore = function_body(GATEWAY_BLE, "gateway_restore_click_journal_runtime")

        self.assertIn("gateway_ble_stream_state.count >= GATEWAY_BLE_STREAM_QUEUE_DEPTH", restore)
        self.assertIn("gateway_ble_stream_state.restore_staging_active", restore)

    def test_click_reservation_blocks_occupied_journal_before_stream_reservation(self):
        reserve = last_function_body(
            GATEWAY_BLE, "gateway_ble_reserve_stream_packet"
        )
        occupancy = reserve.index(
            "app_mesh_persistence_gateway_click_journal_matches"
        )
        stream_reserve = reserve.index("gateway_ble_stream_reserve_packet")

        self.assertIn("gateway_click_journal_restore_pending", reserve)
        self.assertIn("gateway_click_journal_clear_pending", reserve)
        self.assertLess(occupancy, stream_reserve)

    def test_gateway_click_journal_accepts_extended_payloads(self):
        self.assertRegex(
            PERSISTENCE_H,
            r"#define APP_MESH_GATEWAY_CLICK_JOURNAL_MAX_PAYLOAD_LEN\s+\\\s*"
            r"PACKET_EXT_MAX_PAYLOAD_LEN",
        )
        restore = function_body(
            GATEWAY_BLE, "gateway_restore_click_journal_runtime"
        )
        self.assertIn("PACKET_EXT_MAX_PAYLOAD_LEN", restore)
        self.assertIn("gateway_ble_stream_enqueue_staged_packet", restore)
        self.assertNotIn("PACKET_MAX_PAYLOAD_LEN", restore)

    def test_extended_restore_moves_overlapping_staging_before_header_build(self):
        staged = function_body(STREAM_C, "gateway_ble_stream_enqueue_staged_packet")
        self.assertIn("memmove(destination_payload", staged)
        self.assertIn("memmove(&record[offset]", STREAM_C)
        self.assertIn("gateway_click_clear_locked", PERSISTENCE_C)

    def test_permanent_payload_corruption_is_quarantined_but_io_errors_retry(self):
        read_payload = function_body(PERSISTENCE_C, "gateway_click_read_payload")
        restore = function_body(
            PERSISTENCE_C, "app_mesh_persistence_restore_gateway_click_journal"
        )
        self.assertIn("read_len == -ENOENT", read_payload)
        self.assertIn("return -EBADMSG", read_payload)
        self.assertIn("if (ret == -EBADMSG)", restore)
        self.assertIn("gateway_click_clear_locked", restore)

    def test_click_clear_retires_payload_before_marker(self):
        clear = function_body(PERSISTENCE_C, "gateway_click_clear_locked")
        payload = clear.index("APP_MESH_NVS_GATEWAY_CLICK_PAYLOAD_ID")
        metadata = clear.index("APP_MESH_NVS_GATEWAY_CLICK_METADATA_ID")
        self.assertLess(payload, metadata)

    def test_malformed_click_is_rejected_before_semantic_acceptance(self):
        accept = function_body(APP, "mesh_gateway_accept_semantic_delivery")
        click_case = accept.index("case MSG_CLICK_REPORT:")
        validate = accept.index("report_validate_click_payload", click_case)
        accepted = accept.index("APP_GATEWAY_SEMANTIC_ACCEPT_NEW", validate)

        self.assertLess(click_case, validate)
        self.assertLess(validate, accepted)
        self.assertIn("return -EINVAL", accept[validate:accepted])

    def test_duplicate_and_rejected_clicks_cancel_reserved_host_custody(self):
        drain = function_body(APP, "mesh_drain_rx_queue_locked")

        rejected = drain.index("if (semantic_ret < 0)")
        first_cancel = drain.index("gateway_ble_cancel_stream_reservation", rejected)
        self.assertIn("APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE", drain)
        duplicate = drain.index("APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE", rejected)
        duplicate_cancel = drain.index(
            "gateway_ble_cancel_stream_reservation", duplicate
        )

        self.assertLess(rejected, first_cancel)
        self.assertLess(duplicate, duplicate_cancel)
        self.assertGreaterEqual(drain.count("gateway_ble_cancel_stream_reservation"), 2)

    def test_custody_gated_clicks_do_not_fall_through_best_effort_streaming(self):
        drain = function_body(APP, "mesh_drain_rx_queue_locked")
        stream = drain.index("gateway_ble_stream_packet")
        processed = drain.index("gateway_semantic_delivery_processed = true")
        best_effort_guard = drain.rfind("!gateway_semantic_delivery_processed", 0, stream)

        self.assertLess(processed, stream)
        self.assertGreaterEqual(best_effort_guard, processed)
        self.assertIn("gateway_ble_stream_packet", drain[best_effort_guard:])

    def test_click_preempted_outbox_resumes_in_the_same_boot(self):
        timeout = function_body(APP, "mesh_tx_timeout_handler")

        restore = timeout.index("app_mesh_persistence_restore_outbox")
        tick = timeout.index("mesh_relay_tick_with_random")
        self.assertLess(restore, tick)
        self.assertIn("!mesh_relay_tx_active(&mesh_runtime)", timeout[:restore])

    def test_paused_click_window_rearms_deferred_outbox_work(self):
        timeout = function_body(APP, "mesh_tx_timeout_handler")
        paused = timeout.index("if (mesh_transport_paused())")
        rearm = timeout.index("mesh_reschedule_delayable", paused)
        early_return = timeout.index("return;", paused)

        self.assertLess(paused, rearm)
        self.assertLess(rearm, early_return)
        self.assertIn("REPORT_TX_RETRY_DELAY_MS", timeout[rearm:early_return])


if __name__ == "__main__":
    unittest.main()
