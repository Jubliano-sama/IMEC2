#!/usr/bin/env python3
"""Systemic guards against failed control work making anchors deaf to clicks."""

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = read_composed_source(ROOT / "app/src/app_anchor.c")
SURVEY = read_composed_source(ROOT / "app/src/app_survey.c")
REPORT = read_composed_source(ROOT / "app/src/app_mesh_report.c")
GATEWAY_CONTROL = read_composed_source(
    ROOT / "app/src/app_anchor.c"
)
GATEWAY_BLE = read_composed_source(ROOT / "app/src/app_gateway_ble.c")
LIFECYCLE = (ROOT / "src/protocol_rx_lifecycle.c").read_text(
    encoding="utf-8"
)
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
WAKE_SCENARIOS = (
    ROOT / "tests/mesh_integration/test_mesh_wake_scenarios.c"
).read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    match = re.search(
        rf"(?m)^(?:static\s+)?(?:[A-Za-z_]\w*\s+)+(?:\*\s*)?"
        rf"{name}\s*\([^;]*?\)\s*\{{",
        source,
        re.S,
    )
    if match is None:
        raise AssertionError(f"missing function {name}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function {name}")


def assert_order(test: unittest.TestCase, source: str, *needles: str) -> None:
    cursor = 0
    for needle in needles:
        index = source.find(needle, cursor)
        test.assertGreaterEqual(
            index, 0, f"missing or out-of-order source boundary: {needle}"
        )
        cursor = index + len(needle)


