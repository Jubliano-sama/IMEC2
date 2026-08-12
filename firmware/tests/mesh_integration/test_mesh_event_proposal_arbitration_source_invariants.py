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
transport = (APP / "app_mesh_report_transport.inc").read_text()
route_control = (APP / "app_mesh_report_route_control.inc").read_text()
anchor_radio = (APP / "app_anchor_radio.inc").read_text()
report_source = (APP / "app_mesh_report.c").read_text()
owner_source = (ROOT / "src" / "mesh_event_owner.c").read_text()

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
remote_owner_guard = propose.index("current_owner->proposal_from_peer")
active_timing_guard = propose.index(
    "mesh_find_active_channel9_timing(", remote_owner_guard
)
proposal_prepare = propose.index("mesh_prepare_event_control_record(")
proposal_send = propose.index("mesh_send_event_control_record(")
owner_begin = propose.index("mesh_event_owner_begin_peer(")
window_arm = propose.index("mesh_event_local_proposal_window_arm(", owner_begin)
assert (
    accept_guard
    < remote_owner_guard
    < active_timing_guard
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
accept_begin = handler.index("app_mesh_event_retry_begin(", reservation)
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
    < accept_begin
    < predecessor
    < repair_token_guard
    < repair_token_copy
    < clear_loser
    < accept_attempt
)
assert (
    "mesh_forwarded_ack_event_repair_authorization.peer_id ==\n"
    "                previous_hop_id"
    in handler[repair_token_guard:repair_token_copy]
)
assert handler.count(
    "mesh_event_accept_retry.c5_repair_authorization ="
) == 1

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
install = finish.index("mesh_install_channel9_timing_direction(")
replace = finish.index("replace_local_owner_after_accept", install)
abandon = finish.index("mesh_event_owner_abandon(", replace)
begin = finish.index("mesh_event_owner_begin_peer(", abandon)
assert finish_guard < install < replace < abandon < begin

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
retry = propose.index("mesh_event_retry_after_failure(", timing_clear)
assert accept_timeout < timeout_discard < timing_clear < retry
assert '"event-accept-timeout"' in propose[timeout_discard:timing_clear]

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

event_end_tx = function_body(event_tx, "mesh_close_channel9_connection")
assert "mesh_ch9_ack_batch_discard_if_safe(" in event_end_tx
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

# Passive relay/backoff and route-wait records must not suppress the low-duty
# Channel-5 contact scan that stale forwarded ACK repair depends on.  The scan
# still defers for every owner that represents actual RF use or an imminent
# Channel-9 reservation.
scan = function_body(anchor_radio, "anchor_uwb_scan_work_handler")
assert "relay_tx_active = mesh_relay_tx_active(&mesh_runtime);" in scan
assert "mesh_route_waiting_tx_active()" in scan
block_start = scan.index("if (anchor_uwb_window_active()")
block_end = scan.index("uwb_radio_busy) {", block_start) + len("uwb_radio_busy) {")
block_condition = scan[block_start:block_end]
assert "anchor_uwb_window_active()" in block_condition
assert "app_anchor_survey_runtime_radio_active()" in block_condition
assert "app_anchor_survey_runtime_discovery_is_pending()" not in block_condition
assert "mesh_rx_active" in block_condition
assert "ch9_rx_conflict" in block_condition
assert "uwb_radio_busy" in block_condition
assert "relay_tx_active" not in block_condition
assert "route_waiting_active" not in block_condition
guard_acquire = scan.index(
    "radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN,",
    block_end,
)
assert block_end < guard_acquire
