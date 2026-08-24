#!/usr/bin/env python3
"""Cross-owner invariants for the DDD enumeration failure class."""

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GATEWAY_CONTROL = (
    ROOT / "app/src/app_anchor_gateway_control.inc"
).read_text(encoding="utf-8")
RX_POLICY = (ROOT / "app/src/app_mesh_rx_policy.c").read_text(
    encoding="utf-8"
)
REPORT_RX = (ROOT / "app/src/app_mesh_report_rx.inc").read_text(
    encoding="utf-8"
)
ANCHOR_RADIO = (ROOT / "app/src/app_anchor_radio.inc").read_text(
    encoding="utf-8"
)
OBSERVABILITY = (
    ROOT / "app/src/app_gateway_command_observability.c"
).read_text(encoding="utf-8")
OBSERVABILITY_HEADER = (
    ROOT / "app/src/app_gateway_command_observability.h"
).read_text(encoding="utf-8")
BLE = (ROOT / "app/src/app_gateway_ble.c").read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.S)
    if match is None:
        raise AssertionError(f"missing function {name}")
    start = match.start()
    brace = source.index("{", match.start())
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
        if depth == 0:
            return source[start:index + 1]
    raise AssertionError(f"unterminated function {name}")


class DddRfObservabilityContractTests(unittest.TestCase):
    def test_queue_admission_is_not_a_physical_flood_attempt(self) -> None:
        open_round = function_body(
            GATEWAY_CONTROL,
            "gateway_discovery_assignment_open_claim_round_locked",
        )
        observe_rf = function_body(
            GATEWAY_CONTROL,
            "gateway_discovery_assignment_observe_rf_start_locked",
        )

        self.assertNotIn("GATEWAY_COMMAND_EVENT_STAGE_FLOOD_ATTEMPT", open_round)
        attempts = observe_rf.index("attempts_started == 0u")
        flood = observe_rf.index(
            "GATEWAY_COMMAND_EVENT_STAGE_FLOOD_ATTEMPT", attempts
        )
        self.assertLess(attempts, flood)

    def test_claim_partial_send_precedes_strict_nonclaim_radio_terminal(
        self,
    ) -> None:
        service = function_body(
            GATEWAY_CONTROL,
            "gateway_discovery_assignment_service_delivery",
        )
        transport_delivered = service.index(
            "effective_delivered = "
            "event.reason == NODE_COMM_TERMINAL_DELIVERED"
        )
        claim = service.index(
            "kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM",
            transport_delivered,
        )
        semantic = service.index(
            "app_discovery_assignment_semantic_terminal_success(", claim
        )
        table = service.index(
            "kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE",
            semantic,
        )
        compact_rx = service.index(
            'mesh_start_uwb_rx("compact-enumeration")', table
        )
        hard_terminal = service.index(
            "gateway_discovery_assignment_state.round_open = false;",
            compact_rx,
        )
        radio_terminal = service.index(
            "gateway_discovery_assignment_fail_locked(COMMAND_RADIO_ERROR",
            hard_terminal,
        )
        self.assertEqual(
            service.count(
                "app_discovery_assignment_semantic_terminal_success("
            ),
            1,
        )
        self.assertLess(transport_delivered, claim)
        self.assertLess(claim, semantic)
        self.assertLess(semantic, table)
        self.assertLess(table, compact_rx)
        self.assertLess(compact_rx, hard_terminal)
        self.assertLess(hard_terminal, radio_terminal)
        self.assertNotIn(
            "GATEWAY_COMMAND_EVENT_REASON_NO_ANCHORS",
            service[hard_terminal:radio_terminal],
        )

    def test_hard_recovery_churn_cannot_refresh_watchdog_progress(self) -> None:
        classify = function_body(
            RX_POLICY, "app_mesh_rx_policy_dwm_attempt_made_progress"
        )
        self.assertIn("ret == 0", classify)
        self.assertIn("ret == -ETIMEDOUT", classify)
        self.assertIn("ret == -ECANCELED", classify)
        self.assertIn("ret == -EMSGSIZE", classify)
        self.assertNotIn("ret == -EIO", classify)
        self.assertNotIn(
            "app_mesh_rx_policy_gateway_ch9_rx_error_recoverable",
            classify,
        )
        self.assertNotIn(
            "app_anchor_rx_failure_detected_preamble", classify
        )

        # A later hard result must replace, rather than OR with, an earlier
        # functional result in a multi-attempt work invocation.
        self.assertNotRegex(
            REPORT_RX,
            r"functional_rx_outcome\s*=\s*functional_rx_outcome\s*\|\|",
        )
        classifications = re.findall(
            r"functional_rx_outcome\s*=\s*\n?\s*"
            r"app_mesh_rx_policy_dwm_attempt_made_progress\s*\(",
            REPORT_RX,
        )
        self.assertGreaterEqual(len(classifications), 4)

        progress_sites = [
            match.start()
            for match in re.finditer(
                r"app_watchdog_note_radio_progress\s*\(\s*\)", REPORT_RX
            )
        ]
        self.assertGreaterEqual(len(progress_sites), 2)
        for progress in progress_sites:
            guard = REPORT_RX.rfind(
                "else if (functional_rx_outcome)", 0, progress
            )
            self.assertGreater(guard, -1)
            self.assertNotIn("}", REPORT_RX[guard:progress])

        anchor_scan = function_body(
            ANCHOR_RADIO, "anchor_uwb_scan_work_handler"
        )
        final_release = anchor_scan.index(
            "release_ret = radio_guard_uwb_release_finish(&radio_lease,"
        )
        final_progress = anchor_scan.index(
            "app_watchdog_note_radio_progress();", final_release
        )
        progress_guard = anchor_scan.rfind("if (low_power_ret == 0", 0,
                                            final_progress)
        self.assertGreater(progress_guard, final_release)
        self.assertIn(
            "app_mesh_rx_policy_dwm_attempt_made_progress(ret, rx_failure)",
            anchor_scan[progress_guard:final_progress],
        )
        self.assertNotIn(
            "app_anchor_rx_failure_detected_preamble",
            anchor_scan[progress_guard:final_progress],
        )

    def test_compact_enumeration_progress_requires_successful_radio_finish(
        self,
    ) -> None:
        compact = function_body(
            REPORT_RX, "mesh_gateway_run_enumeration_response_slice"
        )

        evidence = compact.index("bool functional_rx_outcome = false;")
        receive = compact.index(
            "dwm3000_driver_receive_frame_continuous(", evidence
        )
        classify = compact.index(
            "functional_rx_outcome =\n"
            "            app_mesh_rx_policy_dwm_attempt_made_progress(",
            receive,
        )
        hard_receive = compact.index("if (ret < 0)", classify)
        encode_failure = compact.index(
            "functional_rx_outcome = false;", hard_receive
        )
        ack_send = compact.index(
            "dwm3000_driver_send_frame_tracked_until(", encode_failure
        )
        hard_ack = compact.index("if (ack_ret < 0)", ack_send)
        clear_after_ack = compact.index(
            "functional_rx_outcome = false;", hard_ack
        )

        # Every receive replaces the prior observation, so a hard receive in
        # a later loop iteration erases earlier functional evidence. A hard
        # ACK or ACK-encode result clears it explicitly in the same iteration.
        self.assertLess(receive, classify)
        self.assertLess(classify, hard_receive)
        self.assertNotIn(
            "functional_rx_outcome ||", compact[receive:hard_receive]
        )
        self.assertLess(hard_receive, encode_failure)
        self.assertLess(encode_failure, ack_send)
        self.assertLess(ack_send, hard_ack)
        self.assertLess(hard_ack, clear_after_ack)

        parking = compact.index(
            "mesh_radio_idle_with_bounded_recovery(", clear_after_ack
        )
        finish = compact.index("mesh_rx_radio_finish(&lease, parking_ret)", parking)
        release_failure = compact.index("if (release_ret < 0)", finish)
        release_failure_block = compact[
            release_failure:compact.index("} else if", release_failure)
        ]
        progress_guard = compact.index(
            "else if (functional_rx_outcome)", release_failure
        )
        progress = compact.index(
            "app_watchdog_note_radio_progress();", progress_guard
        )

        self.assertEqual(compact.count("app_watchdog_note_radio_progress();"), 1)
        self.assertLess(parking, finish)
        self.assertLess(finish, release_failure)
        self.assertIn("return true;", release_failure_block)
        self.assertLess(release_failure, progress_guard)
        self.assertLess(progress_guard, progress)
        self.assertNotIn(
            "app_watchdog_note_radio_progress();", compact[:finish]
        )

    def test_disconnect_replay_is_ram_bounded_until_receipt_or_reset(self) -> None:
        self.assertIn(
            "sizeof(struct gateway_command_observability_state) <=\n"
            "               GATEWAY_COMMAND_OBSERVABILITY_RAM_BUDGET_BYTES",
            OBSERVABILITY,
        )
        self.assertIn(
            "#define GATEWAY_COMMAND_EVENT_TERMINAL_BACKLOG_DEPTH 5u",
            OBSERVABILITY_HEADER,
        )
        self.assertIn(
            "#define GATEWAY_COMMAND_OBSERVABILITY_RAM_BUDGET_BYTES 912u",
            OBSERVABILITY_HEADER,
        )
        self.assertNotIn("nvs", OBSERVABILITY.lower())

        disconnected = function_body(BLE, "gateway_ble_disconnected")
        self.assertIn("gateway_ble_stream_cancel_active()", disconnected)
        self.assertIn('gateway_ble_schedule_recovery("disconnected")', disconnected)
        self.assertNotIn("gateway_command_observability_init", disconnected)
        self.assertNotIn("gateway_ble_stream_init", disconnected)

        receipt = function_body(BLE, "gateway_command_event_finish_host_receipt")
        publisher = receipt.index(
            "app_gateway_assignment_publisher_note_host_receipt"
        )
        stream_retire = receipt.index("gateway_ble_stream_mark_sent", publisher)
        event_retire = receipt.index(
            "gateway_observability_mark_sent_state", stream_retire
        )
        self.assertLess(publisher, stream_retire)
        self.assertLess(stream_retire, event_retire)

        initialize = function_body(BLE, "gateway_ble_init")
        self.assertIn("gateway_ble_stream_init", initialize)
        self.assertIn("gateway_command_observability_init", initialize)


if __name__ == "__main__":
    unittest.main()
