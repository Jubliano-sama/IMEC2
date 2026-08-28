#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
REPORT = read_composed_source(ROOT / "app" / "src" / "app_mesh_report.c")
REPORT_UNIT = (ROOT / "app" / "src" / "app_mesh_report.c").read_text(
    encoding="utf-8"
)
REPORT_ROUTE_CONTROL = (
    ROOT / "app" / "src" / "app_mesh_report_route_control.inc"
).read_text(encoding="utf-8")
REPORT_DELIVERY = (
    ROOT / "app" / "src" / "app_mesh_report_delivery.inc"
).read_text(encoding="utf-8")
REPORT_ENCODER = (
    ROOT / "app" / "src" / "app_mesh_report_encode.c"
).read_text(encoding="utf-8")
REPORT_TRANSPORT = (
    ROOT / "app" / "src" / "app_mesh_report_transport.inc"
).read_text(encoding="utf-8")
REPORT_RX = (
    ROOT / "app" / "src" / "app_mesh_report_rx.inc"
).read_text(encoding="utf-8")
REPORT_EVENT_TX = (
    ROOT / "app" / "src" / "app_mesh_report_event_tx.inc"
).read_text(encoding="utf-8")
ANCHOR_RADIO = (
    ROOT / "app" / "src" / "app_anchor_radio.inc"
).read_text(encoding="utf-8")
APP_CONFIG = (ROOT / "app" / "src" / "app_config.h").read_text(
    encoding="utf-8"
)
MESH_CORE = (ROOT / "src" / "mesh.c").read_text(encoding="utf-8")
MESH_RELAY_DELIVERY = (
    ROOT / "src" / "mesh_relay_delivery.inc"
).read_text(encoding="utf-8")
MESH_RELAY_CUSTODY = (
    ROOT / "src" / "mesh_relay_custody.inc"
).read_text(encoding="utf-8")
RADIO_TIMING = (ROOT / "include" / "mesh_radio_timing.h").read_text(
    encoding="utf-8"
)
KCONFIG = (ROOT / "app" / "Kconfig").read_text(encoding="utf-8")
ANCHOR_CONF = (ROOT / "app" / "conf" / "mesh-anchor.conf").read_text(
    encoding="utf-8"
)
DWM3000_RADIO = (
    ROOT / "app" / "src" / "dwm3000_driver_radio.inc"
).read_text(encoding="utf-8")
APP_BOARD = (ROOT / "app" / "src" / "app_board.c").read_text(
    encoding="utf-8"
)
APP_BOARD_HEADER = (ROOT / "app" / "src" / "app_board.h").read_text(
    encoding="utf-8"
)


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


def braced_block_after(source: str, marker: str) -> str:
    marker_index = source.index(marker)
    start = source.index("{", marker_index)
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated block after: {marker}")


