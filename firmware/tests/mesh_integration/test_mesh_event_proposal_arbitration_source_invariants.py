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
    event_tx, "mesh_propose_event_after_channel5_contact"
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
clear_loser = handler.index("mesh_event_propose_clear();", predecessor)
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
    < clear_loser
    < accept_attempt
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
successful_finish = attempt.index("mesh_event_accept_finish_send(")
assert predecessor_guard < accept_send < successful_finish

finish = function_body(event_tx, "mesh_event_accept_finish_send")
finish_guard = finish.index("mesh_event_accept_predecessor_matches()")
install = finish.index("mesh_install_channel9_timing_direction(")
replace = finish.index("replace_local_owner_after_accept", install)
abandon = finish.index("mesh_event_owner_abandon(", replace)
begin = finish.index("mesh_event_owner_begin_peer(", abandon)
assert finish_guard < install < replace < abandon < begin
