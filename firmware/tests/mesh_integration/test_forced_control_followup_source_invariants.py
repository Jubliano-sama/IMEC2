#!/usr/bin/env python3

from pathlib import Path
import re
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
ANCHOR_RADIO = (ROOT / "app/src/app_anchor_radio.inc").read_text(
    encoding="utf-8"
)
ANCHOR_HEADER = (ROOT / "app/src/app_anchor.h").read_text(encoding="utf-8")
APP_CONFIG = (ROOT / "app/src/app_config.h").read_text(encoding="utf-8")
REPORT = read_composed_source(ROOT / "app/src/app_mesh_report.c")
RADIO_TIMING = (ROOT / "include/mesh_radio_timing.h").read_text(
    encoding="utf-8"
)
RADIO_STATE = (ROOT / "src/firmware_state_radio.c").read_text(
    encoding="utf-8"
)
RADIO_STATE_HEADER = (ROOT / "include/firmware_state_machines.h").read_text(
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


class ForcedControlFollowupSourceInvariantTests(unittest.TestCase):
    def test_two_link_cadence_reserves_retune_and_complete_c5_scan(self):
        gap_window = function_body(
            REPORT, "mesh_active_channel9_ch5_gap_window_ms"
        )

        self.assertIn(
            "#define MESH_RADIO_EVENT_INTERVAL_MS 640u", RADIO_TIMING
        )
        self.assertIn(
            "#define MESH_RADIO_EVENT_WINDOW_MS 120u", RADIO_TIMING
        )
        self.assertIn(
            "#define MESH_RADIO_EVENT_GUARD_MS 60u", RADIO_TIMING
        )
        self.assertIn(
            "#define MESH_RADIO_EVENT_RETUNE_GUARD_MS "
            "MESH_RADIO_EVENT_GUARD_MS",
            RADIO_TIMING,
        )
        self.assertIn(
            "#define MESH_RADIO_EVENT_RX_LATE_GUARD_MS "
            "MESH_RADIO_EVENT_GUARD_MS",
            RADIO_TIMING,
        )
        self.assertIn(
            "#define MESH_RADIO_CONTROL_FOLLOWUP_SCAN_MS 20u",
            RADIO_TIMING,
        )
        self.assertIn(
            "#define MESH_EVENT_DEFAULT_INTERVAL_MS "
            "MESH_RADIO_EVENT_INTERVAL_MS",
            APP_CONFIG,
        )
        self.assertIn(
            "#define MESH_EVENT_DEFAULT_SECOND_SLOT_OFFSET_MS "
            "(MESH_EVENT_DEFAULT_INTERVAL_MS / 2u)",
            APP_CONFIG,
        )

        capacity_message = REPORT.index(
            '"two-link relays must retain a complete '
            'retune-and-channel-5 scan gap"'
        )
        capacity_guard = REPORT[capacity_message - 520 : capacity_message]
        for term in (
            "MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS",
            "MESH_EVENT_DEFAULT_WINDOW_MS",
            "MESH_EVENT_RX_LATE_GUARD_MS",
            "MESH_ROUTE_TEST_CH5_GAP_RETUNE_MARGIN_MS",
            "MESH_ROUTE_TEST_CH5_GAP_MIN_SCAN_MS",
            "MESH_EVENT_DEFAULT_SECOND_SLOT_OFFSET_MS",
        ):
            with self.subTest(capacity_term=term):
                self.assertIn(term, capacity_guard)

        next_delay = gap_window.index(
            "delay_ms = mesh_next_channel9_rx_delay_ms(now_ms)"
        )
        bounded_gap = gap_window.index(
            "app_mesh_c5_connected_gap_window_ms(", next_delay
        )
        self.assertLess(next_delay, bounded_gap)
        self.assertIn(
            ".min_scan_ms = MESH_ROUTE_TEST_CH5_GAP_MIN_SCAN_MS",
            gap_window[bounded_gap:],
        )
        self.assertIn(
            ".retune_margin_ms = "
            "MESH_ROUTE_TEST_CH5_GAP_RETUNE_MARGIN_MS",
            gap_window[bounded_gap:],
        )

    def test_gateway_control_handoff_uses_full_deeper_relay_window(self):
        handoff = function_body(REPORT, "mesh_anchor_handoff_route_wake_frame")
        listener = function_body(REPORT, "mesh_listen_for_route_reply")

        self.assertIn(
            "#define MESH_GATEWAY_CONTROL_DEEP_RELAY_LISTEN_MS 2000u",
            APP_CONFIG,
        )
        classify = handoff.index(
            "control_followup = app_mesh_c5_wake_followup_is_control("
        )
        gate = handoff.index("if (control_followup)", classify)
        full_window = handoff.index(
            "listen_ms = MESH_GATEWAY_CONTROL_DEEP_RELAY_LISTEN_MS", gate
        )
        dispatch = handoff.index("mesh_listen_for_route_reply(", full_window)
        self.assertLess(classify, gate)
        self.assertLess(gate, full_window)
        self.assertLess(full_window, dispatch)
        self.assertIn(
            'control_followup ?\n            "gateway-command-wake-followup"',
            handoff[full_window:dispatch],
        )
        contact = listener.index("mesh_c5_contact_exchange(")
        stop = listener.index("mesh_stop_role_scan()", contact)
        claim = listener.index("mesh_transport_radio_claim(", stop)
        receive = listener.index(
            "dwm3000_driver_receive_frame_continuous(", claim
        )
        finish = listener.index("mesh_transport_radio_finish(", receive)
        restart = listener.index("mesh_restart_role_scan()", finish)
        self.assertLess(contact, stop)
        self.assertLess(stop, claim)
        self.assertLess(claim, receive)
        self.assertLess(receive, finish)
        self.assertLess(finish, restart)
        self.assertNotIn("DBG_C5_CONTROL_LISTENER_YIELD_CH9", listener)

    def test_downstream_control_echo_does_not_occupy_the_upstream_rx_slot(self):
        handoff = function_body(REPORT, "mesh_anchor_handoff_route_wake_frame")

        classify = handoff.index(
            "control_followup = app_mesh_c5_wake_followup_is_control("
        )
        downstream = handoff.index(
            "MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM", classify
        )
        ignored = handoff.index(
            "DBG_ANCHOR_DOWNSTREAM_CONTROL_WAKE_IGNORED", downstream
        )
        handle = handoff.index("mesh_handle_channel5_wake_claim(", ignored)
        listen = handoff.index("mesh_listen_for_route_reply(", handle)
        reject = braced_block_after(
            handoff,
            "if (entry->valid && entry->next_hop_id == claim.clicker_id",
        )
        condition = handoff[classify:ignored]

        self.assertLess(classify, downstream)
        self.assertLess(downstream, ignored)
        self.assertLess(ignored, handle)
        self.assertLess(handle, listen)
        self.assertIn("entry->valid", condition)
        self.assertIn("entry->next_hop_id == claim.clicker_id", condition)
        self.assertIn("MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM", condition)
        self.assertIn("return true", reject)
        self.assertNotIn("mesh_handle_channel5_wake_claim(", reject)
        self.assertNotIn("mesh_listen_for_route_reply(", reject)

    def test_generic_rx_rejects_wrong_depth_gateway_control(self):
        gate = REPORT.index("app_mesh_c5_gateway_control_rx_allowed(")
        hops = REPORT.index(
            "CONFIG_IMEC_MESH_ROUTE_TEST_REQUIRED_GATEWAY_RELAY_HOPS", gate
        )
        drop = REPORT.index("return false;", hops)
        discovery = REPORT.rindex("MSG_SURVEY_DISCOVERY_START", 0, gate)
        prepare = REPORT.rindex("MSG_SURVEY_PAIR_PREPARE", 0, gate)

        self.assertLess(discovery, gate)
        self.assertLess(prepare, gate)
        self.assertLess(gate, hops)
        self.assertLess(hops, drop)
        self.assertIn("context.packet.ttl", REPORT[gate:hops])

    def test_rejected_direct_control_hands_off_to_followup_listener(self):
        scan = function_body(ANCHOR_RADIO, "anchor_uwb_scan_work_handler")
        handoff = function_body(REPORT, "mesh_anchor_handoff_route_wake_frame")
        listen_gate = scan.index("if (!app_mesh_c5_route_wake_should_listen(")
        leave_block = braced_block_after(
            scan, "if (!app_mesh_c5_route_wake_should_listen("
        )
        allowed_note = scan.index(
            "if (!app_mesh_c5_route_wake_claim_allowed(", listen_gate
        )
        listen_note = scan.index(
            "DBG_ANCHOR_DIRECT_GATEWAY_WAKE_LISTEN", allowed_note
        )
        dispatch = scan.index("route_wake_handoff = true", listen_note)

        self.assertLess(listen_gate, allowed_note)
        self.assertLess(allowed_note, listen_note)
        self.assertLess(listen_note, dispatch)
        self.assertIn("anchor_relay_control_followup_boost_begin(", leave_block)
        self.assertIn("goto scan_complete", leave_block)
        self.assertNotIn("route_wake_handoff = true", leave_block)
        self.assertIn(
            "app_mesh_c5_route_wake_should_listen(",
            handoff[: handoff.index("mesh_handle_channel5_wake_claim(")],
        )
        self.assertNotIn("anchor_wait_for_relayed_control_wake", ANCHOR_RADIO)

    def test_rejected_direct_control_uses_released_bounded_scan_slices(self):
        scan = function_body(ANCHOR_RADIO, "anchor_uwb_scan_work_handler")
        direct_reject = braced_block_after(
            scan, "if (!app_mesh_c5_route_wake_should_listen("
        )

        boost_begin = direct_reject.index(
            "anchor_relay_control_followup_boost_begin("
        )
        ack_yield = direct_reject.index(
            "app_mesh_c5_control_followup_yields_to_ack("
        )
        ack_yield_block = braced_block_after(
            direct_reject,
            "if (app_mesh_c5_control_followup_yields_to_ack(",
        )
        leave = direct_reject.rindex("goto scan_complete")
        self.assertLess(boost_begin, ack_yield)
        self.assertLess(ack_yield, leave)
        self.assertIn("claim.claimed_duration_ms", direct_reject[boost_begin:ack_yield])
        self.assertIn("ch9_ack_wait_active", direct_reject[ack_yield:leave])
        self.assertIn("DBG_ANCHOR_CONTROL_FOLLOWUP_DEFER_ACK", ack_yield_block)
        self.assertIn("goto scan_complete", ack_yield_block)
        self.assertNotIn("anchor_wait_for_relayed_control_wake", ANCHOR_RADIO)

        boost_active = scan.index(
            "control_followup_boost_active =\n"
            "        anchor_relay_control_followup_boost_active()"
        )
        slice_width = scan.index(
            "scan_rx_ms = MESH_RADIO_CONTROL_FOLLOWUP_SCAN_MS", boost_active
        )
        radio_busy = scan.index("uwb_radio_busy =", slice_width)
        ch9_guard = scan.index(
            "mesh_anchor_low_duty_scan_should_defer(&ch9_retry_ms)",
            radio_busy,
        )
        radio_claim = scan.index("radio_guard_uwb_claim(", ch9_guard)
        receive = scan.index(
            "dwm3000_driver_receive_frame_continuous_extend_on_activity(",
            radio_claim,
        )
        release = scan.index("radio_guard_uwb_release_begin(", receive)
        clamp = scan.index(
            "next_scan_delay_ms > ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS",
            release,
        )
        schedule = scan.index("anchor_uwb_scan_schedule_ms(next_scan_delay_ms)", clamp)
        self.assertLess(boost_active, slice_width)
        self.assertLess(slice_width, radio_busy)
        self.assertLess(radio_busy, ch9_guard)
        self.assertLess(ch9_guard, radio_claim)
        self.assertLess(radio_claim, receive)
        self.assertLess(receive, release)
        self.assertLess(release, clamp)
        self.assertLess(clamp, schedule)
        self.assertIn("scan_rx_ms,", scan[receive : receive + 180])
        self.assertIn("!uwb_radio_busy", scan[radio_busy:ch9_guard])
        self.assertIn(
            "boosted channel-5 slice must end before the channel-9 prepare horizon",
            REPORT,
        )

    def test_first_released_scan_after_channel9_uses_full_followup_slice(self):
        notification = function_body(
            ANCHOR_RADIO, "app_anchor_note_channel9_window_released"
        )
        scan = function_body(ANCHOR_RADIO, "anchor_uwb_scan_work_handler")
        rx_worker = function_body(REPORT, "mesh_uwb_rx_work_handler")

        self.assertIn(
            "void app_anchor_note_channel9_window_released(void);",
            ANCHOR_HEADER,
        )
        self.assertIn("#if DEVICE_ROLE == ROLE_ANCHOR", notification)
        self.assertIn("IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)", notification)
        self.assertIn(
            "atomic_set(&anchor_ch5_post_ch9_recovery_scan_pending, 1)",
            notification,
        )
        self.assertNotIn("atomic_clear", notification)

        release = rx_worker.index(
            "radio_release_ret = mesh_rx_radio_finish(&radio_lease, radio_release_ret)"
        )
        release_failure = rx_worker.index(
            "if (radio_release_ret < 0)", release
        )
        release_failure_block = braced_block_after(
            rx_worker[release_failure:], "if (radio_release_ret < 0)"
        )
        anchor_role = rx_worker.index(
            "#if DEVICE_ROLE == ROLE_ANCHOR", release_failure
        )
        channel9_gate = rx_worker.index("if (channel9_event)", anchor_role)
        notify = rx_worker.index(
            "app_anchor_note_channel9_window_released()", channel9_gate
        )
        route_handoff = rx_worker.index("if (route_control_handoff)", notify)
        event_complete = rx_worker.index("if (channel9_event)", route_handoff)
        reschedule = rx_worker.index("mesh_schedule_uwb_rx(", event_complete)

        self.assertIn("return;", release_failure_block)
        self.assertLess(release, release_failure)
        self.assertLess(release_failure, anchor_role)
        self.assertLess(anchor_role, channel9_gate)
        self.assertLess(channel9_gate, notify)
        self.assertLess(notify, route_handoff)
        self.assertLess(route_handoff, event_complete)
        self.assertLess(event_complete, reschedule)
        self.assertNotIn("radio_release_ret", rx_worker[channel9_gate:notify])

        pending_read = scan.index(
            "atomic_get(&anchor_ch5_post_ch9_recovery_scan_pending) != 0"
        )
        widened = scan.index(
            "control_followup_boost_active || post_ch9_recovery_scan",
            pending_read,
        )
        conflict = scan.index("ch9_rx_conflict =", widened)
        retain = scan.index(
            "atomic_set(&anchor_ch5_post_ch9_recovery_scan_pending, 1)",
            conflict,
        )
        blocked = scan.index("if (anchor_uwb_window_active()", retain)
        claim = scan.index("radio_guard_uwb_claim(", blocked)
        attempted = scan.index("rx_attempted = true", claim)
        receive = scan.index(
            "dwm3000_driver_receive_frame_continuous_extend_on_activity(", attempted
        )
        release = scan.index("radio_guard_uwb_release_begin(", receive)
        consume_gate = scan.index(
            "if (post_ch9_recovery_scan && rx_attempted)", release
        )
        consume = scan.index(
            "atomic_clear(&anchor_ch5_post_ch9_recovery_scan_pending)",
            consume_gate,
        )

        self.assertLess(pending_read, widened)
        self.assertLess(widened, conflict)
        self.assertLess(conflict, retain)
        self.assertLess(retain, blocked)
        self.assertLess(blocked, claim)
        self.assertLess(claim, attempted)
        self.assertLess(attempted, receive)
        self.assertLess(receive, release)
        self.assertLess(release, consume_gate)
        self.assertLess(consume_gate, consume)
        self.assertIn(
            "scan_rx_ms = MESH_RADIO_CONTROL_FOLLOWUP_SCAN_MS",
            scan[widened:conflict],
        )
        self.assertNotIn(
            "atomic_clear(&anchor_ch5_post_ch9_recovery_scan_pending)",
            scan[retain:release],
        )
        self.assertNotIn(
            "atomic_clear(&anchor_ch5_post_ch9_recovery_scan_pending)",
            scan[blocked:attempted],
        )
        self.assertEqual(
            ANCHOR_RADIO.count(
                "atomic_clear(&anchor_ch5_post_ch9_recovery_scan_pending)"
            ),
            1,
        )
        self.assertIn(
            "DBG_ANCHOR_CH5_POST_CH9_RECOVERY_SCAN",
            scan[consume_gate : consume + 240],
        )

    def test_allowed_relay_control_preempts_ack_receive_with_existing_listener(self):
        handoff = function_body(
            REPORT, "mesh_anchor_handoff_route_wake_frame"
        )

        decode = handoff.index("mesh_decode_channel5_wake_claim(")
        handle = handoff.index("mesh_handle_channel5_wake_claim(", decode)
        listen = handoff.index("mesh_listen_for_route_reply(", handle)

        self.assertLess(decode, handle)
        self.assertLess(handle, listen)
        self.assertNotIn("app_mesh_c5_control_followup_yields_to_ack(", handoff)
        self.assertNotIn("mesh_report_ch9_ack_wait_active()", handoff)
        self.assertNotIn("DBG_ANCHOR_CONTROL_FOLLOWUP_DEFER_ACK", handoff)

    def test_boosted_scanner_ignores_incompatible_control_payloads(self):
        scan = function_body(ANCHOR_RADIO, "anchor_uwb_scan_work_handler")
        boosted_skip = braced_block_after(
            scan, "else if (control_followup_boost_active)"
        )
        normal_cooldown = scan.index(
            "uwb_anchor_note_false_wake_cooldown(",
            scan.index("else if (control_followup_boost_active)"),
        )
        boosted_skip_pos = scan.index(
            "DBG_ANCHOR_RELAY_CONTROL_FOLLOWUP_SKIP"
        )

        self.assertIn("goto scan_complete", boosted_skip)
        self.assertNotIn("uwb_anchor_note_false_wake_cooldown", boosted_skip)
        self.assertLess(boosted_skip_pos, normal_cooldown)

    def test_insufficient_first_relay_reopens_standard_probe_for_deeper_control(self):
        probe = function_body(
            REPORT, "mesh_probe_standard_wake_claim"
        )
        listener = function_body(REPORT, "mesh_listen_for_route_reply")

        click = probe.index("mesh_frame_requires_anchor_click_handoff(")
        click_return = probe.index(
            "return MESH_STANDARD_WAKE_PROBE_CLICK", click
        )
        allow = probe.index("allow_relayed_gateway_control", click_return)
        control = probe.index(
            "app_mesh_c5_wake_followup_is_control(candidate.flags)", allow
        )
        route = probe.index(
            "app_mesh_c5_route_wake_claim_allowed(", control
        )
        restore = probe.index(
            "dwm3000_driver_configure_wake_mesh_control_mode()", route
        )
        control_return = probe.index(
            "return MESH_STANDARD_WAKE_PROBE_RELAYED_GATEWAY_CONTROL",
            restore,
        )
        self.assertLess(click, click_return)
        self.assertLess(click_return, allow)
        self.assertLess(allow, control)
        self.assertLess(control, route)
        self.assertLess(route, restore)
        self.assertLess(restore, control_return)

        reject = listener.index(
            "if (!app_mesh_gateway_control_relay_hops_allowed("
        )
        observed = listener.index(
            "const uint8_t observed_relay_hops", reject
        )
        shortfall = listener.index(
            "gateway_control_relay_shortfall = true", observed
        )
        reject_continue = listener.index("continue;", shortfall)
        self.assertLess(reject, observed)
        self.assertLess(observed, shortfall)
        self.assertLess(shortfall, reject_continue)
        self.assertIn(
            "observed_relay_hops <\n"
            "                        CONFIG_IMEC_MESH_ROUTE_TEST_REQUIRED_GATEWAY_RELAY_HOPS",
            listener[observed:shortfall],
        )

        activity = listener.index(
            "contact_purpose ==\n"
            "                               C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD"
        )
        shortfall_gate = listener.index(
            "if (gateway_control_relay_shortfall &&", activity
        )
        wake_seen_gate = listener.index(
            "!gateway_control_followup_wake_seen", shortfall_gate
        )
        deeper_probe = listener.index(
            "mesh_probe_standard_wake_claim(", wake_seen_gate
        )
        allow_true = listener.index("true);", deeper_probe)
        click_result = listener.index(
            "MESH_STANDARD_WAKE_PROBE_CLICK", allow_true
        )
        control_result = listener.index(
            "MESH_STANDARD_WAKE_PROBE_RELAYED_GATEWAY_CONTROL", click_result
        )
        wake_seen = listener.index(
            "gateway_control_followup_wake_seen = true", control_result
        )
        renewed_deadline = listener.index(
            "deadline_ms = k_uptime_get_32() +", wake_seen
        )
        full_relay_window = listener.index(
            "MESH_GATEWAY_CONTROL_DEEP_RELAY_LISTEN_MS", renewed_deadline
        )
        continue_listen = listener.index("continue;", full_relay_window)
        hold = listener.index(
            "DBG_C5_CONTROL_LISTENER_HOLD_EXTENDED", continue_listen
        )
        self.assertLess(shortfall_gate, wake_seen_gate)
        self.assertLess(wake_seen_gate, deeper_probe)
        self.assertLess(deeper_probe, allow_true)
        self.assertLess(allow_true, click_result)
        self.assertLess(click_result, control_result)
        self.assertLess(control_result, wake_seen)
        self.assertLess(wake_seen, renewed_deadline)
        self.assertLess(renewed_deadline, full_relay_window)
        self.assertLess(full_relay_window, continue_listen)
        self.assertLess(continue_listen, hold)
        self.assertIn(
            "bool gateway_control_followup_wake_seen = false;", listener
        )

    def test_allowed_relay_cancels_boost_before_existing_handoff(self):
        scan = function_body(ANCHOR_RADIO, "anchor_uwb_scan_work_handler")

        click_exclusion = scan.index(
            "!app_mesh_c5_wake_claim_requires_anchor_handoff("
        )
        route_policy = scan.index(
            "app_mesh_c5_route_wake_claim_allowed(", click_exclusion
        )
        control_followup = scan.index(
            "app_mesh_c5_wake_followup_is_control(claim.flags)", route_policy
        )
        relay_cancel = scan.index(
            "anchor_relay_control_followup_boost_cancel()", control_followup
        )
        route_publish = scan.index("route_wake_handoff = true", relay_cancel)
        click_handoff = scan.index("anchor_handle_uwb_claim(", route_publish)
        click_cancel = scan.index(
            "anchor_relay_control_followup_boost_cancel()", click_handoff
        )
        release = scan.index("radio_guard_uwb_release_begin(", route_publish)
        relay_handoff = scan.index(
            "mesh_anchor_handoff_route_wake_frame(", release
        )
        boosted_retry = scan.index(
            "anchor_relay_control_followup_boost_active()", relay_handoff
        )

        self.assertLess(click_exclusion, route_policy)
        self.assertLess(route_policy, control_followup)
        self.assertLess(control_followup, relay_cancel)
        self.assertLess(relay_cancel, route_publish)
        self.assertLess(route_publish, click_handoff)
        self.assertLess(click_handoff, click_cancel)
        self.assertLess(route_publish, release)
        self.assertLess(release, relay_handoff)
        self.assertLess(relay_cancel, relay_handoff)
        self.assertLess(relay_handoff, boosted_retry)
        self.assertNotIn(
            "anchor_relay_control_followup_boost_begin(",
            scan[control_followup:route_publish],
        )
        self.assertNotIn(
            "anchor_relay_control_followup_boost_begin(",
            scan[relay_cancel:boosted_retry],
        )
        self.assertIn(
            "route_wake_frame_len = frame_len", scan[relay_cancel:release]
        )
        self.assertIn(
            "route_wake_quality = quality", scan[relay_cancel:release]
        )

    def test_only_validated_gateway_survey_forward_crosses_live_ack_custody(
        self,
    ):
        validator = function_body(
            REPORT, "mesh_c5_gateway_survey_control_candidate_valid"
        )
        mint = function_body(REPORT, "mesh_c5_forwarded_control_intent")
        coordinator = function_body(
            REPORT, "mesh_coordinator_c5_tx_allowed_authorized_intent"
        )
        policy = function_body(RADIO_STATE, "fw_radio_activity_decide")

        self.assertIn("FW_C5_TX_INTENT_GATEWAY_SURVEY_CONTROL", RADIO_STATE_HEADER)
        for exact_identity in (
            "candidate->radio_channel != UWB_CHANNEL_WAKE_CONTACT",
            "candidate->packet.src_id != GATEWAY_ID",
            "candidate->packet.flags != FLAG_DIAGNOSTIC",
            "candidate->packet.payload_len != candidate->payload_len",
            "survey_operation_generation_extract_tlv(",
            "candidate->packet.session_id !=",
            "survey_operation_session_id(operation_generation)",
        ):
            with self.subTest(identity=exact_identity):
                self.assertIn(exact_identity, validator)

        discovery = validator.index(
            "candidate->packet.msg_type == MSG_SURVEY_DISCOVERY_START"
        )
        pair = validator.index(
            "candidate->packet.msg_type == MSG_SURVEY_PAIR_PREPARE",
            discovery,
        )
        discovery_branch = validator[discovery:pair]
        self.assertIn("candidate->packet.dst_id == MESH_BROADCAST_ID", discovery_branch)
        self.assertIn("candidate->next_hop_id == MESH_BROADCAST_ID", discovery_branch)
        self.assertIn("survey_extract_discovery_start_tlvs(", discovery_branch)
        self.assertIn(
            "config.operation_generation == operation_generation",
            discovery_branch,
        )

        self.assertIn(
            "mesh_c5_gateway_survey_control_candidate_valid(candidate)", mint
        )
        self.assertIn("FW_C5_TX_INTENT_GATEWAY_SURVEY_CONTROL", mint)
        self.assertIn("FW_C5_TX_INTENT_BACKGROUND", mint)
        self.assertIn(
            "intent == FW_C5_TX_INTENT_GATEWAY_SURVEY_CONTROL", coordinator
        )
        self.assertIn(
            "!mesh_c5_gateway_survey_control_candidate_valid(candidate)",
            coordinator,
        )

        click = policy.index("if (capture->click_active)")
        survey = policy.index("else if (capture->survey_pending)", click)
        gateway_rx = policy.index("else if (capture->gateway_continuous_ch9)", survey)
        ack_branch = policy.index("capture->ch9_ack_send_pending", gateway_rx)
        exception = policy.index("bool survey_control_preemption", ack_branch)
        self.assertLess(click, survey)
        self.assertLess(survey, gateway_rx)
        self.assertLess(gateway_rx, ack_branch)
        self.assertIn("capture->rx_queue_used == 0u", policy[exception:])
        self.assertIn(
            "FW_C5_TX_INTENT_GATEWAY_SURVEY_CONTROL", policy[exception:]
        )
        self.assertIn(
            "decision->c5_tx_allowed =\n            survey_control_preemption",
            policy[exception:],
        )

    def test_survey_forward_keeps_immutable_deferred_ownership_and_intent(self):
        response = function_body(REPORT, "mesh_send_c5_flood_response")
        store = function_body(REPORT, "mesh_c5_flood_store_deferred")
        worker = function_body(REPORT, "mesh_c5_flood_work_handler")

        mint = response.index("mesh_c5_forwarded_control_intent(out)")
        send = response.index("mesh_send_c5_flood_now_intent(", mint)
        defer = response.index("mesh_c5_flood_store_deferred(", send)
        self.assertLess(mint, send)
        self.assertLess(send, defer)
        self.assertIn("c5_tx_intent", response[send:defer])
        self.assertIn("forward_admission_retained = true", response[defer:])

        copy = store.index("entry->outbound = *out")
        valid = store.index("entry->valid = true", copy)
        schedule = store.index("&mesh_c5_flood_work", valid)
        self.assertLess(copy, valid)
        self.assertLess(valid, schedule)

        load = worker.index("outbound = entry->outbound")
        retry_send = worker.index("mesh_send_c5_flood_now_intent(", load)
        remint = worker.index(
            "mesh_c5_forwarded_control_intent(\n                                            &outbound)",
            retry_send,
        )
        self.assertLess(load, retry_send)
        self.assertLess(retry_send, remint)

    def test_new_generation_cleanup_is_after_durable_local_admission_and_exact(self):
        matcher = function_body(
            REPORT, "mesh_transit_survey_result_precedes_generation"
        )
        retire = function_body(
            REPORT,
            "mesh_retire_stale_transit_survey_result_after_admission",
        )
        drain = function_body(REPORT, "mesh_drain_rx_queue_locked")

        for identity in (
            "packet->msg_type != MSG_SURVEY_DISCOVERY_REPORT",
            "packet->msg_type != MSG_SURVEY_PAIR_RESULT",
            "FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC",
            "packet->src_id == DEVICE_ID",
            "packet->dst_id != GATEWAY_ID",
            "packet->payload_len != payload_len",
            "survey_operation_generation_extract_tlv(",
            "owned_generation < operation_generation",
        ):
            with self.subTest(identity=identity):
                self.assertIn(identity, matcher)
        self.assertIn("TLV_NODE_BOOT_COUNTER", matcher)
        self.assertIn("packet->session_id == proto_get_u32_le(boot_value)", matcher)
        self.assertIn(
            "packet->session_id ==\n            survey_operation_session_id(owned_generation)",
            matcher,
        )

        self.assertIn("incoming->packet.msg_type != MSG_SURVEY_DISCOVERY_START", retire)
        self.assertIn("survey_extract_discovery_start_tlvs(", retire)
        self.assertIn(
            "!mesh_report_anchor_survey_operation_generation_active(", retire
        )
        self.assertIn("!mesh_runtime.pending.gateway_ack_forward_pending", retire)
        self.assertIn("MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD", retire)
        self.assertIn("mesh_transit_survey_result_precedes_generation(", retire)

        local_admission = drain.rindex(
            "mesh_report_anchor_handle_survey_discovery_start("
        )
        cleanup = drain.index(
            "mesh_retire_stale_transit_survey_result_after_admission(",
            local_admission,
        )
        forward = drain.rfind("mesh_handle_result_actions(", 0, local_admission)
        self.assertGreaterEqual(forward, 0)
        self.assertLess(forward, local_admission)
        self.assertLess(local_admission, cleanup)


if __name__ == "__main__":
    unittest.main()
