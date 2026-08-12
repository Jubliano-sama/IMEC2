#!/usr/bin/env python3
"""Source guards for temporary, owner-scoped mesh RX on production clickers."""

from pathlib import Path
import re

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
REPORT = read_composed_source(ROOT / "app/src/app_mesh_report.c")
REPORT_RX = (ROOT / "app/src/app_mesh_report_rx.inc").read_text(
    encoding="utf-8"
)
RX_POLICY = (ROOT / "app/src/app_mesh_rx_policy.c").read_text(
    encoding="utf-8"
)


def function_body(source: str, name: str) -> str:
    match = re.search(
        rf"\b{name}\s*\([^;]*?\)\s*\{{",
        source,
        re.DOTALL,
    )
    assert match is not None, f"missing function definition {name}"
    brace = source.index("{", match.start())
    depth = 0
    for index in range(brace, len(source)):
        depth += source[index] == "{"
        depth -= source[index] == "}"
        if depth == 0:
            return source[match.start() : index + 1]
    raise AssertionError(f"unterminated function {name}")


def assert_order(source: str, *needles: str) -> None:
    cursor = 0
    for needle in needles:
        index = source.find(needle, cursor)
        assert index >= 0, f"missing or out-of-order source boundary: {needle}"
        cursor = index + len(needle)


clicker_rx_required = function_body(
    REPORT, "mesh_clicker_on_demand_rx_required"
)
assert "ROLE_CLICKER" in clicker_rx_required
assert (
    "DEVICE_ROLE != ROLE_CLICKER" in clicker_rx_required
    or "DEVICE_ROLE == ROLE_CLICKER" in clicker_rx_required
), "the temporary RX capability must be exclusive to the clicker compile role"

assert "route_selected(&mesh_runtime.upstream)" in clicker_rx_required
assert "channel9_timing_valid" in clicker_rx_required
assert "mesh_event_owner_registry_find(" in clicker_rx_required
assert "owner->active" in clicker_rx_required
assert "mesh_find_active_channel9_timing(" in clicker_rx_required
assert "mesh_ch9_tx_pending_is_active()" in clicker_rx_required
assert "app_mesh_ch9_core_ack_wait_active(" in clicker_rx_required
assert "mesh_relay_tx_active(&mesh_runtime)" in clicker_rx_required
assert "CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER" not in clicker_rx_required
assert "mesh_channel9_connection_count()" not in clicker_rx_required, (
    "a retained timing entry without a live event owner must not grant a "
    "battery clicker passive RX"
)

role_uses_rx = function_body(REPORT, "mesh_role_uses_uwb_rx")
assert "mesh_clicker_on_demand_rx_required()" in role_uses_rx
assert "CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER" in role_uses_rx
assert_order(
    role_uses_rx,
    "mesh_clicker_on_demand_rx_required()",
    "app_mesh_rx_policy_role_uses_uwb_rx(",
)

schedule_rx = function_body(REPORT, "mesh_schedule_uwb_rx")
assert_order(
    schedule_rx,
    "if (!mesh_role_uses_uwb_rx())",
    "return -EINVAL",
    "mesh_uwb_rx_active = true",
    "mesh_reschedule_owned_work_with_busy_handoff(",
    "&mesh_uwb_rx_work",
    "mesh_defer_uwb_rx_rearm(delay_ms)",
)

tracked_tx = function_body(REPORT, "mesh_start_tracked_tx_with_retry")
ack_wait_start = tracked_tx.index(
    "core_ack_wait = app_mesh_ch9_core_ack_wait_active("
)
ack_arm = tracked_tx.index("mesh_schedule_uwb_rx(0u)", ack_wait_start)
ack_block_end = tracked_tx.index("return 0", ack_arm)
ack_block = tracked_tx[ack_wait_start:ack_block_end]
assert "if (batch_ack_wait || core_ack_wait)" in ack_block
assert "mesh_ch9_event_note_persistent_ack_wait(" in ack_block

