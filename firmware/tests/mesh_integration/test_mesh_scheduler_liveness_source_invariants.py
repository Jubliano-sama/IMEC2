#!/usr/bin/env python3
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "app" / "src"


def function_body(source: str, name: str) -> str:
    definition = re.search(
        rf"(?m)^[A-Za-z_][^;\n]*\b{re.escape(name)}\s*\([^;]*?\)\s*\n?\{{",
        source,
    )
    if definition is None:
        raise AssertionError(f"missing function {name}")
    brace = source.index("{", definition.start())
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unterminated function {name}")


coordination = (APP / "app_mesh_report_coordination.inc").read_text()
transport = (APP / "app_mesh_report_transport.inc").read_text()
delivery = (APP / "app_mesh_report_delivery.inc").read_text()
rx = (APP / "app_mesh_report_rx.inc").read_text()
route_control = (APP / "app_mesh_report_route_control.inc").read_text()
direct_gateway = (
    APP / "app_mesh_report_direct_gateway.inc"
).read_text()
event_tx = (APP / "app_mesh_report_event_tx.inc").read_text()
state = (APP / "app_state.c").read_text()
state_header = (APP / "app_state.h").read_text()
event_owner = (ROOT / "src" / "mesh_event_owner.c").read_text()
event_owner_registry = (
    ROOT / "src" / "mesh_event_owner_registry.c"
).read_text()
relay = (ROOT / "src" / "mesh_relay.c").read_text()

owner_result = function_body(coordination, "mesh_owned_schedule_result")
assert "app_mesh_schedule_recovery_decide" in owner_result
assert "app_watchdog_stop_feeding()" in owner_result

resume = function_body(transport, "mesh_transport_resume")
for owner in (
    "transport-resume-rx",
    "transport-resume-route-action",
    "transport-resume-c5-flood",
    "transport-resume-route-discovery",
):
    assert owner in resume, f"resume does not re-arm {owner}"
assert "mesh_schedule_tx_timeout()" in resume
assert "mesh_schedule_route_waiting_retry_after(" in resume
assert "report_tx_schedule(0u)" in resume
assert "mesh_restart_role_scan()" in resume
admission_resume = resume.index("radio_guard_uwb_admission_resume()")
event_negotiation_resume = resume.index(
    "mesh_event_negotiation_schedule_next()",
    admission_resume,
)
assert admission_resume < event_negotiation_resume

pause = function_body(
    transport, "mesh_transport_pause_preserving_queued"
)
assert (
    "mesh_cancel_delayable(&mesh_event_negotiation_retry_work)"
    in pause
), "transport pause must cancel the event-negotiation retry owner"

event_negotiation_schedule = function_body(
    event_tx, "mesh_event_negotiation_schedule_next"
)
assert (
    "mesh_reschedule_owned_work(&mesh_event_negotiation_retry_work"
    in event_negotiation_schedule
)
assert '"event-negotiation"' in event_negotiation_schedule
assert "mesh_reschedule_delayable(" not in event_negotiation_schedule

route_refresh_schedule = function_body(
    rx, "mesh_route_refresh_schedule"
)
assert "mesh_reschedule_owned_work(" in route_refresh_schedule
assert '"gateway-route-refresh"' in route_refresh_schedule
assert "mesh_reschedule_delayable(" not in route_refresh_schedule

schedule_scan = function_body(transport, "mesh_schedule_uwb_rx")
assert "mesh_reschedule_owned_work(&mesh_uwb_rx_work" in schedule_scan
restart_scan = function_body(transport, "mesh_restart_role_scan")
assert restart_scan.count("mesh_start_uwb_rx(") == 2

report_schedule = function_body(delivery, "report_tx_schedule")
assert "mesh_reschedule_owned_work(&report_tx_work" in report_schedule

