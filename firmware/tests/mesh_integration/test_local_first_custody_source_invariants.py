#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
REPORT = read_composed_source(ROOT / "app" / "src" / "app_mesh_report.c")


def function_body(source: str, name: str, *, required: bool = True) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    if match is None:
        if required:
            raise AssertionError(f"function not found: {name}")
        return ""
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


class LocalFirstCustodySourceInvariantTests(unittest.TestCase):
    def assert_contains(self, needle: str, source: str, context: str):
        if needle not in source:
            self.fail(f"{context}: missing {needle}")

    def assert_absent(self, needle: str, source: str, context: str):
        if needle in source:
            self.fail(f"{context}: forbidden {needle}")

    def test_local_priority_reclaim_does_not_erase_existing_custody(self):
        body = function_body(
            REPORT, "mesh_reclaim_for_local_origin_priority", required=False
        )
        if not body:
            return

        destructive_actions = (
            "mesh_relay_cancel_tx",
            "app_mesh_persistence_clear_outbox",
            "mesh_route_waiting_tx_valid = false",
            "mesh_ch9_tx_pending_clear",
            "mesh_close_channel9_connection",
            "mesh_ch9_ack_batch_clear_for_peer",
            "mesh_relay_abandon_transit_reservations",
        )
        for action in destructive_actions:
            self.assert_absent(action, body, "local-priority reclaim")

    def test_report_selection_promotes_local_work_under_the_queue_owner(self):
        promote = function_body(REPORT, "report_tx_queue_promote_local_locked")
        peek = function_body(REPORT, "report_tx_queue_peek")
        begin = function_body(REPORT, "report_tx_queue_begin_head")

        self.assert_contains(
            "mesh_outbound_is_local_origin_priority", promote, "local promotion"
        )
        self.assert_contains(
            "k_msgq_num_used_get", promote, "local promotion bound"
        )
        self.assert_contains("queue_count", promote, "local promotion bound")
        self.assert_contains("k_msgq_get", promote, "local promotion")
        self.assert_contains("k_msgq_put", promote, "local promotion")
        self.assert_contains(
            "app_mesh_queue_head_owned", promote, "local promotion"
        )
        self.assert_contains(
            "mesh_preempt_report_queue_recover", promote, "local promotion"
        )
        self.assert_absent(
            "app_mesh_paused_delivery_note_drop", promote, "local promotion"
        )
        owner_check = promote.index("app_mesh_queue_head_owned")
        first_pop = promote.index("k_msgq_get")
        self.assertLess(owner_check, first_pop)
        self.assertLess(
            peek.index("report_tx_queue_promote_local_locked"),
            peek.index("k_msgq_peek"),
        )
        self.assertLess(
            begin.index("report_tx_queue_promote_local_locked"),
            begin.index("app_mesh_queue_head_begin"),
        )

    def test_queue_capacity_never_accounts_accepted_transit_as_lost(self):
        body = function_body(REPORT, "queue_anchor_report")

        self.assert_absent(
            "mesh_reclaim_for_local_origin_priority", body, "queue admission"
        )
        self.assert_absent(
            "APP_MESH_QUEUE_RESERVE_REPLACE_TRANSIT_ACCOUNT_LOSS",
            body,
            "queue admission",
        )
        self.assert_absent(
            "APP_MESH_QUEUE_RESERVE_REPLACE_LOCAL_ACCOUNT_LOSS",
            body,
            "queue admission",
        )

        # If a full queue is rotated to admit local work, the displaced packet
        # must become the one recovery owner before the new packet is accepted.
        first_pop = body.find("k_msgq_get")
        if first_pop >= 0:
            recovery = body.find("report_tx_queue_recovery_valid = true", first_pop)
            self.assertGreater(recovery, first_pop)
            self.assert_absent(
                "app_mesh_paused_delivery_note_drop",
                body[first_pop:recovery],
                "queue displacement handoff",
            )

    def test_priority_calls_do_not_cancel_route_or_channel9_custody(self):
        for name in (
            "mesh_queue_anchor_cir_fragment",
            "report_tx_work_handler",
            "queue_anchor_report",
        ):
            with self.subTest(name=name):
                body = function_body(REPORT, name)
                self.assert_absent(
                    "mesh_reclaim_for_local_origin_priority", body, name
                )

    def test_direct_ack_miss_keeps_durable_core_custody(self):
        body = function_body(REPORT, "mesh_handle_direct_gateway_retry_policy")

        self.assert_contains(
            "mesh_relay_tx_active(&mesh_runtime)",
            body,
            "direct gateway ACK miss",
        )
        self.assert_contains(
            "mesh_schedule_tx_timeout()",
            body,
            "direct gateway ACK miss",
        )
        self.assert_absent(
            "mesh_relay_cancel_tx",
            body,
            "direct gateway ACK miss",
        )
        self.assert_absent(
            "app_mesh_persistence_clear_outbox",
            body,
            "direct gateway ACK miss",
        )
        self.assert_absent(
            "mesh_store_route_waiting_tx",
            body,
            "direct gateway ACK miss",
        )


if __name__ == "__main__":
    unittest.main()
