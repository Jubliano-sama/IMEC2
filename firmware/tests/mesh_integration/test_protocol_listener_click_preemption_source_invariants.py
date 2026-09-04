#!/usr/bin/env python3
"""Source invariants for click preemption of extended-PHR protocol RX."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[3]
RADIO = (ROOT / "firmware/app/src/app_anchor_radio.inc").read_text()


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", source,
                      re.DOTALL)
    if match is None:
        raise AssertionError(f"missing function {name}")
    brace = source.index("{", match.start())
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unterminated function {name}")


def test_cross_phy_probe_is_bounded_and_restores_control_phy_on_a_miss() -> None:
    probe = function_body(RADIO, "anchor_protocol_rx_probe_standard_click")

    standard = probe.index("dwm3000_driver_configure_wake_mode()")
    receive = probe.index(
        "dwm3000_driver_receive_frame_continuous_extend_on_activity(",
        standard,
    )
    validate = probe.index("uwb_decode_wake_claim", receive)
    handoff = probe.index(
        "app_mesh_c5_wake_claim_requires_anchor_handoff", validate
    )
    capture = probe.index("DBG_PROTOCOL_RX_CLICK_PROBE_CAPTURE", handoff)
    restore = probe.index(
        "dwm3000_driver_configure_wake_mesh_control_mode()", capture
    )

    assert "ANCHOR_PROTOCOL_CLICK_PROBE_ACQUIRE_MS" in probe[receive:validate]
    assert "ANCHOR_UWB_WAKE_ACTIVITY_HOLD_MS" in probe[receive:validate]
    assert standard < receive < validate < handoff < capture < restore
    assert "ANCHOR_PROTOCOL_CLICK_PROBE_ACQUIRE_MS < WAKE_ADV_MS" in RADIO


def test_compact_enumeration_lane_probes_activity_and_exits_for_click() -> None:
    lane = function_body(RADIO, "anchor_run_compact_enumeration_lane")

    receive = lane.index("dwm3000_driver_receive_frame_continuous(")
    activity = lane.index(
        "app_anchor_rx_failure_detected_preamble(*rx_failure)", receive
    )
    probe = lane.index("anchor_protocol_rx_probe_standard_click(", activity)
    preempt = lane.index("DBG_ENUM_COMPACT_CLICK_PREEMPT", probe)
    return_click = lane.index("return 0;", preempt)
    retry = lane.index("DBG_ENUM_COMPACT_RX_RETRY", return_click)

    assert receive < activity < probe < preempt < return_click < retry
    assert "if (!anchor_enumeration_rx_active())" in lane
    assert "anchor_compact_enumeration_deactivate(config.epoch)" in lane


def test_generic_protocol_listener_uses_short_error_edge_then_hands_off() -> None:
    scan = function_body(RADIO, "anchor_uwb_scan_work_handler")

    completion = scan.index(
        "scan_activity_completion_ms = protocol_continuous_rx ?"
    )
    receive = scan.index(
        "dwm3000_driver_receive_frame_continuous_extend_on_activity(",
        completion,
    )
    activity = scan.index(
        "app_anchor_rx_failure_detected_preamble(rx_failure)", receive
    )
    probe = scan.index("anchor_protocol_rx_probe_standard_click(", activity)
    survey = scan.index("app_survey_anchor_preempt_for_click()", probe)
    frame_ready = scan.index("goto scan_frame_ready", survey)
    normal_handler = scan.index("anchor_handle_uwb_claim(", frame_ready)

    assert "ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS" in scan[completion:receive]
    assert activity < probe < survey < frame_ready < normal_handler
    assert "DBG_PROTOCOL_RX_CLICK_DEFER" not in scan


def test_enumeration_listener_survives_click_only_while_deadline_is_valid() -> None:
    scan = function_body(RADIO, "anchor_uwb_scan_work_handler")
    active = function_body(RADIO, "anchor_enumeration_rx_active")
    recovery = function_body(RADIO, "anchor_enumeration_rx_note_recovery")

    release = scan.index("scan_complete:")
    reevaluate = scan.index(
        "enumeration_continuous_rx = anchor_enumeration_rx_active()", release
    )
    low_power = scan.index(
        "protocol_continuous_rx ? APP_RADIO_LOW_POWER_IDLE", reevaluate
    )
    reschedule = scan.index("anchor_enumeration_rx_active()", low_power)

    assert release < reevaluate < low_power < reschedule
    assert "anchor_compact_enumeration_deactivate(" in active
    assert "reason=timeout" in active
    assert "anchor_compact_enumeration_deactivate(epoch)" in recovery


def test_hia_prearm_stays_short_and_rolls_back_new_config_conflict() -> None:
    prearm = function_body(RADIO, "anchor_enumeration_rx_prearm")

    begin = prearm.index("protocol_rx_lifecycle_begin(")
    conflict = prearm.index("hia_deepest_source_start_ms !=")
    conflict_guard = prearm.index(
        "result == PROTOCOL_RX_BEGIN_ACCEPTED", conflict
    )
    conflict_rollback = prearm.index(
        'epoch, "prearm-config-conflict"', conflict_guard
    )

    assert "anchor_enumeration_rx_begin(" not in prearm[begin:conflict]
    assert "anchor_enumeration_rx_bind_claim(" not in prearm[begin:conflict]
    assert (
        "UWB_ENUM_MAX_HOPS * MESH_GATEWAY_ROUTE_DEPTH_BLOCK_MS" in RADIO
    )
    assert "DISCOVERY_ASSIGNMENT_PREARM_HOLD_MS" in RADIO
    assert conflict < conflict_guard
    assert conflict_guard < conflict_rollback
