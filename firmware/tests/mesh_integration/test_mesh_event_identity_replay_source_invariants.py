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


retry_header = (APP / "app_mesh_event_retry.h").read_text()
retry_source = (APP / "app_mesh_event_retry.c").read_text()
report_source = (APP / "app_mesh_report.c").read_text()
event_tx = (APP / "app_mesh_report_event_tx.inc").read_text()
delivery = (APP / "app_mesh_report_delivery.inc").read_text()
owner_header = (ROOT / "include" / "mesh_event_owner.h").read_text()
owner_source = (ROOT / "src" / "mesh_event_owner.c").read_text()
mesh_source = (ROOT / "src" / "mesh.c").read_text()
registry_header = (
    ROOT / "include" / "mesh_event_owner_registry.h"
).read_text()
registry_source = (
    ROOT / "src" / "mesh_event_owner_registry.c"
).read_text()
app_cmake = (ROOT / "app" / "CMakeLists.txt").read_text()

for source in (retry_header, retry_source, owner_header, owner_source, event_tx):
    assert "payload_fingerprint" not in source

assert "payload_digest[SEMANTIC_DIGEST_SHA256_LEN]" in retry_header
assert "proposal_payload_digest[SEMANTIC_DIGEST_SHA256_LEN]" in owner_header
assert "proposal_payload_digest_valid" in owner_header
assert "local_payload_digest[SEMANTIC_DIGEST_SHA256_LEN]" in owner_header
assert "remote_payload_digest[SEMANTIC_DIGEST_SHA256_LEN]" in owner_header
assert "semantic_digest_sha256(payload, payload_len, digest)" in retry_source
assert "semantic_digest_equal(lhs->payload_digest" in retry_source
assert "semantic_digest_sha256(payload, payload_len, digest)" in owner_source
assert "semantic_digest_equal(digest, previous_digest" in owner_source
proposal_bind_digest = function_body(
    owner_source, "mesh_event_owner_bind_remote_proposal_digest"
)
assert "memcpy(owner->proposal_payload_digest" in proposal_bind_digest
proposal_classify = function_body(owner_source, "classify_proposal")
assert "owner->proposal_payload_digest_valid" in proposal_classify
assert "semantic_digest_equal(" in proposal_classify
assert "session_is_newer(packet->session_id" in proposal_classify
assert "sequence_is_newer(packet->seq" not in proposal_classify

bind = function_body(mesh_source, "mesh_event_timing_bind_proposal_session")
assert "operation_session_id == 0u" in bind
counter_bind = bind.index("timing->event_counter = operation_session_id")
direction_bind = bind.index(
    "mesh_event_timing_set_local_first_slot_tx(timing, true)"
)
assert counter_bind < direction_bind

compatibility = function_body(
    retry_source, "app_mesh_event_accept_timing_compatible"
)
assert "accepted->event_counter == proposed->event_counter" in compatibility

prepare = function_body(event_tx, "mesh_prepare_event_control_record")
session_resolve = prepare.index("operation_session_id = session_id == 0u")
proposal_bind = prepare.index(
    "mesh_event_timing_bind_proposal_session(", session_resolve
)
timing_encode = prepare.index(
    "mesh_append_event_timing_tlvs_at(", proposal_bind
)
packet_init = prepare.index("mesh_init_event_control(", timing_encode)
assert session_resolve < proposal_bind < timing_encode < packet_init

send = function_body(event_tx, "mesh_send_event_control")
session_resolve = send.index("operation_session_id = mesh_event_new_operation_session()")
proposal_bind = send.index(
    "mesh_event_timing_bind_proposal_session(", session_resolve
)
timing_encode = send.index("mesh_append_event_timing_tlvs_at(", proposal_bind)
assert session_resolve < proposal_bind < timing_encode
assert "mesh_event_owner_next_local_sequence(owner)" in send

begin_peer = function_body(event_tx, "mesh_event_owner_begin_peer")
registry_begin = begin_peer.index("mesh_event_owner_registry_begin(")
digest_bind = begin_peer.index(
    "mesh_event_owner_bind_remote_proposal_digest(", registry_begin
)
assert registry_begin < digest_bind
assert "proposal_payload_digest == NULL" in begin_peer

assert "MESH_EVENT_ORIGIN_REPLAY_LIFETIME_MS" in report_source
assert "MESH_EVENT_CONTROL_RX_QUEUE_LIFETIME_MS" in report_source
assert (
    "mesh_event_origin_tombstones[MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS]"
    in report_source
)
assert "mesh_event_owner_registry.c" in app_cmake
assert "reject_until_ms" in registry_header
assert "retain_until_ms" in registry_header

