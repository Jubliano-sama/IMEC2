#!/usr/bin/env python3

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
REPORT = (ROOT / "app" / "src" / "app_mesh_report.c").read_text(
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


class MeshRfRetrySourceInvariantTests(unittest.TestCase):
    def test_route_request_pre_rf_busy_uses_identity_backoff(self):
        body = function_body(REPORT, "mesh_route_request_defer_rf_busy")

        self.assertIn("mesh_route_request_rf_retry_key", body)
        self.assertIn("mesh_route_request_rf_retry_state(phase)", body)
        self.assertIn("mesh_rf_retry_next_delay_ms", body)
        self.assertIn("APP_MESH_RF_RETRY_POLICY_WAKE_TRAIN", body)
        self.assertNotIn("MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS", body)

    def test_route_request_wake_success_cannot_clear_control_retry_round(self):
        body = function_body(REPORT, "mesh_request_route_owned")
        wake_success = body.index(
            "app_mesh_rf_retry_note_success(&mesh_route_request_wake_rf_retry"
        )
        control_send = body.index(
            'mesh_send_outbound(&route_req, "route-request-control")'
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

        self.assertIn("mesh_rf_retry_bank_next_delay_ms", body)
        self.assertIn("report_tx_schedule_backoff", body)
        self.assertIn("return -EALREADY", body)
        self.assertNotIn(
            "report_tx_schedule(MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS)", body
        )

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
        failure_block = body[send_index : send_index + 4200]

        self.assertIn("mesh_rf_retry_bank_next_delay_ms", failure_block)
        self.assertIn("queued-ch9-batch-partial-send", failure_block)
        self.assertIn("report_tx_schedule_backoff", failure_block)
        self.assertIn("return -EALREADY", failure_block)

    def test_deferred_gateway_ack_separates_plan_wait_from_send_failure(self):
        send_body = function_body(
            REPORT, "mesh_try_deferred_gateway_ack_on_channel9"
        )
        waiting_body = function_body(REPORT, "mesh_try_route_waiting_tx")
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
            "mesh_reschedule_delayable(&mesh_route_waiting_work, delay_ms)",
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
        route_wait = function_body(REPORT, "mesh_try_route_waiting_tx")
        async_submit = function_body(REPORT, "mesh_schedule_route_request")
        init = function_body(REPORT, "app_mesh_report_init")

        self.assertEqual(REPORT.count("mesh_request_route("), 3)
        self.assertEqual(discovery_worker.count("mesh_request_route("), 1)
        self.assertEqual(route_wait.count("mesh_request_route("), 1)
        self.assertNotIn("mesh_request_route(", async_submit)
        self.assertEqual(
            REPORT.count("mesh_try_route_waiting_tx("),
            3,
            "only the declaration, definition, and route-wait worker may name it",
        )
        self.assertIn(
            "mesh_reschedule_delayable(&mesh_route_discovery_work, 0u)",
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

    def test_first_gateway_ack_send_failures_enter_identity_backoff(self):
        body = function_body(REPORT, "mesh_handle_result_actions")
        current_start = body.index(
            "gateway ACK current channel-9 send failed"
        ) - 900
        planned_start = body.index(
            "APP_MESH_GATEWAY_ACK_ACTION_SEND_PLANNED_CHANNEL9"
        )
        planned_end = body.index(
            "APP_MESH_GATEWAY_ACK_ACTION_STORE_WAITING_REFRESH_CHANNEL9"
        )

        current_block = body[current_start : current_start + 1500]
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

    def test_direct_channel9_ack_policy_deferrals_advance_once(self):
        selector = function_body(REPORT, "mesh_select_channel9_ack_tx_event")
        result_handler = function_body(REPORT, "mesh_handle_result_actions")

        self.assertIn("mesh_event_plan_is_policy_deferral", selector)
        self.assertIn("mesh_relay_note_channel9_missed", selector)
        self.assertIn("app_mesh_ch9_ack_table_note_send_failure", selector)
        self.assertIn("gateway_ack_policy_deferred", result_handler)
        self.assertIn("gateway-ack-channel5-deferral", result_handler)
        self.assertIn("mesh_rf_retry_next_delay_ms", result_handler)

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

    def test_route_reply_listener_hands_event_control_to_the_worker(self):
        body = function_body(REPORT, "mesh_listen_for_route_reply")
        radio_stop = body.index("radio_guard_uwb_stop()")
        submit = body.index("mesh_submit_work(&mesh_rx_work)", radio_stop)

        self.assertLess(radio_stop, submit)
        self.assertIn("DBG_EVENT_CTRL_POST_RX_QUEUED", body)
        self.assertNotIn("mesh_process_queued_rx_now", body)

    def test_first_deferred_control_flood_uses_identity_backoff(self):
        body = function_body(REPORT, "mesh_c5_flood_store_deferred")

        self.assertIn("mesh_c5_flood_deferred_retry_ms", body)
        self.assertIn("mesh_c5_flood_deferred.retry_count = 1u", body)
        self.assertNotIn("MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS", body)

    def test_successful_accept_releases_single_retry_slot(self):
        body = function_body(REPORT, "mesh_event_accept_finish_send")
        store = body.index("mesh_event_accept_completed_store")
        clear = body.index("mesh_event_accept_clear(false")

        self.assertLess(store, clear)
        self.assertIn("app_mesh_event_retry_note_send_success", body)

    def test_duplicate_accept_replay_is_cached_and_backed_off(self):
        body = function_body(REPORT, "mesh_event_accept_duplicate")
        cache_match = body.index("app_mesh_event_completion_match")
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
        preempt = body.index('mesh_event_accept_clear(false, "event-accept-replay-preempt")', replay)
        generic_busy = body.index("match == APP_MESH_EVENT_REQUEST_CONFLICT", preempt)

        self.assertLess(busy, replay)
        self.assertLess(replay, persist)
        self.assertLess(persist, preempt)
        self.assertLess(preempt, generic_busy)

    def test_exact_duplicate_accept_reinstalls_relative_timing(self):
        classify = function_body(REPORT, "mesh_event_accept_rx_match")
        handler_source = REPORT[
            REPORT.rindex("static bool mesh_handle_event_control") :
        ]
        handler = function_body(handler_source, "mesh_handle_event_control")
        duplicate = handler.index(
            "accept_match == APP_MESH_EVENT_REQUEST_DUPLICATE"
        )
        reinstall = handler.index(
            "mesh_install_channel9_timing_direction", duplicate
        )

        self.assertIn("return match", classify)
        self.assertIn("replayed_event_accept", handler[duplicate:reinstall])
        self.assertIn("if (!replayed_event_accept)", handler[duplicate:reinstall])

    def test_accept_rx_cache_ends_with_its_proposal_or_connection(self):
        propose = function_body(REPORT, "mesh_propose_event_after_channel5_contact")
        new_proposal = propose.index("if (!mesh_event_propose_retry.active)")
        clear = propose.index(
            "app_mesh_event_retry_clear(&mesh_event_accept_rx_cache)",
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

        close = function_body(REPORT, "mesh_close_channel9_connection")
        self.assertIn("mesh_event_accept_rx_clear_peer(peer_id)", close)


if __name__ == "__main__":
    unittest.main()
