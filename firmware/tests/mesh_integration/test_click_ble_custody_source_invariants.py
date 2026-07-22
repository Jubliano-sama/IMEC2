#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
APP = read_composed_source(ROOT / "app" / "src" / "app_mesh_report.c")
RELAY = read_composed_source(ROOT / "src" / "mesh_relay.c")


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


if __name__ == "__main__":
    unittest.main()
