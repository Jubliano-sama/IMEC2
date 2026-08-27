#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
APP_SRC = ROOT / "app" / "src"


def function_body(source: str, name: str) -> str:
    for candidate in re.finditer(rf"\b{re.escape(name)}\s*\(", source):
        paren = source.index("(", candidate.start())
        depth = 0
        for index in range(paren, len(source)):
            depth += source[index] == "("
            depth -= source[index] == ")"
            if depth != 0:
                continue
            next_index = index + 1
            while next_index < len(source) and source[next_index].isspace():
                next_index += 1
            if next_index >= len(source) or source[next_index] != "{":
                break
            depth = 0
            for end in range(next_index, len(source)):
                depth += source[end] == "{"
                depth -= source[end] == "}"
                if depth == 0:
                    return source[candidate.start() : end + 1]
            raise AssertionError(f"unterminated function: {name}")
    raise AssertionError(f"function definition not found: {name}")


class GatewayHostReceiptPrioritySourceInvariantTests(unittest.TestCase):
    def setUp(self):
        self.ble = (APP_SRC / "app_gateway_ble.c").read_text(encoding="utf-8")
        self.ble_header = (APP_SRC / "app_gateway_ble.h").read_text(
            encoding="utf-8"
        )
        self.report = read_composed_source(APP_SRC / "app_mesh_report.c")
        self.report_header = (APP_SRC / "app_mesh_report.h").read_text(
            encoding="utf-8"
        )

    def test_complete_receipt_is_published_only_after_queue_admission(self):
        queue = function_body(self.ble, "gateway_ble_queue_frame")

        classify = queue.index(
            "host_receipt = gateway_ble_pending_is_host_receipt(&pending)"
        )
        enqueue = queue.index(
            "k_msgq_put(&gateway_ble_rx_msgq, &pending, K_NO_WAIT)", classify
        )
        queue_failure = queue.index("if (ret < 0)", enqueue)
        admitted = queue.index("} else {", queue_failure)
        receipt_gate = queue.index("if (host_receipt)", admitted)
        publish = queue.index(
            "atomic_inc(&gateway_ble_host_receipt_ingress_count)", receipt_gate
        )
        notify = queue.index(
            "mesh_gateway_host_receipt_ingress_queued()", publish
        )
        resume = queue.index("gateway_ble_resume_rx()", notify)

        self.assertLess(classify, enqueue)
        self.assertLess(enqueue, queue_failure)
        self.assertLess(queue_failure, admitted)
        self.assertLess(admitted, receipt_gate)
        self.assertLess(receipt_gate, publish)
        self.assertLess(publish, notify)
        self.assertLess(notify, resume)
        self.assertNotIn(
            "mesh_gateway_host_receipt_ingress_queued()",
            queue[queue_failure:admitted],
            "a receipt that was not retained in the BLE queue must not preempt RF",
        )

        pending = function_body(
            self.ble, "gateway_ble_host_receipt_ingress_pending"
        )
        self.assertIn(
            "atomic_get(&gateway_ble_host_receipt_ingress_count) > 0", pending
        )

        worker = function_body(self.ble, "gateway_ble_rx_work_handler")
        receipt = worker.index("gateway_ble_pending_is_host_receipt(&pending)")
        dequeue = worker.index(
            "k_msgq_get(&gateway_ble_rx_msgq, &pending, K_NO_WAIT)", receipt
        )
        classify_semantics = worker.index("gateway_handle_ble_frame(", dequeue)
        clear_ingress = worker.index(
            "atomic_dec(&gateway_ble_host_receipt_ingress_count)",
            classify_semantics,
        )
        self.assertLess(receipt, dequeue)
        self.assertLess(dequeue, classify_semantics)
        self.assertLess(classify_semantics, clear_ingress)

    def test_receipt_notifier_aborts_only_a_proven_active_mesh_scan(self):
        notify = function_body(
            self.report, "mesh_gateway_host_receipt_ingress_queued"
        )

        lock = notify.index("k_spin_lock(&mesh_rx_handoff_lock)")
        observe_scan = notify.index(
            "abort_scan = mesh_rx_handoff.scan_radio_active", lock
        )
        active_gate = notify.index("if (abort_scan)", observe_scan)
        abort = notify.index(
            "dwm3000_driver_request_receive_abort(", active_gate
        )
        owner = notify.index("DWM3000_RECEIVE_ABORT_MESH_CONTROL", abort)
        gate_end = notify.index("}", owner)
        unlock = notify.index("k_spin_unlock(&mesh_rx_handoff_lock", gate_end)
        trace = notify.index('"DBG_GATEWAY_HOST_RECEIPT_PREEMPT', unlock)

        self.assertLess(lock, observe_scan)
        self.assertLess(observe_scan, active_gate)
        self.assertLess(active_gate, abort)
        self.assertLess(abort, owner)
        self.assertLess(owner, gate_end)
        self.assertLess(gate_end, unlock)
        self.assertLess(unlock, trace)
        self.assertEqual(
            1,
            notify.count("dwm3000_driver_request_receive_abort("),
            "receipt ingress may own one conditional scan-abort edge only",
        )
        self.assertNotIn(
            "dwm3000_driver_request_receive_abort(", notify[:active_gate]
        )
        self.assertNotIn(
            "dwm3000_driver_request_receive_abort(", notify[gate_end:]
        )

    def test_pending_receipt_blocks_scan_claim_and_both_continuous_rearms(self):
        priority = function_body(
            self.report, "mesh_gateway_host_receipt_priority_pending"
        )
        self.assertIn("gateway_ble_host_receipt_ingress_pending()", priority)
        self.assertIn("mesh_gateway_host_delivery_pending_state", priority)
        self.assertIn("mesh_gateway_host_receipt_received_state", priority)

        claim = function_body(self.report, "mesh_rx_radio_claim")
        lock = claim.index("k_spin_lock(&mesh_rx_handoff_lock)")
        pending_gate = claim.index(
            "!mesh_gateway_host_receipt_priority_pending()", lock
        )
        begin_scan = claim.index(
            "app_mesh_rx_handoff_try_begin_scan(&mesh_rx_handoff)", pending_gate
        )
        unlock = claim.index("k_spin_unlock(&mesh_rx_handoff_lock", begin_scan)
        self.assertLess(lock, pending_gate)
        self.assertLess(pending_gate, begin_scan)
        self.assertLess(begin_scan, unlock)

        worker = function_body(self.report, "mesh_uwb_rx_work_handler")
        entry_gate = worker.index(
            "if (mesh_gateway_host_receipt_priority_pending())"
        )
        entry_trace = worker.index(
            '"DBG_GATEWAY_CH9_RX_YIELD_HOST_RECEIPT now=%u stage=entry',
            entry_gate,
        )
        role_gate = worker.index("if (!mesh_role_uses_uwb_rx())", entry_trace)
        continuous = worker.index(
            'mesh_rx_radio_claim("mesh gateway continuous channel9 RX"',
            role_gate,
        )
        slice_gate = worker.rindex(
            "if (mesh_gateway_host_receipt_priority_pending())",
            role_gate,
            continuous,
        )
        slice_trace = worker.index(
            '"DBG_GATEWAY_CH9_RX_YIELD_HOST_RECEIPT now=%u stage=slice',
            slice_gate,
        )

        self.assertLess(entry_gate, entry_trace)
        self.assertLess(entry_trace, role_gate)
        self.assertLess(role_gate, slice_gate)
        self.assertLess(slice_gate, slice_trace)
        self.assertLess(slice_trace, continuous)
        self.assertEqual(
            2,
            worker.count("mesh_gateway_host_receipt_priority_pending()"),
            "the worker must yield both before entry and before each radio slice",
        )

    def test_priority_seam_is_declared_at_both_module_boundaries(self):
        self.assertIn(
            "bool gateway_ble_host_receipt_ingress_pending(void);",
            self.ble_header,
        )
        self.assertIn(
            "void mesh_gateway_host_receipt_ingress_queued(void);",
            self.report_header,
        )

    def test_host_receipt_completion_pins_successful_assignment_admission(self):
        preflight = function_body(
            self.report, "mesh_gateway_preflight_semantic_delivery"
        )
        commit = function_body(
            self.report,
            "mesh_gateway_commit_preflighted_semantic_delivery",
        )
        complete = function_body(
            self.report, "mesh_complete_gateway_host_delivery_locked"
        )

        # Assignment admission is a lease acquired by the read-only preflight.
        # The message-specific commit still runs after the exact host receipt,
        # but must not recheck live assignment state: assignment can become
        # active while the already-admitted BLE record waits for that receipt.
        self.assertIn(
            "gateway_discovery_assignment_admit_nonassignment_source(",
            preflight,
        )
        self.assertNotIn(
            "gateway_discovery_assignment_admit_nonassignment_source(",
            commit,
        )

        # Removing the repeated gate must not bypass the per-message semantic
        # commit. These cases are the common pure validators; command results
        # and ACK_CONFIRM retain their live mutation/identity checks as well.
        for message_type in (
            "MSG_CLICK_REPORT",
            "MSG_SELF_TEST_REPORT",
            "MSG_ANCHOR_HEARTBEAT",
            "MSG_MESH_DATA",
            "MSG_GATEWAY_ACK_CONFIRM",
            "MSG_COMMAND_RESULT",
            "MSG_RESULT_BUNDLE",
        ):
            self.assertIn(
                message_type,
                commit,
                f"{message_type} lost its post-receipt semantic commit",
            )

        live_accept = complete.index(
            "mesh_gateway_commit_preflighted_semantic_delivery(pending)"
        )
        pin = complete.index(
            "mesh_gateway_classify_post_receipt_semantic_result(",
            live_accept,
        )
        retry_gate = complete.index("if (ret < 0)", pin)
        accepted = complete.index(
            "atomic_set(&mesh_gateway_host_delivery_semantic_accepted_state, 1)",
            retry_gate,
        )
        self.assertLess(live_accept, pin)
        self.assertLess(pin, retry_gate)
        self.assertLess(retry_gate, accepted)

    def test_split_phase_gateway_ack_does_not_live_in_shared_result_scratch(self):
        complete = function_body(
            self.report, "mesh_complete_gateway_host_delivery_locked"
        )
        timeout = function_body(self.report, "mesh_tx_timeout_handler")
        actions = function_body(self.report, "mesh_handle_result_actions")
        setup_start = self.report.index(
            "mesh_gateway_host_delivery_semantic_acceptance = semantic_ret;"
        )
        setup_end = self.report.index(
            "atomic_set(&mesh_gateway_host_delivery_pending_state, 1)",
            setup_start,
        )
        setup = self.report[setup_start:setup_end]

        self.assertRegex(
            self.report,
            r"static\s+struct\s+mesh_outbound\s+"
            r"mesh_gateway_host_delivery_ack\s*;",
        )
        self.assertRegex(
            self.report,
            r"static\s+bool\s+mesh_gateway_host_delivery_ack_valid\s*;",
        )
        self.assertIn("mesh_work_result", timeout)
        self.assertIn("memset(result, 0, sizeof(*result))", timeout)
        self.assertNotIn("mesh_gateway_host_delivery_ack", timeout)
        self.assertIn(
            "mesh_gateway_host_delivery_ack_valid = false;",
            setup,
            "each newly retained host item must invalidate the previous ACK",
        )

        relay = complete.index("mesh_relay_commit_gateway_delivery(")
        relay_owner = complete.index("result);", relay)
        ack_action = complete.index(
            "MESH_RELAY_ACTION_SEND_GATEWAY_ACK", relay_owner
        )
        freeze = complete.index(
            "mesh_gateway_host_delivery_ack = result->gateway_ack", ack_action
        )
        valid = complete.index(
            "mesh_gateway_host_delivery_ack_valid = true", freeze
        )
        committed = complete.index(
            "atomic_set(&mesh_gateway_host_delivery_relay_committed_state, 1)",
            valid,
        )
        valid_guard = complete.index(
            "if (!mesh_gateway_host_delivery_ack_valid)", committed
        )
        reconstruct_clear = complete.index(
            "memset(result, 0, sizeof(*result))", valid_guard
        )
        reconstruct_action = complete.index(
            "result->actions = MESH_RELAY_ACTION_SEND_GATEWAY_ACK",
            reconstruct_clear,
        )
        reconstruct_ack = complete.index(
            "result->gateway_ack = mesh_gateway_host_delivery_ack",
            reconstruct_action,
        )
        handoff = complete.index("mesh_handle_result_actions(", reconstruct_ack)
        retry = complete.index("goto retry;", handoff)
        self.assertLess(relay, relay_owner)
        self.assertLess(relay_owner, ack_action)
        self.assertLess(ack_action, freeze)
        self.assertLess(freeze, valid)
        self.assertLess(valid, committed)
        self.assertLess(committed, valid_guard)
        self.assertLess(valid_guard, reconstruct_clear)
        self.assertLess(reconstruct_clear, reconstruct_action)
        self.assertLess(reconstruct_action, reconstruct_ack)
        self.assertLess(reconstruct_ack, handoff)
        self.assertLess(handoff, retry)

        # The synchronous action helper snapshots the reconstructed ACK again
        # before guard sleeps or RF handoff can expose shared scratch.
        snapshot = actions.index(
            "struct mesh_outbound gateway_ack_snapshot = result->gateway_ack"
        )
        send = actions.index("mesh_send_causal_channel9_response(", snapshot)
        self.assertLess(snapshot, send)


if __name__ == "__main__":
    unittest.main()