class ControlFailureClickServiceabilitySourceTests(unittest.TestCase):
    def test_assignment_terminal_custody_releases_rf_before_host_receipt(self):
        complete = function_body(
            GATEWAY_CONTROL,
            "gateway_discovery_assignment_complete_success_locked",
        )
        detach = function_body(
            GATEWAY_BLE,
            "gateway_assignment_publication_detach_rf_owner",
        )

        assert_order(
            self,
            complete,
            "gateway_commit_host_command_result_reserved(",
            "result_reservation_token = 0u",
            "app_gateway_assignment_publisher_capture_terminal(&event)",
            "gateway_assignment_publication_detach_rf_owner(&event)",
            "gateway_operation_owner_release(",
            "gateway_discovery_assignment_state.active = false",
        )
        self.assertNotIn(
            "if (gateway_discovery_assignment_state.replay)", complete
        )
        self.assertIn(
            "gateway_membership_identity_matches_event(", detach
        )
        self.assertIn("diagnostics.terminal_pending", detach)
        self.assertIn(
            "gateway_membership_publication_live_owner = false", detach
        )
        self.assertIn(
            "gateway_membership_publication_owner_release_pending = false",
            detach,
        )

    def test_gateway_listener_start_failures_have_exact_cleanup(self):
        pipeline = function_body(
            GATEWAY_CONTROL, "gateway_enumeration_pipeline_start"
        )
        service = function_body(
            GATEWAY_CONTROL, "gateway_discovery_assignment_service_delivery"
        )

        self.assertNotIn(
            '(void)mesh_start_uwb_rx("enumeration-hia-pipeline")', pipeline
        )
        assert_order(
            self,
            pipeline,
            'ret = mesh_start_uwb_rx("enumeration-hia-pipeline")',
            "gateway_enumeration_prearm_valid_locked()",
            "gateway_discovery_assignment_state.epoch == epoch",
            "gateway_enumeration_prearm_reset_locked()",
        )
        self.assertNotIn(
            '(void)mesh_start_uwb_rx("compact-enumeration")', service
        )
        compact_start = service.index(
            'ret = mesh_start_uwb_rx("compact-enumeration")'
        )
        compact_failure = service.index(
            "gateway_discovery_assignment_fail_locked(", compact_start
        )
        compact_reschedule = service.index(
            '"compact-response-lane"', compact_start
        )
        self.assertLess(compact_start, compact_failure)
        self.assertLess(compact_failure, compact_reschedule)

    def test_here_i_am_and_missing_table_cannot_leave_an_unbounded_owner(self):
        prearm = function_body(ANCHOR, "anchor_enumeration_rx_prearm")
        begin = function_body(ANCHOR, "anchor_enumeration_rx_begin")
        active = function_body(ANCHOR, "anchor_enumeration_rx_active")
        apply_command = function_body(
            ANCHOR, "anchor_apply_discovery_assignment_command"
        )
        rollback = function_body(
            ANCHOR, "anchor_enumeration_rx_terminate_epoch"
        )
        rollback_unbound = function_body(
            ANCHOR, "anchor_enumeration_rx_rollback_unbound_claim"
        )
        fail_bound = function_body(
            ANCHOR, "anchor_discovery_assignment_fail_bound_claim"
        )
        expire = function_body(LIFECYCLE, "protocol_rx_lifecycle_expire")

        for body, budget in (
            (prearm, "hold_ms"),
            (begin, "operation_budget_ms"),
        ):
            with self.subTest(budget=budget):
                self.assertIn(f"{budget} == 0u", body)
                self.assertIn(f"{budget} > INT32_MAX", body)
                self.assertIn(f"now_ms + {budget}", body)
                self.assertIn("protocol_rx_lifecycle_begin(", body)

        # The pipelined HIA supplies operation authority without extending its
        # short lease. Bind only after configuration is accepted; duplicate
        # HIA must not replace a TABLE already admitted by this operation.
        self.assertNotIn("anchor_enumeration_rx_begin(epoch", prearm)
        self.assertNotIn("protocol_rx_lifecycle_set_deadline(", prearm)
        config = prearm.index("anchor_enumeration_response_config =")
        accepted = prearm.index("if (result == PROTOCOL_RX_BEGIN_ACCEPTED)", config)
        bind = prearm.index("anchor_enumeration_rx_bind_claim(epoch, epoch, epoch)", accepted)
        authority = prearm.index("local_anchor_discovery_assignment_note_claim(epoch)", bind)
        self.assertLess(config, accepted)
        self.assertLess(accepted, bind)
        self.assertLess(bind, authority)
        claim_phase = apply_command.index(
            "phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM"
        )
        claim_begin = apply_command.index(
            "anchor_enumeration_rx_begin(", claim_phase
        )
        claim_bind = apply_command.index(
            "anchor_enumeration_rx_bind_claim(", claim_begin
        )
        self.assertLess(claim_phase, claim_begin)
        self.assertLess(claim_begin, claim_bind)

        # HIA/CLAIM may be only partially delivered and TABLE may never arrive.
        # The anchor must still lazily terminate the exact lifecycle and clear
        # every phase identity when the immutable operation bound is reached.
        assert_order(
            self,
            active,
            "protocol_rx_lifecycle_expire(",
            "anchor_enumeration_rx_clear_phase_identities()",
            "PROTOCOL_RX_MODE_CONTINUOUS_CHANNEL5",
        )
        self.assertIn("deadline_reached(now_ms, lifecycle->deadline_ms)", expire)
        self.assertIn("protocol_rx_lifecycle_init(lifecycle)", expire)
        self.assertIn("anchor_enumeration_rx_terminate_epoch(", prearm)
        self.assertIn('"prearm-config-conflict"', prearm)
        assert_order(
            self,
            rollback,
            "protocol_rx_lifecycle_terminate(",
            "anchor_enumeration_rx_clear_phase_identities()",
            "anchor_compact_enumeration_deactivate(epoch)",
            "anchor_uwb_scan_schedule_ms(anchor_uwb_scan_interval_ms)",
        )
        self.assertIn("anchor_compact_enumeration_deactivate(", active)
        self.assertIn("!anchor_enumeration_rx_claim_identity_valid", rollback_unbound)
        self.assertIn("protocol_rx_lifecycle_terminate(", rollback_unbound)
        self.assertIn("anchor_enumeration_rx_terminate_claim(", fail_bound)
        self.assertIn("app_watchdog_stop_feeding()", fail_bound)
        for reason in (
            '"claim-pending-ack-bind"',
            '"claim-bind"',
        ):
            with self.subTest(unbound_claim_reason=reason):
                self.assertIn(reason, apply_command)
        self.assertEqual(
            apply_command.count("anchor_enumeration_rx_rollback_unbound_claim("),
            2,
        )
        for reason in (
            '"claim-pending-ack-resume"',
            '"claim-hop-parse"',
            '"claim-start-delay-parse"',
            '"claim-response-lane"',
        ):
            with self.subTest(bound_claim_reason=reason):
                self.assertIn(reason, apply_command)
        self.assertEqual(
            apply_command.count("anchor_discovery_assignment_fail_bound_claim("),
            4,
        )

    def test_every_enumeration_or_survey_listener_activity_probes_for_click(self):
        probe = function_body(ANCHOR, "anchor_protocol_rx_probe_standard_click")
        compact = function_body(ANCHOR, "anchor_run_compact_enumeration_lane")
        scan = function_body(ANCHOR, "anchor_uwb_scan_work_handler")

        # The extended-PHR control listener can acquire a standard-PHR click
        # preamble but cannot decode it. The bounded standard-PHR probe is the
        # required handoff; merely treating that activity as another CRC retry
        # recreates the hardware failure this guard exists for.
        self.assertIn("dwm3000_driver_configure_wake_mode()", probe)
        self.assertIn("dwm3000_driver_receive_frame_continuous", probe)
        self.assertIn("uwb_decode_wake_claim", probe)
        self.assertIn("app_mesh_c5_wake_claim_requires_anchor_handoff", probe)
        self.assertIn("dwm3000_driver_configure_wake_mesh_control_mode()", probe)
        self.assertIn("DBG_PROTOCOL_RX_CLICK_PROBE_CAPTURE", probe)

        self.assertIn("anchor_protocol_rx_probe_standard_click(", compact)
        self.assertIn("app_anchor_rx_failure_detected_preamble", compact)
        self.assertIn("anchor_protocol_rx_probe_standard_click(", scan)
        self.assertIn("app_anchor_rx_failure_detected_preamble", scan)
        self.assertIn("DBG_PROTOCOL_RX_CLICK_PREEMPT", scan)

        # A decoded or probed click must reach the same ordinary claim handler;
        # control ownership may resume after ranging only if its deadline still
        # remains live. It must never loop in extended PHR instead.
        self.assertIn("anchor_handle_uwb_claim(&claim", scan)
        self.assertNotIn("DBG_PROTOCOL_RX_CLICK_DEFER", scan)
        assert_order(
            self,
            scan,
            "anchor_protocol_rx_probe_standard_click(",
            "goto scan_frame_ready",
            "anchor_handle_uwb_claim(&claim",
        )

    def test_missing_survey_plan_abort_and_rx_failure_all_terminate(self):
        expire = function_body(SURVEY, "anchor_rx_expire_locked")
        apply = function_body(SURVEY, "app_survey_anchor_apply_control")
        recovery = function_body(
            SURVEY, "app_survey_anchor_rx_note_recovery"
        )
        continuous = function_body(SURVEY, "app_survey_anchor_rx_continuous")

        self.assertIn("now_ms < anchor_state.self_stop_ms", expire)
        self.assertIn("anchor_rx_terminate_locked(false)", expire)
        self.assertIn("anchor_rx_expire_locked", continuous)

        neighbor = apply.index("SURVEY_PHASE_NEIGHBOR_START")
        self_stop = apply.index("anchor_state.self_stop_ms = stop_ms", neighbor)
        schedule = apply.index("anchor_work_reschedule(", self_stop)
        self.assertLess(neighbor, self_stop)
        self.assertLess(self_stop, schedule)

        abort = apply.index("SURVEY_PHASE_ABORT")
        abort_end = apply.index("} else {", abort)
        abort_path = apply[abort:abort_end]
        self.assertIn("anchor_rx_terminate_locked(true)", abort_path)
        self.assertIn("k_work_cancel_delayable(&anchor_work)", abort_path)

        self.assertIn("protocol_rx_lifecycle_note_rx_recovery(", recovery)
        self.assertIn("PROTOCOL_RX_RECOVERY_TERMINATED", recovery)
        self.assertIn("anchor_rx_terminate_locked(true)", recovery)
        self.assertIn("k_work_cancel_delayable(&anchor_work)", recovery)

    def test_long_survey_radio_owners_probe_and_handoff_clicks(self):
        probe = function_body(SURVEY, "anchor_probe_standard_click")
        control_wait = function_body(
            SURVEY, "anchor_control_wait_with_click_probes"
        )
        standard_wait = function_body(
            SURVEY, "anchor_standard_wait_for_click"
        )
        response_lane = function_body(SURVEY, "anchor_run_response_lane")
        neighbors = function_body(SURVEY, "anchor_neighbor_sequence")
        execute = function_body(SURVEY, "anchor_execute_plan")
        worker = function_body(SURVEY, "anchor_work_handler")
        preempt = function_body(
            SURVEY, "app_survey_anchor_preempt_for_click"
        )
        preempt_identity = function_body(
            SURVEY, "anchor_preempt_for_click_identity"
        )
        handoff = function_body(SURVEY, "anchor_handoff_captured_click")

        # A repeated standard-PHR wake train is the physical recovery
        # mechanism while survey owns an incompatible extended-PHR receiver.
        # Keep the probe shorter than one production wake train, and restore
        # the survey PHY only when no valid click was captured.
        self.assertIn("APP_SURVEY_CLICK_PROBE_BUDGET_MS < WAKE_ADV_MS", SURVEY)
        assert_order(
            self,
            probe,
            "dwm3000_driver_configure_wake_mode()",
            "dwm3000_driver_receive_frame_continuous_extend_on_activity_until(",
            "uwb_decode_wake_claim",
            "app_mesh_c5_wake_claim_requires_anchor_handoff",
            "capture->valid = true",
        )
        self.assertIn("APP_SURVEY_CLICK_PROBE_RX_MS", probe)
        self.assertIn("MIN(deadline_ms,", probe)
        self.assertIn("dwm3000_driver_configure_wake_mesh_control_mode()", probe)

        # The roughly 69-second neighbor phase and every response lane have
        # an absolute next-probe edge; neither a sleep nor an extended receive
        # may run past it. The long ranging plan spends idle gaps in standard
        # PHY rather than holding the incompatible control listener.
        self.assertIn("now_ms >= *next_probe_ms", control_wait)
        self.assertIn("MIN(deadline_ms, *next_probe_ms)", control_wait)
        self.assertIn("anchor_probe_standard_click(capture, true,", control_wait)
        self.assertIn("anchor_probe_standard_click(capture, false,", standard_wait)
        self.assertGreaterEqual(
            neighbors.count("anchor_control_wait_with_click_probes("), 3
        )
        self.assertIn("MIN(slot_end_ms, next_probe_ms)", neighbors)
        self.assertIn("anchor_probe_standard_click(capture, true,", neighbors)
        self.assertIn("next_probe_ms < receive_deadline_ms", response_lane)
        self.assertIn("anchor_probe_standard_click(capture, true,", response_lane)
        self.assertIn("anchor_standard_wait_for_click(", execute)

        # Release the exact survey identity and its owned RF before invoking
        # the ordinary click handler. If that handoff cannot accept the claim,
        # a fresh scan is mandatory so the repeating train can recover.
        assert_order(
            self,
            handoff,
            "anchor_preempt_for_click_identity(",
            "survey_ops.anchor_handle_click_wake_claim(",
            "anchor_uwb_scan_schedule_ms(0u)",
        )
        self.assertIn("&snapshot->identity, false", handoff)
        for action in (
            "APP_SURVEY_ANCHOR_ACTION_NEIGHBORS",
            "APP_SURVEY_ANCHOR_ACTION_EXECUTE",
        ):
            branch = worker.index(f"action == {action}")
            capture = worker.index("if (click_capture.valid)", branch)
            handoff_call = worker.index(
                "anchor_handoff_captured_click(&snapshot, action", capture
            )
            return_after = worker.index("return;", handoff_call)
            self.assertLess(capture, handoff_call)
            self.assertLess(handoff_call, return_after)
        assert_order(
            self,
            preempt_identity,
            "anchor_rx_expire_locked(",
            "anchor_rx_terminate_locked(true)",
            "k_work_cancel_delayable(&anchor_work)",
            "dwm3000_driver_request_receive_abort(",
        )
        self.assertIn("anchor_preempt_for_click_identity(NULL, true)", preempt)

        # Hard RF or scheduling failures terminate ownership. They may never
        # silently put the anchor back into the long continuous listener.
        self.assertGreaterEqual(
            worker.count("if (ret < 0 && ret != -ECANCELED)"), 2
        )
        self.assertGreaterEqual(worker.count("anchor_rx_terminate_locked(true)"), 6)
        self.assertNotIn("anchor_rx_continuous_locked", worker)

    def test_failed_survey_control_retries_abort_to_the_remote_terminal_bound(self):
        schedule = function_body(SURVEY, "gateway_work_reschedule_owned")
        cleanup = function_body(SURVEY, "gateway_begin_cleanup_locked")
        abort_cleanup = function_body(
            SURVEY, "gateway_begin_abort_cleanup_locked"
        )
        remote_bound = function_body(
            SURVEY, "gateway_possible_remote_self_stop_locked"
        )
        worker = function_body(SURVEY, "gateway_work_handler")
        abort = function_body(SURVEY, "app_survey_gateway_abort")

        self.assertIn("app_watchdog_stop_feeding()", schedule)
        self.assertIn(
            "gateway_state.cleanup_deadline_ms = gateway_state.self_stop_ms",
            cleanup,
        )
        self.assertIn("gateway_state.cleanup_abort_pending = true", abort_cleanup)
        self.assertIn('gateway_work_reschedule_owned(now_ms, "cleanup-abort")', abort_cleanup)
        self.assertIn("SURVEY_INITIAL_SELF_EXPIRY_MS", remote_bound)
        self.assertIn("self_stop_delay_ms", remote_bound)
        self.assertIn("MAX(gateway_state.self_stop_ms", remote_bound)

        # Both explicit abort and partial START/PLAN delivery preserve the
        # longest self-stop that any receiver may have learned. Cleanup sends
        # ABORT repeatedly until that immutable bound, retaining exact survey
        # identity across every retry.
        self.assertIn("gateway_possible_remote_self_stop_locked(", abort)
        self.assertIn("gateway_begin_abort_cleanup_locked(", abort)
        control_failure = worker.index("if (control_ret != 0 || control_origin_ms == 0u)")
        cleanup_start = worker.index(
            "gateway_begin_abort_cleanup_locked(&event, now_ms)", control_failure
        )
        cleanup_stage = worker.index(
            "gateway_state.stage == APP_SURVEY_GATEWAY_CLEANUP", cleanup_start
        )
        queue_abort = worker.index("queue_abort = true", cleanup_stage)
        send_abort = worker.index("ops.send_control(&abort_control", queue_abort)
        retry = worker.index("gateway_state.cleanup_abort_pending = true", send_abort)
        retry_schedule = worker.index('"cleanup-abort-retry"', retry)
        retry_guard = worker.rfind("if (gateway_state.active", send_abort, retry)
        self.assertLess(control_failure, cleanup_start)
        self.assertLess(cleanup_start, cleanup_stage)
        self.assertLess(cleanup_stage, queue_abort)
        self.assertLess(queue_abort, send_abort)
        self.assertLess(send_abort, retry)
        self.assertLess(retry, retry_schedule)
        self.assertGreaterEqual(retry_guard, send_abort)
        self.assertIn(
            "now_ms < gateway_state.cleanup_deadline_ms",
            worker[retry_guard:retry_schedule],
        )
        self.assertIn(
            "survey_identity_equal(&gateway_state.identity",
            worker[retry_guard:retry_schedule],
        )

    def test_route_and_event_control_interruption_have_fixed_endpoints(self):
        route_listener = function_body(REPORT, "mesh_listen_for_route_reply")
        proposal = function_body(
            REPORT, "mesh_propose_event_after_channel5_contact_authorized"
        )

        self.assertIn("deadline_ms = k_uptime_get_32() + window_ms", route_listener)
        self.assertIn("uptime_deadline_reached(now_ms, deadline_ms)", route_listener)
        self.assertIn("MIN(remaining_ms,", route_listener)
        self.assertIn("MESH_ROUTE_REPLY_READY_POLL_MS", route_listener)
        self.assertIn("mesh_probe_standard_wake_claim(", route_listener)
        self.assertIn("MESH_STANDARD_WAKE_PROBE_CLICK", route_listener)

        self.assertIn("MESH_EVENT_NEGOTIATION_DEADLINE_MS", proposal)
        accept = proposal.index("if (require_accept)")
        timeout = proposal.index("if (ret < 0)", accept)
        timeout_path = proposal[timeout:]
        assert_order(
            self,
            timeout_path,
            "mesh_ch9_ack_batch_discard_if_safe(",
            "mesh_relay_clear_channel9_timing(",
            "mesh_event_owner_abandon_peer(",
            "mesh_event_propose_clear()",
            "return -ETIMEDOUT",
        )

    def test_gateway_control_listener_cannot_hide_click_probe_behind_shortfall(self):
        route_listener = function_body(REPORT, "mesh_listen_for_route_reply")
        gateway_branch_start = route_listener.index(
            "contact_purpose ==\n"
            "                               C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD"
        )
        gateway_branch_end = route_listener.index(
            "if (!rx_failure_diagnostic_logged", gateway_branch_start
        )
        gateway_branch = route_listener[gateway_branch_start:gateway_branch_end]
        probe = gateway_branch.index("mesh_probe_standard_wake_claim(")

        # Every incompatible-PHR activity can be a click, even after a deeper
        # relay wake was seen or when the gateway-control flood reached all
        # modeled hops. Those state checks may influence control resumption,
        # but they may not suppress the only standard-PHR probe.
        probe_prefix = gateway_branch[:probe]
        self.assertNotIn("gateway_control_relay_shortfall", probe_prefix)
        self.assertNotIn("gateway_control_followup_wake_seen", probe_prefix)
        click = gateway_branch.index(
            "MESH_STANDARD_WAKE_PROBE_CLICK", probe
        )
        click_end = gateway_branch.index(
            "MESH_STANDARD_WAKE_PROBE_RELAYED_GATEWAY_CONTROL", click
        )
        click_path = gateway_branch[click:click_end]
        self.assertIn("break;", click_path)
        self.assertNotIn("continue;", click_path)

    def test_deferred_c5_custody_blocks_scan_only_at_its_physical_rf_edge(self):
        due = function_body(REPORT, "mesh_c5_flood_deferred_entry_due")
        stamp = function_body(
            REPORT, "mesh_c5_flood_deferred_schedule_after_locked"
        )
        pending = function_body(REPORT, "mesh_c5_protocol_flood_work_pending")
        store = function_body(REPORT, "mesh_c5_flood_store_deferred")
        worker = function_body(REPORT, "mesh_c5_flood_work_handler")
        send = function_body(REPORT, "mesh_send_c5_flood_now_until")
        scan = function_body(ANCHOR, "anchor_uwb_scan_work_handler")
        schedule_owner = function_body(REPORT, "mesh_owned_schedule_result")
        c5_schedule_loss = function_body(
            REPORT, "mesh_c5_flood_schedule_loss_fail_stop"
        )
        resume = function_body(REPORT, "mesh_transport_resume")

        # A valid deferred record is passive custody until its exact retry
        # edge. Only a due entry or an RF call already in progress may block
        # the standard Channel-5 scan.
        self.assertIn("entry->valid", due)
        self.assertIn("entry->outbound.earliest_tx_valid", due)
        self.assertIn("uptime_deadline_reached(now_ms", due)
        self.assertIn("entry->outbound.earliest_tx_ms = retry_due_ms", stamp)
        self.assertIn("entry->outbound.earliest_tx_valid = true", stamp)
        self.assertIn("mesh_c5_enumeration_relay_burst_active()", pending)
        self.assertIn("mesh_c5_flood_deferred_entry_due(", pending)
        self.assertNotIn("mesh_c5_flood_deferred.valid ||", pending)
        self.assertIn(
            "protocol_flood_pending = mesh_c5_protocol_flood_work_pending()",
            scan,
        )

        # Admission failure restores the exact previous slot instead of
        # leaving an unscheduled valid generation that suppresses scanning.
        assert_order(
            self,
            store,
            "previous_entry = *entry",
            "entry->valid = true",
            "mesh_c5_flood_deferred_schedule_after_locked(",
            "schedule_ret = mesh_reschedule_owned_work(",
            "entry->generation == generation",
            "*entry = previous_entry",
            "return schedule_ret",
        )

        # Every retained worker has a bounded retry/age terminal, every
        # rearm result is checked, and exhausted exact custody is cleared.
        self.assertIn("MESH_C5_LOCAL_DEFER_MAX_AGE_MS", worker)
        self.assertIn("MESH_C5_DEFERRED_MAX_RETRIES", worker)
        self.assertNotIn("(void)mesh_reschedule_owned_work", worker)
        self.assertEqual(
            worker.count("schedule_ret = mesh_reschedule_owned_work("),
            worker.count("if (schedule_ret < 0)"),
        )
        self.assertEqual(
            worker.count("schedule_ret = mesh_reschedule_owned_work("),
            worker.count("mesh_c5_flood_schedule_loss_fail_stop("),
        )
        assert_order(
            self,
            worker,
            "current_generation = entry->valid",
            "entry->generation == generation",
            "entry->valid = false",
            "app_mesh_rf_retry_reset(&entry->rf_retry)",
        )
        self.assertIn("app_watchdog_stop_feeding()", schedule_owner)
        assert_order(
            self,
            c5_schedule_loss,
            "schedule_ret == -ESHUTDOWN && mesh_transport_paused()",
            "return;",
            "app_watchdog_stop_feeding()",
        )
        self.assertIn(
            "mesh_c5_flood_schedule_loss_fail_stop(", resume
        )

        # Enumeration burst custody exists only around the actual wake/data
        # RF calls; it may not cover a multi-second logical flood schedule.
        wake_call = send.index("mesh_send_route_wake_train_with_duration(")
        wake_begin = send.rfind(
            "mesh_c5_enumeration_relay_burst_begin(&tx)", 0, wake_call
        )
        wake_end = send.index(
            "mesh_c5_enumeration_relay_burst_end()", wake_call
        )
        data_calls = [
            send.index(call, wake_end)
            for call in (
                "app_mesh_flood_send_opportunity_resume(",
                "app_mesh_flood_send_opportunity(",
                "app_mesh_flood_send_bounded_resume(",
                "app_mesh_command_orchestrator_serialize_flood(",
                "app_mesh_command_orchestrator_send_flood(",
            )
        ]
        data_call = min(data_calls)
        data_begin = send.rfind(
            "mesh_c5_enumeration_relay_burst_begin(&tx)", wake_end, data_call
        )
        data_end = send.index(
            "mesh_c5_enumeration_relay_burst_end()", data_call
        )
        self.assertGreaterEqual(wake_begin, 0)
        self.assertGreaterEqual(data_begin, wake_end)
        self.assertLess(wake_begin, wake_call)
        self.assertLess(wake_call, wake_end)
        self.assertLess(data_begin, data_call)
        self.assertTrue(all(data_begin < call < data_end for call in data_calls))

    def test_gateway_command_retry_has_attempt_and_schedule_failure_terminals(self):
        schedule = function_body(
            GATEWAY_CONTROL, "gateway_host_command_schedule_priority_retry"
        )
        retry_worker = function_body(
            GATEWAY_CONTROL, "gateway_host_command_retry_work_handler"
        )
        dispatch_worker = function_body(
            GATEWAY_CONTROL, "gateway_host_command_work_handler"
        )

        assert_order(
            self,
            schedule,
            "GATEWAY_HOST_COMMAND_MAX_SEND_ATTEMPTS",
            "gateway_host_command_priority_retry_reset()",
            "gateway_host_command_retire_failed_generation(",
            "gateway_host_command_submit_next_queued()",
        )
        self.assertIn("if (ret < 0)", schedule)
        self.assertIn("gateway_host_command_priority_retry_reset()", schedule)
        self.assertIn("return -EIO;", schedule)
        for worker in (retry_worker, dispatch_worker):
            with self.subTest(worker=worker[:80]):
                self.assertIn("gateway_host_command_schedule_priority_retry(", worker)
                self.assertIn("gateway_host_command_retire_failed_generation(", worker)
                self.assertIn("gateway_host_command_submit_next_queued()", worker)

    def test_click_report_ack_owner_survives_the_short_gateway_ack_slice(self):
        direct_retry = function_body(
            REPORT, "mesh_handle_direct_gateway_retry_policy"
        )
        actions = function_body(REPORT, "mesh_handle_result_actions")

        fast_slice = direct_retry.index("DBG_DIRECT_GW_ACK_DEFER_CORE")
        timeout_owner = direct_retry.index("mesh_schedule_tx_timeout()", fast_slice)
        rx_owner = direct_retry.index("mesh_schedule_uwb_rx(0u)", timeout_owner)
        self.assertLess(fast_slice, timeout_owner)
        self.assertLess(timeout_owner, rx_owner)

        exact_owner = actions.index("direct_gateway_ack_wait_owned =")
        ack_window = actions.index(
            "DBG_RETRANSMIT_DIRECT_GW_ACK_WINDOW", exact_owner
        )
        ack_window_end = actions.index(
            "} else if (!backend_terminal", ack_window
        )
        owner_proof = actions[exact_owner:ack_window]
        window_path = actions[ack_window:ack_window_end]
        # Channel 9 is retired: there is no channel-9 core ACK wait to consult
        # and the uplink next hop may be a parent anchor rather than the
        # gateway. Ownership is now proven by "the send started, the short
        # synchronous ACK slice returned exactly -EAGAIN, and the relay core
        # still holds this identical packet".
        for proof in (
            "direct_gateway_retransmit",
            "backend_rf_started",
            "send_ret == -EAGAIN",
            "mesh_relay_tx_active(&mesh_runtime)",
            "mesh_packet_delivery_identity_matches(",
        ):
            self.assertIn(proof, owner_proof)
        self.assertNotIn("app_mesh_ch9_core_ack_wait_active(", owner_proof)
        self.assertIn("mesh_schedule_tx_timeout()", window_path)
        self.assertNotIn("mesh_relay_note_retransmit_deferred", window_path)

    def test_event_capacity_expires_and_relay_delivery_has_recovery_coverage(self):
        expire = function_body(REPORT, "mesh_expire_channel9_timings")
        proposal_rx = function_body(REPORT, "mesh_handle_event_control")

        self.assertRegex(
            REPORT,
            r"mesh_event_owners\[MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS\]",
        )
        self.assertRegex(
            REPORT,
            r"mesh_event_origin_tombstones"
            r"\[MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS\]",
        )
        assert_order(
            self,
            expire,
            "!mesh_event_timing_usable(&entry->timing, now_ms)",
            "mesh_event_owner_abandon_peer(entry->next_hop_id)",
            "mesh_relay_expire_channel9_timings(&mesh_runtime, now_ms)",
            "mesh_restore_anchor_low_duty_if_no_ch9(reason)",
        )
        self.assertRegex(
            proposal_rx,
            r'mesh_expire_channel9_timings\s*\(\s*now_ms\s*,\s*'
            r'"event-propose-preflight"\s*\)',
        )
        self.assertIn("mesh_event_owner_can_begin_peer", proposal_rx)
        self.assertIn("DBG_EVENT_PROPOSE_OWNER_FULL", proposal_rx)

        # The dynamic simulator test proves a full relay owner becomes usable
        # after its exact custody settles or is cancelled; keep that scenario
        # in both mandatory labels so a source-only inventory cannot mask it.
        self.assertRegex(
            CMAKE,
            r"add_test\(NAME mesh_post_operation_liveness_scenarios[\s\S]*?"
            r'LABELS "mesh_integration;hardware_models;[^"]*liveness',
        )

    def test_cross_phy_handoff_remains_a_real_hardware_model(self):
        # The production handoff relies on standard and extended PHR sharing
        # preamble acquisition while remaining decode-incompatible. Keep the
        # phase sweep honest: only complete-airtime frames inside the standard
        # probe may become a claim, and collision recovery must use a later
        # repeated wake rather than an unsupported direct-delivery fallback.
        for boundary in (
            "mesh_sim_phy_acquisition_compatible(",
            "!mesh_sim_phy_decode_compatible(",
            "MESH_SIM_RX_DECODE_ERROR",
            "schedule_route_listener_probe(",
            "reception_in_window(",
            "MESH_SIM_RX_DECODED",
            '"wake train outside the bounded probe unexpectedly acquired a claim"',
            '"later repeated wake claims did not recover after the probe collision"',
        ):
            self.assertIn(boundary, WAKE_SCENARIOS)
        self.assertRegex(
            CMAKE,
            r"set_tests_properties\(mesh_wake_scenarios[\s\S]*?"
            r'LABELS "mesh_integration;hardware_models;simulator;wake"',
        )


if __name__ == "__main__":
    unittest.main()
