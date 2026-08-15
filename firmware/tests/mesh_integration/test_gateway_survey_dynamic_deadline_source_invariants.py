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

    def test_pair_control_separates_request_and_semantic_deadlines(self) -> None:
        request_timeout = function_body(
            SURVEY, "gateway_survey_request_timeout_ms"
        )
        transaction_timeout = function_body(
            SURVEY, "gateway_survey_transaction_timeout_ms"
        )
        send = function_body(SURVEY, "gateway_survey_send_outbound")

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
            "SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS", transaction_timeout
        )

        request_deadline = send.index("request_deadline_ms =")
        semantic_deadline = send.index("transaction_deadline_ms =")
        submit = send.index("gateway_survey_send_pair_control(")
        submit_request_deadline = send.index("request_deadline_ms", submit)
        transaction = send.index("survey_gateway_transaction_begin(", submit)
        transaction_semantic_deadline = send.index(
            "transaction_deadline_ms", transaction
        )
        result_wait = send.index(
            "gateway_begin_command_result_wait_until(", transaction
        )
        result_semantic_deadline = send.index(
            "(uint32_t)transaction_deadline_ms", result_wait
        )

        self.assertLess(request_deadline, semantic_deadline)
        self.assertLess(semantic_deadline, submit)
        self.assertLess(submit, submit_request_deadline)
        self.assertLess(submit_request_deadline, transaction)
        self.assertLess(transaction, transaction_semantic_deadline)
        self.assertLess(transaction_semantic_deadline, result_wait)
        self.assertLess(result_wait, result_semantic_deadline)

    def test_manual_pair_control_uses_command_specific_round_trip_budget(
        self,
    ) -> None:
        route = function_body(CONTROL, "gateway_route_survey_pair_control")

        request = route.index(
            "gateway_survey_natural_request_timeout_ms("
        )
        abort_result = route.index(
            "command_id == CMD_SURVEY_ABORT ?", request
        )
        short_result = route.index(
            "SURVEY_PAIR_ABORT_RESULT_TIMEOUT_MS", abort_result
        )
        full_result = route.index(
            "SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS", short_result
        )
        total = route.index(
            "request_timeout_ms + result_timeout_ms", full_result
        )
        waiter = route.index(
            "gateway_begin_command_result_wait_for(", total
        )
        waiter_total = route.index("transaction_timeout_ms", waiter)
        send = route.index("gateway_survey_send_pair_control(", waiter_total)
        send_request = route.index("request_deadline_ms", send)

        self.assertLess(request, abort_result)
        self.assertLess(abort_result, short_result)
        self.assertLess(short_result, full_result)
        self.assertLess(full_result, total)
        self.assertLess(total, waiter)
        self.assertLess(waiter, waiter_total)
        self.assertLess(waiter_total, send)
        self.assertLess(send, send_request)

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

        self.assertLess(history, reject)
        self.assertLess(reject, note)
        self.assertIn("return -EINVAL", branch[reject:note])

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

    def test_discovery_confirmation_is_telemetry_not_collection_permission(
        self,
    ) -> None:
        note = function_body(SURVEY, "gateway_note_survey_ack_confirm")
        wait = function_body(
            CONTROL, "gateway_survey_wait_for_discovery_collection"
        )

        self.assertIn(
            "gateway_survey_discovery_ack_confirm_mask |= UINT64_C(1) << i",
            note,
        )
        self.assertNotIn("ack_confirms_complete", wait)
        self.assertNotIn("ACK_BARRIER", wait)
        self.assertNotIn("gateway_survey_discovery_ack_confirm_mask", wait)

    def test_accepted_control_waits_for_exact_ack_confirm_proof(self) -> None:
        result = function_body(
            ROUND, "gateway_survey_round_note_control_result"
        )
        confirmation = function_body(
            ROUND, "gateway_survey_round_apply_control_confirmation"
        )

        self.assertIn(
            "app_gateway_survey_round_capture_control_result(", result
        )
        self.assertNotIn(
            "app_gateway_survey_round_note_control_success(", result
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
        emission_choice = wait.index(
            "emission_horizon_elapsed ?", wait_branch
        )
        safety_choice = wait.index(
            "gateway_survey_collection_deadline_ms", emission_choice
        )
        wait_schedule = wait.index(
            "SURVEY_GATEWAY_DUE_BOUNDARY_POLL, wait_ms", safety_choice
        )
        collection_clear = wait.index(
            "gateway_survey_collection_pending = false", wait_schedule
        )

        self.assertIn(
            "gateway_survey_collection_emission_deadline_ms",
            wait[emission_choice:wait_schedule],
        )
        self.assertIn(
            "gateway_survey_collection_deadline_ms",
            wait[emission_choice:wait_schedule],
        )
        self.assertLess(wait_schedule, collection_clear)
        self.assertNotIn("ack_confirm", wait)

    def test_assignment_claim_advances_but_table_commit_requires_confirm(
        self,
    ) -> None:
        barrier = function_body(
            CONTROL,
            "gateway_discovery_assignment_ack_confirm_blocks_locked",
        )
        publish = function_body(
            CONTROL, "gateway_discovery_assignment_publish_work_handler"
        )
        complete = function_body(
            CONTROL, "gateway_discovery_assignment_complete_success_locked"
        )

        self.assertIn(
            "app_mesh_report_gateway_operation_confirmation_pending(",
            barrier,
        )
        self.assertIn("MSG_COMMAND_RESULT, session_id, k_uptime_get_32()", barrier)
        self.assertIn(
            "gateway_discovery_assignment_state.operation_deadline_ms",
            barrier,
        )
        self.assertIn("gateway_discovery_assignment_reschedule(", barrier)

        table_publish = publish.index(
            "gateway_discovery_assignment_publish_table()"
        )
        self.assertNotIn("claim-to-table", publish)
        self.assertNotIn(
            "gateway_discovery_assignment_state.claim_command_seq",
            publish[:table_publish],
        )

        table_barrier = complete.index(
            "gateway_discovery_assignment_ack_confirm_blocks_locked("
        )
        self.assertIn(
            "gateway_discovery_assignment_state.table_command_seq",
            complete[table_barrier:],
        )
        roster = complete.index("committed_anchor_ids", table_barrier)
        self.assertLess(table_barrier, roster)

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
            "gateway_discovery_assignment_complete_success_locked()"
        )

        self.assertNotIn(
            "gateway_discovery_assignment_state.round_open = false",
            fast_complete[:completion_call],
            "the table-round owner must survive a pending ACK_CONFIRM barrier",
        )
        table_barrier = complete.index(
            "gateway_discovery_assignment_ack_confirm_blocks_locked("
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
            "gateway_discovery_assignment_state.active", fifo_wake
        )
        assignment_wake = callback.index(
            "gateway_discovery_assignment_wake_now(", assignment
        )
        survey = callback.index("gateway_survey_active", assignment_wake)
        survey_wake = callback.index(
            "SURVEY_GATEWAY_DUE_CONTROL_DELIVERY, 0u", survey
        )

        self.assertIn("K_NO_WAIT", callback[fifo:fifo_wake + 100])
        self.assertLess(fifo, fifo_wake)
        self.assertLess(fifo_wake, assignment)
        self.assertLess(assignment, assignment_wake)
        self.assertLess(assignment_wake, survey)
        self.assertLess(survey, survey_wake)

    def test_confirmation_callback_replaces_each_delayed_owner_deadline(self) -> None:
        callback = function_body(SURVEY, "gateway_note_survey_ack_confirm")
        fifo_start = callback.index("gateway_host_command_retry_pending")
        assignment_start = callback.index(
            "gateway_discovery_assignment_state.active", fifo_start
        )
        survey_start = callback.index("gateway_survey_active", assignment_start)
        fifo = callback[fifo_start:assignment_start]
        assignment = callback[assignment_start:survey_start]
        survey = callback[survey_start:]
        assignment_wake = function_body(
            CONTROL, "gateway_discovery_assignment_wake_now"
        )

        self.assertRegex(
            fifo,
            r"k_work_reschedule\s*\(\s*"
            r"&gateway_host_command_retry_work\s*,\s*K_NO_WAIT\s*\)",
        )
        self.assertRegex(
            assignment,
            r"gateway_discovery_assignment_wake_now\s*\(\s*"
            r"\"ack-confirm\"\s*\)",
        )
        self.assertIn("k_work_cancel_delayable(", assignment_wake)
        self.assertRegex(
            assignment_wake,
            r"gateway_discovery_assignment_reschedule\s*\(\s*"
            r"K_NO_WAIT\s*,\s*source\s*\)",
        )
        self.assertIn("gateway_survey_work_schedule(", survey)
        self.assertIn("SURVEY_GATEWAY_DUE_CONTROL_DELIVERY, 0u", survey)
        for owner_slice in (fifo, assignment, assignment_wake, survey):
            self.assertNotIn("mesh_gateway_command_priority_submit", owner_slice)
            self.assertNotIn("k_work_submit(", owner_slice)
            self.assertNotIn("k_work_submit_to_queue(", owner_slice)

    def test_confirmation_wakes_are_isolated_to_matching_owners(self) -> None:
        callback = function_body(SURVEY, "gateway_note_survey_ack_confirm")
        assignment_start = callback.index(
            "gateway_discovery_assignment_state.active"
        )
        assignment_end = callback.index(
            "if (!gateway_survey_active)", assignment_start
        )
        assignment = callback[assignment_start:assignment_end]
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

        self.assertIn("identity->msg_type == MSG_COMMAND_RESULT", assignment)
        self.assertIn("claim_command_seq", assignment)
        self.assertIn("table_command_seq", assignment)
        self.assertNotIn("gateway_survey_work_schedule", assignment)
        self.assertNotIn("gateway_host_command_retry_work", assignment)
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

    def test_collection_uses_hop_scaled_custody(self) -> None:
        body = function_body(SURVEY, "gateway_route_survey_reachability")
        lookup = body.index("gateway_survey_known_max_report_hops()")
        scale = body.index("survey_discovery_report_custody_ms(")
        deadline = body.index("collection_delay_ms =", lookup)
        self.assertLess(scale, lookup)
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
            "gateway_survey_collection_emission_deadline_ms",
            "gateway_survey_collection_deadline_ms",
            "gateway_survey_context.report_count",
            "gateway_survey_expected_node_count",
            "gateway_survey_expected_node_count_present",
        ):
            self.assertIn(policy_input, wait[decision:])
        mismatch = wait.index(
            "SURVEY_GATEWAY_COLLECTION_COUNT_MISMATCH", decision
        )
        self.assertIn("gateway_survey_finish_status(", wait[mismatch:])
        self.assertIsNone(
            re.search(r"gateway_survey_context\.report_count\s*=(?!=)", wait),
            "an expected-count mismatch must fail explicitly, never truncate",
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
        survival = re.search(
            r"survey_gateway_discovery_collection_survives_terminal\s*\("
            r"\s*event\.reason\s*==\s*NODE_COMM_TERMINAL_DELIVERED\s*,"
            r"\s*event\.attempts_started\s*\)",
            wait,
        )

        self.assertIsNotNone(survival)
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
        round_drive = worker.index("gateway_survey_round_drive()", telemetry)

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
        rearm = handler.index("gateway_survey_work_rearm_due()")
        self.assertLess(consume, schedule_drive)
        self.assertLess(schedule_drive, rearm)


if __name__ == "__main__":
    unittest.main()