event_owner_begin_peer = function_body(event_tx, "mesh_event_owner_begin_peer")
assert "mesh_event_owner_registry_begin(" in event_owner_begin_peer
event_owner_available = function_body(
    event_tx, "mesh_event_owner_can_begin_peer"
)
assert "mesh_event_owner_registry_can_begin(" in event_owner_available
propose = function_body(
    event_tx, "mesh_propose_event_after_channel5_contact"
)
availability = propose.index("mesh_event_owner_can_begin_peer(peer_id, now_ms)")
prepare = propose.index("mesh_prepare_event_control_record(")
rf_send = propose.index("mesh_send_event_control_record(")
assert availability < prepare < rf_send
event_rx = function_body(event_tx, "mesh_handle_event_control")
proposal_branch = event_rx.index(
    "if (packet->msg_type == MSG_MESH_EVENT_PROPOSE)"
)
availability = event_rx.index(
    "mesh_event_owner_can_begin_peer(previous_hop_id, now_ms)",
    proposal_branch,
)
accept_prepare = event_rx.index(
    "mesh_prepare_event_control_record(", availability
)
accept_send = event_rx.index(
    "mesh_event_accept_attempt(", accept_prepare
)
assert proposal_branch < availability < accept_prepare < accept_send
event_owner_begin = function_body(
    event_owner, "mesh_event_owner_begin_with_boot_nonce"
)
peer_mismatch = event_owner_begin.index(
    "owner->generation != 0u && owner->peer_id != peer_id"
)
peer_mismatch_return = event_owner_begin.index(
    "return PROTO_ERR_NO_SPACE;", peer_mismatch
)
peer_rebind = event_owner_begin.index(
    "reset_peer_history = true;", peer_mismatch_return
)
history_clear = event_owner_begin.index(
    "memset(owner->retired_session_ids", peer_rebind
)
assert peer_mismatch < peer_mismatch_return < peer_rebind < history_clear
registry_tombstone = function_body(
    event_owner_registry, "tombstone_for_rebind"
)
assert (
    "deadline_reached(now_ms, entry->retain_until_ms)"
    in registry_tombstone
)
registry_begin = function_body(
    event_owner_registry, "mesh_event_owner_registry_begin"
)
assert "return PROTO_ERR_NO_SPACE;" in registry_begin
assert "tombstone->retain_until_ms" in registry_begin

route_action = function_body(delivery, "mesh_schedule_route_request_action")
schedule = route_action.index("mesh_reschedule_owned_work(")
assert "mesh_route_request_action_pending = false" not in route_action[schedule:]
route_action_work = function_body(
    delivery, "mesh_route_request_action_work_handler"
)
assert "mesh_reschedule_owned_work(&mesh_route_request_action_work" in (
    route_action_work
)

c5_store = function_body(transport, "mesh_c5_flood_store_deferred")
assert "entry->valid = true" in c5_store
assert "entry->generation++" in c5_store
assert "entry = &mesh_route_adv_deferred" in c5_store
assert "mesh_c5_flood_deferred_lock" in c5_store
assert "mesh_reschedule_owned_work(&mesh_c5_flood_work" in c5_store
c5_worker = function_body(delivery, "mesh_c5_flood_work_handler")
assert c5_worker.count("mesh_reschedule_owned_work(&mesh_c5_flood_work") == 3
assert '"paused-c5-flood"' in c5_worker
assert '"c5-flood-next-lane"' in c5_worker
assert c5_worker.count("mesh_c5_flood_deferred_lock") >= 6
assert "entry->generation == generation" in c5_worker

c5_response = function_body(
    route_control, "mesh_send_c5_flood_response"
)
assert "result.sent_count > 0u" in c5_response
assert "mesh_send_failure_retryable(ret)" in c5_response
assert "store_ret =" in c5_response
assert "mesh_c5_flood_store_deferred(" in c5_response
assert "if (store_ret == 0)" in c5_response
assert "*forward_admission_retained = true" in c5_response
assert "ret = store_ret" in c5_response
rollback = function_body(
    relay, "mesh_relay_rollback_forward_admission"
)
assert "broadcast_command_replay_rollback(" in rollback
assert "entry->valid = false" in rollback
assert "forward_admission_previous_gateway_route_adv_seq" in rollback
result_actions = function_body(delivery, "mesh_handle_result_actions")
assert result_actions.count(
    "mesh_rollback_c5_forward_admission("
) == 1
assert "if (result->route_state_changed)" in result_actions
assert "committed gateway route advertisement could not retain retry" in (
    result_actions
)
assert result_actions.count("mesh_transport_pause_preserving_queued()") >= 2
assert result_actions.count("app_watchdog_stop_feeding()") >= 2

# Absolute radio times are uint32_t uptime values.  A transmission just after
# the 49.7-day rollover still needs its wake lead and late-RX guard to begin
# before rollover; clamping subtraction at zero starts the wake train late and
# can mark a still-live channel-9 slot missed.
uint32_mask = (1 << 32) - 1
post_wrap_tx_ms = 7
wake_advance_ms = 50
pre_wrap_wake_ms = (post_wrap_tx_ms - wake_advance_ms) & uint32_mask
assert pre_wrap_wake_ms == uint32_mask - 42
pre_wrap_now_ms = uint32_mask - 60
assert ((pre_wrap_now_ms - pre_wrap_wake_ms) & uint32_mask) >= (1 << 31)