slot = function_body(registry_source, "tombstone_for_rebind")
assert "entry->peer_id == retired_peer_id" in slot
assert "deadline_reached(now_ms, entry->retain_until_ms)" in slot

begin = function_body(registry_source, "mesh_event_owner_registry_begin")
owner_begin = begin.index("mesh_event_owner_begin_with_boot_nonce(")
tombstone_commit = begin.index("tombstone->peer_id = retired_peer_id", owner_begin)
assert "return PROTO_ERR_NO_SPACE;" in begin[:owner_begin]
assert owner_begin < tombstone_commit
assert "tombstone->reject_until_ms = now_ms + replay_lifetime_ms" in begin
assert "tombstone->retain_until_ms" in begin

classify = function_body(
    registry_source, "mesh_event_owner_registry_classify_proposal"
)
lookup = classify.index("mesh_event_owner_registry_find(")
tombstone_check = classify.index(
    "proposal_arrived_in_retained_window(", lookup
)
ordinary = classify.index("mesh_event_owner_classify_proposal(", tombstone_check)
assert lookup < tombstone_check < ordinary

handler = function_body(event_tx, "mesh_handle_event_control")
queue_expiry = handler.index("MESH_EVENT_CONTROL_RX_QUEUE_LIFETIME_MS")
timing_parse = handler.index("mesh_event_timing_from_tlvs_at(")
assert (
    "timing.local_tx_on_even_events = !timing.local_tx_on_even_events"
    in handler
)
proposal = handler.index("if (packet->msg_type == MSG_MESH_EVENT_PROPOSE)")
registry_classify = handler.index(
    "mesh_event_owner_registry_classify_proposal(", proposal
)
duplicate = handler.index("mesh_event_accept_duplicate(", registry_classify)
capacity = handler.index(
    "mesh_event_owner_can_begin_peer(previous_hop_id, now_ms)",
    duplicate,
)
reservation = handler.index(
    "app_mesh_c5_event_accept_reservation(", capacity
)
assert queue_expiry < timing_parse < proposal
assert proposal < registry_classify < duplicate < capacity < reservation
accept = handler.index("else if (packet->msg_type == MSG_MESH_EVENT_ACCEPT)")
record_valid = handler.index("mesh_event_propose_record.valid", accept)
accept_timing = handler.index(
    "app_mesh_event_accept_timing_compatible(", record_valid
)
assert accept < record_valid < accept_timing

clicker_reject = handler[
    handler.index("clicker ignored mesh event control from non-parent") :
    handler.index("#endif")
]
assert "return false;" in clicker_reject
queue_reject = handler[
    handler.index("MESH_EVENT_CONTROL_RX_QUEUE_LIFETIME_MS") :
    handler.index("if (packet->msg_type == MSG_MESH_EVENT_UPDATE")
]
assert "return false;" in queue_reject
owner_reject = handler[
    handler.index("if (owner_decision != MESH_EVENT_OWNER_APPLY)") :
    handler.index("if (packet->msg_type == MSG_MESH_EVENT_END)")
]
assert "return false;" in owner_reject
assert (
    "if (owner_decision == MESH_EVENT_OWNER_DUPLICATE) {\n"
    "            return true;"
) in handler
cached_result = handler[
    handler.index("enum mesh_event_cached_proposal_result cached") :
    handler.index("if (owner_decision == MESH_EVENT_OWNER_DUPLICATE)", proposal)
]
assert "return cached == MESH_EVENT_CACHED_PROPOSAL_ADMITTED;" in cached_result
accept_reject = handler[
    handler.index("if (accept_match == APP_MESH_EVENT_REQUEST_CONFLICT", accept) :
    handler.index("replayed_event_accept =", accept)
]
assert "return false;" in accept_reject
uncorrelated = handler[
    handler.index("APP_MESH_EVENT_ACCEPT_REJECT", accept) :
    handler.index("APP_MESH_EVENT_ACCEPT_LEGACY", accept)
]
assert "return false;" in uncorrelated

drain = function_body(delivery, "mesh_drain_rx_queue_locked")
delivered = drain.index("delivered_event_control = true")
handled = drain.index("admitted_event_control = mesh_handle_event_control(")
contact = drain.index("mesh_c5_control_rx_semantically_admitted(", handled)
consumed = drain.index("if (delivered_event_control)", contact)
assert delivered < handled < contact < consumed
assert "admitted_event_control))" in drain[contact:consumed]
