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
REPORT_DELIVERY = (ROOT / "app/src/app_mesh_report_delivery.inc").read_text()
REPORT_ROUTE_CONTROL = (
    ROOT / "app/src/app_mesh_report_route_control.inc"
).read_text()
REPORT_RX = (ROOT / "app/src/app_mesh_report_rx.inc").read_text()
APP_NODE_COMM = (ROOT / "app/src/app_node_comm.c").read_text()
APP_MESH_FLOOD = (ROOT / "app/src/app_mesh_flood.c").read_text()
APP_SURVEY = (ROOT / "app/src/app_survey.c").read_text()
DRIVER = (ROOT / "app/src/dwm3000_driver.c").read_text()
DRIVER_IO = (ROOT / "app/src/dwm3000_driver_io.inc").read_text()
LIFECYCLE = (ROOT / "src/protocol_rx_lifecycle.c").read_text()
RELAY_CUSTODY = (ROOT / "src/mesh_relay_custody.inc").read_text()
NODE_COMM = (ROOT / "src/node_comm.c").read_text()
GATEWAY_COMMAND = (ROOT / "src/gateway_command.c").read_text()
SURVEY = (ROOT / "src/survey.c").read_text()
MESH = (ROOT / "include/mesh.h").read_text()
PROTOCOL = (ROOT / "include/protocol.h").read_text()
UWB = (ROOT / "include/uwb.h").read_text()
ENUMERATION_LANE = (ROOT / "include/enumeration_response_lane.h").read_text()
RADIO_TIMING = (ROOT / "include/mesh_radio_timing.h").read_text()
UWB_TIMING = (ROOT / "src/dwm3000_timing.c").read_text()


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


def unsigned_define(source: str, name: str) -> int:
    match = re.search(rf"^#define\s+{name}\s+(\d+)u\s*$", source, re.M)
    if match is None:
        raise AssertionError(f"missing unsigned integer define {name}")
    return int(match.group(1))


def braced_block(source: str, start: int) -> str:
    open_brace = source.index("{", start)
    depth = 1
    index = open_brace + 1
    while index < len(source) and depth:
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
        index += 1
    if depth:
        raise AssertionError("unterminated braced block")
    return source[open_brace + 1 : index - 1]


