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
REPORT_TRANSPORT = (
    ROOT / "app" / "src" / "app_mesh_report_transport.inc"
).read_text(encoding="utf-8")


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
        repair_return = body.index("return -EHOSTUNREACH;", repair)
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
        store = helper.index("mesh_store_route_waiting_tx(out)")
        propose = helper.index(
            "mesh_propose_event_after_channel5_contact("
        )
        hard_branch = helper.index("if (hard_failure)", propose)

        self.assertLess(store, propose)
        self.assertLess(propose, hard_branch)
        self.assertIn('"initial-tx-event-repair"', helper[propose:hard_branch])
        self.assertIn(
            "repair_active = mesh_event_propose_retry.active",
            helper[propose:hard_branch],
        )
        self.assertNotIn("mesh_schedule_route_request", helper[:hard_branch])

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
            "mesh_propose_event_after_channel5_contact("
        )
        classify = helper.index("hard_failure =", propose)
        hard_branch = helper.index("if (hard_failure)", classify)
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
        self.assertLess(classify, hard_branch)
        self.assertLess(hard_branch, invalidate)
        self.assertLess(invalidate, request)
        self.assertEqual(helper.count("mesh_schedule_route_request"), 1)
        self.assertEqual(helper.count("mesh_relay_invalidate_upstream_route"), 1)

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

    def test_non_route_solicit_wake_followups_are_marked_control(self):
        wake = function_body(
            REPORT_ROUTE_CONTROL, "mesh_send_route_wake_train"
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
            r"C5_CONTACT_PURPOSE_ROUTE_SOLICIT\s*\)\s*\{\s*"
            r"config->flags\s*\|=\s*FLAG_CONTROL_FOLLOWUP\s*;",
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
        ack = required.index("mesh_channel9_ack_pending_for_peer", local_tx)
        skip_to_rx = required.index("timing->next_event_time_ms +=")
        self.assertLess(local_tx, pending)
        self.assertLess(pending, skip_to_rx)
        self.assertLess(ack, skip_to_rx)
        self.assertIn("return true", required[local_tx:skip_to_rx])
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
        radio_stop = body.index("radio_guard_uwb_stop()")
        submit = body.index("mesh_submit_work(&mesh_rx_work)", radio_stop)

        self.assertLess(radio_stop, submit)
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
        radio_start = listener.index(
            'mesh_transport_radio_start("mesh route reply RX")', snapshot
        )
        ready_check = listener.index(
            "if (atomic_get(&mesh_route_ready_generation) !=", radio_start
        )
        receive = listener.index(
            "ret = dwm3000_driver_receive_frame_continuous(", ready_check
        )
        ready_branch = listener[ready_check:receive]
        timeout = listener.index("if (ret == -ETIMEDOUT)", receive)
        timeout_continue = listener.index("continue;", timeout)
        standby = listener.index(
            "(void)dwm3000_driver_standby()", timeout_continue
        )
        radio_stop = listener.index("radio_guard_uwb_stop()", standby)

        self.assertLess(snapshot, radio_start)
        self.assertLess(radio_start, ready_check)
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
        self.assertLess(standby, radio_stop)

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

    def test_first_deferred_control_flood_uses_identity_backoff(self):
        body = function_body(REPORT, "mesh_c5_flood_store_deferred")

        self.assertIn("mesh_c5_flood_deferred_retry_ms", body)
        self.assertIn("mesh_c5_flood_deferred.retry_count = 1u", body)
        self.assertNotIn("MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS", body)

    def test_successful_accept_releases_single_retry_slot(self):
        body = function_body(REPORT, "mesh_event_accept_finish_send")
        store = body.index("mesh_event_accept_completed_store")
        clear = body.rindex("mesh_event_accept_clear()")

        self.assertLess(store, clear)
        self.assertIn("app_mesh_event_retry_note_send_success", body)

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
        negotiated = finish.index(
            "&mesh_event_accept_retry.response.timing"
        )
        self.assertLess(negotiated, replay_guard)
        self.assertLess(replay_guard, claim)
        self.assertLess(claim, install)
        self.assertIn(
            "negotiated_timing",
            finish[install : install + 300],
        )
        self.assertNotIn(
            "transmitted_timing,",
            finish[install : install + 300],
        )
        self.assertLess(install, note_success)
        self.assertLess(note_success, store)
        self.assertLess(store, final_clear)
        self.assertNotIn("mesh_install_channel9_timing", clear)
        self.assertNotIn("mesh_relay_clear_channel9_timing", clear)

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
        preempt = body.index("mesh_event_accept_clear()", replay)
        generic_busy = body.index("match == APP_MESH_EVENT_REQUEST_CONFLICT", preempt)

        self.assertLess(busy, replay)
        self.assertLess(replay, persist)
        self.assertLess(persist, preempt)
        self.assertLess(preempt, generic_busy)

    def test_accept_confirms_retained_proposal_phase_and_duplicates_are_inert(self):
        classify = function_body(REPORT, "mesh_event_accept_rx_match")
        propose = function_body(
            REPORT, "mesh_propose_event_after_channel5_contact"
        )
        propose_send = propose.index("mesh_send_event_control_record")
        propose_failure = propose.index("if (ret < 0)", propose_send)
        retain_phase = propose.index(
            "mesh_event_propose_record.timing = transmitted_timing",
            propose_failure,
        )
        accept_listen = propose.index("if (require_accept)", retain_phase)
        handler_source = REPORT[
            REPORT.rindex("static bool mesh_handle_event_control") :
        ]
        handler = function_body(handler_source, "mesh_handle_event_control")
        duplicate = handler.index(
            "accept_match == APP_MESH_EVENT_REQUEST_DUPLICATE"
        )
        replay_exit = handler.index("if (replayed_event_accept)", duplicate)
        retained_phase = handler.index(
            "timing = mesh_event_propose_record.timing", replay_exit
        )
        fresh_install = handler.index(
            "mesh_install_channel9_timing_direction", retained_phase
        )
        fresh_schedule = handler.index("mesh_schedule_uwb_rx", fresh_install)

        self.assertIn("return match", classify)
        self.assertLess(propose_send, propose_failure)
        self.assertLess(propose_failure, retain_phase)
        self.assertLess(retain_phase, accept_listen)
        self.assertIn("return true", handler[replay_exit:retained_phase])
        self.assertLess(replay_exit, retained_phase)
        self.assertLess(retained_phase, fresh_install)
        self.assertIn(
            "&timing",
            handler[fresh_install : fresh_install + 300],
        )
        self.assertIn(
            "mesh_channel9_prepare_start_ms(&timing)",
            handler[fresh_install:fresh_schedule + 300],
        )

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

    def test_stale_proposal_is_rejected_before_active_timing_replacement(self):
        handler_source = REPORT[
            REPORT.rindex("static bool mesh_handle_event_control") :
        ]
        handler = function_body(handler_source, "mesh_handle_event_control")
        proposal = handler.index("packet->msg_type == MSG_MESH_EVENT_PROPOSE")
        duplicate = handler.index("mesh_event_accept_duplicate", proposal)
        classify = handler.index("mesh_event_owner_classify_proposal", duplicate)
        reject = handler.index(
            "owner_decision != MESH_EVENT_OWNER_APPLY", classify
        )
        active_lookup = handler.index(
            "mesh_find_active_channel9_timing", reject
        )
        reserve = handler.index("app_mesh_c5_event_accept_reservation", active_lookup)
        prepare_accept = handler.index("mesh_prepare_event_control_record", reserve)

        self.assertLess(duplicate, classify)
        self.assertLess(classify, reject)
        self.assertIn("return true", handler[reject:active_lookup])
        self.assertLess(reject, active_lookup)
        self.assertLess(active_lookup, reserve)
        self.assertLess(reserve, prepare_accept)


if __name__ == "__main__":
    unittest.main()
