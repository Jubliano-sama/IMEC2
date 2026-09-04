#!/usr/bin/env python3
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "app" / "src"


def function_body(source: str, name: str) -> str:
    definition = re.search(
        rf"(?ms)^[A-Za-z_][^;{{]*?\b{re.escape(name)}\s*\([^;]*?\)\s*\{{",
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


event_tx = (APP / "app_mesh_report_event_tx.inc").read_text()
coordination = (APP / "app_mesh_report_coordination.inc").read_text()
delivery = (APP / "app_mesh_report_delivery.inc").read_text()
transport = (APP / "app_mesh_report_transport.inc").read_text()
route_control = (APP / "app_mesh_report_route_control.inc").read_text()
anchor_radio = (APP / "app_anchor_radio.inc").read_text()
ch9_ack_source = (APP / "app_mesh_ch9_ack.c").read_text()
report_source = (APP / "app_mesh_report.c").read_text()
owner_source = (ROOT / "src" / "mesh_event_owner.c").read_text()
mesh_source = (ROOT / "src" / "mesh.c").read_text()
route_rx_source = (ROOT / "src" / "mesh_relay_route_rx.inc").read_text()

assert (
    "expires_at_ms[MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS]"
    in report_source
)
assert "event reciprocal-window ownership must fit its validity mask" in report_source

arbitrate = function_body(
    owner_source, "mesh_event_owner_arbitrate_reciprocal_proposal"
)
assert "local_id < peer_id" in arbitrate
assert "MESH_EVENT_PROPOSAL_KEEP_LOCAL" in arbitrate
assert "MESH_EVENT_PROPOSAL_ACCEPT_REMOTE" in arbitrate
assert "local_proposal_peer_id == peer_id" in arbitrate
assert "!owner->proposal_from_peer" in arbitrate

window_match = function_body(
    event_tx, "mesh_event_local_proposal_window_matches"
)
assert "uptime_deadline_reached(" in window_match
assert "valid_mask" in window_match
assert "owner->proposal_from_peer" in window_match

propose = function_body(
    event_tx, "mesh_propose_event_after_channel5_contact_authorized"
)
accept_guard = propose.index("mesh_event_accept_retry.retry.active")
active_timing_guard = propose.index(
    "if (mesh_find_active_channel9_timing(", accept_guard
)
active_timing_return = propose.index("return 0;", active_timing_guard)
proposal_prepare = propose.index("mesh_prepare_event_control_record(")
proposal_send = propose.index("mesh_send_event_control_record(")
owner_begin = propose.index("mesh_event_owner_begin_peer(")
window_arm = propose.index("mesh_event_local_proposal_window_arm(", owner_begin)
assert "current_owner" not in propose[accept_guard:proposal_prepare]
assert (
    accept_guard
    < active_timing_guard
    < active_timing_return
    < proposal_prepare
    < proposal_send
    < owner_begin
    < window_arm
)

handler = function_body(event_tx, "mesh_handle_event_control")
proposal = handler.index("if (packet->msg_type == MSG_MESH_EVENT_PROPOSE)")
window_proof = handler.index(
    "mesh_event_local_proposal_window_matches(", proposal
)
arbitration = handler.index(
    "mesh_event_owner_arbitrate_reciprocal_proposal(", window_proof
)
keep_local = handler.index(
    "arbitration == MESH_EVENT_PROPOSAL_KEEP_LOCAL", arbitration
)
classify = handler.index(
    "mesh_event_owner_registry_classify_proposal(", keep_local
)
duplicate = handler.index("mesh_event_accept_duplicate(", classify)
reservation = handler.index(
    "app_mesh_c5_event_accept_reservation(", duplicate
)
guard_check = handler.index(
    "mesh_relay_check_channel9_timing_guarded_direction(", reservation
)
accept_begin = handler.index("app_mesh_event_retry_begin(", guard_check)
predecessor = handler.index(
    "predecessor_owner_generation", accept_begin
)
repair_token_guard = handler.index(
    "mesh_forwarded_ack_event_repair_authorization.valid", predecessor
)
repair_token_copy = handler.index(
    "mesh_event_accept_retry.c5_repair_authorization =", repair_token_guard
)
clear_loser = handler.index("mesh_event_propose_clear();", repair_token_copy)
accept_attempt = handler.index("mesh_event_accept_attempt(", clear_loser)
assert (
    window_proof
    < arbitration
    < keep_local
    < classify
    < duplicate
    < reservation
    < guard_check
    < accept_begin
    < predecessor
    < repair_token_guard
    < repair_token_copy
    < clear_loser
    < accept_attempt
)
guard_call = handler[guard_check:accept_begin]
assert "&reservation_timing" in guard_call
assert "MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM" in guard_call
counterphase = handler.index(
    "mesh_event_accept_align_to_upstream_half_phase(", guard_check
)
counterphase_reservation = handler.index(
    "app_mesh_c5_event_accept_reservation(", counterphase
)
counterphase_guard = handler.index(
    "mesh_relay_check_channel9_timing_guarded_direction(",
    counterphase_reservation,
)
response_prepare = handler.index(
    "mesh_prepare_event_control_record(", counterphase_guard
)
assert (
    guard_check
    < counterphase
    < counterphase_reservation
    < counterphase_guard
    < response_prepare
    < accept_begin
)
assert "accept_phase_shift_ms" in handler[response_prepare:accept_begin]
assert (
    "mesh_forwarded_ack_event_repair_authorization.peer_id ==\n"
    "                previous_hop_id"
    in handler[repair_token_guard:repair_token_copy]
)
assert handler.count(
    "mesh_event_accept_retry.c5_repair_authorization ="
) == 1

counterphase_body = function_body(
    event_tx, "mesh_event_accept_align_to_upstream_half_phase"
)
assert "MESH_RELAY_CHANNEL9_GUARD_INTERVAL_CONFLICT" in counterphase_body
assert "MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM" in counterphase_body
assert "upstream->timing.event_interval_ms != proposed->event_interval_ms" in counterphase_body
assert "upstream->timing.event_window_ms != proposed->event_window_ms" in counterphase_body
assert "upstream->timing.guard_ms != proposed->guard_ms" in counterphase_body
assert "interval_ms / 2u" in counterphase_body
assert "signed_shift_ms == 0" in counterphase_body
assert "app_mesh_event_timing_apply_phase_shift(accepted" in counterphase_body

shift_parser = function_body(
    event_tx, "mesh_event_accept_phase_shift_from_payload"
)
assert "tlv_find_unique(" in shift_parser
assert "TLV_MESH_EVENT_PHASE_SHIFT_MS" in shift_parser
assert "ret == PROTO_ERR_NOT_FOUND" in shift_parser
assert "*phase_shift_ms = 0u" in shift_parser
assert "value_len != sizeof(uint16_t)" in shift_parser
assert "*phase_shift_ms < event_interval_ms" in shift_parser

prepare_control = function_body(event_tx, "mesh_prepare_event_control_record")
assert "else if (phase_shift_ms != 0u)" in prepare_control
assert "TLV_MESH_EVENT_PHASE_SHIFT_MS" in prepare_control

accept_branch = handler.index(
    "} else if (packet->msg_type == MSG_MESH_EVENT_ACCEPT)"
)
shift_parse = handler.index(
    "mesh_event_accept_phase_shift_from_payload(", accept_branch
)
accept_match = handler.index("mesh_event_accept_rx_match(", shift_parse)
replay_guard = handler.index("if (replayed_event_accept)", accept_match)
replay_return = handler.index("return true;", replay_guard)
frozen_proposal = handler.index(
    "timing = mesh_event_propose_record.timing", replay_return
)
apply_shift = handler.index(
    "app_mesh_event_timing_apply_phase_shift(&timing", frozen_proposal
)
bind_parity = handler.index(
    "mesh_event_timing_set_local_first_slot_tx(&timing, true)", apply_shift
)
install_shifted = handler.index(
    "mesh_install_channel9_timing_direction(", bind_parity
)
assert (
    shift_parse
    < accept_match
    < replay_guard
    < replay_return
    < frozen_proposal
    < apply_shift
    < bind_parity
    < install_shifted
)

wire_validator = function_body(mesh_source, "mesh_event_control_payload_validate")
assert "TLV_MESH_EVENT_PHASE_SHIFT_MS" in wire_validator
assert "packet->msg_type != MSG_MESH_EVENT_ACCEPT" in wire_validator
assert "tlv_find_unique(" in wire_validator
assert "phase_shift_len != sizeof(uint16_t)" in wire_validator
assert "proto_get_u16_le(phase_shift_raw) >= timing.event_interval_ms" in wire_validator

route_request = function_body(route_rx_source, "handle_route_request")
route_timing_guard = route_request.index(
    "mesh_relay_check_channel9_timing_guarded_direction("
)
interval_only = route_request.index(
    "MESH_RELAY_CHANNEL9_GUARD_INTERVAL_CONFLICT", route_timing_guard
)
upstream_only = route_request.index(
    "MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM", interval_only
)
omit_install = route_request.index(
    "install_reply_timing = false", upstream_only
)
omit_wire = route_request.index(
    "fields.proposed_channel9_timing_valid = false", omit_install
)
other_reject = route_request.index("if (ret != PROTO_OK)", omit_wire)
route_admission = route_request.index("upsert_reactive_route(", other_reject)
reply_build = route_request.index("build_route_reply(", route_admission)
assert (
    route_timing_guard
    < interval_only
    < upstream_only
    < omit_install
    < omit_wire
    < other_reject
    < route_admission
    < reply_build
)

ordinary_classifier = function_body(
    owner_source, "mesh_event_owner_classify_proposal"
)
reciprocal_classifier = function_body(
    owner_source, "mesh_event_owner_classify_reciprocal_proposal"
)
assert "payload_len, false" in ordinary_classifier
assert "payload_len, true" in reciprocal_classifier
assert "reciprocal_local_proposal_proven" in owner_source
assert "local_id > previous_hop_id" in owner_source

attempt = function_body(event_tx, "mesh_event_accept_attempt")
predecessor_guard = attempt.index(
    "mesh_event_accept_predecessor_matches()"
)
accept_send = attempt.index("mesh_send_event_control_record(")
accept_token = attempt.index(
    "mesh_event_accept_retry.c5_repair_authorization.valid", accept_send
)
causal_intent = attempt.index(
    "FW_C5_TX_INTENT_CAUSAL_RESPONSE", accept_token
)
successful_finish = attempt.index("mesh_event_accept_finish_send(")
assert (
    predecessor_guard
    < accept_send
    < accept_token
    < causal_intent
    < successful_finish
)

# Relative event timing must be reanchored at the physical event-control TX,
# not after the synchronous Channel-5 owner finishes contact teardown. The
# latter can be much later and makes the peer's RX window miss our TX window.
send_record = function_body(event_tx, "mesh_send_event_control_record")
record_anchor = send_record.index("observation.tx_completed ?")
record_timestamp = send_record.index(
    "(uint32_t)observation.tx_completed_at_ms", record_anchor
)
record_reanchor = send_record.index(
    "mesh_event_timing_reanchor_after_control_tx(", record_timestamp
)
assert record_anchor < record_timestamp < record_reanchor

# A source waiting for the gateway ACK may accept only the exact timing
# proposal from its current upstream ACK owner. The coordinator promotes that
# validated record to the narrow state-machine intent on both pre-send checks.
ack_rx_validator = function_body(
    coordination, "mesh_c5_ack_rx_timing_response_candidate_valid"
)
assert "mesh_event_accept_retry.retry.active" in ack_rx_validator
assert "app_mesh_ch9_core_ack_wait_active(" in ack_rx_validator
assert "pending->next_hop_id == candidate->next_hop_id" in ack_rx_validator
assert "candidate->next_hop_id == response->peer_id" in ack_rx_validator
assert "candidate->packet.msg_type == MSG_MESH_EVENT_ACCEPT" in ack_rx_validator
assert "memcmp(candidate->payload" in ack_rx_validator
coordinator = function_body(
    coordination, "mesh_coordinator_c5_tx_allowed_authorized_intent"
)
assert "FW_C5_TX_INTENT_ACK_RX_TIMING_RESPONSE" in coordinator
assert coordinator.count(
    "mesh_c5_ack_rx_timing_response_candidate_valid(candidate)"
) == 2

# Ordinary and completed-response ACCEPTs start from a cleared singleton and
# cannot inherit the retained-ACK capability.  Only the reciprocal arbitration
# block above performs a token transfer.
accept_clear = function_body(event_tx, "mesh_event_accept_clear")
assert "memset(&mesh_event_accept_retry, 0" in accept_clear
duplicate_accept = function_body(event_tx, "mesh_event_accept_duplicate")
assert "memset(&mesh_event_accept_retry, 0" in duplicate_accept
assert "c5_repair_authorization" not in duplicate_accept

finish = function_body(event_tx, "mesh_event_accept_finish_send")
finish_guard = finish.index("mesh_event_accept_predecessor_matches()")
preserved = finish.index(
    "committed_timing = mesh_event_accept_retry.response.timing"
)
defer_first = finish.index(
    "mesh_event_timing_defer_first_start_if_needed("
)
install = finish.index("mesh_install_channel9_timing_direction(")
assert "&committed_timing" in finish[defer_first:install]
replace = finish.index("replace_local_owner_after_accept", install)
abandon = finish.index("mesh_event_owner_abandon(", replace)
begin = finish.index("mesh_event_owner_begin_peer(", abandon)
assert finish_guard < preserved < defer_first < install < replace < abandon < begin
assert "app_mesh_c5_event_accept_realign_is_reserved" not in finish
assert "reservation_timing" not in finish
assert "REALIGN_OUT_OF_BOUNDS" not in finish
assert "MESH_RADIO_EVENT_ACCEPT_REALIGN_SLOP_MS" in finish
install_call = finish[install:finish.index(");", install) + 2]
assert "&committed_timing" in install_call
assert "committed_timing = transmitted_timing" not in finish
assert "transmitted_timing" not in install_call

defer_first_body = function_body(
    event_tx, "mesh_event_timing_defer_first_start_if_needed"
)
assert "timing->next_event_time_ms += defer_ms" in defer_first_body
assert "timing->last_successful_ch9_event_ms += defer_ms" in defer_first_body
assert "timing->event_counter += periods" in defer_first_body

physical_rx = function_body(
    coordination, "mesh_rx_pending_physical_received_at_ms"
)
assert "pending->first_received_at_ms" in physical_rx
assert "pending->received_at_ms" not in physical_rx

drain = function_body(delivery, "mesh_drain_rx_queue_locked")
event_delivery = drain.index("admitted_event_control = mesh_handle_event_control(")
event_delivery_end = drain.index(");", event_delivery)
assert (
    "mesh_rx_pending_physical_received_at_ms(pending)"
    in drain[event_delivery:event_delivery_end]
)

route_listener = function_body(route_control, "mesh_listen_for_route_reply")
timestamp_capture = route_listener.index(
    "dwm3000_driver_last_rx_host_uptime(&received_at_ms)"
)
inline_event = route_listener.index("mesh_handle_event_control(", timestamp_capture)
queued_event = route_listener.index(
    "mesh_queue_from_frame_at_internal(", inline_event
)
assert timestamp_capture < inline_event < queued_event
assert "received_at_ms" in route_listener[
    inline_event : route_listener.index(");", inline_event)
]
assert "received_at_ms" in route_listener[
    queued_event : route_listener.index(");", queued_event)
]

guarded_install = function_body(
    coordination, "mesh_install_channel9_timing_direction"
)
assert "mesh_relay_set_channel9_timing_guarded_direction(" in guarded_install

# A forwarded gateway ACK is the last custody evidence for a child packet.  A
# stale or explicitly closed Channel-9 timing may retire ordinary hop ACKs, but
# it must not discard this preserved envelope. Transit-core ACKs wait for exact
# RF send plus core commit; late terminal forwards wait for exact RF send only.
requires_commit = function_body(
    coordination, "mesh_ch9_ack_batch_requires_physical_commit"
)
assert "batch->preserve_payload" in requires_commit
assert "APP_MESH_CH9_ACK_OWNER_TRANSIT_CORE" in requires_commit
assert "batch->template_ack.packet.msg_type == MSG_GATEWAY_ACK" in requires_commit

late_forward = function_body(
    coordination, "mesh_ch9_ack_batch_is_late_terminal_forward"
)
assert "batch->preserve_payload" in late_forward
assert "APP_MESH_CH9_ACK_OWNER_LATE_TERMINAL_FORWARD" in late_forward
assert "batch->template_ack.packet.msg_type == MSG_GATEWAY_ACK" in late_forward

safe_discard = function_body(coordination, "mesh_ch9_ack_batch_discard_if_safe")
preserve_guard = safe_discard.index("mesh_ch9_ack_batch_preserves_terminal_forward(")
retain = safe_discard.index("return false;", preserve_guard)
ordinary_clear = safe_discard.index("mesh_ch9_ack_batch_clear_for_peer(", retain)
assert preserve_guard < retain < ordinary_clear, (
    "custody-critical forwarded ACKs must be retained while ordinary obsolete "
    "hop ACK batches remain discardable"
)

expire = function_body(coordination, "mesh_expire_channel9_timings")
assert "mesh_ch9_ack_batch_discard_if_safe(" in expire
assert "mesh_ch9_ack_batch_clear_for_peer(" not in expire

select_ack = function_body(transport, "mesh_select_channel9_ack_tx_event")
stale = select_ack.index("if (ret == PROTO_ERR_STALE)")
physical_commit = select_ack.index(
    "mesh_ch9_ack_batch_requires_physical_commit(peer_id)", stale
)
late_terminal = select_ack.index(
    "mesh_ch9_ack_batch_is_late_terminal_forward(peer_id)", physical_commit
)
repair = select_ack.index(
    "mesh_propose_event_after_channel5_contact_authorized(", late_terminal
)
repair_reason = select_ack.index('"forwarded-gateway-ack-event-repair"', repair)
assert "&authorization" in select_ack[repair:repair_reason + 200]
ordinary_discard = select_ack.index("mesh_ch9_ack_batch_discard_if_safe(", repair_reason)
assert stale < physical_commit < late_terminal < repair < repair_reason < ordinary_discard
assert '"ack-tx-stale"' in select_ack[ordinary_discard:]

propose = function_body(
    event_tx, "mesh_propose_event_after_channel5_contact_authorized"
)
accept_timeout = propose.index("if (ret < 0)", propose.index("mesh_listen_for_route_reply("))
timeout_discard = propose.index(
    "mesh_ch9_ack_batch_discard_if_safe(", accept_timeout
)
timing_clear = propose.index("mesh_relay_clear_channel9_timing(", timeout_discard)
proposal_clear = propose.index("mesh_event_propose_clear();", timing_clear)
terminal_return = propose.index("return -ETIMEDOUT;", proposal_clear)
assert accept_timeout < timeout_discard < timing_clear < proposal_clear < terminal_return
assert '"event-accept-timeout"' in propose[timeout_discard:timing_clear]
assert "mesh_event_propose_retry_after_failure(" not in propose

event_handler = function_body(event_tx, "mesh_handle_event_control")
event_end_rx = event_handler.index("if (packet->msg_type == MSG_MESH_EVENT_END)")
event_end_commit = event_handler.index("mesh_event_owner_commit(", event_end_rx)
event_end_discard = event_handler.index(
    "mesh_ch9_ack_batch_discard_if_safe(", event_end_commit
)
event_end_timing_clear = event_handler.index(
    "mesh_relay_clear_channel9_timing(", event_end_discard
)
assert event_end_rx < event_end_commit < event_end_discard < event_end_timing_clear
assert '"event-end-rx"' in event_handler[event_end_discard:event_end_timing_clear]

event_end_arm = function_body(event_tx, "mesh_close_channel9_connection")
assert "mesh_ch9_ack_batch_discard_if_safe(" not in event_end_arm
assert "mesh_relay_clear_channel9_timing(" not in event_end_arm
assert "mesh_ch9_close_intent" in event_end_arm
event_end_tx = function_body(event_tx, "mesh_try_close_channel9_connection")
send_end = event_end_tx.index("mesh_send_event_control(")
discard_after_send = event_end_tx.index(
    "mesh_ch9_ack_batch_discard_if_safe(", send_end
)
clear_after_send = event_end_tx.index(
    "mesh_relay_clear_channel9_timing(", discard_after_send
)
assert send_end < discard_after_send < clear_after_send
assert "mesh_ch9_ack_batch_clear_for_peer(" not in event_end_tx

commit_forward = function_body(
    route_control, "mesh_commit_forwarded_gateway_ack_sent"
)
relay_commit = commit_forward.index("mesh_relay_commit_transit_gateway_ack_forward(")
commit_proof = commit_forward.index("MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED", relay_commit)
assert relay_commit < commit_proof
assert "app_mesh_ch9_ack_table_clear_peer(" not in commit_forward

send_batch = function_body(route_control, "mesh_send_pending_ch9_ack_batch")
physical_send = send_batch.index("mesh_send_outbound(")
send_success = send_batch.index("if (ret == 0)", physical_send)
forwarded_branch = send_batch.index("if (transit_core_commit)", send_success)
forwarded_commit = send_batch.index(
    "mesh_commit_forwarded_gateway_ack_sent(&ack)", forwarded_branch
)
clear_after_commit = send_batch.index(
    "app_mesh_ch9_ack_table_clear_peer(", forwarded_commit
)
assert physical_send < send_success < forwarded_branch < forwarded_commit < clear_after_commit
assert "batch->owner == APP_MESH_CH9_ACK_OWNER_TRANSIT_CORE" in send_batch[
    :physical_send
]

# Passive relay custody, a far-future retry, and route-wait records must not
# suppress the low-duty Channel-5 contact scan. A retry that is due, or would
# become due before the scan and retune finish, gets the next radio turn. Only
# the relay core's live WAIT_GATEWAY_ACK/WAIT_GATEWAY_ACK_FORWARD receive turn
# blocks the scan: queued batch-ACK custody does not prove that RX is live.
scan = function_body(anchor_radio, "anchor_uwb_scan_work_handler")
assert "relay_tx_active = mesh_relay_tx_active(&mesh_runtime);" in scan
assert "mesh_route_waiting_tx_active()" in scan
ack_wait_sample = scan.index(
    "ch9_ack_wait_active = app_mesh_ch9_core_ack_wait_active("
)
ack_wait_sample_end = scan.index(";", ack_wait_sample)
scan_ack_wait = scan[ack_wait_sample:ack_wait_sample_end]
assert "&mesh_runtime.pending" in scan_ack_wait
assert "relay_tx_active" in scan_ack_wait
assert "mesh_report_ch9_ack_wait_active" not in scan_ack_wait
assert "mesh_ch9_tx_pending_is_active" not in scan_ack_wait
assert "app_mesh_ch9_ack_table_any_pending" not in scan_ack_wait
core_ack_wait = function_body(
    ch9_ack_source, "app_mesh_ch9_core_ack_wait_active"
)
assert "pending->state == MESH_RELAY_TX_WAIT_GATEWAY_ACK" in core_ack_wait
assert (
    "pending->state == MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD"
    in core_ack_wait
)
assert "MESH_RELAY_TX_WAIT_RETRY_BACKOFF" not in core_ack_wait
retry_gate = function_body(anchor_radio, "anchor_relay_retry_plan_scan")
assert "mesh_relay_tx_active(&mesh_runtime)" in retry_gate
assert "MESH_RELAY_TX_WAIT_RETRY_BACKOFF" in retry_gate
assert "uptime_deadline_reached(" in retry_gate
assert "uptime_ms_until_deadline(" in retry_gate
assert "mesh_runtime.pending.retry_after_ms" in retry_gate
assert "scan_rx_ms" in retry_gate
assert "ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS" in retry_gate
assert "MESH_RADIO_EVENT_RETUNE_GUARD_MS" in retry_gate
assert "ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS" in retry_gate
block_start = scan.index("if (anchor_uwb_window_active()")
block_end = scan.index("uwb_radio_busy) {", block_start) + len("uwb_radio_busy) {")
block_condition = scan[block_start:block_end]
assert "anchor_uwb_window_active()" in block_condition
assert "mesh_rx_active" in block_condition
assert "ch9_ack_wait_active" in block_condition
assert "ch9_rx_conflict" in block_condition
assert "uwb_radio_busy" in block_condition
assert "relay_tx_retry_blocks_scan" in block_condition
assert "relay_tx_active" not in block_condition
assert "route_waiting_active" not in block_condition
assert ack_wait_sample < block_start
retry_select_end = scan.index("uint32_t retry_ms =", block_end)
retry_select_end = scan.index(";", retry_select_end) + 1
retry_selection = scan[block_end:retry_select_end]
assert (
    "ch9_ack_wait_active ? ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS"
    in retry_selection
)
blocked_trace = scan.index("DBG_ANCHOR_CH5_SCAN_BLOCKED", retry_select_end)
blocked_schedule = scan.index(
    "anchor_uwb_scan_schedule_ms(retry_ms);", blocked_trace
)
blocked_return = scan.index("return;", blocked_schedule)
assert "ch9_ack_wait_active ? 1u : 0u" in scan[
    blocked_trace:blocked_schedule
]
guard_acquire = scan.index(
    "radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN,",
    blocked_return,
)
assert block_end < blocked_schedule < blocked_return < guard_acquire

# Operation responses remain source-owned until their end-to-end ACK.  A
# relay may release only the exact retained topology ACK after the bounded
# child-contact repair exhausts, while queued local deliveries suppress any
# new background repair before RF.
release = function_body(
    event_tx, "mesh_release_exhausted_operation_ack_forward"
)
assert "mesh_pending_is_retry_owned_operation_uplink" in release
assert "app_mesh_ch9_c5_repair_owner_matches" in release
assert "mesh_relay_release_topology_gateway_ack_forward" in release
assert "mesh_ch9_ack_batch_clear_for_peer" in release

terminal = function_body(event_tx, "mesh_event_propose_terminal_failure")
release_at = terminal.index("mesh_release_exhausted_operation_ack_forward")
clear_at = terminal.index("mesh_event_propose_clear()")
assert release_at < clear_at

ack_select = function_body(transport, "mesh_select_channel9_ack_tx_event")
local_gate = ack_select.index(
    "mesh_local_delivery_blocks_background_event_repair()"
)
repair_start = ack_select.index(
    "mesh_propose_event_after_channel5_contact_authorized(", local_gate
)
assert local_gate < repair_start
assert "mesh_ch9_ack_note_send_failure" in ack_select[
    local_gate:repair_start
]

deferred_repair = function_body(
    event_tx, "mesh_retry_deferred_forwarded_ack_event_repair"
)
assert "mesh_local_delivery_blocks_background_event_repair()" in deferred_repair
assert "mesh_event_propose_retry.retry_due_ms" not in deferred_repair

# Topology proposal exhaustion during discovery assignment is
# cadence contention on the shared parent's single downstream slot: the
# selected parent stays valid and the retained requester schedules the retry.
# The exhausted branch must never abandon, hold down, or rediscover.
propose_terminal = function_body(event_tx, "mesh_event_propose_terminal_failure")
topology_gate_at = propose_terminal.index(
    "if (topology_operation)"
)
cadence_at = propose_terminal.index('"topology-cadence-wait"', topology_gate_at)
cadence_end_at = (
    propose_terminal.index("1u)", cadence_at) + len("1u)")
)
wait_block = propose_terminal[topology_gate_at:cadence_end_at]
assert "mesh_schedule_route_waiting_retry_after(" in wait_block
assert "mesh_parent_contact_retry_delay_ms(peer_id, 1u)" in wait_block
for forbidden_route_collapse in (
    "mesh_relay_abandon_upstream_parent_at(",
    "mesh_event_retry_after_failure(",
    "hold_down",
):
    assert forbidden_route_collapse not in wait_block
