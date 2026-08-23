#!/usr/bin/env python3
"""Guard the anchor's operation-owned enumeration Channel-5 residency."""

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = (ROOT / "app/src/app_anchor.c").read_text()
COMMANDS = (ROOT / "app/src/app_anchor_commands.inc").read_text()
GATEWAY_CONTROL = (
    ROOT / "app/src/app_anchor_gateway_control.inc"
).read_text()
RADIO = (ROOT / "app/src/app_anchor_radio.inc").read_text()
REPORT_TRANSPORT = (ROOT / "app/src/app_mesh_report_transport.inc").read_text()
REPORT_ROUTE_CONTROL = (
    ROOT / "app/src/app_mesh_report_route_control.inc"
).read_text()
REPORT_RX = (ROOT / "app/src/app_mesh_report_rx.inc").read_text()
DRIVER = (ROOT / "app/src/dwm3000_driver.c").read_text()
DRIVER_IO = (ROOT / "app/src/dwm3000_driver_io.inc").read_text()
LIFECYCLE = (ROOT / "src/protocol_rx_lifecycle.c").read_text()
NODE_COMM = (ROOT / "src/node_comm.c").read_text()
UWB = (ROOT / "include/uwb.h").read_text()
RADIO_TIMING = (ROOT / "include/mesh_radio_timing.h").read_text()


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.S)
    if match is None:
        raise AssertionError(f"missing function {name}")
    start = match.end()
    depth = 1
    index = start
    while index < len(source) and depth:
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
        index += 1
    if depth:
        raise AssertionError(f"unterminated function {name}")
    return source[start : index - 1]