c5_send = function_body(transport, "mesh_send_c5_flood_now_until")
assert re.search(
    r"wake_start_ms\s*=\s*send_wake_train\s*\?\s*"
    r"first_tx_ms\s*-\s*WAKE_ADV_MS\s*:\s*first_tx_ms\s*;",
    c5_send,
)
assert "first_tx_ms > WAKE_ADV_MS" not in c5_send

c5_resume = function_body(transport, "mesh_try_send_c5_flood_resume")
assert re.search(
    r"wake_start_ms\s*=\s*first_tx_ms\s*-\s*WAKE_ADV_MS\s*;",
    c5_resume,
)
assert "first_tx_ms > WAKE_ADV_MS" not in c5_resume

late_guard_ms = 20
post_wrap_now_ms = 3
pre_wrap_skip_reference_ms = (
    post_wrap_now_ms - late_guard_ms
) & uint32_mask
assert pre_wrap_skip_reference_ms == uint32_mask - 16
skip_reference = function_body(
    coordination, "mesh_channel9_skip_reference_ms"
)
assert re.search(
    r"return\s+now_ms\s*-\s*MESH_EVENT_RX_LATE_GUARD_MS\s*;",
    skip_reference,
)
assert "now_ms > MESH_EVENT_RX_LATE_GUARD_MS" not in skip_reference

route_wait = function_body(
    coordination, "mesh_schedule_route_waiting_retry_after"
)
assert "mesh_reschedule_owned_work(&mesh_route_waiting_work" in route_wait
route_discovery = function_body(coordination, "mesh_schedule_route_request")
assert "app_mesh_async_route_request_submit(" in route_discovery
assert "mesh_reschedule_owned_work(&mesh_route_discovery_work" in (
    route_discovery
)
route_discovery_worker = function_body(
    coordination, "mesh_route_discovery_work_handler"
)
route_snapshot = route_discovery_worker.index(
    "app_mesh_async_route_request_snapshot("
)
route_request = route_discovery_worker.index(
    "mesh_request_route(", route_snapshot
)
route_complete = route_discovery_worker.index(
    "app_mesh_async_route_request_complete(", route_request
)
route_defer = route_discovery_worker.index(
    "app_mesh_async_route_request_defer(", route_request
)
route_retry = route_discovery_worker.index(
    "mesh_reschedule_owned_work(", route_defer
)
assert route_snapshot < route_request < route_complete
assert route_request < route_defer < route_retry
assert "mesh_route_waiting_tx_valid" in route_discovery_worker[
    route_request:route_complete
]
assert "app_mesh_async_route_request_transfer_matches(" in (
    route_discovery_worker[route_request:route_complete]
)
assert "app_mesh_async_route_request_retry_delay_ms(" in resume
tx_timeout = function_body(coordination, "mesh_schedule_tx_timeout")
assert "mesh_reschedule_owned_work(&mesh_tx_timeout_work" in tx_timeout

for name in (
    "mesh_rx_work_handler",
    "mesh_process_queued_rx_now",
    "mesh_queue_from_frame_at_internal",
    "mesh_submit_queued_rx",
):
    body = function_body(rx, name)
    assert "mesh_submit_owned_work(&mesh_rx_work" in body, name
for owner in ("route-listener-rx", "route-listener-click-rx"):
    assert owner in route_control
assert "(void)mesh_submit_work(" not in route_control

timeout_handler = function_body(rx, "mesh_tx_timeout_handler")
active_restore = timeout_handler.index("app_mesh_persistence_restore_outbox(")
active_error = timeout_handler.index(
    "if (restore_ret < 0 && restore_ret != -ENOTSUP)", active_restore
)
active_return = timeout_handler.index("return;", active_error)
deferred_restore = timeout_handler.index(
    "app_mesh_persistence_restore_deferred_outbox(", active_restore
)
assert active_error < active_return < deferred_restore
assert "mesh_relay_tx_active" not in timeout_handler[
    active_error:active_return
]
assert "active-outbox-restore" in timeout_handler[
    active_error:active_return
]

