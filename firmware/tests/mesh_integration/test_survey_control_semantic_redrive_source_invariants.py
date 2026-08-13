#!/usr/bin/env python3
"""Pin exact, bounded semantic redrive to survey controls that return results."""

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
NODE_COMM = (ROOT / "src/node_comm.c").read_text()
APP_NODE_COMM = (ROOT / "app/src/app_node_comm.c").read_text()
CONTROL = (ROOT / "app/src/app_anchor_gateway_control.inc").read_text()
SURVEY = (ROOT / "app/src/app_anchor_gateway_survey.inc").read_text()
SURVEY_HEADER = (ROOT / "include/survey.h").read_text()
NODE_COMM_HEADER = (ROOT / "include/node_comm.h").read_text()
DISCOVERY_ASSIGNMENT_HEADER = (ROOT / "include/discovery_assignment.h").read_text()
APP_CONFIG = (ROOT / "app/src/app_config.h").read_text()
ANCHOR_DISCOVERY = (ROOT / "app/src/app_anchor_survey_discovery.c").read_text()
ANCHOR_SURVEY_RUNTIME = (ROOT / "app/src/app_anchor_survey_runtime.c").read_text()
RESULT_DELIVERY = (
    ROOT / "app/src/app_anchor_survey_result_delivery.c"
).read_text()


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