rx_worker = function_body(REPORT, "mesh_uwb_rx_work_handler")
clicker_leaf = rx_worker.index(
    "if (DEVICE_ROLE == ROLE_CLICKER && !channel9_event)"
)
transmitter_wait = rx_worker.index(
    "if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)",
    clicker_leaf,
)
connected_gap_scan = rx_worker.index(
    "channel5_gap_window_ms = mesh_active_channel9_ch5_gap_window_ms(",
    transmitter_wait,
)
clicker_leaf_block = rx_worker[clicker_leaf:transmitter_wait]
assert "mesh_next_channel9_rx_delay_ms(" in clicker_leaf_block
assert "mesh_schedule_uwb_rx(" in clicker_leaf_block
assert "return;" in clicker_leaf_block
assert "channel5_gap" not in clicker_leaf_block
assert clicker_leaf < transmitter_wait < connected_gap_scan

restart_scan = function_body(REPORT, "mesh_restart_role_scan")
assert "DEVICE_ROLE == ROLE_ANCHOR" in restart_scan
assert "DEVICE_ROLE == ROLE_GATEWAY" in restart_scan
assert "ROLE_CLICKER" not in restart_scan, (
    "ordinary scan restart must never turn a production clicker into a "
    "permanent mesh receiver"
)

resume = function_body(REPORT, "mesh_transport_resume")
assert re.search(
    r"if\s*\(\s*DEVICE_ROLE\s*==\s*ROLE_CLICKER\s*&&\s*"
    r"mesh_clicker_on_demand_rx_required\s*\(\s*\)\s*\)\s*\{\s*"
    r"\(void\)\s*mesh_schedule_uwb_rx\s*\(\s*0u\s*\)\s*;",
    resume,
    re.DOTALL,
), (
    "transport resume must restore an exact clicker ACK/owned-timing RX "
    "owner without enabling ordinary background scan restart"
)

pure_policy = function_body(
    RX_POLICY, "app_mesh_rx_policy_role_uses_uwb_rx"
)
assert "permanent_receiver_role" in pure_policy
assert "scheduled_receiver_enabled && channel9_schedule_installed" in (
    pure_policy
)
assert "clicker" not in pure_policy.lower(), (
    "clicker RX must remain an explicit owner-scoped capability in the "
    "composed runtime, not a permanent-role policy"
)

queue_frame = function_body(
    REPORT_RX, "mesh_queue_from_frame_at_internal"
)
clicker_filter = queue_frame.index("#if DEVICE_ROLE == ROLE_CLICKER")
queue_put = queue_frame.index("k_msgq_put(&mesh_rx_msgq", clicker_filter)
queue_failure = queue_frame.index("if (ret < 0)", queue_put)
queue_failure_end = queue_frame.index(
    "if (DEVICE_ROLE == ROLE_ANCHOR)", queue_failure
)
queue_failure_block = queue_frame[queue_failure:queue_failure_end]
assert_order(
    queue_frame,
    "#if DEVICE_ROLE == ROLE_CLICKER",
    "context.packet.dst_id != DEVICE_ID",
    "MSG_GATEWAY_ACK",
    "MSG_MESH_HOP_ACK",
    "MSG_RELAY_BUSY",
    "k_msgq_put(&mesh_rx_msgq",
)
assert "gateway_command_result_validation_release_reserved(" in (
    queue_failure_block
)
assert "return false;" in queue_failure_block
assert "mesh_relay_" not in queue_failure_block
assert "mesh_event_" not in queue_failure_block

process_frame = function_body(REPORT_RX, "mesh_process_received_frame")
queue_attempt = process_frame.index("if (mesh_queue_from_frame_at(")
peer_progress = process_frame.index(
    "mesh_relay_note_channel9_rx(", queue_attempt
)
queue_reject_cleanup = process_frame.index(
    "gateway_command_result_validation_release_reserved(",
    peer_progress,
)
assert queue_attempt < peer_progress < queue_reject_cleanup
