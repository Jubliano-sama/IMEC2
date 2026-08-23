#!/usr/bin/env python3
"""Keep gateway survey collection timing tied to the remembered route depth."""

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SURVEY = (ROOT / "app/src/app_anchor_gateway_survey.inc").read_text()
CONTROL = (ROOT / "app/src/app_anchor_gateway_control.inc").read_text()
ROUND = (ROOT / "app/src/app_anchor_gateway_survey_round.inc").read_text()
COORDINATION = (ROOT / "app/src/app_mesh_report_coordination.inc").read_text()
EVENT_TX = (ROOT / "app/src/app_mesh_report_event_tx.inc").read_text()
NODE_COMM = (ROOT / "app/src/app_node_comm.c").read_text()


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.S)
    if match is None:
        raise AssertionError(f"missing function {name}")
    start = match.end()
    depth = 1
    index = start
    while index < len(source) and depth != 0:
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
        index += 1
    if depth != 0:
        raise AssertionError(f"unterminated function {name}")
    return source[start : index - 1]


class GatewaySurveyDynamicDeadlineTests(unittest.TestCase):
    def test_gateway_orchestration_budget_is_independent_from_anchor_discovery_phase(self) -> None:
        route = function_body(SURVEY, "gateway_route_survey_reachability")
        policy_branch = route[
            route.index("if (policy_candidate.updates.discovery_present)") :
            route.index("config.survey_id = survey_id;")
        ]

        self.assertNotIn("command_budget_ms !=", policy_branch)
        self.assertNotIn("command_budget_ms =", policy_branch)
        self.assertNotIn("budget_explicit =", policy_branch)
        self.assertIn(
            ".operation_budget_ms =\n"
            "            policy_candidate.resolved.discovery.operation_budget_ms",
            route,
        )
        self.assertIn(
            "admitted_discovery.operation_budget_ms <\n"
            "                               required_budget_ms",
            route,
        )
        self.assertIn(
            "gateway_survey_operation_deadline_ms = command_origin_ms + command_budget_ms",
            route,
        )
        self.assertIn(
            ".value.discovery = policy_candidate.resolved.discovery",
            route,
        )

    def test_admitted_discovery_slack_extends_collection_before_command_cap(self) -> None:
        route = function_body(SURVEY, "gateway_route_survey_reachability")
        validation = route.index(
            "required_budget_ms < collection_delay_ms"
        )
        slack = route.index(
            "collection_delay_ms +=\n"
            "        admitted_discovery.operation_budget_ms - required_budget_ms"
        )
        command_cap = route.index(
            "collection_delay_ms = gateway_command_budget_window_ms(",
            slack,
        )
        frozen_deadline = route.index(
            "gateway_survey_collection_deadline_ms =",
            command_cap,
        )

        self.assertLess(validation, slack)
        self.assertLess(slack, command_cap)
        self.assertLess(command_cap, frozen_deadline)
        self.assertIn(
            "true, command_budget_ms, 1u, collection_delay_ms",
            route[command_cap:frozen_deadline],
        )
        self.assertNotIn("if (budget_explicit)", route[validation:frozen_deadline])

    def test_pair_control_uses_route_specific_request_and_result_deadlines(self) -> None:
        natural_timeout = function_body(
            SURVEY, "gateway_survey_natural_request_timeout_ms"
        )
        request_timeout = function_body(
            SURVEY, "gateway_survey_request_timeout_ms"
        )
        transaction_timeout = function_body(
            SURVEY, "gateway_survey_transaction_timeout_ms"
        )
        send = function_body(SURVEY, "gateway_survey_send_outbound")
        arm = function_body(SURVEY, "gateway_survey_arm_control_transaction")

        self.assertIn(
            "gateway_survey_natural_request_timeout_ms(target_id)",
            request_timeout,
        )
        self.assertNotIn(
            "SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS", request_timeout
        )
        self.assertIn(
            "gateway_survey_natural_request_timeout_ms(target_id)",
            transaction_timeout,
        )
        self.assertIn(
            "SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS", natural_timeout
        )
        self.assertNotIn(
            "SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS", transaction_timeout
        )
        self.assertIn(
            "request_timeout_ms + request_timeout_ms", transaction_timeout
        )

        request_deadline = send.index("request_deadline_ms =")
        submit = send.index("gateway_survey_send_pair_control(")
        submit_request_deadline = send.index("request_deadline_ms", submit)
        arm_call = send.index("gateway_survey_arm_control_transaction(")
        semantic_deadline = arm.index("transaction_deadline_ms =")
        transaction = arm.index("survey_gateway_transaction_begin(")
        transaction_semantic_deadline = arm.index(
            "transaction_deadline_ms", transaction
        )
        result_wait = arm.index(
            "gateway_begin_command_result_wait_until(", transaction
        )
        result_semantic_deadline = arm.index(
            "(uint32_t)transaction_deadline_ms", result_wait
        )

        self.assertLess(request_deadline, submit)
        self.assertLess(submit, submit_request_deadline)
        self.assertLess(submit_request_deadline, arm_call)
        self.assertLess(semantic_deadline, transaction)
        self.assertLess(transaction, transaction_semantic_deadline)
        self.assertLess(transaction_semantic_deadline, result_wait)
        self.assertLess(result_wait, result_semantic_deadline)

    def test_manual_pair_control_uses_route_specific_round_trip_budget(
        self,
    ) -> None:
        route = function_body(CONTROL, "gateway_route_survey_pair_control")

        self.assertEqual(
            route.count("gateway_survey_natural_request_timeout_ms("), 2
        )
        self.assertEqual(
            route.count("uint32_t result_timeout_ms = request_timeout_ms"), 2
        )
        self.assertEqual(
            route.count("request_timeout_ms + result_timeout_ms"), 2
        )
        self.assertNotIn("command_id == CMD_SURVEY_ABORT ?", route)
        self.assertNotIn("SURVEY_PAIR_ABORT_RESULT_TIMEOUT_MS", route)
        self.assertNotIn("SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS", route)

        request = route.index("gateway_survey_natural_request_timeout_ms(")
        result = route.index(
            "uint32_t result_timeout_ms = request_timeout_ms", request
        )
        total = route.index("request_timeout_ms + result_timeout_ms", result)
        request_deadline = route.index("request_deadline_ms =", total)
        transaction_deadline = route.index("transaction_deadline_ms =", request_deadline)
        self.assertLess(request, result)
        self.assertLess(result, total)
        self.assertLess(total, request_deadline)
        self.assertLess(request_deadline, transaction_deadline)

    def test_ordinary_targeted_command_reserves_request_plus_response(self) -> None:
        route = function_body(CONTROL, "gateway_route_mesh_host_packet")

        legacy = route.index(
            "tracking_mode == GATEWAY_COMMAND_TRACK_LEGACY_RESULT"
        )
        waiter = route.index("gateway_begin_command_result_wait_for(", legacy)
        round_trip = route.index(
            "GATEWAY_COMMAND_RESULT_ROUND_TRIP_TIMEOUT_MS", waiter
        )
        submit = route.index(
            "app_node_comm_submit_protocol_response(", round_trip
        )
        request = route.index("GATEWAY_COMMAND_RESULT_TIMEOUT_MS", submit)

        self.assertLess(legacy, waiter)
        self.assertLess(waiter, round_trip)
        self.assertLess(round_trip, submit)
        self.assertLess(submit, request)
        self.assertNotIn(
            "GATEWAY_COMMAND_RESULT_ROUND_TRIP_TIMEOUT_MS",
            route[submit:],
        )

    def test_discovery_ack_confirm_is_proven_by_gateway_history(self) -> None:
        accept = function_body(
            COORDINATION, "mesh_gateway_accept_semantic_delivery"
        )
        branch = accept[
            accept.index("case MSG_GATEWAY_ACK_CONFIRM:") :
            accept.index("case MSG_CLICK_REPORT:")
        ]
        history = branch.index(
            "mesh_relay_gateway_ack_confirm_history_match("
        )
        reject = branch.index("if (ret != PROTO_OK)", history)
        note = branch.index("mesh_report_gateway_note_ack_confirm(", reject)
        miss = branch[reject:note]

        self.assertLess(history, reject)
        self.assertLess(reject, note)
        self.assertIn("mesh_gateway_ack_confirm_payload_parse(", miss)
        self.assertIn("APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE", miss)
        self.assertIn("return -EINVAL", miss)

    def test_discovery_ack_confirm_matches_exact_live_report_owner(self) -> None:
        note = function_body(SURVEY, "gateway_note_survey_ack_confirm")

        for exact_field in (
            "identity->msg_type != MSG_SURVEY_DISCOVERY_REPORT",
            "identity->flags !=",
            "identity->session_id != expected_session",
            "gateway_survey_context.node_ids[i] != confirm_packet->src_id",
            "gateway_survey_context.reports[i].metadata == UINT8_MAX",
        ):
            self.assertIn(exact_field, note)
        self.assertIn(
            "gateway_survey_discovery_ack_confirm_mask |= UINT64_C(1) << i",
            note,
        )
        self.assertIn("gateway_survey_work_schedule(", note)
        self.assertIn("SURVEY_GATEWAY_DUE_BOUNDARY_POLL, 0u", note)

    def test_discovery_confirmation_only_proves_reports_settled(
        self,
    ) -> None:
        note = function_body(SURVEY, "gateway_note_survey_ack_confirm")
        admission = function_body(
            SURVEY, "gateway_handle_survey_discovery_report"
        )
        wait = function_body(
            CONTROL, "gateway_survey_wait_for_discovery_collection"
        )

        self.assertIn(
            "gateway_survey_discovery_ack_confirm_mask |= UINT64_C(1) << i",
            note,
        )
        self.assertNotIn("ack_confirms_complete", wait)
        self.assertNotIn("ACK_BARRIER", wait)
        self.assertNotIn(
            "gateway_survey_discovery_ack_confirm_mask", admission
        )

        decision = wait.index("survey_gateway_collection_decide(")
        decision_end = wait.index(");", decision)
        decision_call = wait[decision:decision_end]
        self.assertIn(
            "gateway_survey_discovery_ack_confirm_mask", decision_call
        )
        self.assertIn("gateway_survey_context.report_count", decision_call)
        self.assertNotIn(
            "gateway_survey_discovery_ack_confirm_mask", wait[:decision]
        )
        self.assertNotIn(
            "gateway_survey_discovery_ack_confirm_mask",
            wait[decision_end:],
        )

    def test_only_successful_responder_start_skips_ack_confirm_barrier(
        self,
    ) -> None:
        result = function_body(
            ROUND, "gateway_survey_round_note_control_result"
        )
        confirmation = function_body(
            ROUND, "gateway_survey_round_apply_control_confirmation"
        )

        self.assertIn(
            "app_gateway_survey_round_capture_control_result(", result
        )
        fast_path = result.index(
            "control.stage == APP_GATEWAY_SURVEY_CONTROL_START_RESPONDER"
        )
        fast_end = result.index("return true", fast_path)
        fast = result[fast_path:fast_end]
        self.assertIn("status == COMMAND_OK", fast)
        self.assertIn("!ack_confirm_already_received", fast)
        self.assertIn(
            "app_gateway_survey_round_clear_control_confirmation(", fast
        )
        self.assertIn("app_gateway_survey_round_note_control_success(", fast)
        self.assertEqual(
            result.count("app_gateway_survey_round_note_control_success("),
            1,
        )
        self.assertIn(
            "app_gateway_survey_round_control_confirmation_ready(",
            confirmation,
        )
        self.assertIn(
            "app_gateway_survey_round_note_control_success(", confirmation
        )

    def test_collection_closes_from_accepted_reports_after_frozen_window(
        self,
    ) -> None:
        wait = function_body(
            CONTROL, "gateway_survey_wait_for_discovery_collection"
        )

        decision = wait.index("survey_gateway_collection_decide(")
        wait_branch = wait.index(
            "if (decision == SURVEY_GATEWAY_COLLECTION_WAIT)", decision
        )
        adaptive_choice = wait.index(
            "now_ms, receive_deadline_ms", wait_branch
        )
        wait_schedule = wait.index(
            "SURVEY_GATEWAY_DUE_BOUNDARY_POLL, wait_ms", adaptive_choice
        )
        collection_clear = wait.index(
            "gateway_survey_collection_pending = false", wait_schedule
        )

        self.assertIn(
            "receive_deadline_ms",
            wait[adaptive_choice:wait_schedule],
        )
        self.assertLess(wait_schedule, collection_clear)
        decision_call = wait[decision:wait.index(");", decision)]
        self.assertIn(
            "gateway_survey_discovery_ack_confirm_mask", decision_call
        )
        self.assertNotIn(
            "gateway_survey_discovery_ack_confirm_mask",
            wait[wait_branch:],
            "ACK confirmation may settle complete reports but must not grant "
            "the ordinary emission or safety horizon",
        )

    def test_assignment_claim_advances_but_table_commit_requires_confirm(
        self,
    ) -> None:
        confirm = function_body(
            CONTROL,
            "gateway_discovery_assignment_note_ack_confirm",
        )
        publish = function_body(
            CONTROL, "gateway_discovery_assignment_publish_work_handler"
        )
        table = function_body(
            CONTROL, "gateway_discovery_assignment_publish_table"
        )
        complete = function_body(
            CONTROL, "gateway_discovery_assignment_complete_success_locked"
        )

        for exact_guard in (
            "identity->msg_type != MSG_COMMAND_RESULT",
            "GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS",
            "gateway_discovery_assignment_state.table_command_seq",
            "gateway_discovery_assignment_state.claim_response_mask",
            "gateway_discovery_assignment_state.ack_mask",
        ):
            self.assertIn(exact_guard, confirm)
        self.assertIn(
            "gateway_discovery_assignment_state.confirmation_mask |= bit",
            confirm,
        )
        self.assertIn("gateway_discovery_assignment_wake_now(", confirm)

        table_publish = publish.index(
            "gateway_discovery_assignment_publish_table()"
        )
        self.assertNotIn("claim-to-table", publish)
        self.assertNotIn(
            "gateway_discovery_assignment_state.claim_command_seq",
            publish[:table_publish],
        )

        durable_prepare = table.index(
            "gateway_discovery_assignment_prepare_durable_table_locked()"
        )
        table_send = table.index(
            "gateway_discovery_assignment_submit_control_flood_locked(",
            durable_prepare,
        )
        self.assertLess(durable_prepare, table_send)
        self.assertIn(
            "gateway_discovery_assignment_missing_confirmation_count_locked() !=",
            complete,
        )
        self.assertIn(
            "app_gateway_assignment_publisher_commit_prepared_batch",
            complete,
        )

    def test_table_dispatch_cannot_publish_a_precommit_completion_head(
        self,
    ) -> None:
        table = function_body(
            CONTROL, "gateway_discovery_assignment_publish_table"
        )
        complete = function_body(
            CONTROL, "gateway_discovery_assignment_complete_success_locked"
        )

        self.assertNotIn(
            "GATEWAY_COMMAND_EVENT_STAGE_ENUMERATION_COMPLETE",
            table,
            "a generic ACK-required completion event can be host-accepted "
            "while the durable publisher is only prepared, leaving the BLE "
            "head unretirable and stranding every post-END record behind it",
        )
        self.assertNotIn("gateway_observe_command_event", table)
        mapping_commit = complete.index(
            "app_gateway_assignment_publisher_commit_prepared_batch"
        )
        table_ready = complete.index(
            "app_gateway_assignment_publisher_stage_table_ready"
        )
        terminal_capture = complete.index(
            "gateway_observe_command_event(&event, true)"
        )
        self.assertLess(mapping_commit, table_ready)
        self.assertLess(table_ready, terminal_capture)

    def test_assignment_table_round_owner_survives_ack_confirm_barrier(
        self,
    ) -> None:
        finalize = function_body(
            CONTROL, "gateway_discovery_assignment_finalize_work_handler"
        )
        complete = function_body(
            CONTROL, "gateway_discovery_assignment_complete_success_locked"
        )
        quorum = finalize.index(
            "gateway_discovery_assignment_state.stage ==\n"
            "            GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS"
        )
        expiry = finalize.index(
            "if (app_discovery_assignment_operation_expired(", quorum
        )
        fast_complete = finalize[quorum:expiry]
        completion_call = fast_complete.index(
            "gateway_discovery_assignment_publish_end()"
        )

        self.assertNotIn(
            "gateway_discovery_assignment_state.round_open = false",
            fast_complete[:fast_complete.index(
                "gateway_discovery_assignment_state.response_ack_settle_armed = false"
            )],
            "the table-round owner must survive a pending ACK_CONFIRM barrier",
        )
        self.assertIn(
            "gateway_discovery_assignment_state.round_open = false",
            fast_complete[:completion_call],
            "the exact END must take ownership after the ACK_CONFIRM barrier",
        )
        table_barrier = complete.index(
            "gateway_discovery_assignment_missing_confirmation_count_locked()"
        )
        owner_release = complete.index(
            "gateway_discovery_assignment_state.round_open = false",
            table_barrier,
        )
        self.assertLess(table_barrier, owner_release)

    def test_directed_fifo_holds_exact_head_without_spending_send_retries(
        self,
    ) -> None:
        worker = function_body(CONTROL, "gateway_host_command_work_handler")
        peek = worker.index("k_msgq_peek(&gateway_host_command_msgq, &item)")
        barrier = worker.index(
            "app_mesh_report_gateway_origin_confirmation_pending("
        )
        owner = worker.index(
            "gateway_host_command_ack_confirm_wait_admission_id", barrier
        )
        deadline = worker.index("GATEWAY_COMMAND_BUDGET_MAX_MS", owner)
        pending = worker.index("!uptime_deadline_reached(", deadline)
        poll = worker.index(
            "GATEWAY_HOST_COMMAND_ACK_CONFIRM_POLL_MS", pending
        )
        reschedule = worker.index("gateway_host_command_retry_work", poll)
        pending_return = worker.index("return", reschedule)
        dequeue = worker.index("k_msgq_get(&gateway_host_command_msgq", pending_return)
        terminal = worker.index("COMMAND_TIMEOUT", dequeue)
        lifecycle = worker.index(
            "gateway_host_command_lifecycle_begin_locked", terminal
        )
        send = worker.index("gateway_route_host_packet(", lifecycle)

        self.assertIn("mesh_id_is_unicast(item.packet.dst_id)", worker[:barrier])
        self.assertIn("item.packet.dst_id != DEVICE_ID", worker[:barrier])
        self.assertLess(peek, barrier)
        self.assertLess(barrier, owner)
        self.assertLess(owner, deadline)
        self.assertLess(deadline, pending)
        self.assertLess(pending, poll)
        self.assertLess(poll, reschedule)
        self.assertLess(reschedule, pending_return)
        self.assertLess(pending_return, dequeue)
        self.assertLess(dequeue, terminal)
        self.assertLess(terminal, lifecycle)
        self.assertLess(lifecycle, send)
        self.assertNotIn("ret = -EAGAIN", worker[:lifecycle])
        self.assertNotIn("gateway_host_command_retry_round", worker[:lifecycle])

    def test_exact_confirm_wakes_fifo_assignment_and_survey_barriers(self) -> None:
        callback = function_body(SURVEY, "gateway_note_survey_ack_confirm")
        fifo = callback.index("gateway_host_command_retry_pending")
        fifo_wake = callback.index("gateway_host_command_retry_work", fifo)
        assignment = callback.index(
            "gateway_discovery_assignment_note_ack_confirm(", fifo_wake
        )
        survey = callback.index("gateway_survey_active", assignment)
        survey_wake = callback.index(
            "SURVEY_GATEWAY_DUE_CONTROL_DELIVERY, 0u", survey
        )

        self.assertIn("K_NO_WAIT", callback[fifo:fifo_wake + 100])
        self.assertLess(fifo, fifo_wake)
        self.assertLess(fifo_wake, assignment)
        self.assertLess(assignment, survey)
        self.assertLess(survey, survey_wake)

    def test_confirmation_callback_replaces_each_delayed_owner_deadline(self) -> None:
        callback = function_body(SURVEY, "gateway_note_survey_ack_confirm")
        fifo_start = callback.index("gateway_host_command_retry_pending")
        assignment_start = callback.index(
            "gateway_discovery_assignment_note_ack_confirm(", fifo_start
        )
        survey_start = callback.index("gateway_survey_active", assignment_start)
        fifo = callback[fifo_start:assignment_start]
        assignment = callback[assignment_start:survey_start]
        survey = callback[survey_start:]
        assignment_confirm = function_body(
            CONTROL, "gateway_discovery_assignment_note_ack_confirm"
        )
        assignment_wake = function_body(
            CONTROL, "gateway_discovery_assignment_wake_now"
        )

        self.assertRegex(
            fifo,
            r"k_work_reschedule\s*\(\s*"
            r"&gateway_host_command_retry_work\s*,\s*K_NO_WAIT\s*\)",
        )
        self.assertIn(
            "gateway_discovery_assignment_note_ack_confirm(confirm_packet,",
            assignment,
        )
        self.assertIn('gateway_discovery_assignment_wake_now(', assignment_confirm)
        self.assertIn('"ack-confirm"', assignment_confirm)
        self.assertIn("k_work_cancel_delayable(", assignment_wake)
        self.assertRegex(
            assignment_wake,
            r"gateway_discovery_assignment_reschedule\s*\(\s*"
            r"K_NO_WAIT\s*,\s*source\s*\)",
        )
        self.assertIn("gateway_survey_work_schedule(", survey)
        self.assertIn("SURVEY_GATEWAY_DUE_CONTROL_DELIVERY, 0u", survey)
        for owner_slice in (
            fifo,
            assignment,
            assignment_confirm,
            assignment_wake,
            survey,
        ):
            self.assertNotIn("mesh_gateway_command_priority_submit", owner_slice)
            self.assertNotIn("k_work_submit(", owner_slice)
            self.assertNotIn("k_work_submit_to_queue(", owner_slice)

    def test_confirmation_wakes_are_isolated_to_matching_owners(self) -> None:
        callback = function_body(SURVEY, "gateway_note_survey_ack_confirm")
        assignment = function_body(
            CONTROL, "gateway_discovery_assignment_note_ack_confirm"
        )
        assignment_start = callback.index(
            "gateway_discovery_assignment_note_ack_confirm("
        )
        assignment_end = callback.index(
            "if (!gateway_survey_active)", assignment_start
        )
        survey_start = callback.index("if (!gateway_survey_active)", assignment_end)
        proof_start = callback.index(
            "struct app_gateway_survey_round_ack_confirm confirm", survey_start
        )
        digest_copy = callback.index(
            "memcpy(confirm.semantic_digest,", proof_start
        )
        proof_note = callback.index(
            "app_gateway_survey_round_note_control_ack_confirm(", digest_copy
        )
        proof_success = callback.index("if (ret == PROTO_OK)", proof_note)
        survey_wake = callback.index(
            "SURVEY_GATEWAY_DUE_CONTROL_DELIVERY, 0u", proof_success
        )
        proof = callback[proof_start:proof_note]

        self.assertIn("identity->msg_type != MSG_COMMAND_RESULT", assignment)
        self.assertIn("identity->session_id !=", assignment)
        self.assertIn("table_command_seq", assignment)
        self.assertIn("confirm_packet->src_id", assignment)
        self.assertNotIn(
            "gateway_survey_work_schedule",
            callback[assignment_start:assignment_end],
        )
        self.assertNotIn(
            "gateway_host_command_retry_work",
            callback[assignment_start:assignment_end],
        )
        for exact_field in (
            ".source_id = confirm_packet->src_id",
            ".destination_id = confirm_packet->dst_id",
            ".session_id = identity->session_id",
            ".seq = identity->seq",
            ".msg_type = identity->msg_type",
        ):
            self.assertIn(exact_field, proof)
        self.assertNotIn("gateway_survey_work_schedule", callback[survey_start:proof_start])
        self.assertIn("identity->digest", callback[digest_copy:proof_note])
        self.assertLess(proof_start, digest_copy)
        self.assertLess(digest_copy, proof_note)
        self.assertLess(proof_note, proof_success)
        self.assertLess(proof_success, survey_wake)
        self.assertIn("return 0", callback[survey_wake:])

    def test_node_comm_confirmation_recomputes_and_reschedules_due_owner(self) -> None:
        confirm = function_body(
            NODE_COMM, "app_node_comm_note_gateway_confirmed_internal"
        )
        schedule = function_body(
            NODE_COMM, "app_node_comm_schedule_delivery_locked"
        )
        confirm_policy = confirm.index("app_node_comm_service_policy_locked(")
        confirm_wake = confirm.index(
            "app_node_comm_retain_delivery_schedule_locked(", confirm_policy
        )

        self.assertLess(confirm_policy, confirm_wake)
        self.assertIn(
            "k_work_reschedule(&node_comm_delivery_due_kick_work", schedule
        )
        self.assertIn(
            "mesh_route_work_reschedule(&node_comm_delivery_work", schedule
        )
        self.assertNotIn("k_work_submit(", schedule)
        self.assertNotIn("k_work_submit_to_queue(", schedule)

    def test_route_and_event_wakeups_replace_delayed_deadlines(self) -> None:
        route = function_body(
            COORDINATION, "mesh_schedule_route_waiting_retry_after"
        )
        event = function_body(
            EVENT_TX, "mesh_event_negotiation_schedule_next"
        )

        self.assertIn(
            "mesh_reschedule_owned_work(&mesh_route_waiting_work", route
        )
        self.assertIn("delay_ms", route)
        self.assertIn(
            "mesh_reschedule_owned_work(&mesh_event_negotiation_retry_work",
            event,
        )
        self.assertIn("delay_ms", event)
        for wake in (route, event):
            self.assertNotIn("mesh_submit_owned_work", wake)
            self.assertNotIn("k_work_submit(", wake)
            self.assertNotIn("k_work_submit_to_queue(", wake)

    def test_route_depth_scan_is_bounded_by_downlink_capacity(self) -> None:
        body = function_body(SURVEY, "gateway_survey_known_max_report_hops")
        self.assertIn("mesh_relay_downlink_capacity(&mesh_runtime)", body)
        self.assertIn("mesh_relay_downlink_at(&mesh_runtime", body)
        self.assertIn("route->hop_count > max_hops", body)

    def test_completed_enumeration_depth_requires_exact_current_proof(self) -> None:
        proof = function_body(
            SURVEY,
            "gateway_survey_completed_enumeration_max_report_hops",
        )
        route = function_body(SURVEY, "gateway_route_survey_reachability")

        for guard in (
            "gateway_discovery_assignment_state.active",
            "gateway_discovery_assignment_state.replay",
            "gateway_discovery_assignment_state.claim_count != roster_count",
            "gateway_discovery_assignment_state.epoch != assignment_epoch",
            "gateway_discovery_assignment_state.table_command_seq !=",
            "discovery_assignment_table_commitment_equal(",
            "gateway_discovery_assignment_state.claim_response_mask !=",
            "gateway_discovery_assignment_state.ack_mask & complete_mask",
            "gateway_discovery_assignment_state.confirmation_mask &",
            "hop_count == 0u",
            "hop_count > DISCOVERY_ASSIGNMENT_MAX_HOPS",
        ):
            self.assertIn(guard, proof)
        self.assertIn("roster_ids[roster_index]", proof)
        self.assertIn("roster_slots[roster_index]", proof)

        enum_lookup = route.index(
            "gateway_survey_completed_enumeration_max_report_hops("
        )
        cache_lookup = route.index("gateway_survey_known_max_report_hops()")
        fallback = route.index("if (max_report_hops == 0u)")
        self.assertLess(enum_lookup, cache_lookup)
        self.assertLess(cache_lookup, fallback)

    def test_cold_roster_depth_is_inferred_from_the_admitted_policy(self) -> None:
        infer = function_body(
            SURVEY, "gateway_survey_policy_max_report_hops"
        )
        route = function_body(SURVEY, "gateway_route_survey_reachability")

        for policy_field in (
            ".start_delay_ms = config->start_delay_ms",
            ".slot_ms = config->slot_ms",
            ".slot_count = config->slot_count",
            ".round_count = config->round_count",
            ".report_grace_ms = report_grace_ms",
            ".operation_budget_ms = operation_budget_ms",
        ):
            self.assertIn(policy_field, infer)
        self.assertIn(
            "candidate_limit = MIN(candidate_limit, "
            "(uint8_t)SURVEY_DEFAULT_TTL)",
            infer,
        )
        self.assertIn(
            "for (uint8_t hop_count = 1u; "
            "hop_count <= candidate_limit; hop_count++)",
            infer,
        )
        custody = infer.index(
            "survey_discovery_report_custody_ms(hop_count)"
        )
        candidate = infer.index("terms.max_hop_count = hop_count", custody)
        required = infer.index(
            "operation_policy_discovery_required_budget_ms(", candidate
        )
        covered = infer.index(
            "required_budget_ms > operation_budget_ms", required
        )
        remember = infer.index("max_hops = hop_count", covered)
        self.assertLess(custody, candidate)
        self.assertLess(candidate, required)
        self.assertLess(required, covered)
        self.assertLess(covered, remember)

        fallback = route.index("if (max_report_hops == 0u)")
        infer_call = route.index(
            "gateway_survey_policy_max_report_hops(", fallback
        )
        custody_use = route.index(
            "survey_discovery_report_custody_ms(max_report_hops)", infer_call
        )
        report_timing = route.index(
            "survey_discovery_report_delay_ms(&config", custody_use
        )
        call = route[infer_call:custody_use]
        self.assertIn("&config", call)
        self.assertIn("duration_ms", call)
        self.assertIn(
            "policy_candidate.resolved.discovery.operation_budget_ms", call
        )
        self.assertIn("assigned_roster_count", call)
        self.assertIn("SURVEY_DEFAULT_TTL", call)
        self.assertLess(fallback, infer_call)
        self.assertLess(infer_call, custody_use)
        self.assertLess(custody_use, report_timing)
        self.assertIsNone(
            re.search(
                r"max_report_hops\s*=\s*\(uint8_t\)\s*MIN\s*\(\s*"
                r"assigned_roster_count",
                route,
            ),
            "a cold roster count is a candidate limit, never a hop-depth proof",
        )

    def test_start_delay_validation_receives_the_resolved_durable_depth(self) -> None:
        route = function_body(SURVEY, "gateway_route_survey_reachability")

        prepare = route.index("app_operation_policy_prepare_payload(")
        prepare_end = route.index("&policy_candidate);", prepare)
        policy_admission = route[prepare:prepare_end]
        self.assertIn("APP_OPERATION_POLICY_DISCOVERY_MASK", policy_admission)
        self.assertIn("APP_OPERATION_POLICY_PAIR_MASK", policy_admission)

        durable_topology = route.index(
            "gateway_survey_completed_enumeration_max_report_hops("
        )
        fallback = route.index("if (max_report_hops == 0u)", durable_topology)
        custody = route.index(
            "survey_discovery_report_custody_ms(max_report_hops)", fallback
        )
        report_timing = route.index(
            "survey_discovery_report_delay_ms(&config", custody
        )
        admitted = route.index(
            "admitted_discovery = (struct operation_policy_discovery)",
            report_timing,
        )
        terms = route.index(
            ".max_hop_count = max_report_hops", admitted
        )
        full_validation = route.index(
            "operation_policy_discovery_required_budget_ms(", terms
        )
        validation_end = route.index(
            "required_budget_ms < collection_delay_ms", full_validation
        )
        owner_claim = route.index(
            "gateway_operation_owner_claim(", validation_end
        )

        self.assertLess(durable_topology, fallback)
        self.assertLess(fallback, custody)
        self.assertLess(fallback, report_timing)
        self.assertLess(report_timing, admitted)
        self.assertLess(admitted, terms)
        self.assertLess(terms, full_validation)
        self.assertLess(full_validation, validation_end)
        self.assertLess(validation_end, owner_claim)
        self.assertIn(
            "&admitted_discovery, &budget_terms, &required_budget_ms",
            route[full_validation:validation_end],
        )
        self.assertIn(
            "gateway_reject_survey_request(",
            route[validation_end:owner_claim],
        )
        self.assertIn(
            "COMMAND_MALFORMED_PAYLOAD",
            route[validation_end:owner_claim],
        )

    def test_collection_uses_hop_scaled_custody(self) -> None:
        body = function_body(SURVEY, "gateway_route_survey_reachability")
        lookup = body.index("gateway_survey_known_max_report_hops()")
        scale = body.index("survey_discovery_report_custody_ms(")
        deadline = body.index("collection_delay_ms =", lookup)
        self.assertLess(lookup, scale)
        self.assertLess(lookup, deadline)
        self.assertIn("report_custody_ms +", body[deadline:])
        self.assertNotIn(
            "SURVEY_DISCOVERY_REPORT_CUSTODY_TIMEOUT_MS +",
            body[deadline:],
        )

    def test_expected_count_is_bound_to_the_gateway_collection_policy(self) -> None:
        route = function_body(SURVEY, "gateway_route_survey_reachability")
        wait = function_body(
            CONTROL, "gateway_survey_wait_for_discovery_collection"
        )

        self.assertIn("survey_extract_expected_node_count_tlv(", route)
        self.assertIn(
            "gateway_survey_collection_emission_deadline_ms", route
        )
        self.assertIn("gateway_survey_expected_node_count =", route)
        self.assertIn(
            "gateway_survey_expected_node_count_present =", route
        )

        decision = wait.index("survey_gateway_collection_decide(")
        for policy_input in (
            "receive_deadline_ms",
            "gateway_survey_context.report_count",
            "gateway_survey_expected_node_count",
            "gateway_survey_expected_node_count_present",
        ):
            self.assertIn(policy_input, wait[decision:])
        mismatch = wait.index(
            "SURVEY_GATEWAY_COLLECTION_COUNT_MISMATCH", decision
        )
        self.assertIn("gateway_survey_finish_status(", wait[mismatch:])
        self.assertIn("DBG_SURVEY_COLLECTION_PARTIAL", wait[mismatch:])
        self.assertIn("return false;", wait[mismatch:])
        self.assertIsNone(
            re.search(r"gateway_survey_context\.report_count\s*=(?!=)", wait),
            "partial continuation must never fabricate or truncate reports",
        )

    def test_collection_deadline_advances_one_observed_depth_at_a_time(self) -> None:
        route = function_body(SURVEY, "gateway_route_survey_reachability")
        refresh = function_body(
            SURVEY, "gateway_survey_refresh_adaptive_collection_deadline"
        )
        report = function_body(
            SURVEY, "gateway_handle_survey_discovery_report"
        )
        wait = function_body(
            CONTROL, "gateway_survey_wait_for_discovery_collection"
        )

        self.assertIn(
            "gateway_survey_adaptive_depth_wait_ms(1u)", route
        )
        self.assertIn(
            "gateway_survey_adaptive_observed_hop_count + 1u", refresh
        )
        self.assertIn(
            "gateway_survey_adaptive_max_hop_count", refresh
        )
        self.assertIn(
            "gateway_survey_collection_deadline_ms", refresh
        )
        self.assertIn(
            "gateway_survey_refresh_adaptive_collection_deadline(", report
        )
        self.assertIn(
            "receive_deadline_ms = gateway_survey_collection_receive_deadline_ms()",
            wait,
        )

    def test_late_valid_discovery_report_retires_transport_without_mutation(self) -> None:
        preflight = function_body(
            SURVEY, "gateway_survey_preflight_discovery_report"
        )
        report = function_body(
            SURVEY, "gateway_handle_survey_discovery_report"
        )
        closed = preflight.index("if (!gateway_survey_collection_pending)")
        deadline = preflight.index(
            "gateway_survey_collection_receive_deadline_ms()"
        )

        self.assertLess(closed, deadline)
        self.assertIn(
            "return APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;",
            preflight[closed:deadline],
        )
        self.assertIn(
            "if (ret == APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE)",
            report,
        )

    def test_collection_safety_deadline_is_frozen_at_command_origin(self) -> None:
        route = function_body(SURVEY, "gateway_route_survey_reachability")
        wait = function_body(
            CONTROL, "gateway_survey_wait_for_discovery_collection"
        )

        self.assertIn(
            "gateway_survey_operation_deadline_ms = "
            "command_origin_ms + command_budget_ms",
            " ".join(route.split()),
        )
        self.assertIn(
            "gateway_survey_collection_deadline_ms = "
            "command_origin_ms + collection_delay_ms",
            " ".join(route.split()),
        )
        self.assertIn(
            ".terminal_scheduling_guard_ms = "
            "GATEWAY_SURVEY_OPERATION_TERMINAL_SCHEDULING_GUARD_MS",
            " ".join(route.split()),
        )
        self.assertIn(
            "gateway_survey_collection_window_armed = true", wait
        )
        self.assertNotIn(
            "k_uptime_get_32() + gateway_survey_collection_duration_ms",
            wait,
        )

    def test_post_rf_terminal_keeps_delayed_report_horizon_alive(self) -> None:
        wait = function_body(
            CONTROL, "gateway_survey_wait_for_discovery_collection"
        )
        prior_delivery = re.search(
            r"prior_delivery_completed\s*=\s*"
            r"gateway_survey_discovery_redrive_count\s*>\s*0u\s*;",
            wait,
        )
        survival = re.search(
            r"survey_gateway_discovery_collection_survives_terminal\s*\("
            r"\s*event\.reason\s*==\s*NODE_COMM_TERMINAL_DELIVERED\s*"
            r"\|\|\s*prior_delivery_completed\s*,"
            r"\s*event\.attempts_started\s*\)",
            wait,
        )

        self.assertIsNotNone(prior_delivery)
        self.assertIsNotNone(survival)
        take_event = wait.index("app_node_comm_take_delivery_event_for(")
        first_clear = wait.index(
            "gateway_survey_discovery_delivery_handle = 0u;",
            prior_delivery.end(),
        )
        redrive_clear = wait.index(
            "gateway_survey_discovery_redrive_count = 0u;",
            prior_delivery.end(),
        )
        self.assertLess(take_event, prior_delivery.start())
        self.assertLess(prior_delivery.end(), first_clear)
        self.assertLess(prior_delivery.end(), redrive_clear)
        self.assertLess(redrive_clear, survival.start())
        self.assertLess(
            survival.start(),
            wait.index("gateway_survey_finish_status(", survival.start()),
        )
        self.assertIn(
            "gateway_survey_collection_window_armed = true",
            wait[survival.start() :],
        )

    def test_pair_plan_control_floor_is_checked_before_remote_state(self) -> None:
        worker = function_body(CONTROL, "gateway_survey_work_handler")
        plan = worker.index("survey_gateway_plan_pairs(")
        remaining = worker.index("uptime_ms_until_deadline(", plan)
        budget = worker.index(
            "survey_gateway_transaction_pair_plan_fits_minimum_budget(",
            remaining,
        )
        terminal = worker.index("gateway_survey_finish_status(", budget)
        telemetry = worker.index("gateway_survey_emit_collection_telemetry()")
        round_drive = worker.index("gateway_survey_round_drive(", telemetry)

        self.assertLess(plan, remaining)
        self.assertLess(remaining, budget)
        self.assertLess(budget, terminal)
        self.assertLess(terminal, telemetry)
        self.assertLess(telemetry, round_drive)
        self.assertIn("SURVEY_GATEWAY_PAIR_MINIMUM_CONTROL_MS", worker[budget:])

    def test_shared_worker_keeps_the_earliest_named_due_owner(self) -> None:
        schedule = function_body(SURVEY, "gateway_survey_work_schedule")
        handler = function_body(CONTROL, "gateway_survey_work_handler")
        round_drive = function_body(ROUND, "gateway_survey_round_drive")
        drive = function_body(SURVEY, "gateway_survey_schedule_drive")

        self.assertIn("survey_gateway_due_registry_schedule_after(", schedule)
        self.assertIn("gateway_survey_work_arm", schedule)
        self.assertIn("SURVEY_GATEWAY_DUE_ROUND_OBSERVATION", round_drive)
        self.assertIn("SURVEY_GATEWAY_DUE_BOUNDARY_POLL", drive)
        consume = handler.index("gateway_survey_work_consume_due()")
        schedule_drive = handler.index("gateway_survey_schedule_drive()")
        rearm = handler.index(
            "gateway_survey_work_rearm_due()", schedule_drive
        )
        self.assertLess(consume, schedule_drive)
        self.assertLess(schedule_drive, rearm)

    def test_stale_shared_callback_is_rearmed_without_cancel_or_watchdog_fault(self) -> None:
        cancel = function_body(SURVEY, "gateway_survey_work_cancel_owner")
        reset = function_body(SURVEY, "gateway_survey_work_reset_schedule")
        consume = function_body(SURVEY, "gateway_survey_work_consume_due")
        handler = function_body(CONTROL, "gateway_survey_work_handler")

        for registry_only_mutation in (cancel, reset):
            self.assertNotIn(
                "k_work_cancel_delayable(&gateway_survey_work)",
                registry_only_mutation,
            )
            self.assertNotIn("app_watchdog_stop_feeding()", registry_only_mutation)

        self.assertIn("survey_gateway_due_registry_consume_due(", consume)
        self.assertRegex(consume, r"return\s+[A-Za-z_]\w*\s*;")
        stale_callback = re.search(
            r"if\s*\(\s*!gateway_survey_work_consume_due\(\)\s*\)\s*\{"
            r"(?P<body>.*?)\}",
            handler,
            re.S,
        )
        self.assertIsNotNone(stale_callback)
        stale_body = stale_callback.group("body")
        self.assertIn("gateway_survey_work_rearm_due()", stale_body)
        self.assertIn("return;", stale_body)
        self.assertNotIn("app_watchdog_stop_feeding()", stale_body)


if __name__ == "__main__":
    unittest.main()