class EnumerationRxLifecycleSourceTests(unittest.TestCase):
    def test_relayed_claim_cannot_replace_here_i_am_gateway_parent(self) -> None:
        apply = function_body(
            COMMANDS, "anchor_apply_discovery_assignment_command"
        )

        self.assertNotIn("anchor_learn_flood_parent_candidate", COMMANDS)
        self.assertNotIn("mesh_relay_note_flood_parent_candidate", apply)
        self.assertNotIn("route_selected(&mesh_runtime.upstream)", apply)
        self.assertNotRegex(
            apply,
            r"mesh_relay_[A-Za-z0-9_]*parent[A-Za-z0-9_]*\s*\(",
        )

    def test_end_and_abort_retire_exact_live_response_handle(self) -> None:
        apply = function_body(
            COMMANDS, "anchor_apply_discovery_assignment_command"
        )
        retire = function_body(
            COMMANDS, "anchor_retire_discovery_response_for_terminal_locked"
        )
        serialized = function_body(
            COMMANDS,
            "anchor_apply_discovery_assignment_command_serialized",
        )
        abort = apply[
            apply.index("if (phase == DISCOVERY_ASSIGNMENT_PHASE_ABORT)") :
            apply.index("if (phase == DISCOVERY_ASSIGNMENT_PHASE_END)")
        ]
        end = apply[
            apply.index("if (phase == DISCOVERY_ASSIGNMENT_PHASE_END)") :
            apply.index("if (phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM)")
        ]

        abort_accept = abort.index("anchor_enumeration_rx_terminate_claim(")
        abort_retire = abort.index(
            'anchor_retire_discovery_response_for_terminal_locked(epoch, "abort")',
            abort_accept,
        )
        abort_commit = abort.index(
            "app_operation_policy_commit_prepared", abort_retire
        )
        end_accept = end.index("anchor_enumeration_rx_terminate_table(")
        end_retire = end.index(
            'anchor_retire_discovery_response_for_terminal_locked(epoch, "end")',
            end_accept,
        )
        end_commit = end.index(
            "app_operation_policy_commit_prepared", end_retire
        )

        self.assertLess(abort_accept, abort_retire)
        self.assertLess(abort_retire, abort_commit)
        self.assertLess(end_accept, end_retire)
        self.assertLess(end_retire, end_commit)
        transaction_lock = serialized.index(
            "k_mutex_lock(&anchor_discovery_assignment_transaction_mutex"
        )
        apply_call = serialized.index(
            "anchor_apply_discovery_assignment_command(", transaction_lock
        )
        transaction_unlock = serialized.index(
            "k_mutex_unlock(&anchor_discovery_assignment_transaction_mutex",
            apply_call,
        )
        self.assertLess(transaction_lock, apply_call)
        self.assertLess(apply_call, transaction_unlock)
        self.assertIn("anchor_discovery_claim_pending.active", retire)
        self.assertIn("anchor_discovery_claim_pending.epoch == epoch", retire)
        self.assertIn("handle = retired.delivery_handle", retire)
        self.assertIn("anchor_discovery_claim_pending.active = false", retire)
        self.assertIn(
            "anchor_discovery_claim_pending.delivery_handle = 0u", retire
        )
        self.assertIn("anchor_discovery_claim_next_generation", retire)
        self.assertIn("k_work_cancel_delayable(&anchor_discovery_claim_work)", retire)
        self.assertIn("app_node_comm_abandon_delivery(handle)", retire)
        self.assertIn("anchor_discovery_claim_failed_abandon_handle", retire)
        self.assertIn("app_watchdog_stop_feeding()", retire)

    def test_gateway_claim_uses_bounded_actual_control_copies(self) -> None:
        submit = function_body(
            GATEWAY_CONTROL,
            "gateway_discovery_assignment_submit_control_flood_locked",
        )
        policies = NODE_COMM[
            NODE_COMM.index("static const struct node_comm_profile_policy") :
            NODE_COMM.index("_Static_assert", NODE_COMM.index(
                "static const struct node_comm_profile_policy"
            ))
        ]
        bounded = policies[
            policies.index("[NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD]") :
            policies.index("[NODE_COMM_PROFILE_RELIABLE_UPLINK]")
        ]

        claim = submit.index(
            "kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM"
        )
        bounded_profile = submit.index(
            "NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD", claim
        )
        later_phases = submit.index(
            "kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE",
            bounded_profile,
        )
        single_origin = submit.index(
            "NODE_COMM_PROFILE_SINGLE_CONTROL_ORIGIN", later_phases
        )
        delivery = submit.index("app_node_comm_submit_delivery(", single_origin)

        self.assertLess(claim, bounded_profile)
        self.assertLess(bounded_profile, later_phases)
        self.assertLess(later_phases, single_origin)
        self.assertLess(single_origin, delivery)
        self.assertIn(".max_attempts = 4u", bounded)
        self.assertIn(".successful_attempts_required = 4u", bounded)
        self.assertNotIn(".max_attempts = 1u", bounded)

    def test_claim_and_table_admit_one_exact_ram_owner(self) -> None:
        apply = function_body(
            COMMANDS, "anchor_apply_discovery_assignment_command"
        )
        begin = function_body(RADIO, "anchor_enumeration_rx_begin")

        self.assertGreaterEqual(apply.count("anchor_enumeration_rx_begin("), 2)
        self.assertGreaterEqual(
            apply.count("anchor_enumeration_rx_begin_table("), 6
        )
        self.assertIn("PROTOCOL_RX_OPERATION_ENUMERATION", begin)
        self.assertIn("operation_budget_ms > INT32_MAX", begin)
        self.assertIn("allow_supersede", begin)
        self.assertIn(
            "Later phases and exact RF replays cannot extend the operation cap",
            LIFECYCLE,
        )

    def test_end_requires_exact_pending_or_committed_table_identity(self) -> None:
        apply = function_body(
            COMMANDS, "anchor_apply_discovery_assignment_command"
        )
        end = apply[
            apply.index("if (phase == DISCOVERY_ASSIGNMENT_PHASE_END)") :
            apply.index("if (phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM)")
        ]

        self.assertIn("discovery_assignment_extract_end_identity", end)
        self.assertIn("end_identity.epoch != epoch", end)
        self.assertIn("pending_table_command_seq", end)
        self.assertIn("table_command_seq == end_identity.table_command_seq", end)
        self.assertGreaterEqual(
            end.count("discovery_assignment_table_commitment_equal"), 2
        )
        self.assertIn("anchor_enumeration_rx_terminate_table", end)
        self.assertIn("reason=inactive-observation", end)

        begin_table = function_body(RADIO, "anchor_enumeration_rx_begin_table")
        terminate_table = function_body(
            RADIO, "anchor_enumeration_rx_terminate_table"
        )
        self.assertIn("anchor_enumeration_rx_table_identity_valid", begin_table)
        self.assertIn("table_command_seq", begin_table)
        self.assertIn("discovery_assignment_table_commitment_equal", begin_table)
        self.assertIn("protocol_rx_lifecycle_terminate", terminate_table)
        unlisted = apply[apply.index("DBG_DISCOVERY_SLOT_UNASSIGNED") :]
        self.assertIn("anchor_enumeration_rx_begin_table", unlisted)
        self.assertLess(
            unlisted.index("anchor_enumeration_rx_begin_table"),
            unlisted.index("anchor_schedule_late_discovery_claim"),
        )

    def test_listener_owns_operation_before_response_scheduling(self) -> None:
        apply = function_body(
            COMMANDS, "anchor_apply_discovery_assignment_command"
        )
        claim = apply[
            apply.index("if (phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM)") :
            apply.index("discovery_assignment_parse_table_tlvs")
        ]
        late_table = apply[
            apply.index("APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM") :
            apply.index("APP_DISCOVERY_ASSIGNMENT_TABLE_REPLAY")
        ]
        replay_start = apply.index(
            "if (table_decision == APP_DISCOVERY_ASSIGNMENT_TABLE_REPLAY)"
        )
        replay = apply[
            replay_start : apply.index("An exact replay", replay_start)
        ]

        self.assertLess(
            claim.index("anchor_enumeration_rx_begin("),
            claim.index("anchor_schedule_discovery_claim"),
        )
        self.assertLess(
            late_table.index("anchor_enumeration_rx_begin_table"),
            late_table.index("anchor_schedule_late_discovery_claim"),
        )
        self.assertLess(
            replay.index("anchor_enumeration_rx_begin_table"),
            replay.index("anchor_resume_pending_discovery_assignment_ack"),
        )

    def test_abort_requires_exact_active_claim_identity(self) -> None:
        apply = function_body(
            COMMANDS, "anchor_apply_discovery_assignment_command"
        )
        abort = apply[
            apply.index("if (phase == DISCOVERY_ASSIGNMENT_PHASE_ABORT)") :
            apply.index("if (phase == DISCOVERY_ASSIGNMENT_PHASE_END)")
        ]
        bind = function_body(RADIO, "anchor_enumeration_rx_bind_claim")
        terminate = function_body(
            RADIO, "anchor_enumeration_rx_terminate_claim"
        )

        self.assertIn("discovery_assignment_extract_abort_identity", abort)
        self.assertIn("abort_identity.epoch != epoch", abort)
        self.assertIn("abort_identity.claim_session_id", abort)
        self.assertIn("abort_identity.claim_command_seq", abort)
        self.assertIn("anchor_enumeration_rx_terminate_claim", abort)
        self.assertIn("anchor_enumeration_rx_claim_identity_valid", bind)
        self.assertIn("anchor_enumeration_rx_lifecycle.generation == epoch", bind)
        self.assertIn("protocol_rx_lifecycle_terminate", terminate)
        self.assertIn("anchor_enumeration_rx_clear_phase_identities", terminate)

    def test_continuous_windows_rearm_without_parking_or_reconfigure(self) -> None:
        scan = function_body(RADIO, "anchor_uwb_scan_work_handler")
        rearm_start = scan.index("enumeration_rx_window:")
        rearm = scan[
            rearm_start : scan.index("preamble_detected =", rearm_start)
        ]

        self.assertIn("dwm3000_driver_receive_frame_continuous_extend_on_activity", rearm)
        self.assertIn("goto enumeration_rx_window", rearm)
        self.assertGreaterEqual(scan.count("goto enumeration_rx_window"), 3)
        self.assertIn(
            "scan_rx_elapsed_us = u32_saturating_add(scan_rx_elapsed_us,",
            scan,
        )
        self.assertNotIn("dwm3000_driver_configure", rearm)
        self.assertNotIn("anchor_enter_low_power", rearm)
        self.assertIn(
            "enumeration_continuous_rx ? APP_RADIO_LOW_POWER_IDLE", scan
        )
        self.assertIn("anchor_enumeration_rx_active()) {", scan)
        self.assertIn("next_scan_delay_ms = 0u", scan)

    def test_continuous_listener_uses_extended_phr_and_protocol_max_slice(self) -> None:
        scan = function_body(RADIO, "anchor_uwb_scan_work_handler")
        selection_start = scan.index(
            "enumeration_continuous_rx = anchor_enumeration_rx_active()"
        )
        select = scan[
            scan.index("if (enumeration_continuous_rx)", selection_start) :
            scan.index("route_waiting_active =", selection_start)
        ]
        configure = scan[
            scan.index("ret = enumeration_continuous_rx ?") :
            scan.index("if (ret == 0)", scan.index("ret = enumeration_continuous_rx ?"))
        ]
        recovery = scan[
            scan.index("dwm3000_driver_force_recovery()") :
            scan.index("bool recovered =", scan.index("dwm3000_driver_force_recovery()"))
        ]

        self.assertIn(
            "scan_rx_ms = UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS", select
        )
        self.assertIn("dwm3000_driver_configure_wake_mesh_control_mode()", configure)
        self.assertIn("dwm3000_driver_configure_wake_mode()", configure)
        self.assertLess(
            configure.index("dwm3000_driver_configure_wake_mesh_control_mode()"),
            configure.index("dwm3000_driver_configure_wake_mode()"),
        )
        self.assertIn("dwm3000_driver_configure_wake_mesh_control_mode()", recovery)
        self.assertNotIn("dwm3000_driver_configure_wake_mode()", recovery)

        wake_config = DRIVER[
            DRIVER.index("static dwt_config_t wake_config") :
            DRIVER.index("static dwt_config_t wake_mesh_control_config")
        ]
        control_config = DRIVER[
            DRIVER.index("static dwt_config_t wake_mesh_control_config") :
            DRIVER.index("static dwt_config_t mesh_payload_config")
        ]
        receive = function_body(
            DRIVER_IO, "receive_frame_continuous_extend_on_activity"
        )
        self.assertIn("DWM3000_PHY_PHR_MODE", wake_config)
        self.assertIn("DWM3000_MESH_PHY_PHR_MODE", control_config)
        self.assertIn(
            "wait_status_internal(RX_TERMINAL_STATUS_MASK | RX_ACTIVITY_STATUS_MASK,\n"
            "                               acquire_timeout_ms,",
            receive,
        )
        self.assertNotIn("MIN(acquire_timeout_ms", receive)
        self.assertNotIn("MAX(acquire_timeout_ms", receive)
        self.assertIn("(unsigned int)frame_cap", receive)

    def test_enumeration_scan_caps_before_earliest_required_channel9_activity(self) -> None:
        scan = function_body(RADIO, "anchor_uwb_scan_work_handler")
        required_source = REPORT_TRANSPORT[
            REPORT_TRANSPORT.index(
                "static bool mesh_channel9_next_required_activity("
            ) :
        ]
        required = function_body(
            required_source, "mesh_channel9_next_required_activity"
        )
        earliest = function_body(
            REPORT_TRANSPORT, "mesh_next_channel9_activity_delay_ms"
        )
        exported = function_body(
            REPORT_TRANSPORT,
            "mesh_report_next_channel9_activity_prepare_delay_ms",
        )

        enumeration = scan.index(
            "enumeration_continuous_rx = anchor_enumeration_rx_active()"
        )
        full_window = scan.index(
            "scan_rx_ms = UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS",
            enumeration,
        )
        query = scan.index(
            "mesh_report_next_channel9_activity_prepare_delay_ms(",
            full_window,
        )
        cap_gate = scan.index("if (ch9_receive_prepare_pending)", query)
        reserve = scan.index(
            "ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS +", cap_gate
        )
        viable_margin = scan.index(
            "MESH_RADIO_CONTROL_FOLLOWUP_SCAN_MS", reserve
        )
        too_close = scan.index(
            "ch9_receive_prepare_delay_ms <= scan_reserve_ms", viable_margin
        )
        defer = scan.index("ch9_rx_conflict = true", too_close)
        capped = scan.index(
            "scan_rx_ms =\n"
            "                ch9_receive_prepare_delay_ms - scan_reserve_ms",
            defer,
        )
        conflict = scan.index("if (ch9_rx_conflict)", capped)
        receive = scan.index(
            "dwm3000_driver_receive_frame_continuous_extend_on_activity(",
            conflict,
        )

        # Without active Channel-9 timing the cap gate is false, so the
        # operation keeps its full 2000 ms protocol listener. With timing, the
        # capped window ends before the appointment by both completion and
        # viable rearm margins; a closer appointment starts no C5 scan at all.
        self.assertRegex(
            UWB,
            r"#define\s+UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS\s+2000u",
        )
        self.assertRegex(
            RADIO_TIMING,
            r"#define\s+MESH_RADIO_CONTROL_FOLLOWUP_SCAN_MS\s+20u",
        )
        pre_cap = scan[full_window:cap_gate]
        self.assertIn("IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)", pre_cap)
        self.assertIn("enumeration_continuous_rx", pre_cap)
        self.assertLess(full_window, query)
        self.assertLess(query, cap_gate)
        self.assertLess(cap_gate, reserve)
        self.assertLess(reserve, viable_margin)
        self.assertLess(viable_margin, too_close)
        self.assertLess(too_close, defer)
        self.assertLess(defer, capped)
        self.assertLess(capped, conflict)
        self.assertLess(conflict, receive)
        self.assertIn(
            "ch9_retry_ms = MAX(ch9_receive_prepare_delay_ms, 1u)",
            scan[too_close:capped],
        )

        # The selected boundary covers normal RX turns and only admits a TX
        # turn when local reliable data or a physical ACK is actually pending.
        self.assertIn("mesh_event_timing_local_rx_slot(timing)", required)
        self.assertIn("mesh_event_timing_local_tx_slot(timing)", required)
        self.assertIn(
            "mesh_node_comm_reliable_tx_pending_for_peer(", required
        )
        self.assertIn("mesh_channel9_ack_pending_for_peer", required)
        self.assertIn("mesh_channel9_next_required_activity(", earliest)
        self.assertIn("mesh_channel9_prepare_start_ms(&timing)", earliest)
        self.assertIn(
            "candidate_delay_ms < selected_delay_ms", earliest
        )
        self.assertIn(
            "mesh_next_channel9_activity_delay_ms(now_ms, delay_ms)",
            exported,
        )

        # After a bounded timeout, re-check the same earliest activity before
        # the zero-gap rearm, so a newly due appointment cannot be hidden by
        # another continuous Channel-5 slice.
        rearm_query = scan.index(
            "mesh_report_next_channel9_activity_prepare_delay_ms(", receive
        )
        rearm_due = scan.index(
            "receive_prepare_delay_ms <= scan_reserve_ms", rearm_query
        )
        timeout = scan.index("if (ret == -ETIMEDOUT", rearm_due)
        rearm = scan.index("goto enumeration_rx_window", timeout)
        self.assertIn("!enumeration_owned_work_due", scan[timeout:rearm])
        self.assertLess(rearm_query, rearm_due)
        self.assertLess(rearm_due, timeout)
        self.assertLess(timeout, rearm)

    def test_prepare_due_is_shared_by_both_enumeration_rearm_guards(self) -> None:
        scan = function_body(RADIO, "anchor_uwb_scan_work_handler")
        receive = scan.index(
            "dwm3000_driver_receive_frame_continuous_extend_on_activity("
        )
        prepare_query = scan.index(
            "mesh_report_next_channel9_activity_prepare_delay_ms(", receive
        )
        prepare_due = scan.index(
            "receive_prepare_delay_ms <= scan_reserve_ms", prepare_query
        )
        owned = scan.index("enumeration_owned_work_due =", prepare_due)
        owned_end = scan.index(";", owned)
        first_guard = scan.index(
            "if (ret == -ETIMEDOUT && !enumeration_owned_work_due)",
            owned_end,
        )
        first_rearm = scan.index("goto enumeration_rx_window", first_guard)
        second_guard = scan.index(
            "if (anchor_enumeration_rx_active() && "
            "!enumeration_owned_work_due)",
            first_rearm,
        )
        second_rearm = scan.index("goto enumeration_rx_window", second_guard)
        final_rearm = scan.index("goto enumeration_rx_window", second_rearm + 1)
        scan_complete = scan.index("scan_complete:", final_rearm)

        # One shared ownership bit survives the first timeout branch and gates
        # the later recovery/fallthrough branch as well. This prevents the
        # second unconditional rearm that escaped the original fix.
        self.assertIn("receive_prepare_due", scan[owned:owned_end])
        # One declaration plus one assignment; no later overwrite can discard
        # the prepare-due bit before the second guard.
        self.assertEqual(scan.count("enumeration_owned_work_due ="), 2)
        self.assertLess(prepare_query, prepare_due)
        self.assertLess(prepare_due, owned)
        self.assertLess(owned_end, first_guard)
        self.assertLess(first_guard, first_rearm)
        self.assertLess(first_rearm, second_guard)
        self.assertLess(second_guard, second_rearm)
        self.assertLess(second_rearm, final_rearm)
        self.assertLess(final_rearm, scan_complete)
        self.assertIn(
            "!enumeration_owned_work_due", scan[first_guard:first_rearm]
        )
        self.assertIn(
            "!enumeration_owned_work_due", scan[second_guard:second_rearm]
        )

    def test_local_response_scan_is_one_window_capped_before_retry(self) -> None:
        scan = function_body(RADIO, "anchor_uwb_scan_work_handler")
        planner = function_body(RADIO, "anchor_relay_retry_plan_scan")
        local_response_start = scan.index(
            "if (enumeration_continuous_rx && "
            "local_protocol_response_active)"
        )
        local_response = scan[
            local_response_start : scan.index(
                "route_waiting_active =", local_response_start
            )
        ]
        local_response_branch = local_response[
            : local_response.index("} else if")
        ]
        selection = local_response.index(
            "scan_rx_ms = UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS"
        )
        plan = scan.index("anchor_relay_retry_plan_scan(")
        receive = scan.index(
            "dwm3000_driver_receive_frame_continuous_extend_on_activity("
        )

        self.assertNotIn(
            "scan_rx_ms = MESH_RADIO_CONTROL_FOLLOWUP_SCAN_MS",
            local_response_branch,
        )
        self.assertLess(selection, plan)
        self.assertLess(plan, receive)
        self.assertIn("mesh_runtime.pending.retry_after_ms", planner)
        self.assertIn(
            "scan_guard_ms = ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS +",
            planner,
        )
        self.assertIn(
            "MESH_RADIO_EVENT_RETUNE_GUARD_MS + 1u", planner
        )
        self.assertIn(
            "*scan_rx_ms = remaining_ms - scan_guard_ms", planner
        )
        self.assertLess(
            planner.index("if (remaining_ms <= scan_guard_ms)"),
            planner.index("*scan_rx_ms = remaining_ms - scan_guard_ms"),
        )

    def test_protocol_flood_blocks_scan_start_and_rearm_but_route_adv_does_not(self) -> None:
        scan = function_body(RADIO, "anchor_uwb_scan_work_handler")
        all_pending = function_body(
            REPORT_TRANSPORT, "mesh_c5_flood_work_pending"
        )
        protocol_pending = function_body(
            REPORT_TRANSPORT, "mesh_c5_protocol_flood_work_pending"
        )
        guard_claim = scan.index("radio_guard_uwb_claim(")
        pre_scan_gate = scan[:guard_claim]
        snapshot = pre_scan_gate.index(
            "protocol_flood_pending = mesh_c5_protocol_flood_work_pending()"
        )
        blocked = pre_scan_gate.index(
            "protocol_flood_pending ||", snapshot
        )
        blocked_return = pre_scan_gate.index("return;", blocked)
        receive = scan.index(
            "dwm3000_driver_receive_frame_continuous_extend_on_activity("
        )
        owned_due = scan.index("enumeration_owned_work_due =", receive)
        live_pending = scan.index(
            "mesh_c5_protocol_flood_work_pending()", owned_due
        )
        timeout_rearm = scan.index(
            "if (ret == -ETIMEDOUT && !enumeration_owned_work_due)",
            live_pending,
        )
        same_lease_rearm = scan.index(
            "goto enumeration_rx_window", timeout_rearm
        )
        scan_complete = scan.index("scan_complete:", same_lease_rearm)
        completion_snapshot = scan.index(
            "protocol_flood_pending = mesh_c5_protocol_flood_work_pending()",
            scan_complete,
        )
        retry = scan.index(
            "next_scan_delay_ms = ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS",
            completion_snapshot,
        )
        zero_gap = scan.index(
            "next_scan_delay_ms = 0u", retry
        )

        self.assertNotIn("mesh_c5_flood_work_pending()", pre_scan_gate)
        self.assertLess(snapshot, blocked)
        self.assertLess(blocked, blocked_return)
        self.assertLess(blocked_return, guard_claim)
        self.assertLess(receive, owned_due)
        self.assertLess(owned_due, live_pending)
        self.assertLess(live_pending, timeout_rearm)
        self.assertLess(timeout_rearm, same_lease_rearm)
        self.assertIn("!enumeration_owned_work_due", scan[timeout_rearm:same_lease_rearm])
        self.assertLess(scan_complete, completion_snapshot)
        self.assertLess(completion_snapshot, retry)
        self.assertLess(retry, zero_gap)
        self.assertIn(
            "else if (anchor_relay_control_followup_boost_active() ||",
            scan[retry:zero_gap],
        )
        self.assertNotIn("mesh_c5_flood_work_pending()", scan)
        self.assertEqual(
            scan.count("mesh_c5_protocol_flood_work_pending()"), 3
        )
        self.assertIn("mesh_c5_flood_deferred.valid", all_pending)
        self.assertIn("mesh_route_adv_deferred.valid", all_pending)
        self.assertNotIn("MSG_GATEWAY_ROUTE_ADV", all_pending)
        self.assertGreaterEqual(
            protocol_pending.count("MSG_GATEWAY_ROUTE_ADV"), 2
        )
        self.assertGreaterEqual(protocol_pending.count("!=\n"), 2)
        self.assertIn("mesh_c5_flood_deferred.valid", protocol_pending)
        self.assertIn("mesh_route_adv_deferred.valid", protocol_pending)
        self.assertIn("mesh_c5_flood_deferred_lock", protocol_pending)

    def test_response_priority_flood_aborts_only_background_anchor_scan(self) -> None:
        store = function_body(
            REPORT_TRANSPORT, "mesh_c5_flood_store_deferred"
        )
        valid = store.index("entry->valid = true")
        schedule = store.index("mesh_reschedule_owned_work(", valid)
        eligibility = store.index(
            "if (DEVICE_ROLE == ROLE_ANCHOR && response_priority &&",
            schedule,
        )
        mark_abort = store.index(
            "abort_background_scan = true", eligibility
        )
        unlock = store.index(
            "k_mutex_unlock(&mesh_c5_flood_deferred_lock)", mark_abort
        )
        abort_gate = store.index("if (abort_background_scan)", unlock)
        request = store.index(
            "dwm3000_driver_request_receive_abort(", abort_gate
        )

        admission = store[eligibility:mark_abort]
        self.assertIn("out->packet.msg_type != MSG_GATEWAY_ROUTE_ADV", admission)
        self.assertIn(
            "radio_guard_uwb_owner_client() ==\n"
            "            RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN",
            admission,
        )
        self.assertIn("!anchor_uwb_window_active()", admission)
        self.assertIn("!anchor_click_window_active()", admission)
        self.assertIn(
            "!mesh_report_anchor_survey_radio_active()", admission
        )
        self.assertLess(valid, schedule)
        self.assertLess(schedule, eligibility)
        self.assertLess(eligibility, mark_abort)
        self.assertLess(mark_abort, unlock)
        self.assertLess(unlock, abort_gate)
        self.assertLess(abort_gate, request)
        self.assertIn(
            "DWM3000_RECEIVE_ABORT_MESH_CONTROL", store[request:]
        )
        self.assertNotIn("DWM3000_RECEIVE_ABORT_NODE_COMM", store)
        self.assertNotIn("DWM3000_RECEIVE_ABORT_GATEWAY_PRIORITY", store)
        self.assertEqual(
            store.count("dwm3000_driver_request_receive_abort("), 1
        )

    def test_unexpected_rx_error_uses_bounded_recovery_or_fails_closed(self) -> None:
        scan = function_body(RADIO, "anchor_uwb_scan_work_handler")
        unexpected = scan.index("if (ret != 0 && ret != -ETIMEDOUT)")
        recovery = scan.index("dwm3000_driver_force_recovery()", unexpected)
        reconfigure = scan.index(
            "dwm3000_driver_configure_wake_mesh_control_mode()", recovery
        )
        terminal = scan.index(
            'recovered ? "rx-recovered" : "rx-recovery-failed"',
            reconfigure,
        )
        recovered_rearm = scan.index("goto enumeration_rx_window", terminal)
        release = scan.index("scan_complete:", terminal)

        self.assertLess(unexpected, recovery)
        self.assertLess(recovery, reconfigure)
        self.assertLess(reconfigure, terminal)
        self.assertLess(terminal, recovered_rearm)
        self.assertLess(terminal, release)
        self.assertIn("enumeration_owned_work_due", scan)

    def test_protocol_response_aborts_and_releases_continuous_anchor_rx(self) -> None:
        send = function_body(
            REPORT_TRANSPORT, "mesh_try_send_reliable_uplink_view"
        )
        response_active = function_body(REPORT_RX, "mesh_rx_response_active")
        scan = function_body(RADIO, "anchor_uwb_scan_work_handler")

        response = send[
            send.index("if (view->packet->msg_type == MSG_COMMAND_RESULT)") :
            send.index("memset(out, 0", send.index("MSG_COMMAND_RESULT"))
        ]
        self.assertIn("RADIO_GUARD_UWB_CLIENT_MESH_RX", response)
        self.assertIn("RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN", response)
        self.assertIn("!anchor_uwb_window_active()", response)
        self.assertIn("!anchor_click_window_active()", response)
        self.assertIn("!mesh_report_anchor_survey_radio_active()", response)
        self.assertIn("DWM3000_RECEIVE_ABORT_MESH_CONTROL", response)
        self.assertIn("mesh_rx_handoff_wait_for_control", response)
        abort_request = send.index(
            "dwm3000_driver_request_receive_abort(",
            send.index("MSG_COMMAND_RESULT"),
        )
        wait_ready = send.index("mesh_rx_handoff_wait_for_control()", abort_request)
        first_attempt = send.index(
            "mesh_start_tracked_tx_with_retry(", wait_ready
        )
        handoff_end = send.index("mesh_rx_handoff_end_control()", first_attempt)
        role_scan_restart = send.index("mesh_restart_role_scan()", handoff_end)
        self.assertLess(abort_request, wait_ready)
        self.assertLess(wait_ready, first_attempt)
        self.assertLess(first_attempt, handoff_end)
        self.assertLess(handoff_end, role_scan_restart)

        self.assertIn("mesh_rx_handoff_control_active()", response_active)
        receive = scan.index(
            "dwm3000_driver_receive_frame_continuous_extend_on_activity"
        )
        owned = scan.index("enumeration_owned_work_due =", receive)
        timeout_rearm = scan.index("ret == -ETIMEDOUT", owned)
        recovery_guard = scan.index(
            "anchor_enumeration_rx_active() && !enumeration_owned_work_due",
            timeout_rearm,
        )
        cancelled_release = scan.index("if (ret == -ECANCELED)", recovery_guard)
        recovery = scan.index("dwm3000_driver_force_recovery()", cancelled_release)
        release = scan.index("scan_complete:", recovery)

        self.assertIn("mesh_rx_response_active()", scan[owned:timeout_rearm])
        self.assertLess(owned, timeout_rearm)
        self.assertLess(timeout_rearm, recovery_guard)
        self.assertLess(recovery_guard, cancelled_release)
        self.assertLess(cancelled_release, recovery)
        self.assertLess(recovery, release)
        self.assertIn("ret != -ECANCELED", scan)
        self.assertIn("next_scan_delay_ms = 0u", scan[release:])

    def test_reset_is_ram_only_and_response_failure_keeps_listener(self) -> None:
        failure = function_body(COMMANDS, "anchor_discovery_claim_work_handler")

        begin = function_body(RADIO, "anchor_enumeration_rx_begin")
        terminate = function_body(
            RADIO, "anchor_enumeration_rx_note_recovery"
        )
        self.assertIn("static struct protocol_rx_lifecycle", RADIO)
        self.assertNotIn("durable", begin + terminate)
        self.assertNotIn("nvs", (begin + terminate).lower())
        self.assertIn("DBG_DISCOVERY_SLOT_RESPONSE_FAILED", failure)
        self.assertIn("response admission failed", failure)
        self.assertIn("response retry scheduling failed", failure)
        self.assertNotIn("anchor_enumeration_rx_terminate", failure)
        self.assertNotIn("anchor_enumeration_rx_note_recovery", failure)

    def test_downstream_activation_marks_once_and_clears_after_end_relay(self) -> None:
        send = function_body(REPORT_TRANSPORT, "mesh_send_c5_flood_now_until")

        self.assertIn("protocol_rx_downstream_activation_expire", send)
        self.assertIn("discovery_assignment_epoch_strictly_newer", send)
        stale = send.index("return -ESTALE;")
        replace = send.index("protocol_rx_downstream_activation_clear", stale)
        needs_wake = send.index("protocol_rx_downstream_activation_needs_wake")
        self.assertLess(stale, replace)
        self.assertLess(replace, needs_wake)
        self.assertIn("send_wake_train = false", send)
        self.assertIn("aggregate_result.sent_count > 0u", send)
        end_clear = send[
            send.index("enumeration_phase == DISCOVERY_ASSIGNMENT_PHASE_END") :
        ]
        self.assertIn("DISCOVERY_ASSIGNMENT_PHASE_ABORT", end_clear)
        self.assertIn("aggregate_result.sent_count == attempt_count", end_clear)
        self.assertIn("protocol_rx_downstream_activation_clear", end_clear)

    def test_gateway_claim_alone_gets_long_first_activation_train(self) -> None:
        identity = function_body(
            REPORT_TRANSPORT, "mesh_c5_gateway_enumeration_claim"
        )
        send = function_body(REPORT_TRANSPORT, "mesh_send_c5_flood_now_until")

        self.assertIn("#if DEVICE_ROLE == ROLE_GATEWAY", identity)
        self.assertIn("command_id == CMD_ASSIGN_DISCOVERY_SLOTS", identity)
        self.assertIn("phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM", identity)
        self.assertIn("gateway_enumeration_claim", send)
        self.assertIn("attempt == 0u", send)
        self.assertIn(
            "MESH_RADIO_ENUMERATION_ACTIVATION_WAKE_TRAIN_MS", send
        )
        self.assertIn("WAKE_ADV_MS : 0u", send)
        self.assertIn("if (wake_train_ms != 0u)", send)
        self.assertIn("mesh_send_route_wake_train_with_duration", send)

    def test_wake_train_retry_preserves_requested_duration_unless_boosted(self) -> None:
        wake = function_body(
            REPORT_ROUTE_CONTROL,
            "mesh_send_route_wake_train_with_duration",
        )

        self.assertIn(".wake_adv_ms = wake_train_ms", wake)
        attempt = wake[
            wake.index("wake_train_attempt:") : wake.index(
                "mesh_stop_role_scan()", wake.index("wake_train_attempt:")
            )
        ]
        restore = attempt.index(
            "wake_train_config.wake_adv_ms = wake_train_ms;"
        )
        evaluate = attempt.index("boost_single_shot =")
        conditional = attempt.index("if (boost_single_shot)", evaluate)
        shorten = attempt.index(
            "wake_train_config.wake_adv_ms = "
            "MESH_ROUTE_WAKE_ADV_BOOST_ACTIVE_MS;",
            conditional,
        )

        self.assertLess(evaluate, conditional)
        self.assertLess(restore, conditional)
        self.assertLess(conditional, shorten)
        self.assertNotIn("boost_single_shot ?", attempt)
        self.assertEqual(attempt.count("wake_train_config.wake_adv_ms ="), 2)

    def test_long_wake_train_claimed_duration_stays_protocol_valid(self) -> None:
        claimed = function_body(
            REPORT_ROUTE_CONTROL,
            "mesh_route_wake_claimed_duration_ms",
        )

        self.assertIn("UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS", claimed)
        self.assertNotIn("UINT16_MAX", claimed)

    def test_gateway_enumeration_activation_uses_bounded_random_gap(self) -> None:
        wake = function_body(
            REPORT_ROUTE_CONTROL,
            "mesh_send_route_wake_train_with_duration",
        )
        random_value = wake.index("uint32_t random_value = sys_rand32_get()")
        gateway = wake.index("DEVICE_ROLE == ROLE_GATEWAY", random_value)
        activation = wake.index(
            "MESH_RADIO_ENUMERATION_ACTIVATION_WAKE_TRAIN_MS", gateway
        )
        bounded_gap = wake.index(
            "MESH_RADIO_ENUMERATION_WAKE_GAP_JITTER_MAX_US + 1u", activation
        )
        wait = wake.index("k_busy_wait(jitter_us)", bounded_gap)

        self.assertLess(random_value, gateway)
        self.assertLess(gateway, activation)
        self.assertLess(activation, bounded_gap)
        self.assertLess(bounded_gap, wait)
        self.assertNotIn("PHASE_WALK", wake)
        self.assertNotIn("phase_walk", wake)


if __name__ == "__main__":
    unittest.main()
