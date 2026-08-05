#!/usr/bin/env python3
"""Compile-role guard for the production clicker's mesh relay runtime."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
CMAKE = (ROOT / "app" / "CMakeLists.txt").read_text(encoding="utf-8")
BOARD = (ROOT / "app" / "src" / "app_board.c").read_text(encoding="utf-8")
GATEWAY_RUNTIME = (
    ROOT / "app" / "src" / "app_gateway_result_runtime.inc"
).read_text(encoding="utf-8")
REPORT_RX = (
    ROOT / "app" / "src" / "app_mesh_report_rx.inc"
).read_text(encoding="utf-8")
EVENT_TX = (
    ROOT / "app" / "src" / "app_mesh_report_event_tx.inc"
).read_text(encoding="utf-8")
ROUTE_CONTROL = (
    ROOT / "app" / "src" / "app_mesh_report_route_control.inc"
).read_text(encoding="utf-8")
TRANSPORT = (
    ROOT / "app" / "src" / "app_mesh_report_transport.inc"
).read_text(encoding="utf-8")
MESH_REPORT_SOURCES = {
    path: path.read_text(encoding="utf-8")
    for path in (
        [ROOT / "app" / "src" / "app_mesh_report.c"]
        + sorted(
            (ROOT / "app" / "src").glob("app_mesh_report*.inc")
        )
    )
}


def function_body(source: str, name: str) -> str:
    match = re.search(
        rf"\b{name}\s*\([^;]*?\)\s*\{{",
        source,
        re.DOTALL,
    )
    assert match is not None, f"missing function definition {name}"
    brace = source.index("{", match.start())
    return braced_body(source, match.start(), brace)


def braced_body(source: str, start: int, brace: int) -> str:
    depth = 0
    for index in range(brace, len(source)):
        depth += source[index] == "{"
        depth -= source[index] == "}"
        if depth == 0:
            return source[start : index + 1]
    raise AssertionError("unterminated braced source block")


def assert_failed_park_blocks_click_handoff(function_name: str) -> None:
    body = function_body(ROUTE_CONTROL, function_name)
    park = body.index("mesh_radio_standby_with_bounded_recovery(")
    handoff = body.index("mesh_handoff_anchor_click_claim", park)
    failure = body.index("if (release_ret < 0)", park, handoff)
    failure_brace = body.index("{", failure)
    failure_body = braced_body(body, failure, failure_brace)
    guard_release = body.index("radio_guard_uwb_stop()", park)

    assert park < failure < guard_release < handoff
    assert (
        "return release_ret;" in failure_body
        or (
            "click_captured = false;" in failure_body
            and "ret = release_ret;" in failure_body
            and "goto out_unlock;" in failure_body
        )
    ), (
        f"{function_name} must make failed standby terminal for the captured "
        "click before guard release and claim handoff"
    )


clicker_preset_start = CMAKE.index(
    'elseif(IMEC_BUILD_PRESET STREQUAL "mesh_clicker")'
)
clicker_preset_end = CMAKE.index(
    'elseif(IMEC_BUILD_PRESET MATCHES "^ml_anchor_', clicker_preset_start
)
clicker_preset = CMAKE[clicker_preset_start:clicker_preset_end]
assert "IMEC_MESH_ROUTE_TEST_BUILD ON" in clicker_preset
assert "IMEC_MESH_ROUTE_TEST_DEVICE_NAME" in clicker_preset
assert "IMEC_MESH_ROUTE_TEST_TRANSMITTER_BUILD ON" not in clicker_preset
assert "IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_ROUTE_REQ" not in clicker_preset
assert "IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_GATEWAY_CONTROL" not in clicker_preset
assert "mesh_transmitter" not in clicker_preset
assert "forcedhop" not in clicker_preset

reserve_status1 = function_body(
    BOARD, "reserve_status1_for_power_indicator"
)
assert "CONFIG_IMEC_MESH_ROUTE_TEST" in reserve_status1
assert "DEVICE_ROLE == ROLE_ANCHOR" in reserve_status1
assert "DEVICE_ROLE == ROLE_GATEWAY" in reserve_status1
assert "ROLE_CLICKER" not in reserve_status1
reserve_status0 = function_body(
    BOARD, "reserve_status0_for_route_test_power"
)
activity_leds = function_body(BOARD, "mesh_route_activity_leds_enabled")
assert "reserve_status1_for_power_indicator()" in reserve_status0
assert "reserve_status1_for_power_indicator()" in activity_leds
for pulse_helper in (
    "status1_debug_pulse",
    "status0_debug_pulse_for",
):
    pulse = function_body(BOARD, pulse_helper)
    role_gate = pulse.index("mesh_route_activity_leds_enabled()")
    physical_write = pulse.index("status_led", role_gate)
    assert role_gate < physical_write
for channel_pulse in (
    "status_debug_uwb_rx_channel_pulse",
    "status_debug_uwb_tx_channel_pulse",
):
    pulse = function_body(BOARD, channel_pulse)
    assert "debug_rtt_write(" in pulse, (
        "clicker route RTT diagnostics may remain available even though "
        "physical route-activity pulses are role-gated"
    )


init_start = REPORT_RX.index("int app_mesh_report_init(")
init_end = REPORT_RX.index("\n}", init_start)
init = REPORT_RX[init_start : init_end + 2]
clicker_guard = init.index("#if DEVICE_ROLE == ROLE_CLICKER")
clicker_end = init.index("#endif", clicker_guard)
clicker_init = init[clicker_guard:clicker_end]

assert "mesh_relay_init(&mesh_runtime" in clicker_init
assert "MESH_RELAY_ROLE_CLICKER" in clicker_init
assert "DEVICE_ID" in clicker_init
assert "GATEWAY_ID" in clicker_init
assert "MESH_RELAY_ROLE_ANCHOR" not in clicker_init
assert "MESH_RELAY_ROLE_GATEWAY" not in clicker_init
assert clicker_guard < init.index("app_mesh_report_encode_init(")
assert clicker_guard < init.index("k_work_init(&mesh_rx_work")

queue = function_body(REPORT_RX, "mesh_queue_from_frame_at_internal")
queue_guard = queue.index("#if DEVICE_ROLE == ROLE_CLICKER")
queue_put = queue.index("k_msgq_put(&mesh_rx_msgq")
assert queue_guard < queue_put
for allowed in (
    "MSG_ROUTE_REPLY",
    "MSG_GATEWAY_ACK",
    "MSG_MESH_HOP_ACK",
    "MSG_RELAY_BUSY",
    "MSG_MESH_EVENT_PROPOSE",
    "MSG_MESH_EVENT_ACCEPT",
    "MSG_MESH_EVENT_UPDATE",
    "MSG_MESH_EVENT_END",
):
    assert allowed in queue[queue_guard:queue_put]
assert "context.packet.dst_id != DEVICE_ID" in queue[queue_guard:queue_put]

event_rx = function_body(EVENT_TX, "mesh_handle_event_control")
event_guard = event_rx.index("#if DEVICE_ROLE == ROLE_CLICKER")
event_owner_lookup = event_rx.index(
    "mesh_event_owner_for_peer(previous_hop_id)"
)
assert event_guard < event_owner_lookup
assert "route_selected(&mesh_runtime.upstream)" in event_rx[
    event_guard:event_owner_lookup
]
assert "parent->next_hop_id != previous_hop_id" in event_rx[
    event_guard:event_owner_lookup
]

route_capture = function_body(ROUTE_CONTROL, "mesh_listen_for_route_reply")
inline_event = route_capture.index(
    "app_mesh_c5_route_capture_requires_inline_timing_install("
)
assert "mesh_packet_rx_envelope_validate(" in route_capture, (
    "dedicated channel-5 capture must validate event controls before "
    "inline timing mutation"
)
envelope_validation = route_capture.index(
    "mesh_packet_rx_envelope_validate("
)
assert envelope_validation < inline_event
assert "UWB_CHANNEL_WAKE_CONTACT" in route_capture[
    envelope_validation:inline_event
]

park_helper = function_body(
    TRANSPORT, "mesh_radio_standby_with_bounded_recovery"
)
first_standby = park_helper.index("dwm3000_driver_standby()")
force_recovery = park_helper.index(
    "dwm3000_driver_force_recovery()", first_standby
)
second_standby = park_helper.index(
    "dwm3000_driver_standby()", first_standby + 1
)
terminal_watchdog = park_helper.index(
    "app_watchdog_stop_feeding()", force_recovery
)
assert first_standby < force_recovery < second_standby
assert force_recovery < terminal_watchdog

keep_idle = function_body(
    TRANSPORT, "mesh_route_test_keeps_radio_idle_between_channel9_turns"
)
exact_ack = keep_idle.index("bool exact_ack_exchange")
clicker_guard = keep_idle.index("if (DEVICE_ROLE == ROLE_CLICKER)")
clicker_brace = keep_idle.index("{", clicker_guard)
clicker_body = braced_body(
    keep_idle, clicker_guard, clicker_brace
)
non_clicker_return = keep_idle.index(
    "return mesh_channel9_connection_count()", clicker_brace
)
for exact_pending_source in (
    "mesh_ch9_tx_pending_is_active()",
    "app_mesh_ch9_core_ack_wait_active(",
    "app_mesh_ch9_ack_table_any_pending(",
):
    assert exact_pending_source in keep_idle[exact_ack:clicker_guard], (
        "clicker radio IDLE retention must be derived from the complete "
        "exact ACK send/wait state"
    )
assert "return exact_ack_exchange;" in clicker_body
assert "mesh_channel9_connection_count()" not in clicker_body, (
    "a retained channel-9 timing alone must never pin a production "
    "clicker's DWM3000 in IDLE"
)
assert clicker_guard < non_clicker_return
assert "exact_ack_exchange" in keep_idle[non_clicker_return:], (
    "anchor/gateway connected-idle behavior must retain exact ACK state "
    "alongside installed channel-9 timing"
)

release_after_turn = function_body(
    TRANSPORT, "mesh_release_radio_after_mesh_turn"
)
assert "mesh_radio_standby_with_bounded_recovery(" in release_after_turn
assert "return dwm3000_driver_standby();" not in release_after_turn, (
    "shared mesh-turn cleanup must make a failed standby transition bounded "
    "and terminal"
)

generic_send = function_body(
    TRANSPORT, "mesh_send_outbound_with_release_on_channel_until"
)
assert (
    "release_ret = mesh_radio_standby_with_bounded_recovery("
    in generic_send
)
assert "release_ret = dwm3000_driver_standby();" not in generic_send, (
    "failed sends and channel-5 sends must use the same bounded standby "
    "cleanup before the radio guard is released"
)

bounded_idle = function_body(
    TRANSPORT, "mesh_radio_idle_with_bounded_recovery"
)
bounded_standby = function_body(
    TRANSPORT, "mesh_radio_standby_with_bounded_recovery"
)
raw_idle_calls = sum(
    len(re.findall(r"\bdwm3000_driver_idle\s*\(\s*\)", source))
    for source in MESH_REPORT_SOURCES.values()
)
raw_standby_calls = sum(
    len(
        re.findall(
            r"\bdwm3000_driver_standby\s*\(\s*\)", source
        )
    )
    for source in MESH_REPORT_SOURCES.values()
)
assert raw_idle_calls == len(
    re.findall(r"\bdwm3000_driver_idle\s*\(\s*\)", bounded_idle)
), "raw mesh-report IDLE transitions must remain inside bounded recovery"
assert raw_standby_calls == len(
    re.findall(
        r"\bdwm3000_driver_standby\s*\(\s*\)", bounded_standby
    )
), "raw mesh-report standby transitions must remain inside bounded recovery"
for bounded_helper in (bounded_idle, bounded_standby):
    assert "dwm3000_driver_force_recovery()" in bounded_helper
    assert "app_watchdog_stop_feeding()" in bounded_helper

slot_begin = function_body(TRANSPORT, "mesh_ch9_slot_tx_begin")
configure = slot_begin.index("dwm3000_driver_configure_mesh_payload_mode()")
configure_failure = slot_begin.index("if (ret < 0)", configure)
configure_failure_brace = slot_begin.index("{", configure_failure)
configure_failure_body = braced_body(
    slot_begin, configure_failure, configure_failure_brace
)
park_assignment = re.search(
    r"\b([a-zA-Z_][a-zA-Z0-9_]*)\s*=\s*"
    r"mesh_radio_standby_with_bounded_recovery\([^;]*\);",
    configure_failure_body,
)
assert park_assignment is not None, (
    "channel-9 slot setup failure must retain bounded radio-recovery result"
)
park_result = park_assignment.group(1)
park_call = configure_failure_body.index(park_assignment.group(0))
park_failure = configure_failure_body.index(
    f"if ({park_result} < 0)", park_call
)
park_failure_brace = configure_failure_body.index("{", park_failure)
park_failure_body = braced_body(
    configure_failure_body, park_failure, park_failure_brace
)
guard_release = configure_failure_body.index(
    "radio_guard_uwb_stop()", park_failure
)
scan_restart = configure_failure_body.index(
    "mesh_restart_role_scan()", guard_release
)
assert f"return {park_result};" in park_failure_body
assert park_call < park_failure < guard_release < scan_restart

for listener_name in (
    "mesh_listen_for_route_reply_ack",
    "mesh_send_route_wake_train",
    "mesh_listen_for_route_reply",
):
    assert_failed_park_blocks_click_handoff(listener_name)

restore = function_body(
    GATEWAY_RUNTIME, "gateway_restore_host_journal_runtime"
)
assert "struct proto_packet staging_packet = {0};" in restore
assert "struct proto_packet *packet = &staging_packet;" in restore
assert not re.search(
    r"\bpacket\s*=\s*&gateway_ble_stream_state\.items\s*\[",
    restore,
)
for staging_consumer in (
    "app_mesh_persistence_restore_gateway_host_journal_projection",
    "gateway_recover_prepared_host_journal",
    "gateway_ble_stream_enqueue_staged_packet",
    "gateway_ble_stream_enqueue_staged_bundle_projection",
):
    assert re.search(
        rf"\b{staging_consumer}\s*\(\s*(?:&gateway_ble_stream_state,\s*)?"
        r"packet\s*,",
        restore,
        re.DOTALL,
    ), f"{staging_consumer} must consume the disjoint local staging packet"