class SurveyControlSemanticRedriveTests(unittest.TestCase):
    def test_pair_result_abort_installs_exact_nonblocking_retirement(self) -> None:
        abort = function_body(
            RESULT_DELIVERY,
            "app_anchor_survey_result_delivery_abort_round",
        )
        stage = function_body(
            RESULT_DELIVERY,
            "app_anchor_survey_result_delivery_stage_reserved",
        )
        service = function_body(
            RESULT_DELIVERY,
            "result_delivery_service_slot",
        )
        finish = function_body(
            RESULT_DELIVERY,
            "app_anchor_survey_result_delivery_producer_finished",
        )

        tombstone = abort.index("result_abort_tombstone.abort_requested = true")
        mark = abort.index("slot->retirement_in_progress = true", tombstone)
        self.assertLess(tombstone, mark)
        self.assertNotIn("return -EINPROGRESS", abort)
        self.assertIn("round_commitment", stage)
        self.assertIn("result_delivery_candidate_aborted_locked", stage)
        retirement = service.index("if (slot->retirement_in_progress)")
        admission = service.index("if (slot->admission_in_progress)", retirement)
        normal_admission = service.index("if (slot->admission_in_progress)", admission + 1)
        self.assertLess(retirement, admission)
        self.assertLess(admission, normal_admission)
        self.assertIn("app_node_comm_abandon_delivery(handle)", service)
        self.assertIn("result_abort_tombstone.producer_active = false", finish)

    def test_failed_cleanup_quarantines_until_remote_lease_expiry(self) -> None:
        quarantine = function_body(SURVEY, "gateway_survey_quarantine_cleanup")
        service = function_body(SURVEY, "gateway_survey_service_cleanup")
        drive = function_body(SURVEY, "gateway_survey_drive_state")

        self.assertIn("cleanup->terminal_failure_published", quarantine)
        self.assertIn("now_ms + SURVEY_PAIR_PREPARED_LEASE_MS", quarantine)
        self.assertIn("cleanup->quarantined = true", quarantine)
        self.assertNotIn("app_watchdog_stop_feeding", quarantine)
        quarantine_branch = service.index("if (cleanup->quarantined)")
        expiry = service.index(
            "survey_gateway_transaction_note_cleanup_lease_expired(",
            quarantine_branch,
        )
        clear = service.index("memset(cleanup, 0, sizeof(*cleanup))", expiry)
        self.assertLess(quarantine_branch, expiry)
        self.assertLess(expiry, clear)
        self.assertIn("!gateway_survey_cleanup.quarantined", drive)

    def test_core_rearms_only_a_delivered_bounded_control_in_place(self) -> None:
        redrive = function_body(NODE_COMM, "node_comm_redrive_delivered")

        profile = redrive.index(
            "slot->request.profile != NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD"
        )
        delivered = redrive.index(
            "slot->owner.terminal.reason != NODE_COMM_TERMINAL_DELIVERED"
        )
        snapshot = redrive.index("before = *slot")
        separate_due = redrive.index("slot->retry_due_ms = not_before_ms", snapshot)
        same_handle = redrive.index(
            "delivery_begin(comm, slot, handle, next_delivery_generation(comm))",
            separate_due,
        )
        prior = redrive.index(
            "*prior_terminal_out = before.owner.terminal", same_handle
        )

        self.assertIn("not_before_ms < now_ms", redrive)
        self.assertIn("absolute_deadline_ms <= not_before_ms", redrive)
        self.assertIn("slot_is_terminal(slot)", redrive)
        self.assertIn("slot->owner.terminal.attempts_started == 0u", redrive)
        self.assertIn("slot->terminal_pending", NODE_COMM)
        self.assertIn("slot->owner.terminal", NODE_COMM)
        self.assertLess(profile, delivered)
        self.assertLess(delivered, snapshot)
        self.assertLess(snapshot, separate_due)
        self.assertLess(separate_due, same_handle)
        self.assertLess(same_handle, prior)
        self.assertNotIn("next_handle", redrive)

    def test_adapter_preserves_frozen_record_and_schedules_new_due(self) -> None:
        redrive = function_body(
            APP_NODE_COMM, "app_node_comm_redrive_delivered_control"
        )

        record = redrive.index("app_node_comm_delivery_record_for_handle(handle)")
        profile = redrive.index("NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD", record)
        core = redrive.index("node_comm_redrive_delivered(", profile)
        deadline = redrive.index(
            "record->time.delivery.absolute_deadline_ms = absolute_deadline_ms",
            core,
        )
        schedule = redrive.index("app_node_comm_schedule_delivery_locked(now_ms)", deadline)

        self.assertLess(record, profile)
        self.assertLess(profile, core)
        self.assertLess(core, deadline)
        self.assertLess(deadline, schedule)
        self.assertNotIn("app_node_comm_freeze_delivery", redrive)

    def test_automatic_pair_control_redrives_before_consuming_terminal(self) -> None:
        service = function_body(CONTROL, "gateway_survey_service_active_delivery")

        peek = service.index("app_node_comm_peek_delivery_event_for(")
        horizon = service.index(
            "gateway_survey_natural_request_timeout_ms(", peek
        )
        fit = service.index(
            "redrive_due_ms < transaction->spec.absolute_deadline_ms", horizon
        )
        redrive = service.index("app_node_comm_redrive_delivered_control(", fit)
        same_handle = service.index(
            "transaction->request_delivery_handle", redrive
        )
        same_deadline = service.index(
            "transaction->spec.absolute_deadline_ms", same_handle
        )
        transaction = service.index(
            "survey_gateway_transaction_note_delivery_redrive(", same_deadline
        )
        take = service.index("app_node_comm_take_delivery_event_for(", transaction)

        self.assertLess(peek, horizon)
        self.assertLess(horizon, fit)
        self.assertLess(fit, redrive)
        self.assertLess(redrive, same_handle)
        self.assertLess(same_handle, same_deadline)
        self.assertLess(same_deadline, transaction)
        self.assertLess(transaction, take)

        result_winner = function_body(
            SURVEY, "gateway_survey_complete_accepted_delivery"
        )
        self.assertIn("gateway_survey_cancel_take_active_delivery(&action)", result_winner)

    def test_manual_pair_control_retains_one_exact_transport_owner(self) -> None:
        route = function_body(CONTROL, "gateway_route_survey_pair_control")
        service = function_body(SURVEY, "gateway_manual_survey_control_service")
        release = function_body(SURVEY, "gateway_manual_survey_control_release")
        clear_owner = function_body(SURVEY, "gateway_manual_survey_control_clear")
        result_side_effect = function_body(
            CONTROL, "gateway_command_result_side_effects"
        )
        timeout_side_effect = function_body(
            CONTROL, "gateway_command_timeout_side_effects"
        )

        submit = route.index("gateway_survey_send_pair_control(")
        store_packet = route.index(
            "control_command = outbound.packet", submit
        )
        store_deadline = route.index(
            "control_transaction_deadline_ms", store_packet
        )
        store_handle = route.index("control_delivery_handle = delivery_handle", store_deadline)
        publish = route.index("control_delivery_active = true", store_handle)
        schedule = route.index("gateway_survey_work_schedule(", publish)
        self.assertNotIn("app_node_comm_auto_reap_delivery", route)
        self.assertLess(submit, store_packet)
        self.assertLess(store_packet, store_deadline)
        self.assertLess(store_deadline, store_handle)
        self.assertLess(store_handle, publish)
        self.assertLess(publish, schedule)
        self.assertIn(
            "SURVEY_GATEWAY_DUE_CONTROL_DELIVERY",
            route[schedule : schedule + 120],
        )

        self.assertIn("app_node_comm_peek_delivery_event_for(", service)
        self.assertIn("app_node_comm_redrive_delivered_control(", service)
        self.assertIn("control_delivery_handle", service)
        self.assertIn("control_transaction_deadline_ms", service)

        abandon = release.index("app_node_comm_abandon_delivery(handle)")
        error_guard = release.index("if (ret < 0", abandon)
        clear = release.index("gateway_manual_survey_control_clear()", error_guard)
        self.assertNotEqual(
            error_guard,
            -1,
            "a failed abandon must retain the manual transport owner",
        )
        self.assertIn("app_watchdog_stop_feeding()", release[error_guard:clear])
        self.assertIn("return ret", release[error_guard:clear])
        self.assertIn("control_delivery_active = false", clear_owner)

        for side_effect in (result_side_effect, timeout_side_effect):
            release_call = side_effect.index("gateway_manual_survey_control_complete(")
            state_advance = side_effect.index(
                "gateway_manual_survey_pair_note_terminal(", release_call
            )
            self.assertLess(release_call, state_advance)

    def test_cleanup_abort_redrives_until_result_or_immutable_deadline(self) -> None:
        cleanup = function_body(SURVEY, "gateway_survey_service_cleanup")

        peek = cleanup.index("app_node_comm_peek_delivery_event_for(")
        delivered = cleanup.index(
            "event.reason == NODE_COMM_TERMINAL_DELIVERED", peek
        )
        horizon = cleanup.index("survey_pair_control_timeout_ms(cleanup->hop_count)", delivered)
        fit = cleanup.index("redrive_due_ms < cleanup->absolute_deadline_ms", horizon)
        redrive = cleanup.index("app_node_comm_redrive_delivered_control(", fit)
        same_handle = cleanup.index("cleanup->handle", redrive)
        same_deadline = cleanup.index("cleanup->absolute_deadline_ms", same_handle)
        result = cleanup.index("if (cleanup->result_terminal)", same_deadline)
        abandon = cleanup.index("app_node_comm_abandon_delivery(cleanup->handle)", result)

        self.assertLess(peek, delivered)
        self.assertLess(delivered, horizon)
        self.assertLess(horizon, fit)
        self.assertLess(fit, redrive)
        self.assertLess(redrive, same_handle)
        self.assertLess(same_handle, same_deadline)
        self.assertLess(same_deadline, result)
        self.assertLess(result, abandon)

    def test_discovery_start_gets_four_fixed_origin_redrives_before_execution(self) -> None:
        admission = function_body(SURVEY, "gateway_route_survey_reachability")
        service = function_body(
            CONTROL, "gateway_survey_wait_for_discovery_collection"
        )
        finish = function_body(SURVEY, "gateway_survey_finish_status")

        self.assertIn(
            "#define NODE_COMM_BOUNDED_CONTROL_HOP_BUDGET_MS 10000u",
            NODE_COMM_HEADER,
        )
        self.assertRegex(
            SURVEY_HEADER,
            r"SURVEY_DISCOVERY_CONTROL_HOP_BUDGET_MS\s+\\\s*"
            r"NODE_COMM_BOUNDED_CONTROL_HOP_BUDGET_MS",
        )
        self.assertRegex(
            DISCOVERY_ASSIGNMENT_HEADER,
            r"DISCOVERY_ASSIGNMENT_CONTROL_FLOOD_DEADLINE_MS\s+\\\s*"
            r"NODE_COMM_BOUNDED_CONTROL_HOP_BUDGET_MS",
        )
        self.assertRegex(
            SURVEY_HEADER,
            r"#define\s+SURVEY_DISCOVERY_ORIGIN_REDRIVE_COUNT\s+4u\b",
        )
        self.assertRegex(
            APP_CONFIG,
            r"BUILD_ASSERT\(SURVEY_DISCOVERY_START_DELAY_MS\s*>\s*"
            r"\(SURVEY_DEFAULT_TTL\s*\+\s*"
            r"SURVEY_DISCOVERY_ORIGIN_REDRIVE_COUNT\)\s*\*\s*"
            r"SURVEY_DISCOVERY_CONTROL_HOP_BUDGET_MS\s*\+\s*"
            r"SURVEY_DISCOVERY_PHY_PREP_BUDGET_MS",
        )
        self.assertIn(
            "static uint8_t gateway_survey_discovery_redrive_count;",
            (ROOT / "app/src/app_anchor.c").read_text(),
        )

        hop_budget_ms = 10_000
        origin_redrive_count = 4
        start_delay_ms = 90_000
        phy_prep_ms = 103
        due_offsets_ms = [
            (count + 1) * hop_budget_ms
            for count in range(origin_redrive_count)
        ]
        self.assertEqual(due_offsets_ms, [10_000, 20_000, 30_000, 40_000])
        self.assertLess(
            due_offsets_ms[-1]
            + 4 * hop_budget_ms
            + phy_prep_ms,
            start_delay_ms,
        )

        handle = admission.index(
            "gateway_survey_discovery_delivery_handle = flood_delivery_handle"
        )
        origin = admission.index(
            "gateway_survey_discovery_origin_uptime_ms = command_origin_uptime_ms",
            handle,
        )
        execution = admission.index(
            "command_origin_uptime_ms + config.start_delay_ms", origin
        )
        arm = admission.index(
            "gateway_survey_discovery_redrive_count = 0u", execution
        )
        self.assertLess(handle, origin)
        self.assertLess(origin, execution)
        self.assertLess(execution, arm)

        peek = service.index("app_node_comm_peek_delivery_event_for(")
        path_guard = service.index(
            "(uint64_t)SURVEY_DEFAULT_TTL *\n"
            "            SURVEY_DISCOVERY_CONTROL_HOP_BUDGET_MS",
            peek,
        )
        latest = service.index(
            "gateway_survey_discovery_start_deadline_ms - path_guard_ms",
            path_guard,
        )
        fixed_due = service.index(
            "gateway_survey_discovery_origin_uptime_ms +\n"
            "            ((uint64_t)gateway_survey_discovery_redrive_count + 1u) *\n"
            "                SURVEY_DISCOVERY_CONTROL_HOP_BUDGET_MS",
            latest,
        )
        clamp = service.index("if (redrive_due_ms < now_uptime_ms)", fixed_due)
        delivered = service.index(
            "event.reason == NODE_COMM_TERMINAL_DELIVERED", clamp
        )
        bounded_waves = service.index(
            "gateway_survey_discovery_redrive_count <\n"
            "                SURVEY_DISCOVERY_ORIGIN_REDRIVE_COUNT",
            delivered,
        )
        overflow_guard = service.index(
            "redrive_due_ms <=\n"
            "                UINT64_MAX - SURVEY_DISCOVERY_PHY_PREP_BUDGET_MS",
            bounded_waves,
        )
        strict_fit = service.index(
            "redrive_due_ms + SURVEY_DISCOVERY_PHY_PREP_BUDGET_MS <\n"
            "                latest_gateway_start_ms",
            overflow_guard,
        )
        redrive = service.index(
            "app_node_comm_redrive_delivered_control(", strict_fit
        )
        same_handle = service.index("handle,", redrive)
        immutable_deadline = service.index("latest_gateway_start_ms", same_handle)
        success = service.index("if (redrive_ret == 0)", immutable_deadline)
        publish = service.index(
            "gateway_survey_discovery_redrive_count++", success
        )
        retain_terminal = service.index("return true;", publish)
        take = service.index("app_node_comm_take_delivery_event_for(", publish)

        self.assertLess(peek, path_guard)
        self.assertLess(path_guard, latest)
        self.assertLess(latest, fixed_due)
        self.assertLess(fixed_due, clamp)
        self.assertLess(clamp, delivered)
        self.assertLess(delivered, bounded_waves)
        self.assertLess(bounded_waves, overflow_guard)
        self.assertLess(overflow_guard, strict_fit)
        self.assertLess(strict_fit, redrive)
        self.assertLess(redrive, same_handle)
        self.assertLess(same_handle, immutable_deadline)
        self.assertLess(immutable_deadline, success)
        self.assertLess(success, publish)
        self.assertLess(publish, retain_terminal)
        self.assertLess(retain_terminal, take)
        before_terminal_take = service[peek:take]
        self.assertNotIn(
            "gateway_survey_discovery_delivery_handle =", before_terminal_take
        )
        self.assertNotIn(
            "gateway_survey_discovery_origin_uptime_ms =", before_terminal_take
        )
        self.assertNotIn(
            "gateway_survey_discovery_start_deadline_ms =", before_terminal_take
        )
        self.assertNotIn(
            "gateway_survey_discovery_start_deadline_ms = now_uptime_ms",
            service,
        )

        abandon = finish.index("app_node_comm_abandon_delivery(")
        clear_origin = finish.index(
            "gateway_survey_discovery_origin_uptime_ms = 0u", abandon
        )
        clear_execution = finish.index(
            "gateway_survey_discovery_start_deadline_ms = 0u", clear_origin
        )
        clear_redrive = finish.index(
            "gateway_survey_discovery_redrive_count = 0u", clear_execution
        )
        self.assertLess(abandon, clear_origin)
        self.assertLess(clear_origin, clear_execution)
        self.assertLess(clear_execution, clear_redrive)

        receiver = function_body(
            ANCHOR_DISCOVERY, "app_anchor_survey_discovery_handle_start"
        )
        duplicate = receiver.index(
            "admission == APP_ANCHOR_SURVEY_DISCOVERY_DUPLICATE"
        )
        return_without_requeue = receiver.index("return;", duplicate)
        queue = receiver.index("discovery_ops.queue_start(", return_without_requeue)
        self.assertLess(duplicate, return_without_requeue)
        self.assertLess(return_without_requeue, queue)

        admit = function_body(
            ANCHOR_SURVEY_RUNTIME,
            "app_anchor_survey_runtime_admit_discovery",
        )
        self.assertIn(
            "discovery_config.operation_generation ==\n"
            "                   config->operation_generation",
            admit,
        )
        self.assertIn("discovery_config.survey_id == config->survey_id", admit)
        self.assertIn("APP_ANCHOR_SURVEY_DISCOVERY_DUPLICATE", admit)

    def test_redrive_is_not_applied_to_protocols_with_their_own_owner(self) -> None:
        route = function_body(CONTROL, "gateway_route_mesh_host_packet")
        assignment = function_body(
            CONTROL, "gateway_discovery_assignment_publish_work_handler"
        )

        self.assertIn("app_node_comm_submit_protocol_response(", route)
        self.assertIn("mesh_send_gateway_command_flood(", route)
        self.assertIn("gateway_discovery_assignment_publish_table()", assignment)
        self.assertIn("gateway_discovery_assignment_open_claim_round_locked()", assignment)
        self.assertNotIn("app_node_comm_redrive_delivered_control", assignment)
        self.assertEqual(
            APP_NODE_COMM.count("app_node_comm_redrive_delivered_control("),
            1,
            "the adapter defines the primitive but must not choose protocol policy",
        )


if __name__ == "__main__":
    unittest.main()