class EnumerationRxLifecycleSourceTests(unittest.TestCase):
    def test_retryable_local_survey_commands_leave_result_to_worker(self) -> None:
        for function_name in (
            "gateway_start_survey",
            "gateway_submit_survey_plan",
        ):
            handler = function_body(GATEWAY_CONTROL, function_name)
            retryable = handler.index(
                "app_gateway_command_ingress_contention_retryable(ret)"
            )
            busy = handler.index("COMMAND_BUSY", retryable)
            internal = handler.index("COMMAND_INTERNAL_ERROR", busy)

            self.assertLess(retryable, busy)
            self.assertLess(busy, internal)

    def test_forced_hop_route_adv_depth_gate_precedes_rx_queue(self) -> None:
        queue = function_body(REPORT_RX, "mesh_queue_from_frame_at_internal")
        forced_depth = queue.index(
            "CONFIG_IMEC_MESH_ROUTE_TEST_REQUIRED_GATEWAY_RELAY_HOPS"
        )
        route_adv = queue.index(
            "context.packet.msg_type == MSG_GATEWAY_ROUTE_ADV",
            forced_depth,
        )
        depth_gate = queue.index(
            "!app_mesh_c5_gateway_route_adv_rx_allowed(", route_adv
        )
        rejection = queue.index("return false;", depth_gate)
        packet_copy = queue.index("pending.packet = context.packet;")
        queue_admission = queue.index("k_msgq_put(&mesh_rx_msgq")

        self.assertLess(forced_depth, route_adv)
        self.assertLess(route_adv, depth_gate)
        self.assertLess(depth_gate, rejection)
        self.assertLess(rejection, packet_copy)
        self.assertLess(packet_copy, queue_admission)
        self.assertEqual(
            queue.count("app_mesh_c5_gateway_route_adv_rx_allowed("), 1
        )

    def test_survey_controls_reuse_enumeration_handoff_without_wake(self) -> None:
        classify = function_body(
            GATEWAY_COMMAND,
            "gateway_command_uses_compact_scheduled_flood",
        )
        relay = function_body(RELAY_CUSTODY, "build_broadcast_forward")
        send = function_body(
            REPORT_TRANSPORT, "mesh_send_c5_flood_now_until"
        )
        activation = function_body(
            REPORT_TRANSPORT, "mesh_c5_compact_scheduled_activation"
        )
        gateway = function_body(
            GATEWAY_CONTROL, "gateway_survey_send_control"
        )
        schedule = function_body(SURVEY, "survey_control_delivery_delay_ms")

        self.assertIn("command_id == CMD_SURVEY_START", classify)
        self.assertIn("command_id == CMD_SURVEY_PLAN", classify)
        self.assertIn("command_id == CMD_SURVEY_CANCEL", classify)
        self.assertIn("command_id == CMD_SURVEY_START", activation)
        self.assertNotIn("CMD_SURVEY_PLAN", activation)
        self.assertIn(
            "gateway_command_uses_compact_scheduled_flood", relay
        )
        self.assertIn(
            "compact_primary_control = enumeration_control ||", relay
        )
        self.assertIn(
            "MESH_ENUMERATION_RELAY_COPY_COUNT - 1u", relay
        )
        self.assertIn(
            "DISCOVERY_ASSIGNMENT_UPSTREAM_COPY_BURST_REMAINDER_MS", relay
        )
        self.assertIn("mesh_enumeration_relay_delay_ms", relay)

        self.assertIn(
            "compact_scheduled_control = "
            "mesh_c5_compact_scheduled_control(&tx)",
            send,
        )
        self.assertIn(
            "compact_primary_control = enumeration_control ||", send
        )
        self.assertIn(
            "compact_scheduled_activation =\n"
            "        mesh_c5_compact_scheduled_activation(&tx)",
            send,
        )
        wake_free = send.index("if (compact_scheduled_control)")
        wake_suppression = send.index("send_wake_train = false", wake_free)
        start_validation = send.index(
            "if (compact_scheduled_activation)", wake_suppression
        )
        handoff_epoch = send.index(
            "mesh_c5_survey_start_assignment_epoch(", start_validation
        )
        handoff_intent = send.index(
            "!mesh_enumeration_downstream_survey_follows", handoff_epoch
        )
        handoff_live = send.index(
            "protocol_rx_downstream_activation_needs_wake(", handoff_intent
        )
        long_activation = send.index(
            "long_gateway_activation = gateway_enumeration_claim"
        )
        attempt_loop = send.index(
            "for (uint16_t attempt = 0u; attempt < attempt_count; attempt++)"
        )
        one_activation = send.index("one_activation_per_wave", attempt_loop)
        first_attempt = send.index("attempt != 0u", one_activation)
        long_select = send.index("long_gateway_activation ?", first_attempt)
        wake_send = send.index(
            "mesh_send_route_wake_train_with_duration(", long_select
        )
        three_copy_send = send.index(
            "app_mesh_flood_send_opportunity(&tx", wake_send
        )
        self.assertIn(
            "(single_opportunity || compact_primary_control)", send
        )
        self.assertLess(wake_free, wake_suppression)
        self.assertLess(wake_suppression, start_validation)
        self.assertLess(start_validation, handoff_epoch)
        self.assertLess(handoff_epoch, handoff_intent)
        self.assertLess(handoff_intent, handoff_live)
        self.assertLess(handoff_live, long_activation)
        self.assertLess(long_activation, attempt_loop)
        self.assertLess(attempt_loop, one_activation)
        self.assertLess(one_activation, first_attempt)
        self.assertLess(first_attempt, long_select)
        self.assertLess(long_select, wake_send)
        self.assertLess(wake_send, three_copy_send)
        self.assertNotIn(
            "compact_scheduled_control && DEVICE_ROLE == ROLE_ANCHOR", send
        )
        self.assertNotIn(
            "compact_scheduled_activation && DEVICE_ROLE == ROLE_GATEWAY",
            send,
        )

        self.assertIn(
            "outbound.flood_retry_count = "
            "MESH_ENUMERATION_RELAY_COPY_COUNT - 1u",
            gateway,
        )
        self.assertIn("NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD", gateway)
        self.assertIn("SURVEY_CONTROL_ORIGIN_BUDGET_MS", schedule)
        self.assertNotIn("SURVEY_CONTROL_ACTIVATION_BUDGET_MS", schedule)
        self.assertIn("SURVEY_CONTROL_PER_HOP_BUDGET_MS", schedule)
        self.assertIn("SURVEY_CONTROL_REDUNDANCY_MS", schedule)

    def test_survey_rx_lifecycle_covers_start_plan_and_every_terminal(self) -> None:
        apply = function_body(APP_SURVEY, "app_survey_anchor_apply_control")
        work = function_body(APP_SURVEY, "anchor_work_handler")
        terminate = function_body(APP_SURVEY, "anchor_rx_terminate_locked")
        expire = function_body(APP_SURVEY, "anchor_rx_expire_locked")
        active = function_body(APP_SURVEY, "app_survey_anchor_active")
        continuous = function_body(
            APP_SURVEY, "app_survey_anchor_rx_continuous"
        )
        recovery = function_body(
            APP_SURVEY, "app_survey_anchor_rx_note_recovery"
        )
        start = apply[
            apply.index("control->phase == SURVEY_PHASE_NEIGHBOR_START") :
            apply.index("} else if (!anchor_state.active")
        ]
        plan = apply[
            apply.index("control->phase == SURVEY_PHASE_PLAN") :
            apply.index("control->phase == SURVEY_PHASE_ABORT")
        ]
        abort = apply[apply.index("control->phase == SURVEY_PHASE_ABORT") :]

        self.assertLess(
            start.index("protocol_rx_lifecycle_begin("),
            start.index("anchor_consume_enumeration_handoff"),
        )
        self.assertLess(
            start.index("anchor_consume_enumeration_handoff"),
            start.index("anchor_state.active = true"),
        )
        self.assertIn("anchor_rx_terminate_locked(false)", start)
        self.assertIn("PROTOCOL_RX_OPERATION_SURVEY", start)
        self.assertLess(
            plan.index("protocol_rx_lifecycle_set_deadline("),
            plan.index("anchor_state.plan = control->plan"),
        )
        self.assertIn("PROTOCOL_RX_OPERATION_SURVEY", plan)
        self.assertIn("protocol_rx_lifecycle_rf_begin(", work)
        self.assertIn("protocol_rx_lifecycle_rf_end(", work)
        self.assertIn("anchor_rx_terminate_locked(true)", abort)
        self.assertIn("protocol_rx_lifecycle_terminate(", terminate)
        self.assertIn("anchor_state.active = false", terminate)
        self.assertIn("anchor_rx_terminate_locked(false)", expire)
        self.assertIn("anchor_rx_expire_locked", active)
        self.assertIn("PROTOCOL_RX_MODE_CONTINUOUS_CHANNEL5", continuous)
        self.assertIn("protocol_rx_lifecycle_note_rx_recovery(", recovery)
        self.assertIn("PROTOCOL_RX_RECOVERY_TERMINATED", recovery)

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
        end_accept = end.index("anchor_enumeration_rx_finish_table(")
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
        self.assertIn(".max_attempts = 3u", bounded)
        self.assertIn(".successful_attempts_required = 3u", bounded)
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
        self.assertIn("anchor_enumeration_rx_finish_table", end)
        self.assertIn("reason=inactive-observation", end)

        begin_table = function_body(RADIO, "anchor_enumeration_rx_begin_table")
        finish_table = function_body(
            RADIO, "anchor_enumeration_rx_finish_table"
        )
        self.assertIn("anchor_enumeration_rx_table_identity_valid", begin_table)
        self.assertIn("table_command_seq", begin_table)
        self.assertIn("discovery_assignment_table_commitment_equal", begin_table)
        self.assertIn("protocol_rx_lifecycle_terminate", finish_table)
        self.assertIn("SURVEY_ENUMERATION_HANDOFF_HOLD_MS", finish_table)
        self.assertIn("protocol_rx_lifecycle_set_deadline", finish_table)
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

        self.assertIn("anchor_enumeration_rx_begin(", claim)
        self.assertLess(
            late_table.index("anchor_enumeration_rx_begin_table"),
            late_table.index("anchor_schedule_late_discovery_claim"),
        )
        self.assertIn("anchor_enumeration_rx_begin_table", replay)

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
        rearm_start = scan.index("protocol_rx_window:")
        rearm = scan[
            rearm_start : scan.index("preamble_detected =", rearm_start)
        ]

        self.assertIn("dwm3000_driver_receive_frame_continuous_extend_on_activity", rearm)
        self.assertIn("goto protocol_rx_window", rearm)
        self.assertGreaterEqual(scan.count("goto protocol_rx_window"), 3)
        self.assertIn(
            "scan_rx_elapsed_us = u32_saturating_add(scan_rx_elapsed_us,",
            scan,
        )
        self.assertNotIn("dwm3000_driver_configure", rearm)
        self.assertNotIn("anchor_enter_low_power", rearm)
        self.assertIn(
            "protocol_continuous_rx ? APP_RADIO_LOW_POWER_IDLE", scan
        )
        self.assertIn(
            "survey_continuous_rx = app_survey_anchor_rx_continuous()", scan
        )
        self.assertIn(
            "protocol_continuous_rx = enumeration_continuous_rx ||", scan
        )
        self.assertIn("next_scan_delay_ms = 0u", scan)

    def test_every_generic_enumeration_reentry_rechecks_compact_prepare(
        self,
    ) -> None:
        scan = function_body(RADIO, "anchor_uwb_scan_work_handler")
        reentry = scan.index("protocol_rx_window:")
        generic_rx = scan.index(
            "dwm3000_driver_receive_frame_continuous_extend_on_activity(",
            reentry,
        )
        gate = scan[reentry:generic_rx]

        current_clock = gate.index(
            "enumeration_now_ms = (uint64_t)k_uptime_get()"
        )
        compact_check = gate.index(
            "anchor_compact_enumeration_window(", current_clock
        )
        prepare_check = gate.index(
            "anchor_compact_enumeration_prepare_window(", compact_check
        )
        compact_switch = gate.index(
            "anchor_run_compact_enumeration_lane(", prepare_check
        )
        current_start_distance = gate.index(
            "anchor_compact_enumeration_ms_until_prepare(", compact_switch
        )
        receive_cap = gate.index(
            "scan_rx_ms = compact_lane_start_wait_ms", current_start_distance
        )

        self.assertIn("if (enumeration_continuous_rx)", gate)
        self.assertIn("enumeration_now_ms, NULL, NULL", gate)
        self.assertIn("(\n                    enumeration_now_ms);", gate)
        self.assertLess(current_clock, compact_check)
        self.assertLess(compact_check, prepare_check)
        self.assertLess(prepare_check, compact_switch)
        self.assertLess(compact_switch, current_start_distance)
        self.assertLess(current_start_distance, receive_cap)
        self.assertGreaterEqual(scan.count("goto protocol_rx_window"), 3)

    def test_anchor_compact_lane_prearms_rx_without_early_tx(self) -> None:
        prepare = function_body(
            RADIO, "anchor_compact_enumeration_prepare_window"
        )
        until_prepare = function_body(
            RADIO, "anchor_compact_enumeration_ms_until_prepare"
        )
        run = function_body(RADIO, "anchor_run_compact_enumeration_lane")
        scan = function_body(RADIO, "anchor_uwb_scan_work_handler")

        self.assertEqual(
            unsigned_define(
                ENUMERATION_LANE, "ENUMERATION_RESPONSE_ANCHOR_PREPARE_MS"
            ),
            40,
        )
        for helper in (prepare, until_prepare):
            self.assertIn(
                "start_ms >\n"
                "                    ENUMERATION_RESPONSE_ANCHOR_PREPARE_MS",
                helper,
            )
            self.assertIn(
                "start_ms -\n"
                "                    ENUMERATION_RESPONSE_ANCHOR_PREPARE_MS",
                helper,
            )
        self.assertIn("now_ms >= prepare_start_ms", prepare)
        self.assertIn("prepare_start_ms - now_ms", until_prepare)

        self.assertEqual(
            scan.count("anchor_compact_enumeration_prepare_window("), 3
        )
        self.assertGreaterEqual(
            scan.count("anchor_compact_enumeration_window("), 3
        )
        self.assertEqual(
            scan.count("anchor_compact_enumeration_ms_until_prepare("), 2
        )

        lane_started = run.index("bool lane_started = now_ms >= config.start_ms")
        prestart = run.index("if (!lane_started)", lane_started)
        prestart_block = braced_block(run, prestart)
        receive = run.index(
            "dwm3000_driver_receive_frame_continuous(", prestart
        )
        timeout = run.index("if (ret == -ETIMEDOUT)", receive)
        timeout_block = braced_block(run, timeout)

        self.assertIn("round_deadline_ms = config.start_ms", prestart_block)
        self.assertIn("receive_deadline_ms = config.start_ms", prestart_block)
        self.assertNotIn("anchor_compact_enumeration_try_tx", prestart_block)
        self.assertLess(prestart, receive)
        self.assertIn("remaining_ms", run[receive : run.index(");", receive)])
        self.assertIn("continue;", timeout_block)
        self.assertIn(
            "if (!lane_started)", run[receive:timeout]
        )

    def test_continuous_listener_uses_extended_phr_and_protocol_max_slice(self) -> None:
        scan = function_body(RADIO, "anchor_uwb_scan_work_handler")
        selection_start = scan.index(
            "enumeration_continuous_rx = anchor_enumeration_rx_active()"
        )
        select = scan[
            selection_start : scan.index("route_waiting_active =", selection_start)
        ]
        configure = scan[
            scan.index("ret = protocol_continuous_rx ?") :
            scan.index("if (ret == 0)", scan.index("ret = protocol_continuous_rx ?"))
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

    def test_compact_lane_keeps_anchor_and_gateway_on_extended_phr(self) -> None:
        anchor_lane = function_body(
            RADIO, "anchor_run_compact_enumeration_lane"
        )
        gateway_lane = function_body(
            REPORT_RX, "mesh_gateway_run_enumeration_response_slice"
        )

        self.assertEqual(
            anchor_lane.count(
                "dwm3000_driver_configure_wake_mesh_control_mode()"
            ),
            1,
        )
        self.assertEqual(
            gateway_lane.count(
                "dwm3000_driver_configure_wake_mesh_control_mode()"
            ),
            3,
        )
        self.assertNotIn(
            "dwm3000_driver_configure_wake_mode()", anchor_lane
        )
        self.assertNotIn(
            "dwm3000_driver_configure_wake_mode()", gateway_lane
        )

        ch5_control_profile = UWB_TIMING[
            UWB_TIMING.index("[DWM3000_TIMING_PHY_CH5_MESH_CONTROL]") :
            UWB_TIMING.index("[DWM3000_TIMING_PHY_CH9_MESH]")
        ]
        self.assertIn(
            "DWM3000_TIMING_EXTENDED_PSDU_MAX_BYTES - "
            "DWM3000_TIMING_FCS_BYTES",
            ch5_control_profile,
        )
        self.assertIn(
            ".phr_mode = DWM3000_TIMING_PHR_EXTENDED",
            ch5_control_profile,
        )

        extended_payload_cap = (
            unsigned_define(UWB, "UWB_PHY_EXTENDED_FRAME_MAX_LEN")
            - unsigned_define(UWB, "UWB_PHY_FCS_LEN")
        )
        max_control_frame = (
            unsigned_define(UWB, "UWB_MESH_FRAME_HEADER_LEN")
            + unsigned_define(PROTOCOL, "PACKET_EXT_HEADER_LEN")
            + unsigned_define(PROTOCOL, "PACKET_EXT_MAX_PAYLOAD_LEN")
            + unsigned_define(PROTOCOL, "PACKET_CRC_LEN")
            + unsigned_define(UWB, "UWB_FRAME_CRC_LEN")
        )
        compact_bundle = (
            unsigned_define(UWB, "UWB_ENUM_BUNDLE_BASE_LEN")
            + unsigned_define(UWB, "UWB_ENUM_RECORDS_PER_BUNDLE")
            * unsigned_define(UWB, "UWB_ENUM_RECORD_LEN")
        )
        compact_ack = unsigned_define(UWB, "UWB_ENUM_HOP_ACK_LEN")

        self.assertLessEqual(max_control_frame, extended_payload_cap)
        self.assertLessEqual(compact_bundle, extended_payload_cap)
        self.assertLessEqual(compact_ack, extended_payload_cap)

    def test_compact_bundle_acks_have_no_fixed_turnaround_delay(self) -> None:
        anchor_ack = function_body(
            RADIO, "anchor_compact_enumeration_handle_raw"
        )
        gateway_lane = function_body(
            REPORT_RX, "mesh_gateway_run_enumeration_response_slice"
        )

        for ack_path in (anchor_ack, gateway_lane):
            encode = ack_path.index("uwb_encode_enumeration_hop_ack(")
            send = ack_path.index(
                "dwm3000_driver_send_frame_tracked_until(", encode
            )
            ack_turnaround = ack_path[encode:send]

            self.assertNotIn("k_sleep(", ack_turnaround)
            self.assertNotIn("k_msleep(", ack_turnaround)
            self.assertNotIn("ACK_TURNAROUND", ack_turnaround)
            self.assertNotIn("ACK_TX_BUDGET", ack_turnaround)

        self.assertNotIn("ANCHOR_ENUMERATION_ACK_TURNAROUND_MS", RADIO)
        self.assertNotIn("ANCHOR_ENUMERATION_ACK_TX_BUDGET_MS", RADIO)
        self.assertNotIn("GATEWAY_ENUMERATION_ACK_TURNAROUND_MS", REPORT_RX)
        self.assertNotIn("GATEWAY_ENUMERATION_ACK_TX_BUDGET_MS", REPORT_RX)
        self.assertIn("DBG_ENUM_COMPACT_ACK_TX", anchor_ack)
        self.assertIn("DBG_ENUM_COMPACT_ACK_SKIP", anchor_ack)
        self.assertIn("DBG_ENUM_COMPACT_GATEWAY_ACK_TX", gateway_lane)
        self.assertIn("DBG_ENUM_COMPACT_GATEWAY_ACK_SKIP", gateway_lane)

    def test_only_valid_current_epoch_bundles_advance_empty_band_proof(
        self,
    ) -> None:
        handle = function_body(
            GATEWAY_CONTROL,
            "app_gateway_enumeration_response_handle_bundle",
        )
        note_record = function_body(
            GATEWAY_CONTROL,
            "gateway_discovery_assignment_note_compact_record_locked",
        )

        self.assertIn("bool valid_bundle_activity = false;", handle)
        self.assertEqual(handle.count("valid_bundle_activity = true;"), 1)
        epoch_check = handle.index(
            "bundle->epoch != gateway_discovery_assignment_state.epoch"
        )
        timing_check = handle.index(
            "enumeration_response_timing_at_depth(", epoch_check
        )
        stale_exit = handle.index("goto out;", timing_check)
        note = handle.index(
            "gateway_discovery_assignment_note_compact_record_locked("
        )
        reject = handle.index("if (note_ret < 0)", note)
        reject_exit = handle.index("goto out;", reject)
        activity = handle.index("valid_bundle_activity = true;", reject_exit)
        depth_guard = handle.index("if (valid_bundle_activity)", activity)

        self.assertLess(epoch_check, stale_exit)
        self.assertLess(stale_exit, note)
        self.assertLess(note, reject)
        self.assertLess(reject, reject_exit)
        self.assertLess(reject_exit, activity)
        self.assertLess(activity, depth_guard)
        self.assertNotIn(
            "APP_GATEWAY_SEMANTIC_ACCEPT_NEW", handle[reject_exit:activity]
        )
        self.assertNotIn(
            "APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE",
            handle[reject_exit:activity],
        )
        self.assertRegex(
            note_record,
            r"(?s)anchor_hop_counts\[anchor_index\]\s*!=\s*"
            r"record->hop_count\s*\)\s*\{\s*return\s+-EBADMSG\s*;\s*\}"
            r".*return\s+APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE\s*;",
        )

        depth_block = braced_block(handle, depth_guard)
        self.assertIn("uint8_t observed_hop_count", depth_block)
        self.assertIn("MAX(observed_hop_count, timing.depth)", depth_block)
        self.assertIn("active_hop_count + 1u", depth_block)
        self.assertIn("claim_collection_deadline_ms =", depth_block)
        self.assertIn('"compact-next-depth"', depth_block)

        ack = handle.index("*ack =", depth_guard)
        post_ack = handle[ack:]
        self.assertNotIn("expected_claim_count", post_ack)
        self.assertNotIn("claim_collection_deadline_ms =", post_ack)
        self.assertNotIn('"compact-roster-quiet"', handle)

    def test_expected_roster_never_shortens_adaptive_compact_deadline(
        self,
    ) -> None:
        handle = function_body(
            GATEWAY_CONTROL,
            "app_gateway_enumeration_response_handle_bundle",
        )
        activity = handle.index("if (valid_bundle_activity)")
        adaptive = braced_block(handle, activity)
        ack = handle.index("*ack =", activity)
        post_ack = handle[ack:]

        self.assertIn("depth_deadline >", adaptive)
        self.assertIn(
            "gateway_discovery_assignment_state.claim_collection_deadline_ms",
            adaptive,
        )
        self.assertIn("claim_collection_deadline_ms =", adaptive)
        self.assertNotIn("expected_claim_count", post_ack)
        self.assertNotIn("claim_collection_deadline_ms =", post_ack)
        self.assertNotIn("gateway_discovery_assignment_reschedule", post_ack)

    def test_known_incomplete_roster_aborts_before_table_publication(
        self,
    ) -> None:
        missing = function_body(
            GATEWAY_CONTROL,
            "gateway_discovery_assignment_expected_claims_missing_locked",
        )
        publish = function_body(
            GATEWAY_CONTROL,
            "gateway_discovery_assignment_publish_work_handler",
        )
        finalize = function_body(
            GATEWAY_CONTROL,
            "gateway_discovery_assignment_finalize_work_handler",
        )

        self.assertIn("expected_claim_count != 0u", missing)
        self.assertRegex(
            missing,
            r"gateway_discovery_assignment_current_claim_count_locked\s*"
            r"\(\)\s*!=\s*0u",
        )
        self.assertRegex(
            missing,
            r"gateway_discovery_assignment_current_claim_count_locked\s*"
            r"\(\)\s*<\s*\n?\s*"
            r"gateway_discovery_assignment_state.expected_claim_count",
        )

        collect = publish.index(
            "GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS"
        )
        lane_close = publish.index(
            "response_lane_active = false", collect
        )
        missing_check = publish.index(
            "gateway_discovery_assignment_expected_claims_missing_locked()",
            lane_close,
        )
        missing_block = braced_block(publish, missing_check)
        first_table = publish.index(
            "gateway_discovery_assignment_publish_table()", missing_check
        )
        self.assertIn("gateway_discovery_assignment_fail_locked(", missing_block)
        self.assertIn("GATEWAY_ASSIGNMENT_PUBLISH_RETURN()", missing_block)
        self.assertLess(missing_check, first_table)

        finalize_missing = finalize.index(
            "gateway_discovery_assignment_expected_claims_missing_locked()"
        )
        finalize_block = braced_block(finalize, finalize_missing)
        self.assertIn(
            "gateway_discovery_assignment_fail_locked(", finalize_block
        )
        self.assertIn("GATEWAY_ASSIGNMENT_FINALIZE_RETURN()", finalize_block)

        final_table = publish.rindex(
            "gateway_discovery_assignment_publish_table()"
        )
        unknown_fallback = publish[
            publish.rindex(
                "gateway_discovery_assignment_current_claim_count_locked() == 0u",
                collect,
                final_table,
            ) : final_table
        ]
        self.assertIn("gateway_discovery_assignment_fail_locked(", unknown_fallback)
        self.assertNotIn("expected_claim_count", unknown_fallback)

    def test_assignment_start_handoff_failure_finishes_without_abort(self) -> None:
        validate = function_body(
            GATEWAY_CONTROL,
            "gateway_discovery_assignment_validate_host_options",
        )
        start = function_body(
            GATEWAY_CONTROL,
            "gateway_start_discovery_assignment",
        )
        handoff = start.index('"assignment-start"')
        failure = braced_block(start, start.index("if (ret < 0)", handoff))

        self.assertIn(
            "gateway_discovery_assignment_finish_failure_locked(",
            failure,
        )
        self.assertNotIn(
            "gateway_discovery_assignment_fail_locked(",
            failure,
        )
        self.assertIn("struct gateway_command_options host_options;", validate)
        self.assertIn(
            "gateway_discovery_assignment_validate_host_options(",
            start,
        )
        self.assertNotIn("struct gateway_command_options host_options;", start)

    def test_compact_semantic_lane_survives_scanner_reentry(self) -> None:
        owner = ANCHOR.index(
            "static struct enumeration_response_lane "
            "anchor_enumeration_response_lane;"
        )
        owner_guard = ANCHOR.rfind(
            "#if DEVICE_ROLE == ROLE_ANCHOR", 0, owner
        )
        owner_end = ANCHOR.index("#endif", owner)
        lane_state = braced_block(
            ENUMERATION_LANE,
            ENUMERATION_LANE.index("struct enumeration_response_lane"),
        )
        run = function_body(RADIO, "anchor_run_compact_enumeration_lane")
        resume = function_body(
            RADIO, "anchor_compact_enumeration_lane_resume"
        )
        start = function_body(
            COMMANDS, "anchor_start_compact_enumeration_response_lane"
        )

        self.assertGreater(owner_guard, -1)
        self.assertGreater(owner_end, owner)
        self.assertEqual(
            ANCHOR.count("anchor_enumeration_response_lane;"), 1
        )
        self.assertNotIn("encoded", lane_state)
        self.assertNotIn("frame", lane_state)
        self.assertNotIn("UWB_ENUM_BUNDLE_MAX_LEN", lane_state)
        self.assertNotIn("UWB_ENUM_HOP_ACK_LEN", lane_state)

        self.assertIn("&anchor_enumeration_response_lane", run)
        self.assertIn("anchor_compact_enumeration_lane_resume(&config)", run)
        self.assertNotRegex(
            run, r"struct\s+enumeration_response_lane\s+lane\s*;"
        )
        self.assertNotIn("enumeration_response_lane_begin(", run)
        self.assertNotIn("enumeration_response_lane_stop(", run)

        exact = resume.index("if (lane->active")
        exact_return = resume.index("return PROTO_OK;", exact)
        begin = resume.index("enumeration_response_lane_begin(")
        for identity in (
            "lane->network_id == NETWORK_ID",
            "lane->epoch == config->epoch",
            "enumeration_response_lane_local_id(lane) == DEVICE_ID",
            "lane->parent_id == config->parent_id",
            "lane->hop_count == config->hop_count",
            "lane->start_ms == config->start_ms",
        ):
            self.assertIn(identity, resume[exact:exact_return])
        self.assertLess(exact_return, begin)
        self.assertEqual(resume.count("enumeration_response_lane_begin("), 1)
        self.assertEqual(RADIO.count("enumeration_response_lane_begin("), 1)
        self.assertNotIn("enumeration_response_lane_stop(", resume)
        self.assertNotIn("memset(", resume)

        active_replay = start[
            start.index("if (anchor_enumeration_response_config.active") :
            start.index("} else {", start.index(
                "if (anchor_enumeration_response_config.active"
            ))
        ]
        for identity in (
            "anchor_enumeration_response_config.epoch == epoch",
            "anchor_enumeration_response_config.hop_count == hop_count",
            "anchor_enumeration_response_config.max_hop_count ==",
            "anchor_enumeration_response_config.start_ms == start_ms",
            "anchor_enumeration_response_config.parent_id ==",
            "selected->next_hop_id",
        ):
            self.assertIn(identity, active_replay)
        self.assertIn("PROTO_OK : PROTO_ERR_STALE", active_replay)

        begin_table = function_body(RADIO, "anchor_enumeration_rx_begin_table")
        terminate_claim = function_body(
            RADIO, "anchor_enumeration_rx_terminate_claim"
        )
        finish_table = function_body(
            RADIO, "anchor_enumeration_rx_finish_table"
        )
        for terminal in (begin_table, terminate_claim, finish_table):
            self.assertEqual(
                terminal.count("anchor_compact_enumeration_deactivate(epoch)"),
                1,
            )
        self.assertNotIn(
            "anchor_enumeration_response_lane", terminate_claim
        )
        self.assertNotIn(
            "anchor_enumeration_response_lane", finish_table
        )

    def test_enumeration_transport_owns_exactly_one_physical_send_per_copy(self) -> None:
        send = function_body(
            REPORT_TRANSPORT, "mesh_send_c5_flood_now_until"
        )
        opportunity = function_body(
            APP_MESH_FLOOD, "app_mesh_flood_send_opportunity"
        )
        forward = function_body(RELAY_CUSTODY, "build_broadcast_forward")
        gateway_origin = function_body(
            GATEWAY_CONTROL, "gateway_build_discovery_assignment_command"
        )
        gateway_hold = function_body(
            GATEWAY_CONTROL,
            "gateway_discovery_assignment_propagation_hold_ms_locked",
        )
        copy_count = unsigned_define(
            MESH, "MESH_ENUMERATION_RELAY_COPY_COUNT"
        )

        attempts = send.index(
            "attempt_count = 1u + tx.flood_retry_count;"
        )
        loop = send.index("attempt < attempt_count", attempts)
        select = send.index(
            "(single_opportunity || compact_primary_control)", loop
        )
        one_copy = send.index(
            "app_mesh_flood_send_opportunity", select
        )
        generic_flood = send.index(
            "app_mesh_command_orchestrator_serialize_flood", one_copy
        )
        next_attempt = send.index("attempt + 1u < attempt_count", generic_flood)
        diversification = send.index(
            "mesh_flood_copy_diversification_ms", next_attempt
        )
        enumeration_guard = send.index(
            "MESH_ENUMERATION_RELAY_COPY_GUARD_MS", diversification
        )

        self.assertLess(attempts, loop)
        self.assertLess(loop, select)
        self.assertLess(select, one_copy)
        self.assertLess(one_copy, generic_flood)
        self.assertLess(generic_flood, next_attempt)
        self.assertLess(next_attempt, diversification)
        self.assertLess(diversification, enumeration_guard)
        self.assertIn(
            "app_mesh_flood_send_resume_limit(out, ops, &progress, result, 1u)",
            opportunity,
        )
        self.assertIn(
            "compact_primary_control = enumeration_control ||", forward
        )
        self.assertIn("compact_primary_control ?", forward)
        self.assertIn(
            "MESH_ENUMERATION_RELAY_COPY_COUNT - 1u", forward
        )
        self.assertIn("memset(outbound, 0, sizeof(*outbound))", gateway_origin)
        self.assertNotIn("flood_retry_count", gateway_origin)
        self.assertIn(
            "discovery_assignment_control_propagation_hold_ms(hop_count)",
            gateway_hold,
        )
        self.assertRegex(
            MESH,
            r"#define\s+MESH_ENUMERATION_RELAY_COPY_TAIL_MS\s*\\\s*"
            r"\(\(MESH_ENUMERATION_RELAY_COPY_COUNT\s*-\s*1u\)\s*\*\s*\\\s*"
            r"\s*MESH_ENUMERATION_RELAY_COPY_SPACING_MAX_MS\)",
        )

        gateway_retry_count = 0
        relay_retry_count = copy_count - 1
        self.assertEqual(copy_count, 3)
        self.assertEqual(1 + gateway_retry_count, 1)
        self.assertEqual(1 + relay_retry_count, 3)
        old_nested_flood_count = (1 + relay_retry_count) * 4
        self.assertEqual(old_nested_flood_count, 12)
        self.assertNotEqual(old_nested_flood_count, copy_count)

    def test_table_and_end_root_copy_bursts_finish_before_anchor_relays(
        self,
    ) -> None:
        submit = function_body(
            GATEWAY_CONTROL,
            "gateway_discovery_assignment_submit_control_flood_locked",
        )
        send = function_body(
            REPORT_TRANSPORT, "mesh_send_c5_flood_now_until"
        )
        quick_root = function_body(
            REPORT_TRANSPORT,
            "mesh_c5_gateway_enumeration_quick_copy_burst",
        )
        node_comm = function_body(
            APP_NODE_COMM, "app_node_comm_service_deliveries"
        )
        forward = function_body(RELAY_CUSTODY, "build_broadcast_forward")
        copy_count = unsigned_define(
            MESH, "MESH_ENUMERATION_RELAY_COPY_COUNT"
        )

        retry_assignment = submit.index(
            "outbound->flood_retry_count ="
        )
        retry_guard = submit.rfind("if (", 0, retry_assignment)
        retry_condition = submit[
            retry_guard : submit.index("{", retry_guard)
        ]
        retry_block = braced_block(submit, retry_guard)
        self.assertIn(
            "GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE",
            retry_condition,
        )
        self.assertIn(
            "GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_END",
            retry_condition,
        )
        self.assertNotIn(
            "GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM",
            retry_condition,
        )
        self.assertNotIn(
            "GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_ABORT",
            retry_condition,
        )
        self.assertIn(
            "outbound->flood_retry_count =\n"
            "            MESH_ENUMERATION_RELAY_COPY_COUNT - 1u",
            retry_block,
        )
        self.assertEqual(
            submit.count("outbound->flood_retry_count ="),
            1,
        )
        profile = submit.index(
            "NODE_COMM_PROFILE_SINGLE_CONTROL_ORIGIN", retry_assignment
        )
        delivery = submit.index("app_node_comm_submit_delivery(", profile)
        self.assertLess(retry_assignment, profile)
        self.assertLess(profile, delivery)

        single_origin = node_comm.index(
            "attempt_record.profile == NODE_COMM_PROFILE_SINGLE_CONTROL_ORIGIN"
        )
        view_send = node_comm.index(
            "mesh_try_send_c5_flood_view(", single_origin
        )
        self.assertLess(single_origin, view_send)

        attempts = send.index("attempt_count = 1u + tx.flood_retry_count")
        physical = send.index(
            "(single_opportunity || compact_primary_control)", attempts
        )
        one_copy = send.index("app_mesh_flood_send_opportunity", physical)
        self.assertLess(attempts, physical)
        self.assertLess(physical, one_copy)

        self.assertIn("#if DEVICE_ROLE == ROLE_GATEWAY", quick_root)
        self.assertIn("DISCOVERY_ASSIGNMENT_PHASE_TABLE", quick_root)
        self.assertIn("DISCOVERY_ASSIGNMENT_PHASE_END", quick_root)
        self.assertNotIn("DISCOVERY_ASSIGNMENT_PHASE_CLAIM", quick_root)
        root_classification = send.index(
            "mesh_c5_gateway_enumeration_quick_copy_burst(&tx)"
        )
        copy_gap = send.index(
            "tx.packet.msg_type == MSG_GATEWAY_ROUTE_ADV ||",
            root_classification,
        )
        short_gap = send.index(
            "MESH_GATEWAY_ROUTE_ADV_COPY_GAP_MAX_MS + 1u",
            copy_gap,
        )
        relay_guard = send.index(
            "MESH_ENUMERATION_RELAY_COPY_GUARD_MS", short_gap
        )
        self.assertLess(root_classification, copy_gap)
        self.assertLess(copy_gap, short_gap)
        self.assertLess(short_gap, relay_guard)

        schedule = forward.index("out->earliest_tx_ms = now_ms +")
        upstream_burst = forward.index(
            "DISCOVERY_ASSIGNMENT_UPSTREAM_COPY_BURST_REMAINDER_MS",
            schedule,
        )
        relay_slot = forward.index(
            "mesh_enumeration_relay_delay_ms", upstream_burst
        )
        self.assertLess(schedule, upstream_burst)
        self.assertLess(upstream_burst, relay_slot)
        self.assertEqual(copy_count, 3)

    def test_gateway_table_end_burst_keeps_one_handoff_and_requires_all_copies(
        self,
    ) -> None:
        view_send = function_body(
            REPORT_TRANSPORT, "mesh_try_send_c5_flood_view"
        )
        burst_send = function_body(
            REPORT_TRANSPORT, "mesh_send_c5_flood_now_until"
        )
        physical_send = function_body(
            REPORT_TRANSPORT,
            "mesh_send_outbound_with_release_on_channel_until",
        )
        restart_scan = function_body(
            REPORT_TRANSPORT, "mesh_restart_role_scan"
        )
        burst_active = function_body(
            REPORT_TRANSPORT, "mesh_c5_enumeration_relay_burst_active"
        )
        burst_begin = function_body(
            REPORT_TRANSPORT, "mesh_c5_enumeration_relay_burst_begin"
        )
        burst_end = function_body(
            REPORT_TRANSPORT, "mesh_c5_enumeration_relay_burst_end"
        )
        defer = function_body(
            REPORT_TRANSPORT, "mesh_c5_flood_defer_active_cb"
        )

        # The gateway's outer RX-to-control handoff encloses the complete
        # logical flood call, rather than one physical-copy callback.
        handoff_begin = view_send.index("mesh_rx_handoff_begin_control(")
        logical_send = view_send.index(
            "mesh_send_c5_flood_now_until(", handoff_begin
        )
        handoff_end = view_send.index(
            "mesh_rx_handoff_end_control()", logical_send
        )
        final_rearm = view_send.index("mesh_restart_role_scan()", handoff_end)
        self.assertLess(handoff_begin, logical_send)
        self.assertLess(logical_send, handoff_end)
        self.assertLess(handoff_end, final_rearm)

        # TABLE/END and relayed anchor controls share one ref-counted burst
        # owner. It is acquired before the copy loop and released after the
        # all-copies completeness check on every exit.
        quick = burst_send.index(
            "mesh_c5_gateway_enumeration_quick_copy_burst(&tx)"
        )
        owner_if = burst_send.index(
            "if (compact_primary_control || gateway_enumeration_quick_copies)",
            quick,
        )
        owner_condition = burst_send[owner_if:burst_send.index("{", owner_if)]
        owner_begin = burst_send.index(
            "mesh_c5_enumeration_relay_burst_begin(&tx)", owner_if
        )
        copy_loop = burst_send.index(
            "attempt < attempt_count", owner_begin
        )
        completeness = burst_send.index(
            "aggregate_result.sent_count != attempt_count", copy_loop
        )
        out_label = burst_send.index("out:", completeness)
        owner_end = burst_send.index(
            "mesh_c5_enumeration_relay_burst_end()", out_label
        )
        self.assertIn("compact_primary_control", owner_condition)
        self.assertIn(
            "compact_primary_control = enumeration_control ||",
            burst_send[:owner_if],
        )
        self.assertIn("gateway_enumeration_quick_copies", owner_condition)
        self.assertLess(owner_begin, copy_loop)
        self.assertLess(copy_loop, completeness)
        self.assertLess(completeness, out_label)
        self.assertLess(out_label, owner_end)
        self.assertEqual(
            burst_send.count("mesh_c5_enumeration_relay_burst_begin(&tx)"),
            1,
        )
        self.assertEqual(
            burst_send.count("mesh_c5_enumeration_relay_burst_end()"),
            1,
        )

        # A physical-copy send still reaches its generic restart call, so the
        # role restart itself must reject rearm while the burst ref is live.
        nested_rearm = physical_send.index("mesh_restart_role_scan()")
        active_guard = restart_scan.index(
            "if (mesh_c5_enumeration_relay_burst_active())"
        )
        ordinary_rearm = restart_scan.index("mesh_start_uwb_rx(", active_guard)
        self.assertGreater(nested_rearm, 0)
        self.assertLess(active_guard, ordinary_rearm)
        self.assertIn(
            "atomic_get(&mesh_c5_enumeration_relay_burst_count) != 0",
            burst_active,
        )
        self.assertIn(
            "atomic_inc(&mesh_c5_enumeration_relay_burst_count)", burst_begin
        )
        self.assertIn(
            "atomic_dec(&mesh_c5_enumeration_relay_burst_count)", burst_end
        )

        # Pending scanner work cannot split an already-admitted quick burst,
        # and a failed copy cannot be rewritten as success merely because an
        # earlier copy reached RF.
        quick_defer = defer.index(
            "mesh_c5_enumeration_relay_burst_active()"
        )
        no_defer = defer.index("return false;", quick_defer)
        pending_rx = defer.index("mesh_rx_pending_count()", no_defer)
        failed_copy = burst_send.index("if (ret != 0)", copy_loop)
        failed_block = braced_block(burst_send, failed_copy)
        incomplete_if = burst_send.rfind("if (", copy_loop, completeness)
        incomplete_condition = burst_send[
            incomplete_if:burst_send.index("{", incomplete_if)
        ]
        incomplete_block = braced_block(burst_send, incomplete_if)
        self.assertIn(
            "mesh_c5_gateway_enumeration_quick_copy_burst(",
            defer[quick_defer:no_defer],
        )
        self.assertLess(quick_defer, no_defer)
        self.assertLess(no_defer, pending_rx)
        self.assertIn("!gateway_enumeration_quick_copies", failed_block)
        self.assertIn("aggregate_result.sent_count > 0u", failed_block)
        self.assertIn("ret = 0", failed_block)
        self.assertIn(
            "gateway_enumeration_quick_copies", incomplete_condition
        )
        self.assertIn("ret == 0", incomplete_condition)
        self.assertIn(
            "aggregate_result.sent_count != attempt_count",
            incomplete_condition,
        )
        self.assertIn("ret = -EIO", incomplete_block)

    def test_gateway_command_relay_wave_keeps_all_admitted_copies_atomic(
        self,
    ) -> None:
        send = function_body(
            REPORT_TRANSPORT, "mesh_send_c5_flood_now_until"
        )
        defer = function_body(
            REPORT_TRANSPORT, "mesh_c5_flood_defer_active_cb"
        )
        quiet = function_body(
            REPORT_TRANSPORT, "mesh_c5_flood_quiet_cb"
        )
        relay = function_body(
            REPORT_ROUTE_CONTROL, "mesh_send_c5_flood_response"
        )
        wake = function_body(
            REPORT_ROUTE_CONTROL,
            "mesh_send_route_wake_train_with_duration",
        )

        # Every gateway command owns the primary no-politeness wave, including
        # enumeration. Enumeration still retains its separate lifecycle and
        # one-physical-copy-per-attempt path below.
        enumeration = send.index(
            "enumeration_control = mesh_c5_flood_enumeration_identity("
        )
        classify = send.index("atomic_gateway_control =", enumeration)
        install = send.index(
            "flood_ctx.atomic_gateway_control = atomic_gateway_control",
            classify,
        )
        classification = send[classify:install]
        self.assertIn(
            "purpose == C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD",
            classification,
        )
        self.assertNotIn("enumeration_control", classification)

        # Once admitted, neither politeness callback may listen and mistake a
        # sibling's copy for unrelated work.
        for body, bypass_action, ordinary_action in (
            (defer, "return false;", "mesh_rx_pending_count()"),
            (quiet, "return true;", "mesh_stop_role_scan()"),
        ):
            atomic = body.index(
                "flood_ctx != NULL && flood_ctx->atomic_gateway_control"
            )
            bypass = body.index(bypass_action, atomic)
            ordinary = body.index(ordinary_action, bypass + 1)
            self.assertLess(atomic, bypass)
            self.assertLess(bypass, ordinary)

        # The atomic branch owns a resumable four-copy opportunity for each
        # outer relay attempt and reports success only when every copy exists.
        loop = send.index("attempt < attempt_count", install)
        progress = send.index(
            "struct app_mesh_flood_progress attempt_progress = {0}", loop
        )
        enumeration_path = send.index(
            "(single_opportunity || compact_primary_control)", progress
        )
        atomic_path = send.index("atomic_gateway_control ?", enumeration_path)
        bounded = send.index(
            "app_mesh_flood_send_bounded_resume", atomic_path
        )
        background = send.index(
            "app_mesh_command_orchestrator_serialize_flood", bounded
        )
        self.assertLess(enumeration_path, atomic_path)
        self.assertLess(atomic_path, bounded)
        self.assertLess(bounded, background)
        self.assertIn("&attempt_progress", send[bounded:background])

        failed = send.index("if (ret != 0)", background)
        failed_block = braced_block(send, failed)
        self.assertIn("!atomic_gateway_control", failed_block)
        self.assertIn("aggregate_result.sent_count > 0u", failed_block)
        expected = send.index(
            "if (atomic_gateway_control && ret == 0)", failed
        )
        expected_block = braced_block(send, expected)
        self.assertIn("app_mesh_flood_repeat_limit()", expected_block)
        self.assertIn(
            "(single_opportunity || compact_primary_control) ? 1u",
            expected_block,
        )
        self.assertIn(
            "expected_copies = attempt_count * copies_per_attempt",
            expected_block,
        )
        self.assertIn(
            "aggregate_result.sent_count != expected_copies",
            expected_block,
        )
        self.assertIn("ret = -EIO", expected_block)

        # A partial physical wave is retained for retry instead of being
        # acknowledged as forwarding success.
        accepted = relay.index("if (ret == 0 && result.sent_count > 0u)")
        retained = relay.index("*forward_admission_retained = true", accepted)
        deferred = relay.index("mesh_c5_flood_store_deferred(", retained)
        self.assertLess(accepted, retained)
        self.assertLess(retained, deferred)

        # The same purpose-only exclusion covers the wake train, including an
        # enumeration activation train, without command-specific decoding.
        wake_atomic = wake.index("bool atomic_gateway_control =")
        wake_classification = wake[
            wake_atomic : wake.index(";", wake_atomic) + 1
        ]
        self.assertIn(
            "purpose == C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD",
            wake_classification,
        )
        self.assertNotIn("authorization_candidate", wake_classification)
        self.assertNotIn("CMD_ASSIGN_DISCOVERY_SLOTS", wake_classification)

        pre = wake.index("if (!atomic_gateway_control)", wake_atomic)
        pre_sniff = wake.index(
            'mesh_route_wake_sniff_activity("pre"', pre
        )
        train = wake.index(
            "if (!atomic_gateway_control && local_can_range_clicks", pre_sniff
        )
        train_listen = wake.index("mesh_route_wake_listen_for_click", train)
        post = wake.index(
            "if (!atomic_gateway_control && ret >= 0 && sent_count > 0u)",
            train_listen,
        )
        post_sniff = wake.index(
            'mesh_route_wake_sniff_activity("post"', post
        )
        self.assertLess(pre, pre_sniff)
        self.assertLess(pre_sniff, train)
        self.assertLess(train, train_listen)
        self.assertLess(train_listen, post)
        self.assertLess(post, post_sniff)

    def test_compact_gateway_clock_domain_stays_64_bit_across_wrap(
        self,
    ) -> None:
        state = braced_block(
            ANCHOR,
            ANCHOR.index("struct gateway_discovery_assignment_state"),
        )
        claim = function_body(
            GATEWAY_CONTROL,
            "gateway_send_discovery_assignment_claim_request_locked",
        )
        window = function_body(
            GATEWAY_CONTROL,
            "app_gateway_enumeration_response_window",
        )
        lane = function_body(
            REPORT_RX, "mesh_gateway_run_enumeration_response_slice"
        )

        self.assertRegex(
            state,
            r"uint64_t\s+claim_collection_deadline_ms\s*;",
        )
        self.assertRegex(
            state,
            r"uint64_t\s+response_lane_start_ms\s*;",
        )
        self.assertRegex(
            GATEWAY_CONTROL,
            r"bool\s+app_gateway_enumeration_response_window\s*\(\s*"
            r"uint64_t\s+now_ms\s*,\s*"
            r"uint64_t\s*\*\s*round_deadline_ms\s*\)",
        )
        self.assertEqual(
            len(re.findall(
                r"bool\s+app_gateway_enumeration_response_window\s*\(\s*"
                r"uint64_t\s+now_ms\s*,\s*"
                r"uint64_t\s*\*\s*round_deadline_ms\s*\)",
                GATEWAY_CONTROL,
            )),
            2,
        )
        self.assertRegex(
            (ROOT / "app/src/app_anchor.h").read_text(),
            r"bool\s+app_gateway_enumeration_response_window\s*\(\s*"
            r"uint64_t\s+now_ms\s*,\s*"
            r"uint64_t\s*\*\s*round_deadline_ms\s*\)",
        )
        self.assertNotIn("uint64_t now = now_ms", window)
        self.assertIn("uint64_t round_deadline_ms = 0u;", lane)
        self.assertIn("uint64_t claim_origin_ms", claim)
        self.assertIn("(uint64_t)k_uptime_get()", claim)
        self.assertNotIn("uint32_t claim_origin_ms", claim)
        self.assertGreaterEqual(
            len(re.findall(
                r"app_gateway_enumeration_response_window\s*\(\s*"
                r"\(uint64_t\)k_uptime_get\s*\(\)",
                lane,
            )),
            2,
        )
        self.assertNotRegex(
            lane,
            r"app_gateway_enumeration_response_window\s*\(\s*"
            r"k_uptime_get_32\s*\(",
        )

    def test_gateway_channel9_slice_yields_at_compact_prepare_boundary(
        self,
    ) -> None:
        pending = function_body(
            GATEWAY_CONTROL,
            "app_gateway_enumeration_response_pending_wait_ms",
        )
        scanner = function_body(REPORT_RX, "mesh_uwb_rx_work_handler")
        gateway = scanner[scanner.index("if (mesh_gateway_route_test_role())") :]

        self.assertIn(
            "gateway_discovery_assignment_state.active", pending
        )
        self.assertIn(
            "gateway_discovery_assignment_state.response_lane_active",
            pending,
        )
        self.assertEqual(
            unsigned_define(
                ENUMERATION_LANE, "ENUMERATION_RESPONSE_GATEWAY_PREPARE_MS"
            ),
            40,
        )
        self.assertIn(
            ".response_lane_start_ms > ENUMERATION_RESPONSE_GATEWAY_PREPARE_MS",
            pending,
        )
        self.assertIn(
            "response_lane_start_ms -\n"
            "            ENUMERATION_RESPONSE_GATEWAY_PREPARE_MS : 0u",
            pending,
        )
        self.assertIn("now_ms < prepare_start_ms", pending)
        self.assertIn("remaining_ms = prepare_start_ms - now_ms", pending)

        active_yield = gateway.index(
            "app_gateway_enumeration_response_window("
        )
        pending_wait = gateway.index(
            "app_gateway_enumeration_response_pending_wait_ms(", active_yield
        )
        cap = gateway.index(
            "enumeration_wait_ms < remaining_ms", pending_wait
        )
        apply_cap = gateway.index(
            "remaining_ms = enumeration_wait_ms", cap
        )
        receive = gateway.index(
            "dwm3000_driver_receive_frame_continuous(", apply_cap
        )
        receive_call = gateway[receive : gateway.index(");", receive)]

        self.assertLess(active_yield, pending_wait)
        self.assertLess(pending_wait, cap)
        self.assertLess(cap, apply_cap)
        self.assertLess(apply_cap, receive)
        self.assertIn("remaining_ms", receive_call)

    def test_gateway_compact_window_prepares_early_but_rejects_early_bundles(
        self,
    ) -> None:
        window = function_body(
            GATEWAY_CONTROL, "app_gateway_enumeration_response_window"
        )
        handle = function_body(
            GATEWAY_CONTROL,
            "app_gateway_enumeration_response_handle_bundle",
        )
        note = function_body(
            GATEWAY_CONTROL,
            "gateway_discovery_assignment_note_compact_record_locked",
        )

        self.assertIn(
            ".response_lane_start_ms > ENUMERATION_RESPONSE_GATEWAY_PREPARE_MS",
            window,
        )
        self.assertIn("now_ms >= prepare_start_ms", window)
        self.assertIn(
            "now_ms < gateway_discovery_assignment_state\n"
            "                         .response_lane_start_ms",
            window,
        )
        self.assertIn(
            ".response_lane_start_ms + ENUMERATION_RESPONSE_ROUND_MS",
            window,
        )

        semantic_timing = handle.index(
            "enumeration_response_timing_at_depth("
        )
        nominal_start = handle.index(
            "gateway_discovery_assignment_state.response_lane_start_ms",
            semantic_timing,
        )
        received_at = handle.index("received_at_ms", nominal_start)
        self.assertLess(semantic_timing, nominal_start)
        self.assertLess(nominal_start, received_at)
        self.assertNotIn(
            "ENUMERATION_RESPONSE_GATEWAY_PREPARE_MS",
            handle[semantic_timing:received_at],
        )
        self.assertIn(
            "received_at_ms <\n"
            "            gateway_discovery_assignment_state.response_lane_start_ms",
            note,
        )

    def test_compact_gateway_ack_precedes_record_observability(self) -> None:
        note = function_body(
            GATEWAY_CONTROL,
            "gateway_discovery_assignment_note_compact_record_locked",
        )
        handle = function_body(
            GATEWAY_CONTROL,
            "app_gateway_enumeration_response_handle_bundle",
        )
        publish = function_body(
            GATEWAY_CONTROL,
            "app_gateway_enumeration_response_publish_pending",
        )
        lane = function_body(
            REPORT_RX, "mesh_gateway_run_enumeration_response_slice"
        )

        self.assertIn("compact_observability_pending_mask |=", note)
        self.assertNotIn("gateway_observe_command_event", note)
        self.assertNotIn("gateway_observe_command_event", handle)
        self.assertNotIn("DBG_ENUM_COMPACT_RECORD", note)
        self.assertNotIn("DBG_ENUM_COMPACT_RECORD", handle)
        self.assertIn("gateway_observe_command_event", publish)
        self.assertIn("DBG_ENUM_COMPACT_RECORD_PUBLISHED", publish)

        accept = lane.index(
            "app_gateway_enumeration_response_handle_bundle("
        )
        ack_send = lane.index(
            "dwm3000_driver_send_frame_tracked_until(", accept
        )
        radio_finish = lane.index("mesh_rx_radio_finish(", ack_send)
        publish_pending = lane.index(
            "app_gateway_enumeration_response_publish_pending()",
            radio_finish,
        )

        self.assertLess(accept, ack_send)
        self.assertLess(ack_send, radio_finish)
        self.assertLess(radio_finish, publish_pending)
        self.assertNotIn(
            "app_gateway_enumeration_response_publish_pending()",
            lane[accept:radio_finish],
        )

    def test_anchor_enumeration_relay_holds_scan_handoff_across_copy_waits(
        self,
    ) -> None:
        submit = function_body(
            REPORT_ROUTE_CONTROL, "mesh_send_c5_flood_response"
        )
        burst = function_body(
            REPORT_TRANSPORT, "mesh_send_c5_flood_now_until"
        )
        begin = function_body(
            REPORT_TRANSPORT,
            "mesh_c5_enumeration_relay_burst_begin",
        )
        end = function_body(
            REPORT_TRANSPORT, "mesh_c5_enumeration_relay_burst_end"
        )
        active = function_body(
            REPORT_TRANSPORT,
            "mesh_c5_enumeration_relay_burst_active",
        )
        pending = function_body(
            REPORT_TRANSPORT, "mesh_c5_protocol_flood_work_pending"
        )
        scanner = function_body(RADIO, "anchor_uwb_scan_work_handler")

        outer_classify = submit.index("mesh_c5_flood_enumeration_identity(")
        outer_begin = submit.index(
            "mesh_c5_enumeration_relay_burst_begin(out)", outer_classify
        )
        immediate_send = submit.index(
            "mesh_send_c5_flood_now_intent(", outer_begin
        )
        immediate_result = submit.index(
            "if (ret == 0 && result.sent_count > 0u)", immediate_send
        )
        immediate_end = submit.index(
            "mesh_c5_enumeration_relay_burst_end()", immediate_result
        )
        deferred_store = submit.index(
            "mesh_c5_flood_store_deferred(", immediate_end
        )
        deferred_end = submit.index(
            "mesh_c5_enumeration_relay_burst_end()", deferred_store
        )
        self.assertLess(outer_classify, outer_begin)
        self.assertLess(outer_begin, immediate_send)
        self.assertLess(immediate_send, immediate_result)
        self.assertLess(immediate_result, immediate_end)
        self.assertLess(immediate_end, deferred_store)
        self.assertLess(deferred_store, deferred_end)

        classify = burst.index("mesh_c5_flood_enumeration_identity(")
        burst_begin = burst.index(
            "mesh_c5_enumeration_relay_burst_begin(&tx)", classify
        )
        first_defer_check = burst.index(
            "mesh_c5_flood_defer_active_cb(&flood_ctx)", burst_begin
        )
        loop = burst.index("attempt < attempt_count", first_defer_check)
        inter_copy_wait = burst.index("mesh_wait_until_ms(", loop)
        physical_copy = burst.index(
            "app_mesh_flood_send_opportunity", inter_copy_wait
        )
        next_copy = burst.index("attempt + 1u < attempt_count", physical_copy)
        next_copy_guard = burst.index(
            "MESH_ENUMERATION_RELAY_COPY_GUARD_MS", next_copy
        )
        cleanup = burst.index("\nout:", next_copy_guard)
        result_accounting = burst.index("*result = aggregate_result", cleanup)
        activation_accounting = burst.index(
            "protocol_rx_downstream_activation_mark", result_accounting
        )
        burst_end = burst.index(
            "mesh_c5_enumeration_relay_burst_end()",
            activation_accounting,
        )
        final_return = burst.index("return ret", burst_end)
        self.assertLess(classify, burst_begin)
        self.assertLess(burst_begin, first_defer_check)
        self.assertLess(first_defer_check, loop)
        self.assertLess(loop, inter_copy_wait)
        self.assertLess(inter_copy_wait, physical_copy)
        self.assertLess(physical_copy, next_copy)
        self.assertLess(next_copy, next_copy_guard)
        self.assertLess(next_copy_guard, cleanup)
        self.assertLess(cleanup, result_accounting)
        self.assertLess(result_accounting, activation_accounting)
        self.assertLess(activation_accounting, burst_end)
        self.assertLess(burst_end, final_return)
        self.assertNotIn(
            "mesh_c5_enumeration_relay_burst_end()",
            burst[burst_begin:cleanup],
        )
        self.assertNotIn("return ", burst[burst_begin:cleanup])

        self.assertIn(
            "atomic_inc(&mesh_c5_enumeration_relay_burst_count)", begin
        )
        self.assertIn("RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN", begin)
        self.assertIn("dwm3000_driver_request_receive_abort", begin)
        self.assertIn(
            "atomic_dec(&mesh_c5_enumeration_relay_burst_count)", end
        )
        self.assertIn(
            "atomic_get(&mesh_c5_enumeration_relay_burst_count) != 0",
            active,
        )
        active_gate = pending.index(
            "mesh_c5_enumeration_relay_burst_active()"
        )
        deferred_gate = pending.index("mesh_c5_flood_deferred.valid")
        self.assertLess(active_gate, deferred_gate)

        scanner_pending = scanner.index(
            "protocol_flood_pending = mesh_c5_protocol_flood_work_pending()"
        )
        scanner_block = scanner.index("protocol_flood_pending ||", scanner_pending)
        scanner_claim = scanner.index("radio_guard_uwb_claim(", scanner_block)
        self.assertLess(scanner_pending, scanner_block)
        self.assertLess(scanner_block, scanner_claim)

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
        rearm = scan.index("goto protocol_rx_window", timeout)
        self.assertIn("!protocol_owned_work_due", scan[timeout:rearm])
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
        owned = scan.index("protocol_owned_work_due =", prepare_due)
        owned_end = scan.index(";", owned)
        first_guard = scan.index(
            "if (ret == -ETIMEDOUT && !protocol_owned_work_due)",
            owned_end,
        )
        first_rearm = scan.index("goto protocol_rx_window", first_guard)
        second_guard = scan.index(
            "if (protocol_continuous_rx && !protocol_owned_work_due)",
            first_rearm,
        )
        second_rearm = scan.index("goto protocol_rx_window", second_guard)
        final_rearm = scan.index("goto protocol_rx_window", second_rearm + 1)
        scan_complete = scan.index("scan_complete:", final_rearm)

        # One shared ownership bit survives the first timeout branch and gates
        # the later recovery/fallthrough branch as well. This prevents the
        # second unconditional rearm that escaped the original fix.
        self.assertIn("receive_prepare_due", scan[owned:owned_end])
        # One declaration plus one assignment; no later overwrite can discard
        # the prepare-due bit before the second guard.
        self.assertEqual(scan.count("protocol_owned_work_due ="), 2)
        self.assertLess(prepare_query, prepare_due)
        self.assertLess(prepare_due, owned)
        self.assertLess(owned_end, first_guard)
        self.assertLess(first_guard, first_rearm)
        self.assertLess(first_rearm, second_guard)
        self.assertLess(second_guard, second_rearm)
        self.assertLess(second_rearm, final_rearm)
        self.assertLess(final_rearm, scan_complete)
        self.assertIn(
            "!protocol_owned_work_due", scan[first_guard:first_rearm]
        )
        self.assertIn(
            "!protocol_owned_work_due", scan[second_guard:second_rearm]
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

    def test_protocol_flood_blocks_scan_start_and_rearm_including_route_adv(
        self,
    ) -> None:
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
        owned_due = scan.index("protocol_owned_work_due =", receive)
        live_pending = scan.index(
            "mesh_c5_protocol_flood_work_pending()", owned_due
        )
        timeout_rearm = scan.index(
            "if (ret == -ETIMEDOUT && !protocol_owned_work_due)",
            live_pending,
        )
        same_lease_rearm = scan.index(
            "goto protocol_rx_window", timeout_rearm
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
        self.assertIn("!protocol_owned_work_due", scan[timeout_rearm:same_lease_rearm])
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
        self.assertIn("mesh_c5_flood_deferred.valid", protocol_pending)
        self.assertIn("mesh_route_adv_deferred.valid", protocol_pending)
        self.assertIn("mesh_c5_flood_deferred_lock", protocol_pending)
        self.assertNotRegex(
            protocol_pending,
            r"(?:mesh_c5_flood_deferred|mesh_route_adv_deferred)\.valid\s*&&\s*"
            r"(?:\([^)]*\)\s*)?[^;{}]*MSG_GATEWAY_ROUTE_ADV",
        )

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
        self.assertNotIn("MSG_GATEWAY_ROUTE_ADV", admission)
        self.assertIn(
            "radio_guard_uwb_owner_client() ==\n"
            "            RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN",
            admission,
        )
        self.assertIn("!anchor_uwb_window_active()", admission)
        self.assertIn("!anchor_click_window_active()", admission)
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

    def test_deferred_route_adv_retry_keeps_relay_wake_and_is_not_starved(
        self,
    ) -> None:
        response = function_body(
            REPORT_ROUTE_CONTROL, "mesh_send_c5_flood_response"
        )
        store = function_body(
            REPORT_TRANSPORT, "mesh_c5_flood_store_deferred"
        )
        worker = function_body(
            REPORT_DELIVERY, "mesh_c5_flood_work_handler"
        )
        sender = function_body(
            REPORT_TRANSPORT, "mesh_send_c5_flood_now_until"
        )

        initial_policy = response.index("bool send_wake_train")
        initial_send = response.index(
            "mesh_send_c5_flood_now_intent(", initial_policy
        )
        initial_defer = response.index(
            "mesh_c5_flood_store_deferred(", initial_send
        )
        self.assertIn("bool send_wake_train = out != NULL", response)
        self.assertIn(
            "send_wake_train", response[initial_send:initial_defer]
        )

        route_lane = store.index("&mesh_route_adv_deferred")
        retained = store.index("entry->valid = true", route_lane)
        scheduled = store.index("mesh_reschedule_owned_work(", retained)
        self.assertLess(route_lane, retained)
        self.assertLess(retained, scheduled)

        load = worker.index("outbound = entry->outbound")
        retry_send = worker.index("mesh_send_c5_flood_now_intent(", load)
        retry_end = worker.index(");", retry_send)
        retry_setup = worker[load:retry_send]
        retry_call = worker[retry_send:retry_end]
        self.assertIn("send_wake_train = true", retry_setup)
        self.assertIn("send_wake_train", retry_call)
        self.assertIn(
            "tx.packet.msg_type == MSG_GATEWAY_ROUTE_ADV", sender
        )

        local_defer = worker.index(
            "if (current_generation && local_deferral", retry_end
        )
        local_reschedule = worker.index(
            "mesh_reschedule_owned_work(", local_defer
        )
        local_return = worker.index("return;", local_reschedule)
        clear = worker.index("entry->valid = false", local_return)
        self.assertLess(local_defer, local_reschedule)
        self.assertLess(local_reschedule, local_return)
        self.assertLess(local_return, clear)
        self.assertNotIn(
            "entry->valid = false", worker[local_defer:local_return]
        )

    def test_unexpected_rx_error_uses_bounded_recovery_or_fails_closed(self) -> None:
        scan = function_body(RADIO, "anchor_uwb_scan_work_handler")
        unexpected = scan.index("if (ret != 0 && ret != -ETIMEDOUT)")
        recovery = scan.index("dwm3000_driver_force_recovery()", unexpected)
        reconfigure = scan.index(
            "dwm3000_driver_configure_wake_mesh_control_mode()", recovery
        )
        terminal = scan.index('"rx-recovery-failed"', reconfigure)
        recovered_rearm = scan.index("goto protocol_rx_window", terminal)
        release = scan.index("scan_complete:", terminal)

        self.assertLess(unexpected, recovery)
        self.assertLess(recovery, reconfigure)
        self.assertLess(reconfigure, terminal)
        self.assertLess(terminal, recovered_rearm)
        self.assertLess(terminal, release)
        self.assertIn("protocol_owned_work_due", scan)

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
        owned = scan.index("protocol_owned_work_due =", receive)
        timeout_rearm = scan.index("ret == -ETIMEDOUT", owned)
        recovery_guard = scan.index(
            "protocol_continuous_rx && !protocol_owned_work_due",
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
        self.assertIn("send_wake_train = enumeration_needs_wake", send)
        self.assertIn("aggregate_result.sent_count > 0u", send)
        end_clear = send[
            send.index("enumeration_phase == DISCOVERY_ASSIGNMENT_PHASE_END") :
        ]
        self.assertIn("DISCOVERY_ASSIGNMENT_PHASE_ABORT", end_clear)
        self.assertIn("aggregate_result.sent_count == attempt_count", end_clear)
        self.assertIn("protocol_rx_downstream_activation_clear", end_clear)

    def test_gateway_operation_origin_gets_long_first_activation_train(self) -> None:
        identity = function_body(
            REPORT_TRANSPORT, "mesh_c5_gateway_enumeration_claim"
        )
        send = function_body(REPORT_TRANSPORT, "mesh_send_c5_flood_now_until")

        self.assertIn("#if DEVICE_ROLE == ROLE_GATEWAY", identity)
        self.assertIn("command_id == CMD_ASSIGN_DISCOVERY_SLOTS", identity)
        self.assertIn("phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM", identity)
        self.assertIn("gateway_enumeration_claim", send)
        self.assertIn(
            "long_gateway_activation = gateway_enumeration_claim", send
        )
        self.assertNotIn(
            "compact_scheduled_activation && DEVICE_ROLE == ROLE_GATEWAY", send
        )
        self.assertIn("one_activation_per_wave", send)
        self.assertIn("attempt != 0u", send)
        self.assertIn(
            "MESH_RADIO_ENUMERATION_ACTIVATION_WAKE_TRAIN_MS", send
        )
        self.assertIn("WAKE_ADV_MS;", send)
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