event_sequence = function_body(state, "mesh_next_event_control_seq")
lock = event_sequence.index("k_spin_lock(&mesh_event_control_seq_lock)")
increment = event_sequence.index("mesh_event_control_seq++", lock)
skip_zero = event_sequence.index("mesh_event_control_seq = 1u", increment)
unlock = event_sequence.index("k_spin_unlock(&mesh_event_control_seq_lock", skip_zero)
assert lock < increment < skip_zero < unlock
assert "extern uint16_t mesh_event_control_seq" not in state_header

# The application duration helper deliberately returns one millisecond for an
# expired deadline so it is safe to feed directly to delayed work.  Any caller
# that needs to distinguish "future" from "already due" must make that decision
# first, using the same uptime sample, or a scheduler/radio loop can invent a
# new wait after the real deadline.
def signed_u32_delta(left: int, right: int) -> int:
    value = (left - right) & uint32_mask
    return value - (1 << 32) if value >= (1 << 31) else value


def deadline_reached(now_ms: int, deadline_ms: int) -> bool:
    return signed_u32_delta(now_ms, deadline_ms) >= 0


def future_duration(now_ms: int, deadline_ms: int) -> int:
    assert not deadline_reached(now_ms, deadline_ms)
    return (deadline_ms - now_ms) & uint32_mask


pre_wrap_now_ms = uint32_mask - 5
post_wrap_deadline_ms = 3
assert not deadline_reached(pre_wrap_now_ms, post_wrap_deadline_ms)
assert future_duration(pre_wrap_now_ms, post_wrap_deadline_ms) == 9
assert deadline_reached(post_wrap_deadline_ms, post_wrap_deadline_ms)
assert deadline_reached(4, post_wrap_deadline_ms)
far_future_deadline_ms = (
    pre_wrap_now_ms + ((1 << 31) - 1)
) & uint32_mask
assert not deadline_reached(pre_wrap_now_ms, far_future_deadline_ms)
assert future_duration(pre_wrap_now_ms, far_future_deadline_ms) == (
    (1 << 31) - 1
)

preempt_active = function_body(
    coordination, "mesh_gateway_route_test_preempt_active"
)
assert "uptime_deadline_reached(" in preempt_active
preempt_window = function_body(
    coordination, "mesh_gateway_route_test_preempt_window_ms"
)
assert preempt_window.index(
    "mesh_gateway_route_test_preempt_active(now_ms)"
) < preempt_window.index("uptime_ms_until_deadline(")

embedded_hold = function_body(
    coordination, "mesh_route_embedded_wait_before_reply"
)
assert embedded_hold.index(
    "!uptime_deadline_reached("
) < embedded_hold.index("uptime_ms_until_deadline(")

retry_slot = function_body(
    rx, "mesh_defer_due_retry_to_channel9_tx_slot"
)
assert retry_slot.index(
    "uptime_deadline_reached(now_ms, prepare_ms)"
) < retry_slot.index("uptime_ms_until_deadline(now_ms, prepare_ms)")

parent_repair = function_body(
    event_tx, "mesh_try_repair_selected_parent_event"
)
assert parent_repair.index(
    "uptime_deadline_reached("
) < parent_repair.index("uptime_ms_until_deadline(")

for source, name in (
    (direct_gateway, "mesh_wait_for_direct_gateway_ack_configured"),
    (route_control, "mesh_listen_for_route_reply_ack"),
    (route_control, "mesh_listen_for_route_reply"),
    (transport, "mesh_c5_flood_sleep_until_ms"),
    (event_tx, "mesh_wait_until_ms"),
):
    body = function_body(source, name)
    deadline_gate = body.index("uptime_deadline_reached(")
    duration = body.index("uptime_ms_until_deadline(")
    assert deadline_gate < duration, name

rx_worker = function_body(rx, "mesh_uwb_rx_work_handler")
for deadline in (
    "gateway_rx_deadline_ms",
    "deadline_ms",
    "channel5_gap_deadline_ms",
):
    deadline_gate_match = re.search(
        rf"uptime_deadline_reached\(\s*now_ms,\s*"
        rf"{re.escape(deadline)}\s*\)",
        rx_worker,
    )
    duration_match = re.search(
        rf"uptime_ms_until_deadline\(\s*now_ms,\s*"
        rf"{re.escape(deadline)}\s*\)",
        rx_worker,
    )
    assert deadline_gate_match is not None, deadline
    assert duration_match is not None, deadline
    assert deadline_gate_match.start() < duration_match.start(), deadline

for source in (transport, delivery, rx, route_control):
    assert "(void)mesh_reschedule_delayable(" not in source
    assert "(void)mesh_submit_work(" not in source

print("mesh scheduler liveness source invariants passed")