class MeshRfRetrySourceInvariantTests(unittest.TestCase):
    def test_valid_wake_claim_flashes_both_anchor_leds_green(self):
        pulse = function_body(
            APP_BOARD, "status_debug_anchor_wake_claim_rx_pulse"
        )
        decode = function_body(
            REPORT_RX, "mesh_decode_channel5_wake_claim"
        )

        self.assertIn(
            "void status_debug_anchor_wake_claim_rx_pulse(void)",
            APP_BOARD_HEADER,
        )
        self.assertIn("DEVICE_ROLE != ROLE_ANCHOR", pulse)
        self.assertIn("CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER", pulse)
        self.assertIn("status_led0_set(false, true, false)", pulse)
        self.assertIn("status_led1_set(false, true, false)", pulse)
        self.assertIn("status_wake_claim_rx_pulse_active", pulse)
        self.assertIn("status0_debug_pulse_restore_work", pulse)
        self.assertIn("status1_debug_pulse_restore_work", pulse)
        valid = decode.index("if (ret != PROTO_OK)")
        diagnostic = decode.index(
            "status_debug_anchor_wake_claim_rx_pulse()", valid
        )
        self.assertLess(valid, diagnostic)
        self.assertGreaterEqual(
            ANCHOR_RADIO.count(
                "status_debug_anchor_wake_claim_rx_pulse();"
            ),
            4,
        )

    def test_late_click_priority_rebinds_empty_report_custody(self):
        consider = function_body(
            ANCHOR_RADIO, "anchor_consider_active_click_claim"
        )
        rebind = function_body(
            REPORT_DELIVERY, "mesh_range_report_batch_rebind"
        )
        handle = function_body(ANCHOR_RADIO, "anchor_handle_uwb_claim")

        rebind_call = consider.index("mesh_range_report_batch_rebind(")
        phase_restart = consider.index(
            "app_anchor_click_event_runtime_claim(", rebind_call
        )
        self.assertLess(rebind_call, phase_restart)
        self.assertIn("queued_fragment_count != 0u", rebind)
        self.assertIn("control.fragment_count != 0u", rebind)
        self.assertIn(
            "anchor_range_report_batch_reservation.control.clicker_id",
            rebind,
        )
        late = handle.index("struct uwb_wake_claim_frame late_claim")
        late_rebind = handle.index(
            "anchor_consider_active_click_claim(", late
        )
        deadline = handle.index(
            "UWB_CLICK_DISCOVERY_RX_LATE_GUARD_MS", late_rebind
        )
        self.assertTrue(late < late_rebind < deadline)
        self.assertNotIn(
            "ignored late UWB WAKE_CLAIM",
            handle[late:deadline],
        )

    def test_event_accept_listener_covers_responder_retry_contract(self):
        propose = function_body(
            REPORT_EVENT_TX,
            "mesh_propose_event_after_channel5_contact_authorized",
        )
        listen = propose.index("mesh_listen_for_route_reply(")
        listen_end = propose.index(");", listen)
        call = propose[listen:listen_end]

        self.assertIn('"event-accept"', call)
        self.assertIn("MESH_EVENT_NEGOTIATION_DEADLINE_MS", call)
        self.assertNotIn("MESH_ROUTE_TEST_REPLY_RX_WINDOW_MS", call)
        self.assertNotIn(
            "mesh_event_propose_retry.deadline_ms =",
            propose[max(0, listen - 500) : listen],
        )

        duration = function_body(REPORT, "mesh_c5_exchange_duration_ms")
        timing_case = duration[
            duration.index("C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION") :
            duration.index("case C5_CONTACT_PURPOSE_RESULT_OFFER_GRANT")
        ]
        self.assertIn("MESH_EVENT_ACCEPT_RETRY_DEADLINE_MS", timing_case)

        self.assertIn(
            "MESH_EVENT_CONTROL_RX_QUEUE_LIFETIME_MS +\n"
            "                 MESH_EVENT_ACCEPT_RETRY_DEADLINE_MS",
            REPORT,
        )

    def test_busy_parent_queues_reliable_reports_without_route_repair(self):
        classify = function_body(
            MESH_RELAY_CUSTODY,
            "mesh_relay_packet_can_queue_gateway_report",
        )
        for report_type in (
            "MSG_CLICK_REPORT",
            "MSG_SELF_TEST_REPORT",
            "MSG_MESH_DATA",
        ):
            self.assertIn(report_type, classify)
        self.assertNotIn("MSG_ANCHOR_HEARTBEAT", classify)
        self.assertIn("FLAG_GATEWAY_ACK_REQUIRED", classify)

    def test_best_effort_heartbeat_cannot_create_route_or_retry_ownership(self):
        waitable = function_body(REPORT_EVENT_TX, "mesh_tx_can_wait_for_route")
        self.assertNotIn("MSG_ANCHOR_HEARTBEAT", waitable)

        defer = function_body(REPORT_EVENT_TX, "mesh_defer_route_for_outbound")
        repair = function_body(
            REPORT_EVENT_TX, "mesh_try_repair_selected_parent_event"
        )
        self.assertIn("!mesh_tx_can_wait_for_route(out)", defer)
        self.assertIn("!mesh_tx_can_wait_for_route(out)", repair)

        direct = function_body(
            REPORT_EVENT_TX, "mesh_start_tracked_tx_with_retry"
        )
        self.assertIn(
            "direct_gateway_tx_pending =\n"
            "                    (aged_out.packet.flags & "
            "FLAG_GATEWAY_ACK_REQUIRED) != 0u",
            direct,
        )

        heartbeat_drop = MESH_RELAY_DELIVERY[
            MESH_RELAY_DELIVERY.index(
                "packet->msg_type == MSG_ANCHOR_HEARTBEAT"
            ) :
            MESH_RELAY_DELIVERY.index(
                "if (packet->msg_type != MSG_ROUTE_REQ",
                MESH_RELAY_DELIVERY.index(
                    "packet->msg_type == MSG_ANCHOR_HEARTBEAT"
                ),
            )
        ]
        self.assertIn("packet->flags == 0u", heartbeat_drop)
        self.assertIn("mesh_relay_tx_active(relay)", heartbeat_drop)
        self.assertIn("MESH_RELAY_ACTION_DROP", heartbeat_drop)
        self.assertNotIn("add_busy_action", heartbeat_drop)

        busy_gate = MESH_RELAY_DELIVERY.index(
            "if (packet->msg_type != MSG_ROUTE_REQ"
        )
        active_gate = MESH_RELAY_DELIVERY.index(
            "mesh_relay_tx_active(relay)", busy_gate
        )
        self.assertIn(
            "!mesh_relay_packet_can_queue_gateway_report(relay, packet)",
            MESH_RELAY_DELIVERY[busy_gate:active_gate],
        )

        app_gate = function_body(
            REPORT_DELIVERY, "mesh_forward_uses_gateway_batch_queue"
        )
        self.assertIn(
            "mesh_relay_packet_can_queue_gateway_report(&mesh_runtime",
            app_gate,
        )
        self.assertIn("mesh_relay_tx_active(&mesh_runtime)", app_gate)

        queue_dedup = function_body(
            REPORT_DELIVERY, "report_tx_queue_contains_semantic_locked"
        )
        self.assertIn("report_tx_queue_recovery_valid", queue_dedup)
        self.assertIn("k_msgq_peek_at", queue_dedup)
        self.assertGreaterEqual(
            queue_dedup.count("mesh_preempt_outbound_matches"), 2
        )

        queue = function_body(REPORT_DELIVERY, "queue_anchor_report")
        dedup_call = queue.index("report_tx_queue_contains_semantic_locked(")
        queue_put = queue.index("k_msgq_put(", dedup_call)
        self.assertLess(dedup_call, queue_put)
        self.assertIn("if (duplicate_owned)", queue[dedup_call:queue_put])

    def test_gateway_batch_reverse_route_commits_only_after_queue_retention(self):
        queue = function_body(REPORT_DELIVERY, "queue_anchor_report")
        admission_failure = queue.index("if (!admitted)")
        failure_return = queue.index("return -ENOSPC;", admission_failure)
        transit_gate = queue.index(
            "if (queued.packet.dst_id == GATEWAY_ID", failure_return
        )
        commit = queue.index(
            "mesh_relay_commit_queued_transit_reverse_route(", transit_gate
        )
        success_return = queue.index("return 0;", commit)

        self.assertLess(admission_failure, failure_return)
        self.assertLess(failure_return, transit_gate)
        self.assertLess(transit_gate, commit)
        self.assertLess(commit, success_return)
        self.assertNotIn(
            "mesh_relay_commit_queued_transit_reverse_route(",
            queue[:failure_return],
        )
        committed = queue[transit_gate:success_return]
        self.assertIn("&mesh_runtime", committed)
        self.assertIn("&queued", committed)
        self.assertIn("queued.ingress_previous_hop_id", committed)
        self.assertIn("reverse_route_ret != PROTO_OK", committed)
        self.assertIn("app_watchdog_stop_feeding()", committed)

        delivery = function_body(REPORT_DELIVERY, "mesh_handle_result_actions")
        batch = delivery.index(
            "mesh_forward_uses_gateway_batch_queue(&result->forward)"
        )
        queue_call = delivery.index(
            "ret = queue_anchor_report(&result->forward)", batch
        )
        forward_success = delivery.index(
            "forward_sent = ret == 0", queue_call
        )
        hop_handoff = delivery.index(
            "app_mesh_result_handoff_after_forward", forward_success
        )
        self.assertLess(batch, queue_call)
        self.assertLess(queue_call, forward_success)
        self.assertLess(forward_success, hop_handoff)

    def test_live_ack_rx_cannot_be_blanket_suppressed_by_route_wait(self):
        worker = function_body(REPORT, "mesh_uwb_rx_work_handler")

        coordinator = worker.index(
            'mesh_coordinator_decide_now("uwb-rx", &coordinator_decision)'
        )
        decision_gate = worker.index(
            "if (!coordinator_decision.uwb_rx_allowed)", coordinator
        )
        response_gate = worker.index(
            "if (mesh_rx_response_active())", decision_gate
        )
        radio_claim = worker.index("ret = mesh_rx_radio_claim(", response_gate)
        denied = worker[decision_gate:response_gate]
        admitted = worker[response_gate:radio_claim]

        self.assertIn("coordinator_decision.route_wait_allowed", denied)
        self.assertIn("DEVICE_ROLE == ROLE_ANCHOR", denied)
        self.assertIn("mesh_route_waiting_tx_valid", denied)
        self.assertIn("mesh_wake_unarmed_route_waiting_from_rx(", denied)
        self.assertIn("mesh_schedule_uwb_rx(", denied)
        self.assertIn("return;", denied)
        self.assertNotIn("mesh_route_waiting_tx_valid", admitted)
        self.assertNotIn(
            "mesh UWB RX suppressed while route-waiting TX owns channel-9 event",
            worker,
        )
        self.assertLess(coordinator, decision_gate)
        self.assertLess(decision_gate, response_gate)
        self.assertLess(response_gate, radio_claim)

        route_wait_wake = function_body(
            REPORT, "mesh_wake_unarmed_route_waiting_from_rx"
        )
        self.assertIn("k_work_delayable_is_pending(", route_wait_wake)
        self.assertIn(
            "app_mesh_route_wait_tx_should_wake_from_rx(", route_wait_wake
        )
        self.assertIn("mesh_route_waiting_tx_valid", route_wait_wake)
        self.assertIn("work_pending", route_wait_wake)
        self.assertIn(
            "DBG_ROUTE_WAIT_RX_WAKE_PRESERVED", route_wait_wake
        )
        self.assertIn(
            "mesh_schedule_route_waiting_retry_after(reason, 0u)",
            route_wait_wake,
        )

    def test_empty_channel9_turn_advances_without_claiming_peer_failure(self):
        rx = function_body(REPORT, "mesh_uwb_rx_work_handler")
        empty = rx.index('"DBG_CH9_RX_EMPTY\\n"')
        settle = rx.index("if (channel9_event)", empty)
        empty_settlement = rx[empty:settle]

        self.assertIn(
            "mesh_relay_note_channel9_unobserved_turn(", empty_settlement
        )
        self.assertIn("channel9_plan.start_ms", empty_settlement)
        self.assertNotIn("mesh_relay_note_channel9_missed", empty_settlement)
        self.assertNotIn("app_mesh_test_note_ch9_missed", empty_settlement)

        skip = function_body(MESH_CORE, "mesh_event_skip_elapsed")
        self.assertIn("mesh_event_note_unobserved_turn(", skip)
        self.assertNotIn("mesh_event_note_missed(", skip)
        self.assertIn(
            "#define MESH_RADIO_EVENT_SUPERVISION_MS 300000u",
            RADIO_TIMING,
        )

    def test_event_propose_phase_is_finalized_after_wake_before_rf(self):
        attempt = function_body(REPORT_TRANSPORT, "mesh_send_c5_control_attempt")
        wake = attempt.index("mesh_send_route_wake_train(")
        turnaround = attempt.index(
            "mesh_wait_for_c5_control_followup_turnaround(", wake
        )
        finalize = attempt.index(
            "mesh_event_propose_prepare_immediate_send(tx)", turnaround
        )
        exchange = attempt.index("mesh_c5_contact_exchange(", finalize)
        physical_tx = attempt.index(
            "mesh_send_outbound_with_release_on_channel_until(", exchange
        )

        self.assertLess(wake, turnaround)
        self.assertLess(turnaround, finalize)
        self.assertLess(finalize, exchange)
        self.assertLess(exchange, physical_tx)

        helper = function_body(
            REPORT_EVENT_TX, "mesh_event_propose_prepare_immediate_send"
        )
        self.assertIn(
            "record->prepared_rf_attempts !=\n"
            "                     (uint8_t)mesh_event_propose_retry.rf_attempts",
            helper,
        )
        self.assertIn(
            "mesh_event_propose_retry.rf_attempts > UINT8_MAX", helper
        )
        self.assertIn("mesh_event_new_operation_session()", helper)
        self.assertIn("mesh_next_event_control_seq()", helper)
        self.assertIn("mesh_prepare_event_timing(&timing", helper)
        self.assertIn("mesh_event_timing_bind_proposal_session(", helper)
        self.assertIn("mesh_append_event_timing_tlvs_at(", helper)
        self.assertIn("TLV_MESH_EVENT_BOOT_NONCE", helper)
        self.assertIn("mesh_event_propose_record.timing = timing", helper)
        self.assertIn("mesh_event_propose_record.encoded_delay_ms", helper)
        self.assertIn("app_mesh_event_retry_rebind_request(", helper)
        self.assertIn("mesh_event_request_identity(", helper)
        self.assertIn(
            "mesh_event_propose_record.prepared_rf_attempts", helper
        )
        self.assertIn(
            "mesh_event_propose_record.transmit_phase_frozen = false", helper
        )
        self.assertIn("mesh_event_control_retry_scratch.payload", helper)

    def test_exact_assignment_event_propose_skips_only_the_redundant_wake(self):
        classify = function_body(
            REPORT_EVENT_TX, "mesh_outbound_is_assignment_response"
        )
        outbound_propose = function_body(
            REPORT_EVENT_TX, "mesh_propose_event_for_outbound"
        )
        repair = function_body(
            REPORT_EVENT_TX, "mesh_try_repair_selected_parent_event"
        )
        event_control = function_body(
            REPORT_EVENT_TX, "mesh_send_event_control_record"
        )
        attempt = function_body(
            REPORT_TRANSPORT, "mesh_send_c5_control_attempt"
        )
        propose = function_body(
            REPORT_EVENT_TX,
            "mesh_propose_event_after_channel5_contact_authorized",
        )
        normal_propose = function_body(
            REPORT_EVENT_TX, "mesh_propose_event_after_channel5_contact"
        )
        forwarded_ack_repair = function_body(
            REPORT_EVENT_TX,
            "mesh_retry_deferred_forwarded_ack_event_repair",
        )
        event_accept = function_body(
            REPORT_EVENT_TX, "mesh_event_accept_attempt"
        )
        generic_control = function_body(
            REPORT_TRANSPORT, "mesh_send_c5_control"
        )
        causal_control = function_body(
            REPORT_TRANSPORT, "mesh_send_c5_causal_response"
        )

        # Classify the immutable original outbound, before Channel-9 startup
        # can fail and before transient relay pending state exists.
        for exact_guard in (
            "out->packet.msg_type == MSG_COMMAND_RESULT",
            "discovery_assignment_parse_result_tlvs(",
            "== PROTO_OK",
            "DISCOVERY_ASSIGNMENT_PHASE_CLAIM",
            "DISCOVERY_ASSIGNMENT_PHASE_ACK",
        ):
            self.assertIn(exact_guard, classify)
        for broad_or_unrelated_match in (
            "TLV_COMMAND_ID",
            "CMD_ASSIGN_DISCOVERY_SLOTS",
        ):
            self.assertNotIn(broad_or_unrelated_match, classify)
        self.assertNotIn("mesh_runtime.pending", classify)
        self.assertNotIn("mesh_relay_tx_active", classify)

        topology = outbound_propose.index(
            "mesh_outbound_is_topology_operation_uplink(out)"
        )
        assignment = outbound_propose.index(
            "mesh_outbound_is_assignment_response(out)", topology
        )
        self.assertLess(topology, assignment)
        self.assertNotIn("mesh_runtime.pending", outbound_propose)
        self.assertIn(
            "mesh_propose_event_for_outbound(\n"
            "        selected_next_hop, \"initial-tx-event-repair\", out)",
            repair,
        )

        # Preserve the exact bit through EVENT_PROPOSE construction and the
        # control wrapper rather than reconstructing it from mutable state.
        begin = propose.index("app_mesh_event_retry_begin(")
        send = propose.index("mesh_send_event_control_record(", begin)
        assignment_forward = propose.index("assignment_response", send)
        failure = propose.index(
            "mesh_event_propose_retry_after_failure(", assignment_forward
        )
        self.assertLess(begin, send)
        self.assertLess(send, assignment_forward)
        self.assertLess(assignment_forward, failure)
        self.assertNotIn("mesh_outbound_is_assignment_response", propose)

        purpose = event_control.index(
            "C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION"
        )
        transport_forward = event_control.index(
            "assignment_parent_protocol_awake", purpose
        )
        self.assertLess(purpose, transport_forward)

        eligibility = attempt.index("assignment_parent_awake =")
        wake = attempt.index("mesh_send_route_wake_train(", eligibility)
        direct = attempt.index("else if (assignment_parent_awake)", wake)
        finalize = attempt.index(
            "mesh_event_propose_prepare_immediate_send(tx)", direct
        )
        physical_tx = attempt.index(
            "mesh_send_outbound_with_release_on_channel_until(", finalize
        )

        self.assertIn(
            "purpose == C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION",
            attempt[eligibility:wake],
        )
        self.assertIn(
            "assignment_parent_protocol_awake", attempt[eligibility:wake]
        )
        self.assertIn(
            "tx->packet.msg_type == MSG_MESH_EVENT_PROPOSE",
            attempt[eligibility:wake],
        )
        wake_guard = attempt[eligibility:wake]
        self.assertIn("mode == MESH_C5_CONTROL_WAKE_IF_NEEDED", wake_guard)
        self.assertIn("!active_exchange", wake_guard)
        self.assertIn("!assignment_parent_awake", wake_guard)
        self.assertLess(wake, direct)
        self.assertLess(direct, finalize)
        self.assertLess(finalize, physical_tx)

        # Only the exact assignment classifier supplies the wake shortcut.
        self.assertIn(
            "MESH_C5_CONTROL_WAKE_IF_NEEDED", propose[send:failure]
        )
        self.assertIn(
            "APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA", propose[failure:]
        )
        self.assertIn("false", normal_propose)
        self.assertNotIn("mesh_outbound_is_assignment_response", normal_propose)
        self.assertIn("false,\n        false", forwarded_ack_repair)
        self.assertIn("FW_C5_TX_INTENT_CAUSAL_RESPONSE,\n        false", event_accept)
        self.assertIn("FW_C5_TX_INTENT_BACKGROUND,\n                                        false", generic_control)
        self.assertIn(
            "FW_C5_TX_INTENT_CAUSAL_RESPONSE, false", causal_control
        )

    def test_rx_worker_lock_collision_defers_to_direct_drain_owner(self):
        worker = function_body(REPORT, "mesh_rx_work_handler")
        direct = function_body(REPORT, "mesh_process_queued_rx_now_internal")

        collision = braced_block_after(worker, "if (lock_ret != 0)")
        self.assertIn("return", collision)
        self.assertIn("DBG_MESH_RX_WORK_LOCK_BUSY", collision)
        for immediate_retry in (
            "mesh_submit_owned_work(",
            "mesh_route_owner_work_reschedule(",
            "mesh_route_work_reschedule(",
            "k_work_submit(",
            "k_work_reschedule(",
        ):
            self.assertNotIn(
                immediate_retry,
                collision,
                "a worker that collided with the direct RX owner must return "
                "instead of monopolizing its system workqueue with retries",
            )

        # The direct caller that acquired the mutex owns the complete drain
        # transaction.  If processing leaves a remainder, publish follow-up
        # work only after releasing the mutex so the worker can make progress.
        acquired = direct.index("atomic_set(&mesh_rx_handler_active_state, 1)")
        drain = direct.index("mesh_drain_rx_queue_locked(", acquired)
        inactive = direct.index(
            "atomic_set(&mesh_rx_handler_active_state, 0)", drain
        )
        clear_owner = direct.index(
            "mesh_rx_handler_lock_clear_owner()", inactive
        )
        unlock = direct.index("k_mutex_unlock(&mesh_rx_handler_lock)", clear_owner)
        remainder = direct.index(
            "k_msgq_num_used_get(&mesh_rx_msgq) > 0u", unlock
        )
        followup = direct.index("mesh_submit_owned_work(", remainder)
        self.assertLess(acquired, drain)
        self.assertLess(drain, inactive)
        self.assertLess(inactive, clear_owner)
        self.assertLess(clear_owner, unlock)
        self.assertLess(unlock, remainder)
        self.assertLess(remainder, followup)
        self.assertIn('"rx-drain-remainder"', direct[followup:])

    def test_gateway_ch9_boundary_drains_only_after_physical_release(self):
        internal = function_body(
            REPORT, "mesh_process_queued_rx_now_internal"
        )
        ordinary = function_body(REPORT, "mesh_process_queued_rx_now")
        boundary = function_body(
            REPORT, "mesh_process_queued_rx_at_gateway_rx_boundary"
        )
        worker = function_body(REPORT, "mesh_uwb_rx_work_handler")

        role_guard = internal.index("if (gateway_rx_boundary")
        role_reject = internal.index("DEVICE_ROLE != ROLE_GATEWAY", role_guard)
        physical_busy = internal.index("radio_guard_uwb_busy()", role_reject)
        coordinator = internal.index("mesh_coordinator_decide_now(", physical_busy)
        state_exception = internal.index(
            "coordinator_decision.state == FW_RADIO_ACTIVITY_GATEWAY_RX",
            coordinator,
        )
        lock = internal.index("k_mutex_lock(&mesh_rx_handler_lock", state_exception)

        self.assertLess(role_guard, role_reject)
        self.assertLess(role_reject, physical_busy)
        self.assertLess(physical_busy, coordinator)
        self.assertLess(coordinator, state_exception)
        self.assertLess(state_exception, lock)
        self.assertNotIn("FW_RADIO_ACTIVITY_CLICK", internal)
        self.assertIn("mesh_process_queued_rx_now_internal(reason, false)", ordinary)
        self.assertIn('"gateway-ch9-continuous-rx", true', boundary)
        self.assertEqual(
            REPORT.count("mesh_process_queued_rx_at_gateway_rx_boundary("),
            4,
            "only the private helper and the three post-release continuous-RX "
            "boundaries may name the coordinator exception",
        )

        continuous = worker.index(
            'mesh_rx_radio_claim("mesh gateway continuous channel9 RX"'
        )
        validation = worker.index(
            "mesh_gateway_protocol_validation_complete(", continuous
        )
        radio_release = worker.index(
            "mesh_rx_radio_finish(&radio_lease, parking_ret)", validation
        )
        boundary_calls = [
            match.start()
            for match in re.finditer(
                r"mesh_process_queued_rx_at_gateway_rx_boundary\(\)",
                worker,
            )
        ]
        self.assertEqual(len(boundary_calls), 3)
        for call in boundary_calls:
            self.assertGreater(call, radio_release)

        batch_comment = worker.index(
            "Direct anchors can finish adjacent reports"
        )
        batch_loop = worker.index("while (true)", batch_comment)
        batch_queue = worker.index(
            "mesh_queue_from_frame_at_internal(", batch_loop
        )
        batch_rearm = worker.index(
            "dwm3000_driver_receive_frame_continuous_extend_on_activity(",
            batch_queue,
        )
        batch_park = worker.index(
            '"gateway-ch9-frame-batch"', batch_rearm
        )
        batch_before_release = worker[batch_loop:radio_release]
        batch_guard = worker.index(
            "if (gateway_frame_batch_consumed)", radio_release
        )
        batch_drain = worker.index(
            "mesh_process_queued_rx_at_gateway_rx_boundary()",
            batch_guard,
        )
        post_batch_error = worker.index("if (ret != 0)", batch_drain)

        self.assertLess(batch_loop, batch_queue)
        self.assertLess(batch_queue, batch_rearm)
        self.assertLess(batch_rearm, batch_park)
        self.assertLess(batch_park, radio_release)
        self.assertNotIn(
            "mesh_process_queued_rx_at_gateway_rx_boundary()",
            batch_before_release,
        )
        self.assertNotIn(
            "mesh_process_received_frame(", batch_before_release
        )
        self.assertTrue(
            radio_release < batch_guard < batch_drain < post_batch_error
        )

        frame_admission = worker.index(
            "mesh_process_received_frame(", radio_release
        )
        boundary_drain = worker.index(
            "mesh_process_queued_rx_at_gateway_rx_boundary()",
            frame_admission,
        )
        next_rx = worker.index(
            "mesh_schedule_uwb_rx(\n"
            "            app_mesh_rx_policy_gateway_ch9_rearm_delay_ms())",
            boundary_drain,
        )

        self.assertLess(validation, radio_release)
        self.assertLess(radio_release, frame_admission)
        self.assertLess(frame_admission, boundary_drain)
        self.assertLess(boundary_drain, next_rx)

        abort_pending = worker.index(
            "if (dwm3000_driver_receive_abort_pending())", radio_release
        )
        abort_queue = worker.index(
            "mesh_queue_from_frame_at_internal(", abort_pending
        )
        abort_drain = worker.index(
            "mesh_process_queued_rx_at_gateway_rx_boundary()", abort_queue
        )
        control_boundary = worker.index(
            "app_node_comm_gateway_delivery_safe_boundary()", abort_drain
        )
        self.assertLess(radio_release, abort_pending)
        self.assertLess(abort_pending, abort_queue)
        self.assertLess(abort_queue, abort_drain)
        self.assertLess(abort_drain, control_boundary)

    def test_role_scan_busy_restart_transfers_to_rearm_owner_before_watchdog(self):
        schedule = function_body(REPORT, "mesh_schedule_uwb_rx")
        rearm = function_body(REPORT, "mesh_uwb_rx_rearm_work_handler")
        handoff = function_body(
            REPORT, "mesh_reschedule_owned_work_with_busy_handoff"
        )

        self.assertIn("mesh_route_owner_work_reschedule(work, delay_ms)", handoff)
        self.assertIn("if (ret == -EBUSY)", handoff)
        busy_return = handoff.index("return ret;", handoff.index("if (ret == -EBUSY)"))
        failstop = handoff.index("mesh_owned_schedule_result(ret, owner, false)")
        self.assertLess(busy_return, failstop)

        for owner in (schedule, rearm):
            primary = owner.index(
                "mesh_reschedule_owned_work_with_busy_handoff("
            )
            busy = owner.index("if (ret == -EBUSY)", primary)
            fallback = owner.index("mesh_defer_uwb_rx_rearm(delay_ms)", busy)
            self.assertLess(primary, busy)
            self.assertLess(busy, fallback)

        self.assertIn(
            'mesh_submit_owned_work(&mesh_uwb_rx_rearm_work,\n'
            '                                 "role-scan-rearm")',
            REPORT,
        )

    def test_inline_channel9_responses_publish_gateway_control_handoff(self):
        helper = function_body(
            REPORT, "mesh_send_causal_channel9_response"
        )
        begin = helper.index("mesh_rx_handoff_begin_control(")
        stop = helper.index("mesh_stop_role_scan()", begin)
        wait = helper.index("mesh_rx_handoff_wait_for_control()", stop)
        send = helper.index(
            "mesh_send_outbound_keep_channel9_awake(", wait
        )
        end = helper.index("mesh_rx_handoff_end_control()", send)
        restart = helper.index("mesh_restart_role_scan()", end)

        self.assertLess(begin, stop)
        self.assertLess(stop, wait)
        self.assertLess(wait, send)
        self.assertLess(send, end)
        self.assertLess(end, restart)
        self.assertIn("if (DEVICE_ROLE == ROLE_GATEWAY)", helper)
        self.assertIn("if (abort_scan)", helper)
        self.assertIn("DWM3000_RECEIVE_ABORT_MESH_CONTROL", helper)

        # The definition plus both immediate single-ACK policy branches and
        # the batch-ACK call are the complete non-deferred causal Channel-9
        # send surface.
        self.assertEqual(
            REPORT.count("mesh_send_causal_channel9_response("), 4
        )

    def test_anchor_protocol_response_owns_receive_to_control_boundary(self):
        helper = function_body(
            REPORT_TRANSPORT, "mesh_try_send_reliable_uplink_view"
        )
        role = helper.index("#if DEVICE_ROLE == ROLE_ANCHOR")
        classify = helper.index(
            "view->packet->msg_type == MSG_COMMAND_RESULT", role
        )
        begin = helper.index("mesh_rx_handoff_begin_control(", classify)
        stop = helper.index("mesh_stop_role_scan()", begin)
        owner = helper.index("radio_guard_uwb_owner_client()", stop)
        mesh_rx = helper.index(
            "RADIO_GUARD_UWB_CLIENT_MESH_RX", owner
        )
        window_guard = helper.index("!anchor_uwb_window_active()", mesh_rx)
        click_guard = helper.index("!anchor_click_window_active()", window_guard)
        abort = helper.index(
            "dwm3000_driver_request_receive_abort(", click_guard
        )
        wait = helper.index("mesh_rx_handoff_wait_for_control()", abort)
        send = helper.index("mesh_start_tracked_tx_with_retry(", wait)
        end = helper.index("mesh_rx_handoff_end_control()", send)
        restart = helper.index("mesh_restart_role_scan()", end)
        unlock = helper.index(
            "k_mutex_unlock(&mesh_route_wait_scratch_lock)", restart
        )

        self.assertLess(role, classify)
        self.assertLess(classify, begin)
        self.assertLess(begin, stop)
        self.assertLess(stop, owner)
        self.assertLess(owner, mesh_rx)
        self.assertLess(mesh_rx, window_guard)
        self.assertLess(window_guard, click_guard)
        self.assertLess(click_guard, abort)
        self.assertLess(abort, wait)
        self.assertLess(wait, send)
        self.assertLess(send, end)
        self.assertLess(end, restart)
        self.assertLess(restart, unlock)
        self.assertIn("DWM3000_RECEIVE_ABORT_MESH_CONTROL", helper)

    def test_failed_direct_probe_ack_remains_sender_retry_owned(self):
        delivery = function_body(REPORT, "mesh_handle_result_actions")
        route_probe = delivery.index(
            "rx->packet.msg_type == MSG_GATEWAY_ROUTE_REQ"
        )
        retry_owned = delivery.index(
            "DBG_GATEWAY_ROUTE_PROBE_ACK_RETRY_OWNED", route_probe
        )
        drop = delivery.index("goto after_gateway_ack;", retry_owned)
        durable_submit = delivery.index(
            "app_node_comm_submit_control_response(", drop
        )
        narrow_path = delivery[route_probe:drop]

        self.assertLess(route_probe, retry_owned)
        self.assertLess(retry_owned, drop)
        self.assertLess(drop, durable_submit)
        self.assertNotIn("app_node_comm_submit", narrow_path)
        self.assertNotIn("*gateway_ack_handed_off = true", narrow_path)

    def test_direct_gateway_probe_checks_control_lane_before_channel9(self):
        body = function_body(
            REPORT, "mesh_send_direct_gateway_probe_and_wait"
        )
        control_sniff = body.index("mesh_route_wake_sniff_activity(")
        channel9_config = body.index(
            "dwm3000_driver_configure_mesh_payload_mode()"
        )

        self.assertLess(control_sniff, channel9_config)
        self.assertIn('if (ret < 0 || c5_activity)', body)
        self.assertIn('ret = -EBUSY', body)
        self.assertIn(
            '"direct-gateway-probe-c5-yield"', body
        )
        self.assertIn("mesh_restart_role_scan()", body)

    def test_direct_gateway_probe_yields_queued_rx_before_retrying_rf(self):
        body = function_body(
            REPORT, "mesh_try_direct_gateway_route_probe"
        )
        attempt_loop = body.index("while (true)")
        send = body.index(
            "ret = mesh_send_direct_gateway_probe_and_wait", attempt_loop
        )
        success = body.index("if (ret == 0)", send)
        retry_note = body.index(
            "app_mesh_direct_gateway_retry_note(", success
        )
        sleep = body.index("k_msleep(retry_decision.delay_ms)", retry_note)
        unlock = body.index(
            "k_mutex_unlock(&mesh_direct_gateway_probe_scratch_lock)", sleep
        )

        pre_attempt = body[attempt_loop:send]
        post_attempt = body[send:success]
        self.assertRegex(
            pre_attempt,
            r"k_msgq_num_used_get\(&mesh_rx_msgq\)\s*!=\s*0u",
        )
        self.assertIn("break;", pre_attempt)
        self.assertRegex(
            post_attempt,
            r"ret\s*!=\s*0\s*&&[\s\S]*"
            r"k_msgq_num_used_get\(&mesh_rx_msgq\)\s*!=\s*0u",
        )
        self.assertIn("last_ret = -EAGAIN", post_attempt)
        self.assertIn("break;", post_attempt)
        self.assertLess(post_attempt.index("break;"), success - send)
        self.assertLess(success, retry_note)
        self.assertLess(retry_note, sleep)
        self.assertLess(sleep, unlock)

    def test_direct_ack_wait_returns_after_queuing_unmatched_valid_rx(self):
        body = function_body(
            REPORT, "mesh_wait_for_direct_gateway_ack_configured"
        )
        matched_queue = body.index("bool queued = mesh_queue_from_frame_at(")
        unmatched = body.index(
            "bool queued = mesh_queue_from_frame_at(", matched_queue + 1
        )
        tail = body[unmatched:]

        self.assertRegex(
            tail,
            r"queued\s*&&\s*valid_mesh_frame",
        )
        deferral = tail.index("result_ret = -EAGAIN")
        stop = tail.index("break;", deferral)
        self.assertLess(deferral, stop)

    def test_direct_probe_caller_yields_before_route_repair_rf(self):
        body = function_body(REPORT, "mesh_request_route_owned")
        probe = body.index("ret = mesh_try_direct_gateway_route_probe(")
        policy = body.index(
            "route_policy_state.direct_probe_ret = ret", probe
        )
        wake = body.index("mesh_send_route_wake_train(", policy)
        between = body[probe:policy]

        self.assertRegex(
            between,
            r"ret\s*!=\s*0\s*&&[\s\S]*"
            r"k_msgq_num_used_get\(&mesh_rx_msgq\)\s*!=\s*0u",
        )
        self.assertIn("mesh_schedule_route_waiting_retry_after(", between)
        self.assertIn("REPORT_TX_RETRY_DELAY_MS", between)
        self.assertRegex(between, r"return\s+-E(?:BUSY|AGAIN)\s*;")
        self.assertLess(probe, policy)
        self.assertLess(policy, wake)

    def test_untracked_route_probe_ack_is_consumed_without_queue(self):
        body = function_body(
            REPORT, "mesh_wait_for_direct_gateway_ack_configured"
        )
        match = body.index("mesh_direct_gateway_ack_matches_packet(")
        tracked = body.index("if (apply_tracked_ack)", match)
        consume = body.index(
            "if (!apply_tracked_ack &&\n"
            "                probe->packet.msg_type == MSG_GATEWAY_ROUTE_REQ)",
            tracked,
        )
        queue = body.index("bool queued = mesh_queue_from_frame_at(", consume)
        consume_block = body[consume:queue]

        self.assertLess(match, queue)
        self.assertLess(tracked, consume)
        self.assertIn("debug->queued_frame = false", consume_block)
        self.assertIn("debug->valid_frame = true", consume_block)
        self.assertIn("result_ret = 0", consume_block)
        self.assertIn("break;", consume_block)

    def test_tracked_and_non_probe_direct_acks_keep_normal_rx_custody(self):
        body = function_body(
            REPORT, "mesh_wait_for_direct_gateway_ack_configured"
        )
        match = body.index("mesh_direct_gateway_ack_matches_packet(")
        tracked = body.index("if (apply_tracked_ack)", match)
        tracked_handler = body.index(
            "mesh_ch9_tx_pending_handle_ack(", tracked
        )
        narrow_bypass = body.index("if (!apply_tracked_ack &&", tracked_handler)
        route_only = body.index(
            "probe->packet.msg_type == MSG_GATEWAY_ROUTE_REQ", narrow_bypass
        )
        queue = body.index("bool queued = mesh_queue_from_frame_at(", route_only)

        self.assertLess(tracked, tracked_handler)
        self.assertLess(tracked_handler, narrow_bypass)
        self.assertLess(narrow_bypass, route_only)
        self.assertLess(route_only, queue)

    def test_route_continuations_do_not_inline_large_rx_drain(self):
        route = function_body(REPORT, "mesh_request_route_owned")
        rebroadcast = function_body(
            REPORT, "mesh_execute_route_request_action"
        )

        self.assertNotIn("mesh_process_queued_rx_now", route)
        self.assertNotIn("mesh_process_queued_rx_now", rebroadcast)

    def test_rebroadcast_direct_probe_rearms_owner_before_rx_yield(self):
        body = function_body(REPORT, "mesh_execute_route_request_action")
        probe = body.index("ret = mesh_try_direct_gateway_route_probe(")
        direct_success = body.index("if (ret == 0", probe)
        wake = body.index("mesh_send_route_wake_train(", direct_success)
        between = body[probe:direct_success]

        self.assertRegex(
            between,
            r"ret\s*!=\s*0\s*&&[\s\S]*"
            r"k_msgq_num_used_get\(&mesh_rx_msgq\)\s*!=\s*0u",
        )
        pending = between.index("mesh_route_request_action_pending = true")
        reschedule = between.index("mesh_reschedule_owned_work(", pending)
        self.assertIn(
            "&mesh_route_request_action_work",
            between[reschedule : reschedule + 180],
        )
        exits = [
            between.find(marker, reschedule)
            for marker in ("goto out;", "return false;")
        ]
        exits = [index for index in exits if index >= 0]
        self.assertTrue(exits)
        exit_index = min(exits)
        self.assertLess(pending, reschedule)
        self.assertLess(reschedule, exit_index)
        self.assertLess(direct_success, wake)

    def test_c5_flood_delay_keeps_priority_redundancy_and_background_rx_yield(self):
        sleep_body = function_body(REPORT, "mesh_c5_flood_sleep_until_ms")
        priority = sleep_body.index(
            "flood_ctx != NULL && flood_ctx->response_priority"
        )
        bounded_wait = sleep_body.index("mesh_wait_until_ms(due_ms)", priority)
        priority_return = sleep_body.index("return;", bounded_wait)
        rx_loop = sleep_body.index("while (true)", priority_return)
        queue = sleep_body.index("mesh_queue_from_frame_at_internal(")
        queued = sleep_body.index("if (queued)", queue)
        submit = sleep_body.index("mesh_submit_owned_work(&mesh_rx_work", queued)
        valid = sleep_body.index("valid_mesh_frame", submit)
        yield_return = sleep_body.find("return;", submit)
        loop_close = sleep_body.rindex("}")

        self.assertLess(priority, bounded_wait)
        self.assertLess(bounded_wait, priority_return)
        self.assertLess(priority_return, rx_loop)
        self.assertLess(rx_loop, queue)
        self.assertNotIn(
            "mesh_queue_from_frame_at_internal",
            sleep_body[priority:priority_return],
        )
        self.assertLess(queue, queued)
        self.assertLess(queued, submit)
        self.assertLess(submit, valid)
        self.assertGreaterEqual(yield_return, 0)
        self.assertLess(submit, yield_return)
        self.assertLess(yield_return, loop_close)

        defer = function_body(REPORT, "mesh_c5_flood_defer_active_cb")
        coordinator = defer.index("mesh_coordinator_decide_now(")
        pending = defer.find("mesh_rx_pending_count()")
        self.assertGreaterEqual(pending, 0)
        self.assertLess(pending, coordinator)
        self.assertRegex(
            defer[pending:coordinator],
            r"mesh_rx_pending_count\(\)\s*>\s*0u[\s\S]*return true;",
        )

    def test_route_reply_listener_releases_after_every_queued_owner_class(self):
        body = function_body(REPORT, "mesh_listen_for_route_reply")
        queue = body.index("mesh_queue_from_frame_at_internal(")
        release = body.index(
            'mesh_radio_standby_with_bounded_recovery(', queue
        )
        queued_tail = body[queue:release]

        for classifier in (
            "app_mesh_c5_route_capture_yields_to_competing_request",
            "app_mesh_c5_route_capture_completes_discovery",
            "mesh_packet_is_event_control_type",
        ):
            self.assertIn(
                "break;", braced_block_after(queued_tail, classifier)
            )
        self.assertIn(
            "break;",
            braced_block_after(queued_tail, "if (gateway_priority_control)"),
        )

        submit = body.index("mesh_submit_owned_work(&mesh_rx_work", release)
        self.assertLess(queue, release)
        self.assertLess(release, submit)

    def test_queued_gateway_control_yields_without_false_route_success(self):
        listener = function_body(REPORT, "mesh_listen_for_route_reply")
        classify = listener.index("bool gateway_priority_control")
        queue = listener.index("mesh_queue_from_frame_at_internal(", classify)
        release = listener.index(
            "mesh_radio_standby_with_bounded_recovery(", queue
        )
        queued = listener[queue:release]
        gateway_block = braced_block_after(
            queued, "if (gateway_priority_control)"
        )

        yield_match = re.search(
            r"\b([a-zA-Z_][a-zA-Z0-9_]*)\s*=\s*true;\s*"
            r"last_ret\s*=\s*-EAGAIN;",
            gateway_block,
        )
        self.assertIsNotNone(
            yield_match,
            "queued gateway control must be marked as a listener yield, not "
            "a successful route capture",
        )
        assert yield_match is not None
        yield_flag = yield_match.group(1)
        self.assertIn("break;", gateway_block)

        cleanup = listener[release:]
        self.assertRegex(
            cleanup,
            rf"if\s*\([^)]*\b{re.escape(yield_flag)}\b[^)]*\)\s*\{{\s*"
            r"MESH_ROUTE_LISTENER_RETURN\(-EAGAIN\);",
            "the queued control must be processed after radio release while "
            "the interrupted route wait remains incomplete",
        )

        rebroadcast = function_body(
            REPORT, "mesh_execute_route_request_action"
        )
        self.assertIn(
            "handled = route_reply_captured || listen_ret == 0;",
            rebroadcast,
        )

    def test_anchor_route_reply_listener_uses_locked_shared_parse_scratch(self):
        body = function_body(
            REPORT_ROUTE_CONTROL, "mesh_listen_for_route_reply"
        )
        anchor = body.index("#if DEVICE_ROLE == ROLE_ANCHOR")
        other_role = body.index("#else", anchor)
        anchor_setup = body[anchor:other_role]
        lock = body.index(
            "k_mutex_lock(&mesh_route_reply_ack_scratch_lock", other_role
        )
        lock_guard_end = body.index("#endif", lock)
        loop = body.index(
            "while (capture_count < MESH_ROUTE_TEST_REPLY_CAPTURE_MAX)",
            lock_guard_end,
        )
        loop_body = braced_block_after(body[loop:], "while (")
        post_lock = body[lock_guard_end:]

        self.assertIn(
            "#define parsed mesh_route_reply_ack_parsed", anchor_setup
        )
        unlock = anchor_setup.index(
            "k_mutex_unlock(&mesh_route_reply_ack_scratch_lock)"
        )
        returned = anchor_setup.index("return (value);", unlock)
        self.assertLess(unlock, returned)
        self.assertNotIn("struct mesh_frame_parse_context", anchor_setup)
        self.assertNotIn("struct mesh_frame_parse_context", loop_body)
        self.assertIn("memset(&parsed, 0, sizeof(parsed))", loop_body)
        self.assertNotRegex(post_lock, r"(?m)^\s*return\b")
        self.assertGreaterEqual(
            post_lock.count("MESH_ROUTE_LISTENER_RETURN("), 6
        )

    def test_rx_drain_applies_ack_to_core_before_route_repair_rearm(self):
        body = function_body(REPORT, "mesh_drain_rx_queue_locked")
        tracked_ack = body.index("mesh_ch9_tx_pending_handle_ack(")
        core = body.index("mesh_relay_handle_rx_with_random(", tracked_ack)
        repair_action = body.index(
            "MESH_RELAY_ACTION_TRANSIT_GATEWAY_ACK_ROUTE_REPAIR", core
        )
        repair_schedule = body.index(
            "mesh_schedule_route_request_authorized(", repair_action
        )

        self.assertLess(tracked_ack, core)
        self.assertLess(core, repair_action)
        self.assertLess(repair_action, repair_schedule)

    def test_route_request_pre_rf_busy_uses_identity_backoff(self):
        body = function_body(REPORT, "mesh_route_request_defer_rf_busy")

        self.assertIn("mesh_route_request_rf_retry_key", body)
        self.assertIn("mesh_route_request_rf_retry_state(phase)", body)
        self.assertIn("mesh_rf_retry_next_delay_ms", body)
        self.assertIn("APP_MESH_RF_RETRY_POLICY_WAKE_TRAIN", body)
        self.assertNotIn("MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS", body)

    def test_route_discovery_backoff_gates_before_direct_probe_side_effect(self):
        body = function_body(REPORT, "mesh_request_route_owned")
        coordinator_gate = body.index("mesh_coordinator_mesh_work_allowed(")
        backoff_gate = body.index(
            "mesh_relay_route_discovery_backoff_pending("
        )
        exact_schedule = body.index(
            "mesh_schedule_route_waiting_retry_after(", backoff_gate
        )
        policy = body.index("app_mesh_route_request_policy_decide(")
        direct_probe = body.index("mesh_try_direct_gateway_route_probe(")

        self.assertLess(coordinator_gate, backoff_gate)
        self.assertLess(backoff_gate, exact_schedule)
        self.assertLess(exact_schedule, policy)
        self.assertLess(policy, direct_probe)
        backoff_block = body[backoff_gate:policy]
        self.assertIn("route_backoff_remaining_ms", backoff_block)
        self.assertIn("return -EAGAIN", backoff_block)

    def test_exact_async_coalesce_preserves_existing_retry_due_time(self):
        body = function_body(
            REPORT, "mesh_schedule_route_request_authorized"
        )
        snapshot_pending = body.index(
            "prior_pending = mesh_route_discovery_request.pending"
        )
        submit = body.index("app_mesh_async_route_request_submit(")
        same_generation = body.index(
            "prior_generation == mesh_route_discovery_request.generation"
        )
        remaining_delay = body.index(
            "app_mesh_async_route_request_retry_delay_ms(", same_generation
        )
        reschedule = body.index(
            "mesh_reschedule_owned_work(&mesh_route_discovery_work",
            remaining_delay,
        )

        self.assertLess(snapshot_pending, submit)
        self.assertLess(submit, same_generation)
        self.assertLess(same_generation, remaining_delay)
        self.assertLess(remaining_delay, reschedule)
        self.assertIn("schedule_delay_ms", body[remaining_delay:reschedule])

    def test_route_request_wake_success_cannot_clear_control_retry_round(self):
        body = function_body(REPORT, "mesh_request_route_owned")
        wake_success = body.index(
            "app_mesh_rf_retry_note_success(&mesh_route_request_wake_rf_retry"
        )
        control_send = body.index(
            "mesh_send_outbound_authorized("
        )
        control_failure = body.index(
            "app_mesh_rf_retry_reset(&mesh_route_request_control_rf_retry)"
        )

        self.assertLess(wake_success, control_send)
        self.assertGreater(control_failure, control_send)
        self.assertNotIn(
            "mesh_route_request_control_rf_retry",
            body[wake_success - 250 : wake_success + 250],
        )

    def test_direct_batch_failure_cannot_fall_through_to_immediate_single_send(self):
        body = function_body(
            REPORT, "mesh_try_send_report_tx_ch9_direct_gateway_batch"
        )
        disabled = body.index(
            "if (!MESH_DIRECT_GATEWAY_BATCHING_ENABLED)"
        )
        fail_closed = body.index("return -ENOTSUP", disabled)
        argument_validation = body.index("if (plan == NULL", fail_closed)

        self.assertLess(disabled, fail_closed)
        self.assertLess(fail_closed, argument_validation)

    def test_report_failures_keep_per_packet_retry_rounds(self):
        body = function_body(REPORT, "report_tx_work_handler")

        self.assertGreaterEqual(
            body.count("mesh_rf_retry_bank_next_delay_ms"), 2
        )
        self.assertIn("mesh_report_rf_retry_bank", body)
        self.assertIn("report_tx_consume_retry_delay_override", body)
        self.assertNotIn(
            "report_tx_consume_retry_delay_override(\n"
            "                MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS",
            body,
        )

    def test_report_channel5_policy_deferral_uses_packet_backoff(self):
        body = function_body(REPORT, "report_tx_work_handler")
        policy_start = body.index("if (ret == -EBUSY && report_policy_deferred)")
        ordinary_wait = body.index("if (ret == -EBUSY) {", policy_start + 1)
        policy_block = body[policy_start:ordinary_wait]

        self.assertIn("APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY", policy_block)
        self.assertIn("mesh_rf_retry_bank_next_delay_ms", policy_block)
        self.assertIn("report_tx_schedule_backoff", policy_block)
        self.assertNotIn("mesh_rf_retry_bank_next_delay_ms", body[ordinary_wait:])

    def test_relay_batch_failure_keeps_failed_packet_backoff_after_partial_send(self):
        body = function_body(REPORT, "mesh_try_send_report_tx_ch9_batch")
        send_marker = "mesh_send_outbound_preconfigured_ch9_locked"
        send_index = body.index(send_marker)
        partial_schedule = body.index(
            '"queued-ch9-batch-partial-send"', send_index
        )
        handled_return = body.rindex("return 0;")

        self.assertIn(
            "mesh_rf_retry_bank_next_delay_ms",
            body[send_index:partial_schedule],
        )
        self.assertIn(
            "report_tx_schedule_backoff",
            body[send_index:partial_schedule],
        )
        self.assertLess(partial_schedule, handled_return)
        caller = function_body(REPORT, "report_tx_work_handler")
        call = caller.index("ret = mesh_try_send_report_tx_ch9_batch()")
        handled = caller.index("if (ret == 0)", call)
        stop = caller.index("return;", handled)
        self.assertLess(call, handled)
        self.assertLess(handled, stop)

    def test_cir_fragment_encoding_runs_only_after_anchor_scan_release(self):
        start = function_body(REPORT_ENCODER, "anchor_cir_report_start")
        refill_wrapper = function_body(
            REPORT_ENCODER, "app_mesh_report_encode_queue_next_cir"
        )
        report_worker = function_body(REPORT, "report_tx_work_handler")

        self.assertIn("anchor_cir_report_stream.active = true", start)
        for forbidden in (
            "anchor_cir_report_queue_next(",
            "app_mesh_report_encode_queue_next_cir(",
            "report_tx_work_handler(",
            "report_tx_schedule(",
            "report_encode_ops.queue_cir_fragment",
            "struct mesh_outbound",
            "PACKET_EXT_MAX_PAYLOAD_LEN",
        ):
            self.assertNotIn(forbidden, start)

        self.assertIn("return anchor_cir_report_queue_next();", refill_wrapper)
        self.assertEqual(REPORT_ENCODER.count("anchor_cir_report_queue_next("), 2)

        refill_call_sites = []
        app_source = ROOT / "app" / "src"
        for source_path in sorted(app_source.iterdir()):
            if source_path.suffix not in (".c", ".inc"):
                continue
            source = source_path.read_text(encoding="utf-8")
            refill_call_sites.extend(
                source_path.name
                for _ in re.finditer(
                    r"\bapp_mesh_report_encode_queue_next_cir\s*\(\s*\)",
                    source,
                )
            )
        self.assertEqual(refill_call_sites, ["app_mesh_report_delivery.inc"])
        self.assertEqual(
            report_worker.count("app_mesh_report_encode_queue_next_cir()"), 1
        )

        for scan_function in (
            "anchor_run_mesh_click_wake_claim",
            "anchor_uwb_scan_work_handler",
        ):
            scan = function_body(ANCHOR_RADIO, scan_function)
            release_finish = scan.index("radio_guard_uwb_release_finish(")
            release_failure = scan.index("if (release_ret < 0)", release_finish)
            report_schedule = scan.index("report_tx_schedule(0u)", release_failure)

            self.assertEqual(scan.count("report_tx_schedule(0u)"), 1)
            self.assertLess(release_finish, release_failure)
            self.assertLess(release_failure, report_schedule)
            self.assertIn(
                "return",
                braced_block_after(scan, "if (release_ret < 0)"),
            )

    def test_compact_cir_remains_in_base_report_when_full_stream_is_off(self):
        full_cir_config = KCONFIG.split(
            "config IMEC_MESH_CLICK_FULL_CIR_REPORTING", 1
        )[1].split("\nconfig ", 1)[0]
        diagnostics = function_body(DWM3000_RADIO, "read_rx_diagnostics")
        report_builder = function_body(
            REPORT_ENCODER, "build_range_report_samples"
        )
        capture = function_body(
            REPORT_ENCODER, "mesh_anchor_click_cir_capture_begin"
        )
        queue_next = function_body(
            REPORT_ENCODER, "app_mesh_report_encode_queue_next_cir"
        )

        self.assertRegex(full_cir_config, r"(?m)^\s*default n\s*$")
        self.assertIn(
            "The compact\n      six-byte CIR tap remains in the base click "
            "report when this is disabled.",
            full_cir_config,
        )
        self.assertNotIn(
            "CONFIG_IMEC_MESH_CLICK_FULL_CIR_REPORTING=y", ANCHOR_CONF
        )

        self.assertIn("read_cir_sample(&diagnostics, cir_sample)", diagnostics)
        self.assertIn("*cir_sampled = true", diagnostics)
        self.assertNotIn(
            "CONFIG_IMEC_MESH_CLICK_FULL_CIR_REPORTING", diagnostics
        )

        compact_sample = report_builder.index(
            "fields.cir_sample = range_result->cir_sampled ?"
        )
        full_stream_gate = report_builder.index(
            "defined(CONFIG_IMEC_MESH_CLICK_FULL_CIR_REPORTING)"
        )
        full_stream_start = report_builder.index(
            "anchor_cir_report_start(", full_stream_gate
        )
        self.assertLess(compact_sample, full_stream_gate)
        self.assertLess(full_stream_gate, full_stream_start)

        disabled_capture = capture.index("#else")
        self.assertIn("*capacity = 0u", capture[disabled_capture:])
        self.assertIn("return NULL", capture[disabled_capture:])
        disabled_queue = queue_next.index("#else")
        self.assertIn("return -ENOENT", queue_next[disabled_queue:])

    def test_cir_refill_is_retryable_and_cannot_be_starved_by_queue_depth(self):
        refill = function_body(
            REPORT_ENCODER, "anchor_cir_report_queue_next"
        )
        report_worker = function_body(REPORT, "report_tx_work_handler")

        queue_call = refill.index(
            "report_encode_ops.queue_cir_fragment(&outbound, &queue_depth)"
        )
        commit_offset = refill.index(
            "anchor_cir_report_stream.next_offset += chunk_len", queue_call
        )
        commit_fragment = refill.index(
            "anchor_cir_report_stream.next_fragment_index++", queue_call
        )
        commit_seq = refill.index(
            "anchor_cir_report_stream.next_seq++", queue_call
        )
        queue_full_return = refill.index("return -ENOSPC;", queue_call)

        for mutation in (
            "anchor_cir_report_stream.next_offset += chunk_len",
            "anchor_cir_report_stream.next_fragment_index++",
            "anchor_cir_report_stream.next_seq++",
            "anchor_cir_report_stream.active = false",
        ):
            self.assertNotIn(mutation, refill[:queue_call])
        self.assertEqual(
            refill.count(
                "anchor_cir_report_stream.next_offset += chunk_len"
            ),
            1,
        )
        self.assertEqual(
            refill.count(
                "anchor_cir_report_stream.next_fragment_index++"
            ),
            1,
        )
        self.assertEqual(
            refill.count("anchor_cir_report_stream.next_seq++"), 1
        )
        self.assertTrue(queue_call < commit_offset < queue_full_return)
        self.assertTrue(queue_call < commit_fragment < queue_full_return)
        self.assertTrue(queue_call < commit_seq < queue_full_return)

        success = braced_block_after(refill[queue_call:], "if (ret == 0)")
        generation_check = success.index(
            "anchor_cir_report_stream.generation != generation"
        )
        self.assertLess(generation_check, success.index(
            "anchor_cir_report_stream.next_offset += chunk_len"
        ))
        self.assertIn(
            "anchor_cir_report_stream.active = false", success
        )

        queue_failure = refill[refill.rindex(
            "key = k_spin_lock(&anchor_cir_report_lock);",
            queue_call,
            queue_full_return,
        ):queue_full_return]
        self.assertIn(
            "anchor_cir_report_stream.queue_in_progress = false",
            queue_failure,
        )
        self.assertNotIn(
            "anchor_cir_report_stream.active = false", queue_failure
        )
        self.assertNotIn(
            "anchor_cir_report_stream.next_offset", queue_failure
        )

        refill_call = report_worker.index(
            "app_mesh_report_encode_queue_next_cir()"
        )
        range_batch_guard = braced_block_after(
            report_worker, "if (range_batch_active)"
        )
        range_batch_guard_end = (
            report_worker.index(range_batch_guard) + len(range_batch_guard)
        )
        before_refill = report_worker[range_batch_guard_end:refill_call]
        self.assertNotIn("report_tx_queue_used()", before_refill)
        self.assertNotIn("return", before_refill)
        queue_state = report_worker.index(
            "report_queue_pending = report_tx_queue_used() > 0u",
            refill_call,
        )
        anchor_busy = report_worker.index("anchor_busy =", refill_call)
        self.assertLess(refill_call, anchor_busy)
        self.assertNotIn("return", report_worker[refill_call:anchor_busy])
        self.assertIn("ret != -ENOSPC", report_worker[refill_call:queue_state])

    def test_anchor_claim_turn_reuses_the_exclusive_scan_frame(self):
        claim = function_body(ANCHOR_RADIO, "anchor_handle_uwb_claim")

        self.assertIn(
            "uint8_t *const frame = anchor_uwb_scan_frame;",
            claim,
        )
        self.assertNotIn("uint8_t frame[UWB_MESH_MAX_FRAME_LEN]", claim)
        self.assertNotIn("sizeof(frame)", claim)
        self.assertGreaterEqual(
            claim.count("sizeof(anchor_uwb_scan_frame)"),
            4,
        )

    def test_anchor_reserves_report_custody_before_discovery_or_ranging(self):
        claim = function_body(ANCHOR_RADIO, "anchor_handle_uwb_claim")
        reserve = function_body(
            REPORT_DELIVERY, "mesh_range_report_batch_reserve_capacity"
        )
        abort = function_body(REPORT_DELIVERY, "mesh_range_report_batch_abort")
        backlog = function_body(
            REPORT_DELIVERY, "mesh_report_tx_backlog_active"
        )
        capture = function_body(
            REPORT_ENCODER, "mesh_anchor_click_cir_capture_begin"
        )
        pending = function_body(
            REPORT_ENCODER, "app_mesh_report_encode_cir_pending"
        )

        preempt = claim.index("mesh_preempt_for_click_event_until(")
        reservation = claim.index(
            "mesh_range_report_batch_reserve_capacity(", preempt
        )
        discovery_config = claim.index(
            "dwm3000_driver_configure_wake_mode", reservation
        )
        discovery_reply = claim.index(
            "dwm3000_driver_send_frame(", discovery_config
        )
        ranging = claim.index(
            "anchor_run_scheduled_uwb_ranges(", discovery_reply
        )
        cleanup = claim.index("mesh_range_report_batch_abort(", ranging)

        self.assertLess(preempt, reservation)
        self.assertLess(reservation, discovery_config)
        self.assertLess(discovery_config, discovery_reply)
        self.assertLess(discovery_reply, ranging)
        self.assertLess(ranging, cleanup)
        self.assertIn("goto claim_complete;", claim[reservation:cleanup])

        self.assertIn(
            "k_msgq_num_free_get(&report_tx_msgq) < fragment_capacity",
            reserve,
        )
        self.assertIn(
            ".queue_prefix_count = (uint8_t)queue_count",
            reserve,
        )
        capacity_gate = reserve[: reserve.index("memset(&control")]
        self.assertNotIn(
            "if (app_mesh_report_encode_cir_pending()", capacity_gate
        )
        self.assertNotIn(
            "||\n        mesh_ch9_tx_pending_is_active()", capacity_gate
        )
        self.assertIn(
            ".queue_prefix_count +\n                                     i",
            abort,
        )
        self.assertIn(
            "MESH_RANGE_REPORT_BATCH_ABORT_ROTATE_PUT", abort
        )
        self.assertIn("app_mesh_report_encode_cir_pending()", backlog)

        active = capture.index("if (anchor_cir_report_stream.active)")
        reject = capture.index("return NULL;", active)
        generation = capture.index("anchor_cir_report_stream.generation++")
        self.assertLess(active, reject)
        self.assertLess(reject, generation)
        self.assertNotIn("anchor_cir_report_stream.active = false", capture)
        self.assertIn("anchor_cir_report_stream.active", pending)
        self.assertIn("k_spin_lock(&anchor_cir_report_lock)", pending)

    def test_deferred_gateway_ack_separates_plan_wait_from_send_failure(self):
        send_body = function_body(
            REPORT, "mesh_try_deferred_gateway_ack_on_channel9"
        )
        waiting_body = function_body(REPORT, "mesh_try_route_waiting_tx")
        event_handler = function_body(
            REPORT_EVENT_TX, "mesh_handle_event_control"
        )
        accept_finish = function_body(
            REPORT_EVENT_TX, "mesh_event_accept_finish_send"
        )
        attempted_branch = waiting_body.index("if (gateway_ack_send_attempted)")
        service_busy_branch = waiting_body.index("if (ret == -EBUSY)")

        self.assertIn("*send_attempted = false", send_body)
        self.assertIn("*send_attempted = true", send_body)
        self.assertLess(attempted_branch, service_busy_branch)
        self.assertIn(
            "APP_MESH_RF_RETRY_OPERATION_DEFERRED_GATEWAY_ACK",
            waiting_body[attempted_branch:service_busy_branch],
        )
        self.assertIn(
            "mesh_rf_retry_next_delay_ms",
            waiting_body[attempted_branch:service_busy_branch],
        )
        self.assertIn("gateway_ack_policy_deferred", waiting_body)
        self.assertIn("gateway-ack-channel5-deferral", waiting_body)
        self.assertIn("gateway_ack_wait_retry_delay_ms", waiting_body)

        # The ACCEPT sender owns RX in the first/current negotiated event. A
        # successful send installs the phase reanchored to that actual RF
        # attempt and arms its RX owner; gateway-ACK forwarding must therefore
        # plan the following TX turn.
        propose_start = event_handler.index(
            "if (packet->msg_type == MSG_MESH_EVENT_PROPOSE)"
        )
        accept_rx_start = event_handler.index(
            "} else if (packet->msg_type == MSG_MESH_EVENT_ACCEPT)",
            propose_start,
        )
        propose = event_handler[propose_start:accept_rx_start]
        rx_parity = propose.index(
            "mesh_event_timing_set_local_first_slot_tx(&timing, false)"
        )
        accept_attempt = propose.index("mesh_event_accept_attempt(", rx_parity)
        self.assertLess(rx_parity, accept_attempt)

        install = accept_finish.index(
            "mesh_install_channel9_timing_direction("
        )
        schedule_rx = accept_finish.index("mesh_schedule_uwb_rx(delay_ms)")
        self.assertIn("committed_timing", accept_finish[install:schedule_rx])
        self.assertLess(install, schedule_rx)

        plan_wait = braced_block_after(
            send_body, "plan.action == MESH_EVENT_PLAN_WAIT"
        )
        self.assertIn("app_mesh_ch9_wait_plan_retry_delay_ms(", plan_wait)
        self.assertIn("wait_retry_delay_ms", plan_wait)
        self.assertNotIn("REPORT_TX_RETRY_DELAY_MS", plan_wait)

        service_wait = braced_block_after(waiting_body, "if (ret == -EBUSY)")
        self.assertIn(
            "uint32_t retry_delay_ms = gateway_ack_wait_retry_delay_ms",
            service_wait,
        )
        self.assertIn(
            "retry_delay_ms = MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS",
            service_wait,
        )
        self.assertIn('"gateway-ack-channel9-wait"', service_wait)
        self.assertIn("mesh_schedule_route_waiting_retry_after(", service_wait)
        self.assertNotIn("REPORT_TX_RETRY_DELAY_MS", service_wait)

    def test_route_wait_delivery_separates_transport_attempt_from_service_wait(self):
        send_body = function_body(REPORT, "mesh_start_tracked_tx_with_retry")
        waiting_body = function_body(REPORT, "mesh_try_route_waiting_tx")
        attempt_branch = waiting_body.index("else if (route_wait_send_attempted)")
        policy_decision = waiting_body.index("app_mesh_route_wait_tx_decide")

        self.assertIn("*send_attempted = false", send_body)
        self.assertIn("*send_attempted = true", send_body)
        self.assertLess(attempt_branch, policy_decision)
        self.assertIn(
            "APP_MESH_RF_RETRY_OPERATION_ROUTE_WAIT_DELIVERY",
            waiting_body[attempt_branch:policy_decision],
        )
        self.assertIn(
            "mesh_rf_retry_next_delay_ms",
            waiting_body[attempt_branch:policy_decision],
        )
        self.assertIn(
            "report_tx_consume_retry_delay_override",
            waiting_body[attempt_branch:policy_decision],
        )

    def test_pre_rf_send_failure_does_not_count_parent_ack_failure(self):
        body = function_body(REPORT, "mesh_start_tracked_tx_with_retry")

        self.assertIn("mesh_relay_note_tx_sent", body)
        self.assertIn("No RF send completed", body)
        self.assertNotIn("mesh_relay_note_delivery_failure", body)

    def test_initial_missing_timing_repairs_selected_relay_before_discovery(self):
        body = function_body(REPORT, "mesh_start_tracked_tx_with_retry")
        unavailable = body.index(
            '"mesh channel-9 timing unavailable for %s; '
            'refreshing channel-5 contact: ret=%d"'
        )
        repair = body.index(
            "mesh_try_repair_selected_parent_event(", unavailable
        )
        repair_return = body.index(
            "return known_route_retry ? -EBUSY : -EHOSTUNREACH;", repair
        )
        fallback = body.index(
            "mesh_defer_route_for_outbound(&aged_out", repair_return
        )

        self.assertLess(unavailable, repair)
        self.assertLess(repair, repair_return)
        self.assertLess(repair_return, fallback)
        self.assertIn("debug_select_ret", body[repair:repair_return])
        self.assertIn("debug_next_hop", body[repair:repair_return])
        self.assertNotIn(
            "mesh_request_route", body[unavailable:fallback]
        )

        helper = function_body(
            REPORT, "mesh_try_repair_selected_parent_event"
        )
        store = helper.index("mesh_store_route_waiting_tx_owned(")
        propose = helper.index(
            "mesh_propose_event_for_outbound("
        )
        hard_branch = helper.index(
            "if (hard_failure && topology_operation)", propose
        )

        self.assertLess(store, propose)
        self.assertLess(propose, hard_branch)
        self.assertIn('"initial-tx-event-repair"', helper[propose:hard_branch])
        self.assertIn(
            "repair_active = mesh_event_propose_retry.active",
            helper[propose:hard_branch],
        )
        self.assertNotIn("mesh_schedule_route_request", helper[:hard_branch])

    def test_busy_parent_route_wait_transfer_keeps_one_report_owner(self):
        worker = function_body(REPORT, "report_tx_work_handler")
        begin = worker.index(
            "report_tx_queue_begin_head(outbound, &head_token)"
        )
        send = worker.index("mesh_start_tracked_tx_with_retry(", begin)
        transfer_guard = worker.index(
            "if (mesh_route_waiting_tx_owns_exact(", send
        )
        busy_fallback = worker.index(
            "if (ret == -EBUSY && report_policy_deferred)", transfer_guard
        )

        self.assertLess(begin, send)
        self.assertLess(send, transfer_guard)
        self.assertLess(transfer_guard, busy_fallback)
        self.assertIn(
            "app_mesh_route_wait_tx_may_store(\n"
            "                                               "
            "APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC)",
            worker[send:transfer_guard],
        )

        transfer_condition = worker[
            transfer_guard : worker.index("{", transfer_guard)
        ]
        self.assertIn(
            "APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC", transfer_condition
        )
        self.assertNotIn("ret ==", transfer_condition)

        transfer = braced_block_after(
            worker, "if (mesh_route_waiting_tx_owns_exact("
        )
        commit = transfer.index(
            "report_tx_queue_commit_head(&head_token, outbound)"
        )
        self.assertEqual(
            transfer.count(
                "report_tx_queue_commit_head(&head_token, outbound)"
            ),
            1,
        )
        self.assertNotIn("report_tx_queue_abort_head", transfer[:commit])
        commit_failure = braced_block_after(transfer, "if (ret != 0)")
        self.assertIn(
            "report_tx_queue_abort_head(&head_token)", commit_failure
        )
        self.assertNotIn("report_tx_queue_append(", transfer)
        self.assertRegex(transfer, r"}\s*return;\s*}$")

        send_body = function_body(
            REPORT, "mesh_start_tracked_tx_with_retry"
        )
        timing_missing = send_body.index(
            '"mesh channel-9 timing unavailable for %s; '
            'refreshing channel-5 contact: ret=%d"'
        )
        repair = send_body.index(
            "mesh_try_repair_selected_parent_event(", timing_missing
        )
        busy_return = send_body.index(
            "return known_route_retry ? -EBUSY : -EHOSTUNREACH;", repair
        )
        self.assertLess(timing_missing, repair)
        self.assertLess(repair, busy_return)

        repair_body = function_body(
            REPORT, "mesh_try_repair_selected_parent_event"
        )
        store = repair_body.index("mesh_store_route_waiting_tx_owned(")
        propose = repair_body.index("mesh_propose_event_for_outbound(", store)
        self.assertIn(
            "APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC",
            repair_body[store:propose],
        )
        self.assertLess(store, propose)

        route_wait = function_body(REPORT, "mesh_try_route_waiting_tx")
        retry_send = route_wait.index("mesh_start_tracked_tx_with_retry(")
        success = route_wait.index(
            "route_wait_send_attempted && wait_state.tx_ret == 0", retry_send
        )
        decide = route_wait.index(
            "app_mesh_route_wait_tx_decide(&wait_state, &wait_decision)",
            success,
        )
        clear = route_wait.index(
            "case APP_MESH_ROUTE_WAIT_TX_ACTION_CLEAR_VALID:", decide
        )
        clear_end = route_wait.index("break;", clear)
        self.assertLess(retry_send, success)
        self.assertLess(success, decide)
        self.assertLess(decide, clear)
        self.assertIn(
            "mesh_route_waiting_tx_valid = false",
            route_wait[clear:clear_end],
        )

        actions = function_body(REPORT, "mesh_handle_result_actions")
        custody = braced_block_after(
            actions, "MESH_RELAY_ACTION_TX_NEXT_HOP_CUSTODY_ACCEPTED)"
        )
        self.assertIn(
            "mesh_relay_commit_next_hop_custody_terminal(", custody
        )
        for forbidden in (
            "report_tx_queue_append(",
            "report_tx_queue_begin_head(",
            "report_tx_queue_commit_head(",
            "report_tx_queue_abort_head(",
            "report_tx_queue_recovery_valid = true",
        ):
            self.assertNotIn(forbidden, custody)

    def test_known_parent_wait_honors_existing_retry_deadline_without_spin(self):
        store = function_body(REPORT, "mesh_store_route_waiting_tx_owned")
        coalesce = store.index("mesh_route_waiting_tx_exact_owner_equal(")
        coalesce_return = store.index("return true;", coalesce)
        self.assertNotIn(
            "mesh_schedule_route_waiting_retry_after",
            store[coalesce:coalesce_return],
        )

        waiting = function_body(REPORT, "mesh_try_route_waiting_tx")
        start = waiting.index("mesh_start_tracked_tx_with_retry(")
        decision = waiting.index("app_mesh_route_wait_tx_decide", start)
        self.assertIn(
            "&wait_state.channel9_retry_delay_ms",
            waiting[start:decision],
        )

    def test_initial_forward_wait_retains_exact_channel9_retry_owner(self):
        wrapper = function_body(REPORT, "mesh_start_tracked_tx")
        send = function_body(REPORT, "mesh_start_tracked_tx_with_retry")
        core = function_body(
            MESH_RELAY_DELIVERY, "mesh_relay_retain_channel9_tx_wait"
        )

        call = wrapper.index("mesh_start_tracked_tx_with_retry(")
        self.assertIn("&wait_retry_delay_ms", wrapper[call:])
        self.assertIn("true", wrapper[call:])
        self.assertNotIn("struct mesh_outbound", wrapper)

        wait = send.index("plan.action == MESH_EVENT_PLAN_WAIT")
        retain_guard = send.index("retain_initial_channel9_wait", wait)
        retain = send.index("mesh_relay_retain_channel9_tx_wait(", retain_guard)
        bind = send.index("mesh_relay_bind_transit_previous_hop(", retain)
        cancel = send.index("mesh_relay_cancel_tx_if_matches(", bind)
        retained = send.index("if (defer_ret == PROTO_OK)", cancel)
        schedule = send.index("mesh_schedule_tx_timeout()", retained)
        custody_success = send.index(
            "return initial_channel9_wait_retained ? 0 : -EBUSY", schedule
        )

        self.assertLess(wait, retain_guard)
        self.assertLess(retain_guard, retain)
        self.assertLess(retain, bind)
        self.assertIn("aged_out.ingress_previous_hop_id", send[bind:cancel])
        self.assertLess(bind, cancel)
        self.assertLess(cancel, retained)
        self.assertLess(retained, schedule)
        self.assertIn(
            "DBG_FORWARD_DEFER_UPSTREAM_SLOT", send[retained:schedule]
        )
        self.assertIn(
            "initial_channel9_wait_retained = true", send[schedule:custody_success]
        )
        self.assertLess(schedule, custody_success)

        start = core.index("mesh_relay_start_tx(")
        active = core.index("mesh_relay_tx_active(relay)", start)
        channel = core.index("out->radio_channel = MESH_EVENT_CHANNEL", active)
        defer = core.index("mesh_relay_note_retransmit_deferred(", channel)
        failure = core.index("if (ret != PROTO_OK)", defer)
        rollback = core.index("mesh_relay_cancel_tx_if_matches(", failure)
        self.assertLess(start, active)
        self.assertLess(active, channel)
        self.assertIn(
            "relay->pending.radio_channel = MESH_EVENT_CHANNEL",
            core[channel:defer],
        )
        self.assertLess(channel, defer)
        self.assertLess(defer, failure)
        self.assertLess(failure, rollback)

    def test_initial_timing_repair_keeps_no_route_and_gateway_fallbacks(self):
        helper = function_body(
            REPORT, "mesh_try_repair_selected_parent_event"
        )
        guard_end = helper.index("if (store_route_wait)")
        guard = helper[:guard_end]

        self.assertIn("select_ret != PROTO_OK", guard)
        self.assertIn("!mesh_id_is_unicast(selected_next_hop)", guard)
        self.assertIn("selected_next_hop == DEVICE_ID", guard)
        self.assertIn("selected_next_hop == GATEWAY_ID", guard)
        self.assertIn("return false", guard)

        body = function_body(REPORT, "mesh_start_tracked_tx_with_retry")
        unavailable = body.index(
            '"mesh channel-9 timing unavailable for %s; '
            'refreshing channel-5 contact: ret=%d"'
        )
        repair = body.index(
            "mesh_try_repair_selected_parent_event(", unavailable
        )
        fallback = body.index(
            "mesh_defer_route_for_outbound(&aged_out", repair
        )
        fallback_exit = body.index("return -EHOSTUNREACH;", fallback)
        self.assertLess(repair, fallback)
        self.assertLess(fallback, fallback_exit)

        route_wait = function_body(REPORT, "mesh_try_route_waiting_tx")
        request_action = route_wait.index(
            "case APP_MESH_ROUTE_WAIT_TX_ACTION_REQUEST_ROUTE:"
        )
        request = route_wait.index("mesh_request_route(", request_action)
        self.assertLess(request_action, request)

    def test_initial_timing_repair_hard_terminal_falls_back_to_discovery(self):
        hard = function_body(REPORT, "mesh_parent_contact_failure_is_hard")
        helper = function_body(
            REPORT, "mesh_try_repair_selected_parent_event"
        )
        propose = helper.index(
            "mesh_propose_event_for_outbound("
        )
        classify = helper.index("hard_failure =", propose)
        topology_branch = helper.index(
            "if (hard_failure && topology_operation)", classify
        )
        hard_branch = helper.index("else if (hard_failure)", topology_branch)
        invalidate = helper.index(
            "mesh_relay_invalidate_upstream_route", hard_branch
        )
        request = helper.index("mesh_schedule_route_request", invalidate)

        for terminal in (
            "-ETIMEDOUT",
            "-EHOSTUNREACH",
            "-ENOTCONN",
            "-ECONNRESET",
        ):
            self.assertIn(terminal, hard)
        self.assertIn("!repair_active", helper[classify:hard_branch])
        self.assertIn("repair_ret < 0", helper[classify:hard_branch])
        self.assertLess(propose, classify)
        self.assertLess(classify, topology_branch)
        self.assertLess(topology_branch, hard_branch)
        self.assertIn(
            "mesh_relay_select_next_hop",
            helper[topology_branch:hard_branch],
        )
        self.assertIn(
            "alternate_parent != selected_next_hop",
            helper[topology_branch:hard_branch],
        )
        self.assertLess(hard_branch, invalidate)
        self.assertLess(invalidate, request)
        self.assertEqual(helper.count("mesh_schedule_route_request"), 1)
        self.assertEqual(helper.count("mesh_relay_invalidate_upstream_route"), 1)

    def test_assignment_ack_miss_enters_prompt_jittered_core_retry_only(self):
        classify = function_body(
            REPORT_EVENT_TX, "mesh_outbound_is_assignment_response"
        )
        policy = function_body(
            REPORT_EVENT_TX, "mesh_handle_direct_gateway_retry_policy"
        )

        for exact_guard in (
            "out->packet.msg_type == MSG_COMMAND_RESULT",
            "discovery_assignment_parse_result_tlvs(",
            "DISCOVERY_ASSIGNMENT_PHASE_CLAIM",
            "DISCOVERY_ASSIGNMENT_PHASE_ACK",
        ):
            self.assertIn(exact_guard, classify)

        custody = policy.index("mesh_relay_tx_active(&mesh_runtime)")
        assignment = policy.index(
            "if (mesh_outbound_is_assignment_response(tx))", custody
        )
        jitter = policy.index("mesh_rf_retry_next_delay_ms(", assignment)
        reliable = policy.index(
            "APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA", jitter
        )
        defer = policy.index(
            "mesh_relay_note_retransmit_deferred(", reliable
        )
        deadline = policy.index("k_uptime_get_32() + retry_delay_ms", defer)
        schedule = policy.index("mesh_schedule_tx_timeout()", deadline)
        generic = policy.index(
            "DBG_DIRECT_GW_ACK_DEFER_CORE", schedule
        )

        self.assertLess(custody, assignment)
        self.assertLess(assignment, jitter)
        self.assertLess(jitter, reliable)
        self.assertLess(reliable, defer)
        self.assertLess(defer, deadline)
        self.assertLess(deadline, schedule)
        self.assertLess(schedule, generic)
        self.assertNotIn(
            "mesh_relay_note_retransmit_deferred(", policy[generic:]
        )
        self.assertIn("mesh_schedule_tx_timeout()", policy[generic:])

    def test_topology_event_contact_gets_one_rf_retry_then_alternate(self):
        classify = function_body(
            REPORT_EVENT_TX, "mesh_outbound_is_topology_operation_uplink"
        )
        propose = function_body(
            REPORT_EVENT_TX,
            "mesh_propose_event_after_channel5_contact_authorized",
        )
        assignment = function_body(
            REPORT_EVENT_TX, "mesh_outbound_is_assignment_response"
        )
        retry = function_body(
            REPORT_EVENT_TX, "mesh_event_propose_retry_after_failure"
        )
        terminal = function_body(
            REPORT_EVENT_TX, "mesh_event_propose_terminal_failure"
        )

        self.assertIn("mesh_outbound_is_assignment_response", classify)
        active = propose.index(
            "if (mesh_event_propose_retry.active"
        )
        active_topology = propose.index(
            "topology_operation &&",
            active,
        )
        promote_once = propose.index(
            "!mesh_event_propose_topology_operation", active_topology
        )
        extend = propose.index(
            "app_mesh_event_retry_extend_deadline(", promote_once
        )
        begin = propose.index("app_mesh_event_retry_begin(", extend)
        initial_topology = propose.index("topology_operation ?", begin)
        topology_deadline = propose.index(
            "mesh_event_topology_contact_deadline_ms(now_ms)",
            initial_topology,
        )
        ordinary_deadline = propose.index(
            "now_ms + MESH_EVENT_PROPOSE_RETRY_DEADLINE_MS",
            topology_deadline,
        )

        self.assertLess(active, active_topology)
        self.assertLess(active_topology, promote_once)
        self.assertLess(promote_once, extend)
        self.assertLess(begin, initial_topology)
        self.assertLess(initial_topology, topology_deadline)
        self.assertLess(topology_deadline, ordinary_deadline)
        topology_horizon = function_body(
            REPORT_EVENT_TX, "mesh_event_topology_contact_deadline_ms"
        )
        self.assertIn(
            "now_ms + MESH_TOPOLOGY_PARENT_CONTACT_DEADLINE_MS",
            topology_horizon,
        )
        self.assertRegex(
            REPORT_UNIT,
            r"#define\s+MESH_TOPOLOGY_PARENT_CONTACT_DEADLINE_MS\s+25000u",
        )
        self.assertIn(
            "MESH_TOPOLOGY_PARENT_CONTACT_DEADLINE_MS >=",
            REPORT_UNIT,
        )
        self.assertIn(
            "MESH_TOPOLOGY_PARENT_CONTACT_DEADLINE_MS <",
            REPORT_UNIT,
        )
        self.assertEqual(
            propose.count("app_mesh_event_retry_extend_deadline("), 1
        )
        self.assertIn("MSG_COMMAND_RESULT", assignment)
        self.assertIn("discovery_assignment_parse_result_tlvs(", assignment)
        self.assertIn("DISCOVERY_ASSIGNMENT_PHASE_CLAIM", assignment)
        self.assertIn("DISCOVERY_ASSIGNMENT_PHASE_ACK", assignment)
        self.assertIn(
            "app_mesh_event_retry_note_failure_limited", retry
        )
        self.assertIn("MESH_TOPOLOGY_PARENT_CONTACT_RETRIES", retry)
        self.assertRegex(
            REPORT_UNIT,
            r"#define\s+MESH_TOPOLOGY_PARENT_CONTACT_RETRIES\s+1u",
        )
        # Topology-operation proposal exhaustion is cadence contention on the
        # parent's single downstream slot: keep the parent and retry the same
        # parent on a short jittered cycle. Abandon, alternate search, and
        # route discovery are forbidden in this path.
        self.assertIn("mesh_schedule_route_waiting_retry_after", terminal)
        self.assertIn('"topology-cadence-wait"', terminal)
        self.assertNotIn("mesh_relay_abandon_upstream_parent_at", terminal)
        self.assertNotIn("ROUTE_DELIVERY_TRY_ALTERNATE", terminal)
        self.assertNotIn("mesh_schedule_route_request", terminal)

    def test_retransmit_repairs_missing_selected_parent_despite_downstream_link(self):
        helper = function_body(
            REPORT, "mesh_selected_relay_parent_needs_channel9_repair"
        )
        handler = function_body(REPORT, "mesh_handle_result_actions")
        unavailable = handler.index(
            '"mesh retransmit deferred until channel-9 timing is refreshed:'
        )
        repair_gate = handler.index(
            "mesh_selected_relay_parent_needs_channel9_repair(", unavailable
        )
        propose = handler.index(
            "mesh_propose_event_for_outbound(", repair_gate
        )
        fallback = handler.index(
            "else if (mesh_channel9_connection_count() == 0u)", propose
        )
        repair = handler[propose:fallback]

        self.assertIn("route_selected(&mesh_runtime.upstream)", helper)
        self.assertIn("selected->next_hop_id == selected_next_hop_id", helper)
        self.assertIn("selected->gateway_id == GATEWAY_ID", helper)
        self.assertIn("!selected->channel9_timing_valid", helper)
        self.assertIn("selected_next_hop_id == GATEWAY_ID", helper)
        self.assertLess(repair_gate, propose)
        self.assertLess(propose, fallback)
        self.assertNotIn(
            "mesh_channel9_connection_count", handler[repair_gate:fallback]
        )
        self.assertRegex(
            repair,
            r"mesh_propose_event_for_outbound\(\s*"
            r"debug_next_hop,\s*\"retransmit-event-repair\",\s*"
            r"retransmit\s*\)",
        )
        self.assertNotIn(
            "mesh_propose_event_after_channel5_contact(",
            handler[repair_gate:fallback],
        )

        outbound_repair = function_body(
            REPORT, "mesh_propose_event_for_outbound"
        )
        self.assertIn(
            "mesh_outbound_is_topology_operation_uplink(out)",
            outbound_repair,
        )

    def test_topology_retransmit_reselects_before_parent_failure_accounting(self):
        handler = function_body(REPORT, "mesh_handle_result_actions")
        repair = handler.index(
            "mesh_propose_event_for_outbound(\n"
            "                            debug_next_hop"
        )
        hard = handler.index(
            "else if (mesh_parent_contact_failure_is_hard(", repair
        )
        hard_block = braced_block_after(handler[hard:], "else if (")

        reselect = hard_block.index("mesh_relay_select_next_hop(")
        changed = hard_block.index("bool topology_route_changed", reselect)
        defer_exact = hard_block.index(
            "mesh_relay_note_retransmit_deferred(", changed
        )
        no_route = hard_block.index(
            "if (select_after_repair != PROTO_OK)", defer_exact
        )
        discovery = hard_block.index("mesh_schedule_route_request(", no_route)
        unchanged_parent = hard_block.index("} else {", discovery)
        account = hard_block.index(
            "mesh_relay_note_pending_parent_failure_status(",
            unchanged_parent,
        )

        changed_gate = hard_block[changed:defer_exact]
        self.assertIn(
            "mesh_outbound_is_topology_operation_uplink(\n"
            "                                    retransmit)",
            changed_gate,
        )
        self.assertIn("select_after_repair != PROTO_OK", changed_gate)
        self.assertIn("selected_after_repair != debug_next_hop", changed_gate)
        self.assertIn(
            "&mesh_runtime,\n                                    retransmit,",
            hard_block[defer_exact:no_route],
        )
        self.assertIn(
            "retransmit->packet.dst_id",
            hard_block[discovery:unchanged_parent],
        )
        self.assertNotIn("retransmit->packet =", hard_block)
        self.assertNotIn("*retransmit =", hard_block)
        self.assertLess(reselect, changed)
        self.assertLess(changed, defer_exact)
        self.assertLess(defer_exact, no_route)
        self.assertLess(no_route, discovery)
        self.assertLess(discovery, unchanged_parent)
        self.assertLess(unchanged_parent, account)

    def test_click_contact_failure_keeps_exact_parent_before_route_repair(self):
        initial = function_body(REPORT, "mesh_try_repair_selected_parent_event")
        initial_retry = initial.index(
            "app_mesh_known_parent_contact_note_hard_failure("
        )
        initial_repair = initial.index(
            "mesh_relay_invalidate_upstream_route(", initial_retry
        )
        self.assertLess(initial_retry, initial_repair)
        self.assertIn(
            "MESH_KNOWN_PARENT_CONTACT_RETRIES", initial[initial_retry:initial_repair]
        )

        handler = function_body(REPORT, "mesh_handle_result_actions")
        retransmit_propose = handler.index(
            '"retransmit-event-repair"'
        )
        retransmit_retry = handler.index(
            "app_mesh_known_parent_contact_note_hard_failure(",
            retransmit_propose,
        )
        parent_failure = handler.index(
            "mesh_relay_note_pending_parent_failure_status(", retransmit_retry
        )
        self.assertLess(retransmit_retry, parent_failure)
        self.assertIn(
            "MESH_KNOWN_PARENT_CONTACT_RETRIES",
            handler[retransmit_retry:parent_failure],
        )

    def test_route_ready_classifies_direct_gateway_as_unscheduled(self):
        delivery = function_body(REPORT_DELIVERY, "mesh_handle_result_actions")
        ready = delivery.index(
            "if (result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY)"
        )
        handoff = delivery.index(
            "app_mesh_route_ready_handoff_on_ready", ready
        )
        route_ready = delivery[ready:handoff]

        self.assertIn(".selected_is_unscheduled_gateway =", route_ready)
        self.assertRegex(
            route_ready,
            r"\.selected_is_unscheduled_gateway\s*=\s*"
            r"route_ready_next_hop_id\s*==\s*GATEWAY_ID",
        )

    def test_direct_gateway_fast_path_normalizes_route_before_classifying(self):
        body = function_body(REPORT, "mesh_start_tracked_tx_with_retry")
        channel9 = body.index("if (mesh_packet_prefers_channel9")
        expire = body.index("mesh_relay_expire_routes(&mesh_runtime, now_ms)",
                            channel9)
        select = body.index(
            "mesh_relay_select_next_hop_for_packet(", expire
        )
        direct = body.index("debug_next_hop == GATEWAY_ID", select)
        start = body.index("mesh_relay_start_tx(&mesh_runtime", direct)

        self.assertLess(channel9, expire)
        self.assertLess(expire, select)
        self.assertLess(select, direct)
        self.assertLess(direct, start)

    def test_timing_negotiation_wake_is_not_marked_control_followup(self):
        wake = function_body(
            REPORT_ROUTE_CONTROL, "mesh_send_route_wake_train_with_duration"
        )
        base_flags = wake.index(
            "config->flags = FLAG_ROUTE_SETUP | FLAG_DIAGNOSTIC | "
            "FLAG_RANGE_ONLY"
        )
        session_start = wake.index("uwb_clicker_session_start", base_flags)
        classification = wake[base_flags:session_start]

        self.assertRegex(
            classification,
            r"if\s*\(\s*purpose\s*!=\s*"
            r"C5_CONTACT_PURPOSE_ROUTE_SOLICIT\s*&&\s*"
            r"purpose\s*!=\s*"
            r"C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION\s*\)\s*\{\s*"
            r"config->flags\s*\|=\s*FLAG_CONTROL_FOLLOWUP\s*;",
        )
        self.assertEqual(
            classification.count("FLAG_CONTROL_FOLLOWUP"),
            1,
            "timing negotiation must not be reclassified by another branch",
        )

    def test_route_wait_retry_has_a_separate_work_owner_from_tx_timeout(self):
        schedule = function_body(
            REPORT, "mesh_schedule_route_waiting_retry_after"
        )
        route_wait_handler = function_body(
            REPORT, "mesh_route_waiting_work_handler"
        )
        tx_timeout_handler = function_body(REPORT, "mesh_tx_timeout_handler")
        init = function_body(REPORT, "app_mesh_report_init")

        self.assertIn(
            "mesh_reschedule_owned_work(&mesh_route_waiting_work",
            schedule,
        )
        self.assertNotIn("mesh_tx_timeout_work", schedule)
        self.assertIn("mesh_try_route_waiting_tx()", route_wait_handler)
        self.assertNotIn("mesh_tx_timeout_handler", route_wait_handler)
        self.assertNotIn("mesh_try_route_waiting_tx()", tx_timeout_handler)
        self.assertIn(
            "k_work_init_delayable(&mesh_tx_timeout_work, "
            "mesh_tx_timeout_handler)",
            init,
        )
        self.assertIn(
            "k_work_init_delayable(&mesh_route_waiting_work,\n"
            "                          mesh_route_waiting_work_handler)",
            init,
        )

    def test_synchronous_route_request_is_owned_only_by_route_workers(self):
        discovery_worker = function_body(
            REPORT, "mesh_route_discovery_work_handler"
        )
        request_with_handoff = function_body(
            REPORT, "mesh_request_route_owned_with_rx_handoff"
        )
        route_wait = function_body(REPORT, "mesh_try_route_waiting_tx")
        async_submit = function_body(
            REPORT, "mesh_schedule_route_request_authorized"
        )
        owned_preflight = function_body(
            REPORT, "mesh_owned_tracked_tx_preflight"
        )
        init = function_body(REPORT, "app_mesh_report_init")

        self.assertEqual(REPORT.count("mesh_request_route("), 3)
        self.assertEqual(discovery_worker.count("mesh_request_route("), 1)
        self.assertEqual(route_wait.count("mesh_request_route("), 1)
        self.assertNotIn("mesh_request_route(", async_submit)
        self.assertEqual(REPORT.count("mesh_request_route_owned("), 2)
        self.assertEqual(
            request_with_handoff.count("mesh_request_route_owned("), 1
        )
        self.assertIn(
            "mesh_request_route_owned_with_rx_handoff(", owned_preflight
        )
        self.assertEqual(
            REPORT.count("mesh_try_route_waiting_tx("),
            3,
            "only the declaration, definition, and route-wait worker may name it",
        )
        self.assertIn(
            "mesh_reschedule_owned_work(&mesh_route_discovery_work",
            async_submit,
        )
        self.assertIn(
            "k_work_init_delayable(&mesh_route_discovery_work, "
            "mesh_route_discovery_work_handler)",
            init,
        )
        self.assertIn(
            "k_work_init_delayable(&mesh_route_waiting_work,\n"
            "                          mesh_route_waiting_work_handler)",
            init,
        )

    def test_gateway_route_request_owns_complete_continuous_rx_handoff(self):
        handoff = function_body(REPORT, "mesh_gateway_rx_control_begin")
        handoff_end = function_body(REPORT, "mesh_gateway_rx_control_end")
        route_request = function_body(
            REPORT, "mesh_request_route_owned_with_rx_handoff"
        )
        command_flood = function_body(
            REPORT, "mesh_send_gateway_command_flood"
        )
        role = handoff.index("mesh_gateway_route_test_role()")
        begin = handoff.index("mesh_rx_handoff_begin_control", role)
        stop = handoff.index("mesh_stop_role_scan()", begin)
        abort = handoff.index(
            "dwm3000_driver_request_receive_abort(", stop
        )
        wait = handoff.index("mesh_rx_handoff_wait_for_control()", abort)
        end = handoff_end.index("mesh_rx_handoff_end_control()")
        restart = handoff_end.index("mesh_restart_role_scan()", end)

        self.assertLess(role, begin)
        self.assertLess(begin, stop)
        self.assertLess(stop, abort)
        self.assertLess(abort, wait)
        self.assertLess(end, restart)
        self.assertIn("DWM3000_RECEIVE_ABORT_MESH_CONTROL", handoff)
        for body in (route_request, command_flood):
            acquire = body.index("mesh_gateway_rx_control_begin(")
            release = body.index("mesh_gateway_rx_control_end(", acquire)
            self.assertLess(acquire, release)
        self.assertLess(
            route_request.index("mesh_request_route_owned("),
            route_request.index("mesh_gateway_rx_control_end("),
        )
        self.assertLess(
            command_flood.index("app_node_comm_gateway_control_send("),
            command_flood.index("mesh_gateway_rx_control_end("),
        )

    def test_first_gateway_ack_send_failures_enter_identity_backoff(self):
        body = function_body(REPORT, "mesh_handle_result_actions")
        current_failure = body.index(
            "gateway ACK current channel-9 send failed"
        )
        current_start = current_failure - 900
        planned_start = body.index(
            "APP_MESH_GATEWAY_ACK_ACTION_SEND_PLANNED_CHANNEL9"
        )
        planned_end = body.index(
            "APP_MESH_GATEWAY_ACK_ACTION_STORE_WAITING_REFRESH_CHANNEL9"
        )

        current_end = body.index("goto after_gateway_ack;", current_failure)
        current_block = body[current_start:current_end]
        planned_block = body[planned_start:planned_end]
        self.assertIn("mesh_rf_retry_next_delay_ms", current_block)
        self.assertIn("mesh_schedule_route_waiting_retry_after", current_block)
        self.assertNotIn("ack_decision.delay_ms);", current_block)
        self.assertIn("mesh_rf_retry_next_delay_ms", planned_block)
        self.assertIn("mesh_store_route_waiting_tx", planned_block)
        self.assertIn("mesh_schedule_route_waiting_retry_after", planned_block)

    def test_gateway_batch_ack_waits_for_backoff_before_service_handoff(self):
        immediate = function_body(
            REPORT, "mesh_send_current_ch9_ack_batch"
        )
        handoff = function_body(
            REPORT, "mesh_gateway_handoff_due_batch_acks"
        )

        self.assertIn(
            "app_mesh_ch9_ack_table_note_send_failure", immediate
        )
        self.assertNotIn("app_node_comm_submit_control_response", immediate)
        ready = handoff.index("app_mesh_ch9_ack_table_retry_ready")
        submit = handoff.index("app_node_comm_submit_control_response")
        clear = handoff.index("app_mesh_ch9_ack_table_clear_peer")
        self.assertLess(ready, submit)
        self.assertLess(submit, clear)
        self.assertIn("app_mesh_ch9_ack_table_note_send_failure", handoff)

    def test_assignment_hop_ack_retires_temporary_timing_only_after_send(self):
        queue = function_body(REPORT, "mesh_ch9_ack_batch_queue")
        send = function_body(REPORT, "mesh_send_pending_ch9_ack_batch")
        consume = function_body(REPORT, "mesh_consume_next_channel9_peer_turn")
        retire = function_body(REPORT, "mesh_retire_assignment_channel9_peer")
        delivery = function_body(REPORT, "mesh_handle_result_actions")
        classify = queue.index(
            "if (ack->packet.msg_type == MSG_MESH_HOP_ACK"
        )

        for exact_guard in (
            "ack->packet.msg_type == MSG_MESH_HOP_ACK",
            "rx->packet.msg_type == MSG_COMMAND_RESULT",
            "discovery_assignment_parse_result_tlvs(",
            "DISCOVERY_ASSIGNMENT_PHASE_CLAIM",
            "DISCOVERY_ASSIGNMENT_PHASE_ACK",
        ):
            self.assertIn(exact_guard, queue[classify:])
        ack_phase = queue.index(
            "assignment.phase == DISCOVERY_ASSIGNMENT_PHASE_ACK", classify
        )
        retire_classify = queue.index(
            "APP_MESH_CH9_ASSIGNMENT_TURN_RETIRE", ack_phase
        )
        claim_phase = queue.index(
            "assignment.phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM",
            retire_classify,
        )
        consume_classify = queue.index(
            "APP_MESH_CH9_ASSIGNMENT_TURN_CONSUME", claim_phase
        )
        self.assertLess(ack_phase, retire_classify)
        self.assertLess(retire_classify, claim_phase)
        self.assertLess(claim_phase, consume_classify)
        self.assertNotIn("consumes_next_peer_turn =", queue)
        self.assertNotIn("retires_peer_timing =", queue)

        marker_snapshot = send.index(
            "app_mesh_ch9_ack_batch_consumes_next_peer_turn(batch)"
        )
        retirement_snapshot = send.index(
            "app_mesh_ch9_ack_batch_retires_peer_timing(batch)",
            marker_snapshot,
        )
        physical_send = send.index("ret = mesh_send_outbound(", retirement_snapshot)
        send_success = send.index("if (ret == 0)", physical_send)
        physical_commit = send.index(
            "mesh_relay_note_tx_sent(&mesh_runtime", send_success
        )
        cadence_tx_commit = send.index(
            "mesh_note_channel9_local_tx(ack.next_hop_id, plan->start_ms)",
            physical_commit,
        )
        batch_clear = send.index(
            "app_mesh_ch9_ack_table_clear_peer(&mesh_ch9_ack_table",
            cadence_tx_commit,
        )
        retirement_guard = send.index("if (retire_peer_timing)", batch_clear)
        parent_retirement = send.index(
            "mesh_retire_assignment_channel9_peer(", retirement_guard
        )
        consume_guard = send.index("if (consume_next_peer_turn)", parent_retirement)
        turn_consume = send.index(
            "mesh_consume_next_channel9_peer_turn(peer_id)",
            consume_guard,
        )
        send_failure = send.index(
            "app_mesh_ch9_ack_table_note_send_failure(", turn_consume
        )

        self.assertLess(marker_snapshot, physical_send)
        self.assertLess(marker_snapshot, retirement_snapshot)
        self.assertLess(retirement_snapshot, physical_send)
        self.assertLess(physical_send, send_success)
        self.assertLess(send_success, physical_commit)
        self.assertLess(physical_commit, cadence_tx_commit)
        self.assertLess(cadence_tx_commit, batch_clear)
        self.assertLess(batch_clear, retirement_guard)
        self.assertLess(retirement_guard, parent_retirement)
        self.assertLess(parent_retirement, consume_guard)
        self.assertLess(batch_clear, consume_guard)
        self.assertLess(consume_guard, turn_consume)
        self.assertLess(turn_consume, send_failure)
        self.assertNotIn("mesh_relay_clear_channel9_timing", send)
        self.assertNotIn("mesh_event_owner_abandon_peer", send)
        self.assertEqual(
            consume.count("mesh_relay_note_channel9_unobserved_turn("), 1
        )
        self.assertIn(
            "turn_start_ms = entry->timing.next_event_time_ms", consume
        )
        self.assertIn(
            "&mesh_runtime, peer_id, turn_start_ms", consume
        )
        self.assertIn("return true", consume[consume.index(
            "mesh_relay_note_channel9_unobserved_turn("
        ):])
        self.assertNotIn("mesh_relay_clear_channel9_timing", consume)
        self.assertNotIn(
            "mesh_consume_next_channel9_peer_turn", send[send_failure:]
        )
        self.assertNotIn(
            "app_mesh_ch9_ack_table_clear_peer", send[send_failure:]
        )
        self.assertNotIn(
            "mesh_retire_assignment_channel9_peer", send[send_failure:]
        )

        for exact_peer_cleanup in (
            "mesh_event_accept_rx_clear_peer(peer_id)",
            "mesh_event_accept_completed_clear_peer(peer_id)",
            "mesh_relay_clear_channel9_timing(&mesh_runtime, peer_id)",
            "mesh_event_owner_abandon_peer(peer_id)",
            "mesh_restore_anchor_low_duty_if_no_ch9(reason)",
        ):
            self.assertIn(exact_peer_cleanup, retire)
        self.assertIn("!mesh_id_is_unicast(peer_id)", retire)
        self.assertIn("peer_id == GATEWAY_ID", retire)

        custody_start = delivery.index(
            "MESH_RELAY_ACTION_TX_NEXT_HOP_CUSTODY_ACCEPTED"
        )
        custody_end = delivery.index(
            "MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING", custody_start
        )
        custody = delivery[custody_start:custody_end]
        phase_parse = custody.index("discovery_assignment_parse_result_tlvs(")
        retained_payload = custody.index(
            "mesh_runtime.pending.payload", phase_parse
        )
        retained_payload_len = custody.index(
            "mesh_runtime.pending.payload_len", retained_payload
        )
        phase_ack = custody.index(
            "assignment.phase == DISCOVERY_ASSIGNMENT_PHASE_ACK",
            retained_payload_len,
        )
        custody_parent_snapshot = custody.index(
            "const uint64_t custody_parent =", phase_ack
        )
        custody_parent_source = custody.index(
            "mesh_runtime.outbox_record.custody_parent",
            custody_parent_snapshot,
        )
        producer_cleanup = custody.index(
            "anchor_delivery_gateway_confirmed(", custody_parent_source
        )
        core_commit = custody.index(
            "mesh_relay_commit_next_hop_custody_terminal(", producer_cleanup
        )
        node_comm_cleanup = custody.index(
            "app_node_comm_note_gateway_confirmed_digest_at(", core_commit
        )
        child_retirement = custody.index(
            "mesh_retire_assignment_channel9_peer(", node_comm_cleanup
        )
        generic_close_guard = custody.index(
            "if (!table_response_custody &&", child_retirement
        )
        generic_close = braced_block_after(
            custody[generic_close_guard:], "if (!table_response_custody &&"
        )
        self.assertLess(phase_parse, phase_ack)
        self.assertLess(phase_parse, retained_payload)
        self.assertLess(retained_payload, retained_payload_len)
        self.assertLess(retained_payload_len, phase_ack)
        self.assertNotIn(
            "confirmed_payload",
            custody[phase_parse:phase_ack],
        )
        self.assertLess(phase_ack, custody_parent_snapshot)
        self.assertLess(custody_parent_snapshot, custody_parent_source)
        self.assertLess(custody_parent_source, producer_cleanup)
        self.assertLess(producer_cleanup, core_commit)
        self.assertLess(core_commit, node_comm_cleanup)
        self.assertLess(node_comm_cleanup, child_retirement)
        self.assertLess(child_retirement, generic_close_guard)
        self.assertIn(
            "mesh_retire_assignment_channel9_peer(\n"
            "                    custody_parent",
            custody[child_retirement:],
        )
        self.assertNotIn("route_selected(", custody[:generic_close_guard])
        self.assertIn("route_selected(&mesh_runtime.upstream)", generic_close)
        self.assertIn("mesh_close_channel9_connection(", generic_close)
        self.assertIn('"assignment-table-custody"', custody[child_retirement:])

    def test_retransmit_send_failure_uses_packet_identity_backoff(self):
        body = function_body(REPORT, "mesh_handle_result_actions")
        marker = '"retransmit-send-failed"'
        marker_index = body.index(marker)
        retry_start = body.rfind("if (ret == PROTO_OK)", 0, marker_index)
        self.assertGreaterEqual(retry_start, 0)
        retry_block = body[retry_start : marker_index + 700]

        self.assertIn("mesh_rf_retry_packet_key", retry_block)
        self.assertIn("mesh_rf_retry_next_delay_ms", retry_block)
        self.assertIn("mesh_relay_note_retransmit_deferred", retry_block)
        self.assertNotIn("MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS", retry_block)

    def test_retransmit_channel5_policy_deferral_uses_packet_backoff(self):
        body = function_body(REPORT, "mesh_handle_result_actions")
        policy_start = body.index(
            "mesh_event_plan_is_policy_deferral(plan.action)",
            body.index("MESH_RELAY_ACTION_RETRANSMIT"),
        )
        wait_start = body.index(
            "plan.action == MESH_EVENT_PLAN_WAIT", policy_start
        )
        policy_block = body[policy_start:wait_start]

        self.assertIn("mesh_relay_note_channel9_missed", policy_block)
        self.assertIn("APP_MESH_RF_RETRY_OPERATION_RETRANSMIT", policy_block)
        self.assertIn("mesh_rf_retry_next_delay_ms", policy_block)

    def test_channel9_retransmit_is_bound_to_the_selected_slot_send_time(self):
        body = function_body(REPORT, "mesh_handle_result_actions")
        retransmit = body.index("MESH_RELAY_ACTION_RETRANSMIT")
        plan = body.index("mesh_relay_require_channel9_tx_event(", retransmit)
        prepare = body.index("mesh_prepare_channel9_outbound(", plan)
        send = body.index("mesh_send_outbound_with_release_until(", prepare)
        planned = body[plan:send]

        self.assertIn("&plan", planned)
        self.assertIn("k_uptime_get_32()", planned)
        self.assertIn("required_ms", planned)
        self.assertLess(plan, prepare)
        self.assertLess(prepare, send)
        self.assertNotIn(
            "retransmit->earliest_tx_valid = false",
            body[prepare:send],
        )

    def test_direct_channel9_ack_policy_deferrals_advance_once(self):
        selector = function_body(REPORT, "mesh_select_channel9_ack_tx_event")
        result_handler = function_body(REPORT, "mesh_handle_result_actions")

        self.assertIn("mesh_event_plan_is_policy_deferral", selector)
        self.assertIn("mesh_relay_note_channel9_missed", selector)
        self.assertIn("app_mesh_ch9_ack_table_note_send_failure", selector)
        self.assertIn("gateway_ack_policy_deferred", result_handler)
        self.assertIn("gateway-ack-channel5-deferral", result_handler)
        self.assertIn("mesh_rf_retry_next_delay_ms", result_handler)

    def test_unscheduled_gateway_ack_rx_requires_exact_live_core_owner(self):
        selector = function_body(
            REPORT_TRANSPORT, "mesh_select_direct_gateway_ack_rx"
        )

        self.assertIn("DEVICE_ROLE != ROLE_ANCHOR", selector)
        self.assertIn("!mesh_relay_tx_active(&mesh_runtime)", selector)
        self.assertIn(
            "pending->state != MESH_RELAY_TX_WAIT_GATEWAY_ACK", selector
        )
        self.assertIn(
            "pending->radio_channel != UWB_CHANNEL_MESH_PAYLOAD", selector
        )
        self.assertIn("pending->gateway_ack_deadline_ms == 0u", selector)
        self.assertIn(
            "uptime_deadline_reached(now_ms, "
            "pending->gateway_ack_deadline_ms)",
            selector,
        )
        self.assertIn(
            "mesh_find_active_channel9_timing(pending->next_hop_id",
            selector,
        )

        # This exception is only for the core's active receive turn. Batch
        # custody, a forwarded ACK, and retry backoff stay on normal cadence.
        self.assertNotIn("mesh_ch9_tx_pending_is_active", selector)
        self.assertNotIn("app_mesh_ch9_ack_table_any_pending", selector)
        self.assertNotIn("MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD", selector)
        self.assertNotIn("MESH_RELAY_TX_WAIT_RETRY_BACKOFF", selector)

        remaining = selector.index("uptime_ms_until_deadline(")
        plan = selector.index("*selected_plan =", remaining)
        deadline = selector.index(
            ".end_ms = pending->gateway_ack_deadline_ms", plan
        )
        window = selector.index(".window_ms =", deadline)
        success = selector.index("return true;", window)
        self.assertLess(remaining, plan)
        self.assertLess(plan, deadline)
        self.assertLess(deadline, window)
        self.assertLess(window, success)
        self.assertIn(".start_ms = now_ms", selector[plan:deadline])
        self.assertIn("MIN(remaining_ms", selector[window:success])
        self.assertNotIn("MESH_EVENT_RX_LATE_GUARD_MS", selector)

    def test_unscheduled_gateway_ack_rx_runs_after_scheduled_turns_and_drains(self):
        worker = function_body(REPORT, "mesh_uwb_rx_work_handler")

        ack_tx_select = worker.index("mesh_select_channel9_ack_tx_event(")
        ack_tx_send = worker.index(
            "mesh_send_pending_ch9_ack_batch(", ack_tx_select
        )
        ack_tx_return = worker.index("return;", ack_tx_send)
        scheduled_rx_select = worker.index(
            "mesh_select_channel9_rx_event(", ack_tx_return
        )
        direct_rx_select = worker.index(
            "mesh_select_direct_gateway_ack_rx(", scheduled_rx_select
        )
        payload_choice = worker.index(
            "channel9_payload_rx = channel9_event || direct_gateway_ack_rx",
            direct_rx_select,
        )

        self.assertLess(ack_tx_select, ack_tx_send)
        self.assertLess(ack_tx_send, ack_tx_return)
        self.assertLess(ack_tx_return, scheduled_rx_select)
        self.assertLess(scheduled_rx_select, direct_rx_select)
        self.assertLess(direct_rx_select, payload_choice)
        self.assertIn(
            "direct_gateway_ack_rx = !channel9_event && "
            "!gateway_route_preempt",
            worker[scheduled_rx_select:payload_choice],
        )

        claim = worker.index("ret = mesh_rx_radio_claim(", payload_choice)
        payload_config = worker.index(
            "dwm3000_driver_configure_mesh_payload_mode()", claim
        )
        deadline = worker.index(
            "const uint32_t deadline_ms = direct_gateway_ack_rx ?",
            payload_config,
        )
        receive = worker.index(
            "dwm3000_driver_receive_frame_continuous_timed(", deadline
        )
        release = worker.index(
            "radio_release_ret = mesh_release_radio_after_mesh_turn(", receive
        )
        release_finish = worker.index(
            "radio_release_ret = mesh_rx_radio_finish(", release
        )
        direct_tail = worker.index("if (direct_gateway_ack_rx) {", release_finish)
        drain = worker.index(
            'mesh_process_queued_rx_now("direct-gateway-ack-rx")',
            direct_tail,
        )
        rearm = worker.index("mesh_schedule_uwb_rx(0u);", drain)

        self.assertLess(claim, payload_config)
        self.assertLess(payload_config, deadline)
        self.assertLess(deadline, receive)
        self.assertLess(receive, release)
        self.assertLess(release, release_finish)
        self.assertLess(release_finish, direct_tail)
        self.assertLess(direct_tail, drain)
        self.assertLess(drain, rearm)
        self.assertIn(
            "channel9_payload_rx ?\n"
            '                              "mesh channel9 UWB RX"',
            worker[claim : worker.index("&radio_lease", claim)],
        )
        self.assertIn(
            "channel9_payload_rx ?\n"
            "          dwm3000_driver_configure_mesh_payload_mode()",
            worker[payload_config - 200 : payload_config + 100],
        )
        self.assertIn(
            "direct_gateway_ack_rx ?\n"
            "            channel9_plan.end_ms :\n"
            "            channel9_plan.end_ms + MESH_EVENT_RX_LATE_GUARD_MS",
            worker[deadline : deadline + 300],
        )
        self.assertGreaterEqual(
            worker.count("direct_gateway_ack_rx ? NULL :"), 2
        )
        self.assertNotIn(
            "mesh_schedule_uwb_rx(", worker[release_finish:drain]
        )

        direct_rearm = braced_block_after(
            worker[direct_tail:], "if (direct_gateway_ack_rx)"
        )
        self.assertIn("mesh_relay_tx_active(&mesh_runtime)", direct_rearm)
        self.assertIn(
            "mesh_runtime.pending.state == "
            "MESH_RELAY_TX_WAIT_GATEWAY_ACK",
            direct_rearm,
        )
        self.assertIn("!uptime_deadline_reached(", direct_rearm)
        self.assertIn(
            "mesh_runtime.pending.gateway_ack_deadline_ms", direct_rearm
        )
        self.assertIn("mesh_schedule_uwb_rx(0u)", direct_rearm)
        self.assertIn("mesh_uwb_rx_active = false", direct_rearm)
        self.assertNotIn("MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD", direct_rearm)
        self.assertNotIn("MESH_RELAY_TX_WAIT_RETRY_BACKOFF", direct_rearm)

    def test_low_duty_scan_cannot_defer_past_an_unarmed_channel9_ack(self):
        body = function_body(
            REPORT, "mesh_anchor_low_duty_scan_should_defer"
        )
        conflict = body.index(
            "if (!found || selected_delay_ms > min_gap_ms)"
        )
        rearm = body.index("mesh_schedule_uwb_rx(selected_delay_ms)", conflict)
        retry = body.index("*retry_ms = selected_delay_ms", rearm)
        success = body.index("return true", retry)

        self.assertLess(conflict, rearm)
        self.assertLess(rearm, retry)
        self.assertLess(retry, success)
        self.assertIn("DBG_ANCHOR_CH9_REARM", body)

    def test_anchor_low_duty_scan_yields_for_due_or_imminent_relay_retry(self):
        owner = function_body(
            REPORT_DELIVERY, "mesh_report_ch9_ack_wait_active"
        )
        relay_snapshot = owner.index(
            "relay_tx_active = mesh_relay_tx_active(&mesh_runtime)"
        )
        direct = owner.index("mesh_ch9_tx_pending_is_active()", relay_snapshot)
        core = owner.index("app_mesh_ch9_core_ack_wait_active(", direct)
        pending = owner.index("&mesh_runtime.pending", core)
        relay = owner.index("relay_tx_active", pending)

        self.assertLess(relay_snapshot, direct)
        self.assertLess(direct, core)
        self.assertLess(core, pending)
        self.assertLess(pending, relay)

        planner = function_body(
            ANCHOR_RADIO, "anchor_relay_retry_plan_scan"
        )
        active = planner.index("mesh_relay_tx_active(&mesh_runtime)")
        backoff = planner.index(
            "MESH_RELAY_TX_WAIT_RETRY_BACKOFF", active
        )
        reached = planner.index("uptime_deadline_reached(", backoff)
        due_rearm = planner.index(
            "ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS", reached
        )
        remaining = planner.index("uptime_ms_until_deadline(", due_rearm)
        scan_guard = planner.index("scan_guard_ms =", remaining)
        completion = planner.index(
            "ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS", scan_guard
        )
        retune = planner.index(
            "MESH_RADIO_EVENT_RETUNE_GUARD_MS", completion
        )
        imminent = planner.index(
            "if (remaining_ms <= scan_guard_ms)", retune
        )
        publish_retry = planner.index("*retry_ms =", imminent)
        yield_scan = planner.index("return true", publish_retry)
        cap_gate = planner.index(
            "if (*scan_rx_ms >= remaining_ms - scan_guard_ms)", yield_scan
        )
        cap_window = planner.index(
            "*scan_rx_ms = remaining_ms - scan_guard_ms", cap_gate
        )
        scan_allowed = planner.index("return false", cap_window)

        self.assertLess(active, backoff)
        self.assertLess(backoff, reached)
        self.assertLess(reached, due_rearm)
        self.assertLess(due_rearm, remaining)
        self.assertLess(remaining, scan_guard)
        self.assertLess(scan_guard, completion)
        self.assertLess(completion, retune)
        self.assertLess(retune, imminent)
        self.assertLess(imminent, publish_retry)
        self.assertLess(publish_retry, yield_scan)
        self.assertLess(yield_scan, cap_gate)
        self.assertLess(cap_gate, cap_window)
        self.assertLess(cap_window, scan_allowed)
        self.assertIn("return false", planner[:reached])
        self.assertIn(
            "*retry_ms = ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS",
            planner[reached:remaining],
        )
        self.assertIn("return true", planner[reached:remaining])
        self.assertIn("*retry_ms = remaining_ms", planner[publish_retry:yield_scan])
        self.assertRegex(
            planner[scan_guard:imminent],
            r"scan_guard_ms\s*=\s*"
            r"ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS\s*\+\s*"
            r"MESH_RADIO_EVENT_RETUNE_GUARD_MS\s*\+\s*1u;",
        )
        self.assertNotIn("packet.msg_type", planner)

        scan = function_body(ANCHOR_RADIO, "anchor_uwb_scan_work_handler")
        relay_snapshot = scan.index(
            "relay_tx_active = mesh_relay_tx_active(&mesh_runtime)"
        )
        block_snapshot = scan.index(
            "anchor_relay_retry_plan_scan(",
            relay_snapshot,
        )
        block_call_end = scan.index(");", block_snapshot)
        block_call = scan[block_snapshot:block_call_end]
        self.assertIn("scan_rx_ms", block_call)
        self.assertIn("&relay_tx_retry_ms", block_call)
        snapshot = scan.index(
            "ch9_ack_wait_active = app_mesh_ch9_core_ack_wait_active(",
            block_call_end,
        )
        blocked = scan.index("if (anchor_uwb_window_active() ||", snapshot)
        claim = scan.index("radio_guard_uwb_claim(", blocked)
        blocked_path = scan[blocked:claim]
        schedule = blocked_path.index("anchor_uwb_scan_schedule_ms(retry_ms)")
        leave = blocked_path.index("return;", schedule)
        arm = scan.index("DBG_ANCHOR_CH5_SCAN_ARM", claim)
        configure = scan.index("dwm3000_driver_configure_wake_mode()", arm)

        # Passive custody remains scan-eligible. The relay core's exact live
        # ACK receive turn blocks the retune, while a due or imminent retry
        # yields before the claim and a later retry only caps the scan window.
        self.assertLess(relay_snapshot, block_snapshot)
        self.assertLess(block_snapshot, blocked)
        self.assertIn("ch9_ack_wait_active ||", blocked_path[:schedule])
        self.assertNotIn("relay_tx_active ||", blocked_path[:schedule])
        self.assertIn("relay_tx_retry_blocks_scan ||", blocked_path[:schedule])
        self.assertIn("ch9_rx_conflict ||", blocked_path[:schedule])
        self.assertIn("uwb_radio_busy", blocked_path[:schedule])
        self.assertIn(
            "relay_tx_retry_blocks_scan ? relay_tx_retry_ms :",
            blocked_path[:schedule],
        )
        self.assertIn(
            "relay_tx_retry_blocks_scan ? 1u : 0u", blocked_path
        )
        self.assertIn("ch9_ack_wait_active ? 1u : 0u", blocked_path)
        self.assertLess(schedule, leave)
        self.assertLess(leave, claim)
        self.assertLess(claim, arm)
        self.assertLess(arm, configure)

        topology_ack = function_body(
            REPORT_EVENT_TX, "mesh_outbound_is_topology_ack_confirm"
        )
        self.assertIn("MSG_GATEWAY_ACK_CONFIRM", topology_ack)

    def test_anchor_scan_caps_gateway_ack_wait_before_ack_deadline(self):
        planner = function_body(
            ANCHOR_RADIO, "anchor_relay_retry_plan_scan"
        )
        active = planner.index("mesh_relay_tx_active(&mesh_runtime)")
        backoff = planner.index(
            "MESH_RELAY_TX_WAIT_RETRY_BACKOFF", active
        )
        ack_wait = planner.index("MESH_RELAY_TX_WAIT_GATEWAY_ACK", backoff)
        ack_deadline = planner.index(
            "mesh_runtime.pending.gateway_ack_deadline_ms", ack_wait
        )
        boundary = planner.index(
            "response_boundary_ms =", ack_deadline
        )
        reached = planner.index("uptime_deadline_reached(", ack_deadline)
        due_retry = planner.index("*retry_ms =", reached)
        remaining = planner.index("uptime_ms_until_deadline(", due_retry)
        scan_guard = planner.index("scan_guard_ms =", remaining)
        cap = planner.index(
            "*scan_rx_ms = remaining_ms - scan_guard_ms", scan_guard
        )

        # A locally originated TABLE confirmation waits for its gateway ACK
        # in this state. Its retry deadline must drive the same yield/cap
        # planner as backoff custody, otherwise a long protocol RX window can
        # cross the ACK timeout and suppress the confirmation retry.
        self.assertLess(active, backoff)
        self.assertLess(backoff, ack_wait)
        self.assertLess(ack_wait, ack_deadline)
        self.assertLess(ack_deadline, boundary)
        self.assertLess(ack_deadline, reached)
        self.assertLess(reached, remaining)
        self.assertLess(remaining, cap)
        self.assertIn(
            "local_protocol_response_active", planner[backoff:ack_deadline]
        )
        self.assertIn(
            "response_boundary_ms", planner[reached:due_retry]
        )
        self.assertIn(
            "response_boundary_ms", planner[remaining:scan_guard]
        )
        self.assertNotIn(
            "mesh_runtime.pending.retry_after_ms",
            planner[reached:scan_guard],
        )

    def test_enumeration_listener_uses_deadline_capped_continuous_response_window(self):
        classifier = function_body(
            REPORT_DELIVERY, "mesh_report_local_protocol_response_active"
        )
        self.assertIn("mesh_relay_tx_active(&mesh_runtime)", classifier)
        self.assertIn("pending->packet.src_id != DEVICE_ID", classifier)
        self.assertIn("pending->packet.dst_id != GATEWAY_ID", classifier)
        self.assertIn("FLAG_GATEWAY_ACK_REQUIRED", classifier)
        self.assertIn("MSG_COMMAND_RESULT", classifier)

        scan = function_body(ANCHOR_RADIO, "anchor_uwb_scan_work_handler")
        snapshot = scan.index(
            "mesh_report_local_protocol_response_active()"
        )
        response_branch = scan.index(
            "if (enumeration_continuous_rx && "
            "local_protocol_response_active)",
            snapshot,
        )
        response_window = scan.index(
            "scan_rx_ms = UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS",
            response_branch,
        )
        long_branch = scan.index(
            "else if (enumeration_continuous_rx)", response_window
        )
        long_slice = scan.index(
            "scan_rx_ms = UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS",
            long_branch,
        )
        retry_gate = scan.index(
            "anchor_relay_retry_plan_scan(", long_slice
        )

        self.assertLess(snapshot, response_branch)
        self.assertLess(response_branch, response_window)
        self.assertLess(response_window, long_branch)
        self.assertLess(long_branch, long_slice)
        self.assertLess(long_slice, retry_gate)
        self.assertNotIn(
            "MESH_RADIO_CONTROL_FOLLOWUP_SCAN_MS",
            scan[response_branch:long_branch],
        )
        retry_call_end = scan.index(");", retry_gate)
        self.assertIn("&scan_rx_ms", scan[retry_gate:retry_call_end])

    def test_pending_reliable_response_protects_next_local_tx_slot(self):
        required_source = REPORT_TRANSPORT[
            REPORT_TRANSPORT.index(
                "static bool mesh_channel9_next_required_activity("
            ) :
        ]
        required = function_body(
            required_source, "mesh_channel9_next_required_activity"
        )
        peers = function_body(
            REPORT_TRANSPORT, "mesh_node_comm_reliable_tx_peers"
        )
        delay = function_body(
            REPORT_TRANSPORT, "mesh_next_channel9_activity_delay_ms"
        )
        gap = function_body(
            REPORT_TRANSPORT, "mesh_active_channel9_ch5_gap_window_ms"
        )

        local_tx = required.index("mesh_event_timing_local_tx_slot(timing)")
        pending = required.index(
            "mesh_node_comm_reliable_tx_pending_for_peer", local_tx
        )
        core = required.index(
            "mesh_channel9_core_tx_pending_for_peer", local_tx
        )
        ack = required.index("mesh_channel9_ack_pending_for_peer", local_tx)
        skip_to_rx = required.index("timing->next_event_time_ms +=")
        self.assertLess(local_tx, pending)
        self.assertLess(pending, skip_to_rx)
        self.assertLess(core, skip_to_rx)
        self.assertLess(ack, skip_to_rx)
        self.assertIn("return true", required[local_tx:skip_to_rx])

        core_pending = function_body(
            REPORT_TRANSPORT, "mesh_channel9_core_tx_pending_for_peer"
        )
        self.assertIn("mesh_runtime.pending.next_hop_id == peer_id", core_pending)
        self.assertIn("app_mesh_ch9_core_pending_allows_rx", core_pending)
        self.assertIn("mesh_relay_tx_active(&mesh_runtime)", core_pending)
        self.assertIn("return mesh_event_timing_local_rx_slot(timing)",
                      required[skip_to_rx:])

        self.assertIn("app_node_comm_reliable_delivery_targets", peers)
        self.assertIn("mesh_relay_select_next_hop", peers)
        self.assertIn("peer_ids[peer_count++] = next_hop_id", peers)
        select = delay.index("mesh_channel9_next_required_activity")
        self.assertIn("local_reliable_tx_peers", delay[select:select + 240])
        self.assertIn("entry->next_hop_id", required[pending:pending + 240])
        next_activity = gap.index("mesh_next_channel9_rx_delay_ms(now_ms)")
        scan_window = gap.index("app_mesh_c5_connected_gap_window_ms",
                                next_activity)
        self.assertLess(next_activity, scan_window)

    def test_route_reply_listener_hands_event_control_to_the_worker(self):
        body = function_body(REPORT, "mesh_listen_for_route_reply")
        radio_finish = body.index(
            "mesh_transport_radio_finish(&radio_lease, parking_ret)"
        )
        submit = body.index("mesh_submit_owned_work(", radio_finish)

        self.assertLess(radio_finish, submit)
        self.assertIn("DBG_EVENT_CTRL_POST_RX_QUEUED", body)
        self.assertNotIn("mesh_process_queued_rx_now", body)

    def test_route_reply_listener_observes_async_route_ready_and_releases_radio(self):
        listener = function_body(
            REPORT_ROUTE_CONTROL, "mesh_listen_for_route_reply"
        )
        snapshot = listener.index(
            "route_ready_generation = "
            "atomic_get(&mesh_route_ready_generation)"
        )
        radio_claim = listener.index(
            "mesh_transport_radio_claim(RADIO_GUARD_UWB_CLIENT_MESH_RX,",
            snapshot,
        )
        ready_check = listener.index(
            "if (atomic_get(&mesh_route_ready_generation) !=", radio_claim
        )
        receive = listener.index(
            "ret = dwm3000_driver_receive_frame_continuous(", ready_check
        )
        ready_branch = listener[ready_check:receive]
        timeout = listener.index("if (ret == -ETIMEDOUT)", receive)
        timeout_continue = listener.index("continue;", timeout)
        standby = listener.index(
            "mesh_radio_standby_with_bounded_recovery(",
            timeout_continue,
        )
        radio_finish = listener.index(
            "mesh_transport_radio_finish(&radio_lease, parking_ret)", standby
        )

        self.assertLess(snapshot, radio_claim)
        self.assertLess(radio_claim, ready_check)
        self.assertLess(ready_check, receive)
        self.assertIn("captured_route_reply = true", ready_branch)
        self.assertIn("last_ret = 0", ready_branch)
        self.assertIn("*route_reply_captured = true", ready_branch)
        self.assertIn("DBG_ROUTE_REPLY_LISTEN_ROUTE_READY", ready_branch)
        self.assertIn("break;", ready_branch)
        self.assertRegex(
            listener[receive:timeout],
            r"MIN\(remaining_ms,\s*MESH_ROUTE_REPLY_READY_POLL_MS\)",
        )
        self.assertLess(timeout, timeout_continue)
        self.assertLess(timeout_continue, standby)
        self.assertLess(standby, radio_finish)

        delivery = function_body(REPORT_DELIVERY, "mesh_handle_result_actions")
        ready_action = delivery.index(
            "if (result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY)"
        )
        generation_publish = delivery.index(
            "atomic_inc(&mesh_route_ready_generation)", ready_action
        )
        handoff = delivery.index(
            "app_mesh_route_ready_handoff_on_ready", generation_publish
        )
        self.assertLess(ready_action, generation_publish)
        self.assertLess(generation_publish, handoff)

    def test_control_listener_channel9_yield_targets_downstream_rx_only(self):
        helper = function_body(
            REPORT_TRANSPORT,
            "mesh_next_channel9_receive_prepare_delay_ms",
        )
        advance = helper.index("mesh_advance_all_channel9_timings_past(")
        expire = helper.index("mesh_expire_channel9_timings(", advance)
        loop = helper.index("for (uint8_t i = 0u;", expire)
        valid = helper.index("!entry->valid", loop)
        direction = helper.index(
            "entry->direction != "
            "MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM",
            valid,
        )
        direction_continue = helper.index("continue;", direction)
        copy = helper.index("timing = entry->timing", direction_continue)
        tx_to_rx = helper.index(
            "if (!mesh_event_timing_local_rx_slot(&timing))", copy
        )
        interval = helper.index(
            "timing.next_event_time_ms += timing.event_interval_ms",
            tx_to_rx,
        )
        counter = helper.index("timing.event_counter++", interval)
        require_rx = helper.index(
            "if (!mesh_event_timing_local_rx_slot(&timing) ||", counter
        )
        usable = helper.index(
            "!mesh_event_timing_usable(&timing, now_ms)", require_rx
        )
        prepare = helper.index(
            "prepare_ms = mesh_channel9_prepare_start_ms(&timing)", usable
        )
        due = helper.index(
            "uptime_deadline_reached(now_ms, prepare_ms)", prepare
        )
        earliest = helper.index(
            "if (!found || candidate_delay_ms < selected_delay_ms)", due
        )
        publish = helper.index("*delay_ms = selected_delay_ms", earliest)

        self.assertLess(advance, expire)
        self.assertLess(expire, loop)
        self.assertLess(loop, valid)
        self.assertLess(valid, direction)
        self.assertLess(direction, direction_continue)
        self.assertLess(direction_continue, copy)
        self.assertLess(copy, tx_to_rx)
        self.assertLess(tx_to_rx, interval)
        self.assertLess(interval, counter)
        self.assertLess(counter, require_rx)
        self.assertLess(require_rx, usable)
        self.assertLess(usable, prepare)
        self.assertLess(prepare, due)
        self.assertLess(due, earliest)
        self.assertLess(earliest, publish)
        self.assertNotIn("mesh_node_comm_reliable_tx_pending_for_peer", helper)
        self.assertNotIn("mesh_channel9_ack_pending_for_peer", helper)
        self.assertNotIn("mesh_event_timing_local_tx_slot", helper)
        self.assertNotIn(
            "MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM", helper
        )

    def test_click_handoff_precedes_route_listener_cleanup_restart(self):
        ack_listener = function_body(
            REPORT_ROUTE_CONTROL, "mesh_listen_for_route_reply_ack"
        )
        ack_guard_finish = ack_listener.index(
            "mesh_transport_radio_finish(&radio_lease, parking_ret)"
        )
        ack_click = ack_listener.index("if (click_captured)", ack_guard_finish)
        ack_clear = ack_listener.index(
            'mesh_c5_contact_clear("click-preempt")', ack_click
        )
        ack_handoff = ack_listener.index(
            "mesh_handoff_anchor_click_claim", ack_clear
        )
        ack_restart = ack_listener.index(
            "mesh_restart_role_scan()", ack_handoff
        )
        self.assertLess(ack_guard_finish, ack_click)
        self.assertLess(ack_click, ack_clear)
        self.assertLess(ack_clear, ack_handoff)
        self.assertLess(ack_handoff, ack_restart)
        self.assertIn("k_mutex_unlock(&mesh_route_reply_ack_scratch_lock)",
                      ack_listener[ack_guard_finish:ack_clear])
        self.assertIn("if (!handled)", ack_listener[ack_handoff:ack_restart])

        listener = function_body(
            REPORT_ROUTE_CONTROL, "mesh_listen_for_route_reply"
        )
        guard_finish = listener.index(
            "mesh_transport_radio_finish(&radio_lease, parking_ret)"
        )
        click = listener.index("if (click_captured)", guard_finish)
        clear = listener.index(
            'mesh_c5_contact_clear("click-preempt")', click
        )
        handoff = listener.index("mesh_handoff_anchor_click_claim", clear)
        restart = listener.index("mesh_restart_role_scan()", handoff)
        pending = listener.index("mesh_submit_owned_work(", restart)
        self.assertLess(guard_finish, click)
        self.assertLess(click, clear)
        self.assertLess(clear, handoff)
        self.assertLess(handoff, restart)
        self.assertLess(restart, pending)
        self.assertIn("if (!handled)", listener[handoff:restart])

    def test_click_probe_budget_matches_both_phy_reconfigurations(self):
        helper = function_body(
            REPORT_ROUTE_CONTROL, "mesh_probe_standard_wake_claim"
        )
        standard_config = helper.index(
            "dwm3000_driver_configure_wake_mode()"
        )
        receive = helper.index(
            "dwm3000_driver_receive_frame_continuous_extend_on_activity",
            standard_config,
        )
        control_config = helper.index(
            "dwm3000_driver_configure_wake_mesh_control_mode()", receive
        )
        self.assertLess(standard_config, receive)
        self.assertLess(receive, control_config)
        self.assertIn("ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS",
                      helper[receive:control_config])

        budget = REPORT_UNIT[REPORT_UNIT.index(
            "#define MESH_ROUTE_REPLY_CLICK_PROBE_BUDGET_MS"
        ):]
        self.assertIn("MESH_ROUTE_WAKE_CLICK_RX_MAX_GAP_MS", budget)
        self.assertIn("ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS", budget)
        self.assertIn(
            "MESH_ROUTE_REPLY_CLICK_PROBE_STANDARD_RETUNE_GUARD_MS", budget
        )
        self.assertIn(
            "MESH_ROUTE_REPLY_CLICK_PROBE_CONTROL_RETUNE_GUARD_MS", budget
        )
        guard = REPORT_UNIT.index(
            "MESH_ROUTE_REPLY_CLICK_PROBE_STANDARD_RETUNE_GUARD_MS"
        )
        self.assertIn("MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS",
                      REPORT_UNIT[guard:guard + 180])
        control_guard = REPORT_UNIT.index(
            "MESH_ROUTE_REPLY_CLICK_PROBE_CONTROL_RETUNE_GUARD_MS"
        )
        self.assertIn("MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS",
                      REPORT_UNIT[control_guard:control_guard + 180])
        self.assertRegex(
            REPORT_UNIT,
            r"BUILD_ASSERT\(MESH_ROUTE_REPLY_CLICK_PROBE_BUDGET_MS\s*<\s*"
            r"WAKE_ADV_MS,",
        )

    def test_accepted_control_followup_holds_extended_phr(self):
        listener = function_body(REPORT, "mesh_listen_for_route_reply")
        activity = listener.index(
            "app_wake_train_politeness_rx_activity(ret, rx_failure)"
        )
        control_gate = listener.index(
            "contact_purpose ==\n"
            "                               C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD",
            activity,
        )
        shortfall_gate = listener.index(
            "if (gateway_control_relay_shortfall &&", control_gate
        )
        wake_seen_gate = listener.index(
            "!gateway_control_followup_wake_seen", shortfall_gate
        )
        probe = listener.index("mesh_probe_standard_wake_claim(", wake_seen_gate)
        wake_seen = listener.index(
            "gateway_control_followup_wake_seen = true", probe
        )
        renewed_deadline = listener.index(
            "deadline_ms = k_uptime_get_32() +", wake_seen
        )
        full_relay_window = listener.index(
            "window_ms", renewed_deadline
        )
        hold = listener.index(
            "DBG_C5_CONTROL_LISTENER_HOLD_EXTENDED", full_relay_window
        )

        self.assertLess(activity, control_gate)
        self.assertLess(control_gate, shortfall_gate)
        self.assertLess(shortfall_gate, wake_seen_gate)
        self.assertLess(wake_seen_gate, probe)
        self.assertLess(probe, wake_seen)
        self.assertLess(wake_seen, renewed_deadline)
        self.assertLess(renewed_deadline, full_relay_window)
        self.assertLess(full_relay_window, hold)
        self.assertIn(
            "!gateway_control_followup_wake_seen",
            listener[shortfall_gate:probe],
        )
        self.assertIn(
            "contact_purpose !=",
            listener[activity:probe],
        )
        self.assertIn(
            "contact_purpose ==",
            listener[control_gate:hold],
        )

    def test_gateway_control_listener_uses_here_i_am_route_depth(self):
        handoff = function_body(REPORT, "mesh_anchor_handoff_route_wake_frame")
        control = handoff.index("if (control_followup)")
        route_depth = handoff.index(
            "app_mesh_report_selected_gateway_hop_count()", control
        )
        duration = handoff.index(
            "discovery_assignment_control_listener_duration_ms(", route_depth
        )
        listener = handoff.index("mesh_listen_for_route_reply(", duration)

        self.assertLess(control, route_depth)
        self.assertLess(route_depth, duration)
        self.assertLess(duration, listener)
        self.assertIn("gateway_hop_count", handoff[route_depth:duration])
        self.assertNotIn(
            "MESH_GATEWAY_CONTROL_DEEP_RELAY_LISTEN_MS",
            handoff[control:listener],
        )

    def test_control_followup_waits_after_every_central_wake_path(self):
        helper = function_body(
            REPORT, "mesh_wait_for_c5_control_followup_turnaround"
        )

        self.assertRegex(
            APP_CONFIG,
            r"#define\s+MESH_CONTROL_FOLLOWUP_TURNAROUND_MS\s*\\\s*"
            r"MESH_RADIO_EVENT_RETUNE_GUARD_MS",
        )
        self.assertIn("MESH_CONTROL_FOLLOWUP_TURNAROUND_MS", helper)
        self.assertIn("k_msleep(", helper)
        self.assertRegex(
            REPORT_UNIT,
            r"BUILD_ASSERT\(MESH_CONTROL_FOLLOWUP_TURNAROUND_MS\s*>=\s*"
            r"MESH_ROUTE_REPLY_CLICK_PROBE_CONTROL_RETUNE_GUARD_MS,",
        )

        for name in (
            "mesh_send_c5_control_attempt",
            "mesh_send_c5_flood_now_until",
            "mesh_try_send_c5_flood_resume",
        ):
            body = function_body(REPORT_TRANSPORT, name)
            wake = min(
                index for index in (
                    body.find("mesh_send_route_wake_train("),
                    body.find("mesh_send_route_wake_train_with_duration("),
                )
                if index >= 0
            )
            turnaround = body.index(
                "mesh_wait_for_c5_control_followup_turnaround", wake
            )
            self.assertLess(wake, turnaround, name)

    def test_route_ready_poll_slice_precedes_channel9_retry_boundary(self):
        poll_match = re.search(
            r"#define\s+MESH_ROUTE_REPLY_READY_POLL_MS\s+(\d+)u",
            REPORT_UNIT,
        )
        retry_match = re.search(
            r"#define\s+MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS\s+(\d+)u",
            REPORT_UNIT,
        )

        self.assertIsNotNone(poll_match)
        self.assertIsNotNone(retry_match)
        poll_ms = int(poll_match.group(1))
        retry_ms = int(retry_match.group(1))
        self.assertGreater(poll_ms, 0)
        self.assertLess(poll_ms, retry_ms)
        self.assertRegex(
            REPORT_UNIT,
            r"BUILD_ASSERT\(MESH_ROUTE_REPLY_READY_POLL_MS\s*<\s*"
            r"MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS,",
        )

    def test_queued_event_propose_holds_only_its_accepted_exchange(self):
        listener = function_body(REPORT, "mesh_listen_for_route_reply")
        queue = listener.index("mesh_queue_from_frame_at_internal")
        queue_reject = listener.index("continue;", queue)
        post_rx = listener.index(
            "app_mesh_c5_route_capture_requires_post_rx_response", queue_reject
        )
        hold = listener.index(
            "hold_post_rx_response_contact = true", post_rx
        )
        cleanup = listener.index("if (captured_route_reply", hold)
        clear = listener.index("mesh_c5_contact_clear", cleanup)

        self.assertIn("bool hold_post_rx_response_contact = false", listener)
        self.assertLess(queue, queue_reject)
        self.assertLess(queue_reject, post_rx)
        self.assertLess(post_rx, hold)
        self.assertRegex(
            listener[cleanup:clear],
            r"captured_route_reply\s*\|\|\s*hold_post_rx_response_contact",
        )
        self.assertEqual(
            listener.count("hold_post_rx_response_contact = true"), 1
        )

        send = function_body(REPORT, "mesh_send_c5_control_attempt")
        self.assertRegex(
            send,
            r"mesh_c5_contact_active\(\s*peer_id,\s*purpose,\s*now_ms\s*\)",
        )

        clear_matching = function_body(
            REPORT, "mesh_c5_contact_clear_matching"
        )
        self.assertIn("mesh_c5_contact.peer_id != peer_id", clear_matching)
        self.assertIn("mesh_c5_contact.purpose != purpose", clear_matching)
        self.assertIn("mesh_c5_contact_clear(reason)", clear_matching)

        finish = function_body(REPORT, "mesh_event_accept_finish_send")
        active_check = finish.index("if (transmitted_timing == NULL")
        release = finish.index("mesh_c5_contact_clear_matching", active_check)
        install = finish.index("mesh_install_channel9_timing_direction")
        self.assertIn(
            "C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION",
            finish[release : release + 300],
        )
        self.assertLess(active_check, release)
        self.assertLess(release, install)

    def test_deferred_control_flood_separates_local_arbitration_from_rf_backoff(self):
        admission = function_body(REPORT, "mesh_send_c5_flood")
        response_admission = function_body(REPORT, "mesh_send_c5_flood_response")
        body = function_body(REPORT, "mesh_c5_flood_store_deferred")
        worker = function_body(REPORT, "mesh_c5_flood_work_handler")
        retry = function_body(REPORT, "mesh_c5_flood_deferred_retry_ms")

        self.assertIn("ret == -EAGAIN || ret == -EBUSY", admission)
        self.assertRegex(
            response_admission,
            r"mesh_c5_flood_store_deferred\(\s*"
            r"out, purpose, reason, true,\s*"
            r"ret == -EAGAIN \|\| ret == -EBUSY\)",
        )
        self.assertIn("local_deferral", body)
        local_admission = body.index(
            "retry_ms = local_deferral ? MESH_C5_LOCAL_DEFER_RETRY_MS"
        )
        rf_admission = body.index(
            "mesh_c5_flood_deferred_retry_ms(out, &entry->rf_retry)",
            local_admission,
        )
        retry_count = body.index(
            "entry->retry_count = local_deferral ? 0u : 1u", rf_admission
        )
        schedule = body.index("mesh_reschedule_owned_work(", retry_count)
        self.assertLess(local_admission, rf_admission)
        self.assertLess(rf_admission, retry_count)
        self.assertLess(retry_count, schedule)
        self.assertIn("&entry->rf_retry", body)
        self.assertIn("entry->generation++", body)
        self.assertIn("&mesh_route_adv_deferred", body)

        local_classify = worker.index(
            "bool local_deferral = ret == -EAGAIN || ret == -EBUSY"
        )
        local_guard = worker.index(
            "if (current_generation && local_deferral", local_classify
        )
        local_end = worker.index(
            "if (current_generation && !local_deferral", local_guard
        )
        local_retry = worker[local_guard:local_end]
        self.assertIn("MESH_C5_LOCAL_DEFER_MAX_AGE_MS", local_retry)
        self.assertIn("MESH_C5_LOCAL_DEFER_RETRY_MS", local_retry)
        self.assertIn('"c5-flood-local-defer"', local_retry)
        self.assertNotIn("mesh_c5_flood_deferred_retry_ms", local_retry)
        self.assertNotIn("entry->retry_count++", local_retry)
        self.assertNotIn("entry->rf_retry", local_retry)

        rf_end = worker.index("k_mutex_unlock", local_end)
        rf_retry = worker[local_end:rf_end]
        self.assertIn("mesh_c5_flood_deferred_retry_ms", rf_retry)
        self.assertIn("&entry->rf_retry", rf_retry)
        self.assertIn("entry->retry_count++", rf_retry)
        self.assertIn('"c5-flood-retry"', rf_retry)

        self.assertIn("#define MESH_C5_LOCAL_DEFER_RETRY_MS 5u", REPORT_UNIT)
        self.assertIn(
            "#define MESH_C5_LOCAL_DEFER_MAX_AGE_MS",
            REPORT_UNIT,
        )
        self.assertIn("NODE_COMM_BOUNDED_CONTROL_HOP_BUDGET_MS", REPORT_UNIT)
        self.assertIn("mesh_rf_retry_packet_key", retry)
        self.assertIn("&out->packet", retry)
        self.assertIn("retry_state", retry)
        self.assertIn("mesh_rf_retry_next_delay_ms", retry)
        self.assertNotIn("MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS", body)

    def test_paused_transport_keeps_valid_deferred_flood_live_and_bounded(self):
        body = function_body(REPORT, "mesh_c5_flood_work_handler")
        paused = braced_block_after(body, "if (mesh_transport_paused())")
        valid = braced_block_after(paused, "if (current_generation)")

        self.assertIn("mesh_reschedule_owned_work(", valid)
        self.assertIn("&mesh_c5_flood_work", valid)
        self.assertIn("mesh_c5_flood_deferred.valid", paused)
        self.assertIn("mesh_route_adv_deferred.valid", paused)
        self.assertNotIn(
            "entry->valid = false", paused,
            "transport arbitration must retain deferred flood custody",
        )
        self.assertNotIn(
            "entry->retry_count++", paused,
            "a pause is not an RF attempt and must not consume retry budget",
        )

        lane_select = body.index(
            "entry = route_adv_lane ? &mesh_route_adv_deferred"
        )
        generation_guard = body.index(
            "entry->generation == generation", lane_select
        )
        retry_guard = body.index("entry->retry_count <", generation_guard)
        self.assertIn(
            "MESH_C5_DEFERRED_MAX_RETRIES",
            body[retry_guard : retry_guard + 160],
        )
        self.assertIn("&entry->rf_retry", body[retry_guard : retry_guard + 300])
        retry_increment = body.index("entry->retry_count++", retry_guard)
        retry_schedule = body.index(
            "mesh_reschedule_owned_work(&mesh_c5_flood_work",
            retry_increment,
        )
        custody_release = body.index("entry->valid = false", retry_schedule)
        self.assertLess(lane_select, generation_guard)
        self.assertLess(generation_guard, retry_guard)
        self.assertLess(retry_guard, retry_increment)
        self.assertLess(retry_increment, retry_schedule)
        self.assertLess(retry_schedule, custody_release)
        self.assertRegex(
            REPORT_UNIT,
            r"#define\s+MESH_C5_DEFERRED_MAX_RETRIES\s+[1-9][0-9]*u",
        )

    def test_successful_accept_releases_single_retry_slot(self):
        body = function_body(REPORT, "mesh_event_accept_finish_send")
        store = body.index("mesh_event_accept_completed_store")
        clear = body.rindex("mesh_event_accept_clear()")

        self.assertLess(store, clear)
        self.assertIn("app_mesh_event_retry_note_send_success", body)

    def test_event_retry_scheduler_arms_wrapped_zero_due_time_explicitly(self):
        scheduler = function_body(REPORT, "mesh_event_negotiation_schedule_next")
        proposal = function_body(
            REPORT, "mesh_propose_event_after_channel5_contact_authorized"
        )
        parent_repair = function_body(
            REPORT, "mesh_try_repair_selected_parent_event"
        )
        route_ready = function_body(
            REPORT, "mesh_schedule_route_ready_event_retry"
        )
        duplicate = function_body(REPORT, "mesh_event_accept_duplicate")

        self.assertIn("state->retry_due_armed", scheduler)
        self.assertNotIn("state->retry_due_ms != 0u", scheduler)
        self.assertIn("mesh_event_propose_retry.retry_due_armed", proposal)
        self.assertNotIn(
            "mesh_event_propose_retry.retry_due_ms != 0u", proposal
        )
        self.assertIn(
            "mesh_event_propose_retry.retry_due_armed", parent_repair
        )
        self.assertNotIn(
            "mesh_event_propose_retry.retry_due_ms != 0u", parent_repair
        )
        self.assertIn(
            "mesh_event_propose_retry.retry_due_armed", route_ready
        )
        self.assertNotIn(
            "mesh_event_propose_retry.retry_due_ms != 0u", route_ready
        )
        self.assertIn(
            "!mesh_event_accept_retry.retry.retry_due_armed", duplicate
        )
        self.assertNotIn(
            "mesh_event_accept_retry.retry.retry_due_ms == 0u", duplicate
        )

    def test_accept_reservation_stays_inert_until_successful_send(self):
        handler_source = REPORT[
            REPORT.rindex("static bool mesh_handle_event_control") :
        ]
        handler = function_body(handler_source, "mesh_handle_event_control")
        proposal = handler.index(
            "packet->msg_type == MSG_MESH_EVENT_PROPOSE"
        )
        accept = handler.index(
            "packet->msg_type == MSG_MESH_EVENT_ACCEPT", proposal
        )
        proposal_path = handler[proposal:accept]
        reserve = proposal_path.index(
            "app_mesh_c5_event_accept_reservation"
        )
        begin = proposal_path.index("app_mesh_event_retry_begin", reserve)
        attempt = proposal_path.index("mesh_event_accept_attempt", begin)
        attempt_body = function_body(REPORT, "mesh_event_accept_attempt")
        finish = function_body(REPORT, "mesh_event_accept_finish_send")
        clear = function_body(REPORT, "mesh_event_accept_clear")
        send = attempt_body.index("mesh_send_event_control_record")
        success = attempt_body.index("if (ret == 0)", send)
        finish_call = attempt_body.index(
            "mesh_event_accept_finish_send", success
        )
        backoff = attempt_body.index(
            "mesh_event_retry_after_failure", finish_call
        )
        claim = finish.index("app_mesh_event_retry_claim_timing_install")
        install = finish.index("mesh_install_channel9_timing_direction")
        note_success = finish.index("app_mesh_event_retry_note_send_success")
        store = finish.index("mesh_event_accept_completed_store")
        final_clear = finish.rindex("mesh_event_accept_clear()")

        self.assertLess(reserve, begin)
        self.assertLess(begin, attempt)
        self.assertNotIn("mesh_install_channel9_timing", proposal_path)
        self.assertNotIn("mesh_relay_clear_channel9_timing", proposal_path)
        self.assertLess(send, success)
        self.assertLess(success, finish_call)
        self.assertLess(finish_call, backoff)
        self.assertNotIn("mesh_install_channel9_timing", attempt_body)
        self.assertNotIn("mesh_relay_clear_channel9_timing", attempt_body)
        self.assertEqual(finish.count("mesh_install_channel9_timing_direction"), 1)
        replay_guard = finish.index(
            "!mesh_event_accept_retry.replay_existing_response"
        )
        committed = finish.index(
            "committed_timing = mesh_event_accept_retry.response.timing"
        )
        self.assertLess(committed, replay_guard)
        self.assertLess(replay_guard, claim)
        self.assertLess(claim, install)
        self.assertIn(
            "committed_timing",
            finish[install : install + 300],
        )
        self.assertNotIn("committed_timing = transmitted_timing", finish)
        self.assertNotIn("transmitted_timing", finish[install:note_success])
        self.assertLess(install, note_success)
        self.assertLess(note_success, store)
        self.assertLess(store, final_clear)
        self.assertNotIn("mesh_install_channel9_timing", clear)
        self.assertNotIn("mesh_relay_clear_channel9_timing", clear)

    def test_admitted_proposal_arms_exact_contact_before_immediate_accept(self):
        handler_source = REPORT[
            REPORT.rindex("static bool mesh_handle_event_control") :
        ]
        handler = function_body(handler_source, "mesh_handle_event_control")
        proposal = handler.index("packet->msg_type == MSG_MESH_EVENT_PROPOSE")
        accept_branch = handler.index(
            "packet->msg_type == MSG_MESH_EVENT_ACCEPT", proposal
        )
        proposal_path = handler[proposal:accept_branch]

        retry_begin = proposal_path.index("app_mesh_event_retry_begin(")
        predecessor = proposal_path.index(
            "predecessor_owner_generation", retry_begin
        )
        contact = proposal_path.index(
            "mesh_event_accept_arm_contact(previous_hop_id", predecessor
        )
        immediate_accept = proposal_path.index(
            "mesh_event_accept_attempt(\"mesh-event-accept\")", contact
        )
        contact_call = proposal_path[
            contact : proposal_path.index(");", contact) + 2
        ]

        self.assertLess(retry_begin, predecessor)
        self.assertLess(predecessor, contact)
        self.assertLess(contact, immediate_accept)
        self.assertIn("previous_hop_id", contact_call)
        self.assertIn('"event-propose-admitted"', contact_call)

        arm_contact = function_body(REPORT, "mesh_event_accept_arm_contact")
        self.assertIn("mesh_c5_contact_accept(", arm_contact)
        self.assertIn("peer_id", arm_contact)
        self.assertIn(
            "C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION", arm_contact
        )
        self.assertIn("mesh_c5_exchange_expires_at(", arm_contact)

        # Once the handler has sent and cleared the ACCEPT exchange, the outer
        # queue drain must not recreate that PROPOSE contact on its way out.
        drain = function_body(REPORT_DELIVERY, "mesh_drain_rx_queue_locked")
        delivered = drain.index(
            "admitted_event_control = mesh_handle_event_control("
        )
        admitted = drain.index(
            "mesh_c5_control_rx_semantically_admitted(", delivered
        )
        note = drain.index("mesh_note_c5_control_rx(", admitted)
        outer_guard = drain[admitted:note]
        self.assertIn(
            "pending->packet.msg_type != MSG_MESH_EVENT_PROPOSE", outer_guard
        )

    def test_accept_retry_contact_lifetime_follows_exact_retry_owner(self):
        attempt = function_body(REPORT, "mesh_event_accept_attempt")
        finish = function_body(REPORT, "mesh_event_accept_finish_send")
        clear = function_body(REPORT, "mesh_event_accept_clear")
        arm = function_body(REPORT, "mesh_event_accept_arm_contact")
        duplicate = function_body(REPORT, "mesh_event_accept_duplicate")
        retry_worker = function_body(
            REPORT, "mesh_event_negotiation_retry_work_handler"
        )
        promotion = function_body(
            REPORT, "mesh_event_accept_promote_forwarded_ack_repair"
        )

        predecessor_stale = braced_block_after(
            attempt, "if (!mesh_event_accept_predecessor_matches())"
        )
        retry_exhausted = braced_block_after(
            attempt, "if (!mesh_event_retry_after_failure("
        )
        retry_failure = attempt[
            attempt.index("if (!mesh_event_retry_after_failure(") :
        ]
        success_clear = finish.rindex("mesh_event_accept_clear()")
        expired = braced_block_after(
            retry_worker,
            "if (app_mesh_event_retry_expired(&mesh_event_accept_retry.retry",
        )
        promotion_expired = braced_block_after(
            promotion,
            "if (app_mesh_event_retry_expired(&mesh_event_accept_retry.retry",
        )

        for terminal_path in (
            predecessor_stale,
            retry_exhausted,
            expired,
            promotion_expired,
        ):
            self.assertIn("mesh_event_accept_clear()", terminal_path)
            self.assertNotIn("memset(&mesh_event_accept_retry", terminal_path)

        self.assertIn("mesh_c5_contact_clear_matching(", clear)
        self.assertIn(
            "C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION", clear
        )
        self.assertNotIn("mesh_c5_contact_clear(", clear)
        self.assertIn("mesh_event_accept_retry.retry.peer_id", clear)

        # A retryable send failure returns with both the immutable ACCEPT and
        # its exact accepted exchange alive. Only exhaustion may clear it.
        retry_exhausted_start = retry_failure.index(
            "if (!mesh_event_retry_after_failure("
        )
        retry_exhausted_block = retry_failure.index(
            retry_exhausted, retry_exhausted_start
        )
        retry_exhausted_end = retry_exhausted_block + len(retry_exhausted)
        retryable_tail = retry_failure[retry_exhausted_end:]
        self.assertIn("return ret;", retryable_tail)
        self.assertNotIn("mesh_c5_contact_clear", retryable_tail)

        self.assertGreater(success_clear, finish.index("app_mesh_event_retry_note_send_success"))
        self.assertNotIn("mesh_c5_contact_clear(", finish)

        # A completion-cache replay is a new unsent response owner and must
        # reacquire the exact exchange before its retained retry can run.
        completed_match = duplicate.index(
            "if (match == APP_MESH_EVENT_REQUEST_DUPLICATE)"
        )
        replay = duplicate.index("replay_existing_response = true", completed_match)
        replay_contact = duplicate.index("mesh_event_accept_arm_contact(", replay)
        replay_admitted = duplicate.index(
            "return MESH_EVENT_CACHED_PROPOSAL_ADMITTED;", replay_contact
        )
        self.assertLess(replay, replay_contact)
        self.assertLess(replay_contact, replay_admitted)
        self.assertIn('"event-propose-replay"', duplicate[replay_contact:replay_admitted])
        self.assertIn("mesh_c5_contact_accept(", arm)

    def test_accept_may_defer_only_exact_local_direct_gateway_retry(self):
        owner = function_body(
            REPORT_EVENT_TX, "mesh_event_accept_local_direct_gateway_owner"
        )
        defer_helper = function_body(
            REPORT_EVENT_TX,
            "mesh_event_accept_defer_local_direct_gateway_owner",
        )
        admission = function_body(
            REPORT_EVENT_TX, "mesh_event_accept_downstream_admission_allowed"
        )
        relay_defer = function_body(
            MESH_RELAY_DELIVERY, "mesh_relay_defer_pending_retry"
        )

        for exact_identity in (
            "pending->packet.src_id != DEVICE_ID",
            "pending->packet.dst_id != GATEWAY_ID",
            "pending->next_hop_id != GATEWAY_ID",
            "selected->gateway_id != GATEWAY_ID",
            "selected->next_hop_id != GATEWAY_ID",
            "selected->hop_count != 0u",
        ):
            self.assertIn(exact_identity, owner)
        self.assertIn("route_selected(&mesh_runtime.upstream)", owner)
        self.assertIn("mesh_relay_tx_active(&mesh_runtime)", owner)
        self.assertIn("mesh_route_waiting_tx_valid", owner)
        self.assertIn(
            "mesh_event_accept_async_gateway_route_owner_present()", owner
        )
        self.assertIn("pending->result_offer_active", owner)
        self.assertIn("pending->gateway_ack_confirm_pending", owner)
        self.assertIn("pending->gateway_ack_forward_pending", owner)
        self.assertIn("MESH_RELAY_TX_WAIT_GATEWAY_ACK", owner)
        self.assertIn("MESH_RELAY_TX_WAIT_RETRY_BACKOFF", owner)
        for excluded_owner in (
            "MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD",
            "MESH_RELAY_TX_WAIT_RESULT_GRANT",
            "MESH_RELAY_TX_WAIT_TERMINAL_COMMIT",
        ):
            self.assertNotIn(excluded_owner, owner)

        # Preserve the old ACK or retry boundary exactly. This is a passive
        # scheduling handoff, not a fresh failure, attempt, or backoff round.
        self.assertIn("pending->gateway_ack_deadline_ms", owner)
        self.assertIn("pending->retry_after_ms", owner)
        self.assertRegex(
            owner,
            r"(?s)resume_at\s*=.*MESH_RELAY_TX_WAIT_GATEWAY_ACK.*\?"
            r".*gateway_ack_deadline_ms.*:.*retry_after_ms",
        )
        defer_call = defer_helper.index("mesh_relay_defer_pending_retry(")
        self.assertIn(
            "mesh_event_accept_local_direct_gateway_owner(&resume_at_ms)",
            defer_helper[:defer_call],
        )
        self.assertIn(
            "resume_at_ms", defer_helper[defer_call : defer_call + 220]
        )
        self.assertNotIn("mesh_relay_note_pending_parent_failure", defer_helper)
        self.assertNotIn("mesh_relay_tick", defer_helper)
        self.assertNotIn("mesh_relay_note_tx_sent", defer_helper)

        # The primitive mutates only scheduling state and the persisted state
        # projection; immutable packet identity and attempt counters survive.
        self.assertIn("relay->pending.retry_after_ms", relay_defer)
        self.assertIn("retry_at_ms", relay_defer)
        for forbidden_mutation in (
            "relay->pending.packet =",
            "relay->pending.payload_len =",
            "relay->pending.next_hop_id =",
            "relay->pending.attempts",
            "relay->pending.tx_attempt",
            "route_failure",
        ):
            self.assertNotIn(forbidden_mutation, relay_defer)

        route_wait = admission.index("mesh_route_waiting_tx_valid")
        direct = admission.index(
            "mesh_event_accept_local_direct_gateway_owner(", route_wait
        )
        async_route = admission.index("mesh_route_discovery_request.pending")
        blocked = admission.index("if (blocked_by != NULL)", async_route)
        self.assertLess(route_wait, direct)
        self.assertLess(direct, async_route)
        self.assertLess(async_route, blocked)
        self.assertIn(
            "*defer_local_direct_gateway_owner = true",
            admission[direct:async_route],
        )

        handler_source = REPORT[
            REPORT.rindex("static bool mesh_handle_event_control") :
        ]
        handler = function_body(handler_source, "mesh_handle_event_control")
        retry_begin = handler.index("app_mesh_event_retry_begin(")
        defer_after_custody = handler.index(
            "mesh_event_accept_defer_local_direct_gateway_owner(now_ms)",
            retry_begin,
        )
        arm_contact = handler.index(
            "mesh_event_accept_arm_contact(previous_hop_id", defer_after_custody
        )
        self.assertLess(retry_begin, defer_after_custody)
        self.assertLess(defer_after_custody, arm_contact)

    def test_due_direct_retry_yields_only_to_unsent_causal_accept(self):
        timeout = function_body(REPORT, "mesh_tx_timeout_handler")
        pending_check = timeout.index("mesh_event_accept_response_pending()")
        direct_check = timeout.index(
            "mesh_event_accept_local_direct_gateway_owner(", pending_check
        )
        accept_schedule = timeout.index(
            "mesh_event_negotiation_schedule_next()", direct_check
        )
        timeout_schedule = timeout.index("mesh_schedule_tx_timeout()", direct_check)
        yield_return = timeout.index("return;", direct_check)
        relay_tick = timeout.index("mesh_relay_tick_with_random(", yield_return)

        self.assertLess(pending_check, direct_check)
        self.assertLess(direct_check, accept_schedule)
        self.assertLess(direct_check, timeout_schedule)
        self.assertLess(accept_schedule, yield_return)
        self.assertLess(timeout_schedule, yield_return)
        self.assertLess(yield_return, relay_tick)
        gate = timeout[pending_check:yield_return]
        self.assertIn("mesh_relay_defer_pending_retry(&mesh_runtime", gate)
        self.assertIn("defer_until_ms", gate)
        self.assertNotIn("mesh_route_waiting_tx_valid", gate)
        self.assertNotIn("mesh_route_discovery_request.pending", gate)

        response_pending = function_body(
            REPORT, "mesh_event_accept_response_pending"
        )
        self.assertIn("mesh_event_accept_retry.retry.active", response_pending)
        self.assertIn(
            "!mesh_event_accept_retry.retry.response_sent", response_pending
        )
        self.assertIn("mesh_event_accept_retry.response.valid", response_pending)
        self.assertIn("MSG_MESH_EVENT_ACCEPT", response_pending)

        # Batch ACK timeout servicing remains ahead of the causal-ACCEPT gate;
        # the new rule pauses only the unrelated relay-core retransmission.
        batch_ack = timeout.index("mesh_ch9_tx_pending_handle_timeout(now_ms)")
        self.assertLess(batch_ack, pending_check)

    def test_pending_accept_preempts_background_receive_owners(self):
        active = function_body(REPORT, "mesh_rx_response_active")
        worker_source = REPORT[
            REPORT.rindex("static void mesh_uwb_rx_work_handler") :
        ]
        worker = function_body(worker_source, "mesh_uwb_rx_work_handler")
        priority = worker.index("if (mesh_rx_response_active())")
        channel9 = worker.index("mesh_select_channel9_rx_event", priority)

        self.assertIn("mesh_event_accept_retry.retry.active", active)
        self.assertIn("!mesh_event_accept_retry.retry.response_sent", active)
        self.assertLess(priority, channel9)
        self.assertIn(
            "mesh_schedule_uwb_rx(MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS)",
            worker[priority:channel9],
        )
        self.assertIn("DBG_UWB_RX_YIELD_EVENT_RESPONSE", worker[priority:channel9])

    def test_duplicate_accept_replay_is_cached_and_backed_off(self):
        body = function_body(REPORT, "mesh_event_accept_duplicate")
        cache_match = body.index("mesh_event_accept_completed_match")
        replay = body.index("replay_existing_response = true")
        retry = body.index("mesh_event_retry_after_failure", replay)

        self.assertLess(cache_match, replay)
        self.assertLess(replay, retry)
        self.assertIn("app_mesh_event_retry_resume_backoff", body[replay:retry])
        self.assertIn("APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA", body[replay:])
        self.assertIn("false", body[retry : retry + 300])

    def test_cached_accept_replay_cannot_block_unrelated_proposal(self):
        body = function_body(REPORT, "mesh_event_accept_duplicate")
        busy = body.index("match == APP_MESH_EVENT_REQUEST_BUSY")
        replay = body.index("mesh_event_accept_retry.replay_existing_response", busy)
        persist = body.index(
            "mesh_event_accept_completed_note_retry_round", replay
        )
        preempt = body.index("mesh_event_accept_clear()", replay)
        generic_busy = body.index("match == APP_MESH_EVENT_REQUEST_CONFLICT", preempt)

        self.assertLess(busy, replay)
        self.assertLess(replay, persist)
        self.assertLess(persist, preempt)
        self.assertLess(preempt, generic_busy)

    def test_accept_preserves_proposed_phase_and_duplicates_are_inert(self):
        classify = function_body(REPORT, "mesh_event_accept_rx_match")
        propose = function_body(
            REPORT, "mesh_propose_event_after_channel5_contact_authorized"
        )
        propose_send = propose.index("mesh_send_event_control_record")
        propose_failure = propose.index("if (ret < 0)", propose_send)
        retain_phase = propose.index(
            "mesh_event_propose_record.timing = transmitted_timing",
            propose_failure,
        )
        freeze_first = propose.rfind(
            "if (!mesh_event_propose_record.transmit_phase_frozen)",
            propose_failure,
            retain_phase,
        )
        freeze_block = braced_block_after(
            propose[freeze_first:],
            "if (!mesh_event_propose_record.transmit_phase_frozen)",
        )
        accept_listen = propose.index("if (require_accept)", retain_phase)
        handler_source = REPORT[
            REPORT.rindex("static bool mesh_handle_event_control") :
        ]
        handler = function_body(handler_source, "mesh_handle_event_control")
        parsed_phase = handler.index("mesh_event_timing_from_tlvs_at(&timing")
        accept_branch = handler.index(
            "} else if (packet->msg_type == MSG_MESH_EVENT_ACCEPT)"
        )
        duplicate = handler.index(
            "accept_match == APP_MESH_EVENT_REQUEST_DUPLICATE", accept_branch
        )
        replay_exit = handler.index("if (replayed_event_accept)", duplicate)
        replay_block = braced_block_after(
            handler[replay_exit:], "if (replayed_event_accept)"
        )
        restore_proposed = handler.index(
            "timing = mesh_event_propose_record.timing",
            replay_exit,
        )
        local_first_tx = handler.index(
            "mesh_event_timing_set_local_first_slot_tx(&timing, true)",
            restore_proposed,
        )
        defer_first = handler.index(
            "mesh_event_timing_defer_first_start_if_needed(",
            local_first_tx,
        )
        fresh_install = handler.index(
            "mesh_install_channel9_timing_direction", defer_first
        )
        fresh_schedule = handler.index("mesh_schedule_uwb_rx", fresh_install)
        fresh_accept = handler[replay_exit:fresh_install]

        self.assertIn("return match", classify)
        self.assertLess(propose_send, propose_failure)
        self.assertLess(propose_failure, retain_phase)
        self.assertNotEqual(freeze_first, -1)
        self.assertIn(
            "mesh_event_propose_record.timing = transmitted_timing",
            freeze_block,
        )
        self.assertIn(
            "mesh_event_propose_record.transmit_phase_frozen = true",
            freeze_block,
        )
        self.assertLess(retain_phase, accept_listen)
        self.assertLess(parsed_phase, accept_branch)
        self.assertIn("return true", replay_block)
        self.assertNotIn("mesh_install_channel9_timing", replay_block)
        self.assertNotIn("mesh_event_timing_set_local_first_slot_tx", replay_block)
        self.assertLess(replay_exit, restore_proposed)
        self.assertLess(restore_proposed, local_first_tx)
        self.assertLess(local_first_tx, defer_first)
        self.assertLess(defer_first, fresh_install)
        self.assertIn(
            "&timing",
            handler[fresh_install : fresh_install + 300],
        )
        self.assertIn(
            "mesh_channel9_prepare_start_ms(&timing)",
            handler[fresh_install:fresh_schedule + 300],
        )

    def test_accept_rx_cache_ends_with_its_proposal_or_connection(self):
        propose = function_body(
            REPORT, "mesh_propose_event_after_channel5_contact_authorized"
        )
        new_proposal = propose.index("if (!mesh_event_propose_retry.active)")
        clear = propose.index(
            "mesh_event_accept_rx_clear()",
            new_proposal,
        )
        prepare = propose.index("mesh_prepare_event_control_record", new_proposal)
        self.assertLess(clear, prepare)

        handler_source = REPORT[
            REPORT.rindex("static bool mesh_handle_event_control") :
        ]
        handler = function_body(handler_source, "mesh_handle_event_control")
        end_rx = handler.index("packet->msg_type == MSG_MESH_EVENT_END")
        self.assertIn(
            "mesh_event_accept_rx_clear_peer(previous_hop_id)",
            handler[end_rx:],
        )

        arm_close = function_body(REPORT, "mesh_close_channel9_connection")
        close_attempt = function_body(
            REPORT, "mesh_try_close_channel9_connection"
        )
        self.assertNotIn("mesh_event_accept_rx_clear_peer(peer_id)", arm_close)
        self.assertIn(
            "mesh_event_accept_rx_clear_peer(peer_id)", close_attempt
        )

    def test_accept_duplicate_caches_preserve_identity_with_compact_layout(self):
        cache = REPORT[REPORT.index("struct mesh_event_accept_rx_cache {") :]
        cache = cache[: cache.index("};") + 2]
        self.assertIn("struct app_mesh_event_request_identity request", cache)
        self.assertIn("uint64_t peer_id", cache)
        self.assertIn("uint32_t deadline_ms", cache)
        self.assertIn("bool timing_installed", cache)
        self.assertNotIn("struct app_mesh_event_retry_state retry", cache)
        self.assertIn(
            "BUILD_ASSERT(sizeof(struct mesh_event_accept_rx_cache) == 64u",
            REPORT,
        )

        completed = REPORT[REPORT.index("struct mesh_event_accept_completed {") :]
        completed = completed[: completed.index("};") + 2]
        self.assertIn("struct app_mesh_event_request_identity request", completed)
        self.assertIn("uint32_t expires_at_ms", completed)
        self.assertNotIn("struct app_mesh_event_completion completion", completed)
        self.assertIn(
            "BUILD_ASSERT(sizeof(struct mesh_event_accept_completed) == 208u",
            REPORT,
        )

    def test_proposal_services_supervision_before_owner_classification(self):
        handler_source = REPORT[
            REPORT.rindex("static bool mesh_handle_event_control") :
        ]
        handler = function_body(handler_source, "mesh_handle_event_control")
        proposal = handler.index("packet->msg_type == MSG_MESH_EVENT_PROPOSE")
        expire = handler.index("mesh_expire_channel9_timings", proposal)
        nonce = handler.index("mesh_event_owner_proposal_boot_nonce", proposal)
        owner_lookup = handler.index("mesh_event_owner_for_peer", nonce)
        active_timing = handler.index(
            "had_active_timing = mesh_find_active_channel9_timing", owner_lookup
        )
        orphan_retire = handler.index(
            "mesh_event_owner_abandon_peer(previous_hop_id)", active_timing
        )
        classify = handler.index(
            "mesh_event_owner_registry_classify_proposal", proposal
        )
        reject = handler.index(
            "owner_decision != MESH_EVENT_OWNER_APPLY", classify
        )
        duplicate = handler.index("mesh_event_accept_duplicate", reject)
        reserve = handler.index("app_mesh_c5_event_accept_reservation", duplicate)
        prepare_accept = handler.index("mesh_prepare_event_control_record", reserve)

        self.assertLess(expire, nonce)
        self.assertLess(nonce, owner_lookup)
        self.assertLess(owner_lookup, active_timing)
        self.assertLess(active_timing, orphan_retire)
        self.assertLess(orphan_retire, classify)
        orphan_guard = handler[active_timing:orphan_retire]
        self.assertIn("owner != NULL && owner->active", orphan_guard)
        self.assertIn("!had_active_timing", orphan_guard)
        self.assertLess(classify, reject)
        self.assertLess(reject, duplicate)
        self.assertIn("return true", handler[reject:reserve])
        self.assertLess(duplicate, reserve)
        self.assertLess(reserve, prepare_accept)

    def test_proposal_proves_channel9_installability_before_accepting(self):
        handler_source = REPORT[
            REPORT.rindex("static bool mesh_handle_event_control") :
        ]
        handler = function_body(handler_source, "mesh_handle_event_control")
        proposal = handler.index("packet->msg_type == MSG_MESH_EVENT_PROPOSE")
        reserve = handler.index(
            "app_mesh_c5_event_accept_reservation", proposal
        )
        guard = handler.index(
            "mesh_relay_check_channel9_timing_guarded_direction", reserve
        )
        admission = handler.index(
            "mesh_event_accept_downstream_admission_allowed(", guard
        )
        prepare_accept = handler.index(
            "mesh_prepare_event_control_record", admission
        )
        send_accept = handler.index("mesh_event_accept_attempt", prepare_accept)
        guard_block = handler[guard:admission]
        admission_block = handler[admission:prepare_accept]

        self.assertIn("previous_hop_id", guard_block)
        self.assertIn("&reservation_timing", guard_block)
        self.assertIn("MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM", guard_block)
        self.assertIn("MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS", guard_block)
        self.assertIn("if (ret != PROTO_OK)", guard_block)
        self.assertIn("return false", guard_block)
        self.assertIn("previous_hop_id", admission_block)
        self.assertIn("&reservation_timing", admission_block)
        self.assertIn("return false", admission_block)
        self.assertLess(reserve, guard)
        self.assertLess(guard, admission)
        self.assertLess(admission, prepare_accept)
        self.assertLess(prepare_accept, send_accept)

        admission_helper = function_body(
            REPORT_EVENT_TX,
            "mesh_event_accept_downstream_admission_allowed",
        )
        stable_helper = function_body(
            REPORT_EVENT_TX,
            "mesh_event_accept_local_owner_has_stable_upstream_timing",
        )
        self.assertIn("route_selected(&mesh_runtime.upstream)", stable_helper)
        self.assertIn("selected->gateway_id != GATEWAY_ID", stable_helper)
        self.assertIn("!selected->channel9_timing_valid", stable_helper)
        self.assertIn(
            "mesh_runtime.pending.next_hop_id != selected->next_hop_id",
            stable_helper,
        )
        self.assertIn("entry->valid", stable_helper)
        self.assertIn(
            "entry->next_hop_id == selected->next_hop_id", stable_helper
        )
        self.assertIn(
            "entry->direction == MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM",
            stable_helper,
        )
        self.assertIn(
            "mesh_event_timing_usable(&entry->timing, now_ms)",
            stable_helper,
        )
        self.assertIn("mesh_route_waiting_tx_valid", admission_helper)
        self.assertIn(
            "mesh_route_waiting_tx.packet.src_id == DEVICE_ID",
            admission_helper,
        )
        self.assertIn(
            "mesh_route_waiting_tx.packet.dst_id == GATEWAY_ID",
            admission_helper,
        )
        self.assertIn("mesh_relay_tx_active(&mesh_runtime)", admission_helper)
        self.assertIn(
            "mesh_runtime.pending.packet.src_id == DEVICE_ID",
            admission_helper,
        )
        self.assertIn(
            "mesh_runtime.pending.packet.dst_id == GATEWAY_ID",
            admission_helper,
        )
        stable = admission_helper.index(
            "mesh_event_accept_local_owner_has_stable_upstream_timing("
        )
        route_wait = admission_helper.index("mesh_route_waiting_tx_valid")
        core_pending = admission_helper.index(
            "mesh_relay_tx_active(&mesh_runtime)", route_wait
        )
        stable_exemption = admission_helper.index(
            "!stable_upstream_timing", core_pending
        )
        async_route = admission_helper.index(
            "mesh_route_discovery_request.pending", stable_exemption
        )
        self.assertLess(stable, route_wait)
        self.assertLess(route_wait, core_pending)
        self.assertLess(core_pending, stable_exemption)
        self.assertLess(stable_exemption, async_route)
        self.assertIn("mesh_route_discovery_request.pending", admission_helper)
        self.assertIn(
            "mesh_route_discovery_request.target_id == GATEWAY_ID",
            admission_helper,
        )
        self.assertIn(
            "APP_MESH_ASYNC_ROUTE_TRANSFER_NONE", admission_helper
        )
        self.assertIn(
            "APP_MESH_ASYNC_ROUTE_TRANSFER_NODE_COMM", admission_helper
        )
        self.assertIn(
            "app_node_comm_delivery_owner_matches(",
            admission_helper,
        )
        self.assertNotIn(
            "APP_MESH_ASYNC_ROUTE_TRANSFER_CORE_PENDING", admission_helper
        )
        self.assertIn("if (blocked_by != NULL)", admission_helper)
        reject_blocked = admission_helper.index(
            "return false", admission_helper.index("if (blocked_by != NULL)")
        )
        admit_clear = admission_helper.rindex("return true")
        self.assertLess(reject_blocked, admit_clear)

    def test_transit_gateway_owner_binds_exact_ingress_before_any_rf(self):
        body = function_body(REPORT, "mesh_start_tracked_tx_with_retry")
        send_prepared = body.index("send_prepared:")
        bind = body.index("mesh_relay_bind_transit_previous_hop", send_prepared)
        cancel = body.index("mesh_relay_cancel_tx_if_matches", bind)
        first_rf = min(
            body.index("mesh_send_c5_control_attempt", bind),
            body.index("mesh_send_direct_gateway_payload_and_wait_ack", bind),
            body.index("mesh_send_outbound_with_release_until", bind),
        )
        gate = body[send_prepared:bind]
        failure = body[bind:first_rf]

        self.assertIn("aged_out.packet.dst_id == GATEWAY_ID", gate)
        self.assertIn("aged_out.packet.src_id != DEVICE_ID", gate)
        self.assertIn("FLAG_GATEWAY_ACK_REQUIRED", gate)
        self.assertIn("aged_out.ingress_previous_hop_id", failure)
        self.assertIn("if (bind_ret != PROTO_OK)", failure)
        self.assertIn("app_watchdog_stop_feeding()", failure)
        self.assertLess(bind, cancel)
        self.assertLess(cancel, first_rf)

    def test_timing_expiry_retires_owner_before_erasing_timing(self):
        expire = function_body(REPORT, "mesh_expire_channel9_timings")
        unusable = expire.index("!mesh_event_timing_usable")
        abandon = expire.index("mesh_event_owner_abandon_peer", unusable)
        erase = expire.index("mesh_relay_expire_channel9_timings", abandon)

        self.assertLess(unusable, abandon)
        self.assertLess(abandon, erase)
        self.assertIn("entry->valid", expire[:abandon])


if __name__ == "__main__":
    unittest.main()
