#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
APP_SRC = ROOT / "app" / "src"
REPORT = read_composed_source(APP_SRC / "app_mesh_report.c")
REPORT_HEADER = (APP_SRC / "app_mesh_report.h").read_text(encoding="utf-8")
REPORT_ENCODER = (APP_SRC / "app_mesh_report_encode.c").read_text(
    encoding="utf-8"
)
ROUTE_WAIT = (APP_SRC / "app_mesh_route_wait_tx.c").read_text(encoding="utf-8")
ROUTE_WAIT_HEADER = (APP_SRC / "app_mesh_route_wait_tx.h").read_text(
    encoding="utf-8"
)
ANCHOR_RADIO = (APP_SRC / "app_anchor_radio.inc").read_text(encoding="utf-8")
RELAY = read_composed_source(ROOT / "src" / "mesh_relay.c")
RELAY_HEADER = (ROOT / "include" / "mesh_relay.h").read_text(encoding="utf-8")
RELAY_DELIVERY = (ROOT / "src" / "mesh_relay_delivery.inc").read_text(
    encoding="utf-8"
)
RX_POLICY = (APP_SRC / "app_mesh_rx_policy.c").read_text(encoding="utf-8")
STACK_BUDGET = (ROOT / "include" / "stack_budget.h").read_text(encoding="utf-8")


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


class MeshRamRebootContractSourceInvariantTests(unittest.TestCase):
    def test_production_has_no_fake_mesh_persistence_owner(self) -> None:
        forbidden = (
            "mesh_save_outbox_durable",
            "mesh_save_child_custody_durable",
            "mesh_outbox_persistence_dirty",
            "mesh_child_custody_persistence_dirty",
            "mesh_persistence_retry_work",
            "mesh_deferred_outbox_pending",
            "route_state_durable",
            "mesh_relay_mark_route_state_durable",
            "persistent_outbox_record",
            "APP_MESH_ROUTE_WAIT_TX_OWNER_DURABLE_LOCAL",
            "ANCHOR_RANGE_FRAGMENT_PERSISTENCE_RETRY_MS",
            "ANCHOR_RANGE_REPORT_PERSISTENCE_DEADLINE_MS",
            "persistence_deadline_ms",
            "mesh_restore_anchor_range_report_journal",
            "anchor_range_report_journal_runtime",
            "persistence_fail_closed",
            "fragment_pending",
            "final_fragment_staged",
        )
        production = (
            REPORT
            + REPORT_HEADER
            + REPORT_ENCODER
            + ROUTE_WAIT
            + ROUTE_WAIT_HEADER
            + RELAY
            + RELAY_HEADER
        )
        for symbol in forbidden:
            with self.subTest(symbol=symbol):
                self.assertNotIn(symbol, production)
        self.assertNotIn("mesh_persistence_retry_work_handler", STACK_BUDGET)

    def test_anchor_range_report_uses_only_ram_batch_and_ack_owners(self) -> None:
        self.assertIn("struct anchor_range_report_ack_runtime", REPORT)
        self.assertIn("struct anchor_range_report_control control", REPORT)
        self.assertIn("int queue_error", REPORT)
        self.assertIn("bool queue_admission_fail_closed", REPORT)
        self.assertNotIn("durable report ownership", ANCHOR_RADIO)
        self.assertNotIn("persistence boundary", ANCHOR_RADIO)
        self.assertNotIn("exact durable result", RELAY_DELIVERY)

    def test_tx_timeout_uses_only_live_ram_owners(self) -> None:
        schedule = function_body(REPORT, "mesh_schedule_tx_timeout")
        predicate = re.search(
            r"app_mesh_tx_timeout_work_needed\s*\((?P<args>.*?)\)\s*\)",
            schedule,
            re.DOTALL,
        )
        self.assertIsNotNone(predicate)
        args = predicate.group("args")
        self.assertIn("mesh_relay_tx_active", args)
        self.assertIn("mesh_ch9_tx_pending_is_active", args)
        self.assertIn("mesh_relay_result_bundle_pending", args)
        self.assertNotIn("deferred", args)

    def test_preboot_route_adv_gate_precedes_queue_admission(self) -> None:
        declaration = REPORT.index(
            "static bool mesh_queue_from_frame_at_internal("
        )
        queue_start = REPORT.index(
            "static bool mesh_queue_from_frame_at_internal(", declaration + 1
        )
        queue_end = REPORT.index(
            "\nstatic bool mesh_queue_from_frame_at(", queue_start
        )
        queue = REPORT[queue_start:queue_end]
        envelope = queue.index("mesh_packet_rx_envelope_validate(")
        expand = queue.index("received_uptime_ms = mesh_expand_uptime32(")
        gate = queue.index("app_mesh_rx_policy_postboot_route_adv_fresh(")
        valid = queue.index("*valid_mesh_frame = true")
        enqueue = queue.index("k_msgq_put(")
        self.assertLess(envelope, expand)
        self.assertLess(expand, gate)
        self.assertLess(gate, valid)
        self.assertLess(valid, enqueue)
        self.assertIn("uint64_t received_uptime_ms", queue)

    def test_route_adv_age_policy_is_64_bit_and_fail_closed(self) -> None:
        policy = function_body(
            RX_POLICY, "app_mesh_rx_policy_postboot_route_adv_fresh"
        )
        self.assertIn("uint64_t received_uptime_ms", RX_POLICY)
        self.assertIn("msg_type != MSG_GATEWAY_ROUTE_ADV", policy)
        self.assertIn("(uint64_t)message_age_ms <= received_uptime_ms", policy)

    def test_snapshot_helpers_are_explicitly_model_only(self) -> None:
        self.assertIn("Serialization-neutral RAM-model helper", RELAY)
        self.assertIn("Production does not write this", RELAY)
        self.assertIn("all-node reboot requires the operation to be rerun", RELAY)


if __name__ == "__main__":
    unittest.main()
