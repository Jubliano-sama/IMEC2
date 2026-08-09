#!/usr/bin/env python3
from pathlib import Path
import re

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = read_composed_source(ROOT / "app/src/app_anchor.c")
BLE = read_composed_source(ROOT / "app/src/app_gateway_ble.c")
RESULT_RUNTIME = (ROOT / "app/src/app_gateway_result_runtime.inc").read_text(
    encoding="utf-8"
)
MAIN = (ROOT / "app/src/main.c").read_text(encoding="utf-8")
SURVEY = (ROOT / "app/src/app_gateway_survey_observability.c").read_text(
    encoding="utf-8"
)
GATEWAY_COMMAND = (ROOT / "src/gateway_command.c").read_text(
    encoding="utf-8"
)


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    assert match is not None, f"missing function {name}"
    brace = source.index("{", match.start())
    depth = 0
    for index in range(brace, len(source)):
        depth += source[index] == "{"
        depth -= source[index] == "}"
        if depth == 0:
            return source[match.start() : index + 1]
    raise AssertionError(f"unterminated function {name}")

command_sequence = function_body(BLE, "gateway_next_command_seq")
sequence_lock = command_sequence.index(
    "k_spin_lock(&gateway_command_seq_lock)"
)
sequence_increment = command_sequence.index("gateway_command_seq++")
sequence_unlock = command_sequence.index(
    "k_spin_unlock(&gateway_command_seq_lock", sequence_increment
)
sequence_return = command_sequence.index("return sequence", sequence_unlock)
assert sequence_lock < sequence_increment < sequence_unlock < sequence_return, (
    "all gateway survey and command callers must share one serialized packet "
    "sequence domain"
)


admit = function_body(ANCHOR, "gateway_host_command_admit")
assert "k_msgq_get" not in admit
assert admit.index("gateway_observe_host_acceptance") < admit.index("k_msgq_put")

survey_work = function_body(ANCHOR, "gateway_survey_work_handler")
boundary_gate = survey_work.index(
    "if (!gateway_survey_flush_boundary_event())"
)
boundary_flush = survey_work.index(
    "gateway_survey_flush_boundary_event", boundary_gate
)
active_gate = survey_work.index("if (!gateway_survey_active)")
deadline_gate = survey_work.index("if (uptime_deadline_reached")
deadline_exit = survey_work.index("goto out;", deadline_gate)
deadline_close = survey_work.index("}", deadline_exit) + 1
pair_finalize = survey_work.index("gateway_survey_finalize_pair_observation")
assert active_gate < deadline_gate < boundary_flush, (
    "pending boundaries must flush at the first safe point after the active "
    "survey and operation-deadline checks"
)
assert not survey_work[deadline_close:boundary_gate].strip(), (
    "the boundary flush must immediately follow the operation-deadline gate"
)
for external_wait_gate in (
    "gateway_survey_wait_for_discovery_collection()",
    "gateway_survey_cleanup_pending()",
    "gateway_survey_response_ack_settle_blocks_progress(",
    "if (gateway_survey_auto.waiting)",
    "gateway_survey_round_drive()",
):
    assert boundary_flush < survey_work.index(external_wait_gate), (
        "a backpressured pair-start boundary must be flushed before external "
        f"wait gate {external_wait_gate}"
    )
assert boundary_flush < pair_finalize, (
    "a pair result must not be finalized before its pending pair-start "
    "boundary has custody"
)
assert boundary_flush < survey_work.index("survey_gateway_auto_next_action")
assert pair_finalize < survey_work.index(
    "survey_gateway_auto_next_action"
)

report_ingress = function_body(ANCHOR, "gateway_handle_survey_discovery_report")
assert "GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED" not in report_ingress
assert "survey_gateway_report_info_at(" in SURVEY
assert "survey_gateway_reverse_hint_for_target(" in SURVEY
assert "reports[state->report_cursor]" not in SURVEY, (
    "observability must reconstruct exact IDs through validated compact APIs"
)
assert "GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED" in SURVEY
assert "GATEWAY_COMMAND_EVENT_STAGE_SCHEDULE_READY" in SURVEY

ble_work = function_body(BLE, "gateway_ble_stream_work_handler")
assert "if (gateway_ble_tx_in_flight)" in ble_work
assert "credit_available = 0u" in ble_work
assert "ret = bt_gatt_notify_cb" in ble_work
assert "else if (source == GATEWAY_BLE_TX_STREAM)" in ble_work
assert "successful async submit consumes the controller credit" in ble_work
assert "app_stack_workload_diag_ble_sample_with_pressure" in ble_work
assert "gateway_ble_schedule_stream_retry(0u);" in ble_work
assert "gateway_ble_schedule_stream_retry(failure_count);" in ble_work
assert "bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN)" not in ble_work
assert re.search(
    r"#define\s+GATEWAY_BLE_TX_IN_FLIGHT_TIMEOUT_MS\s+[1-9][0-9]*u",
    BLE,
)
assert re.search(
    r"#define\s+GATEWAY_BLE_NOTIFY_FAILURE_RESET_THRESHOLD\s+[1-9][0-9]*u",
    BLE,
)

in_flight_gate = ble_work.index("if (gateway_ble_tx_in_flight)")
in_flight_exit = ble_work.index(
    "if (gateway_ble_conn == NULL", in_flight_gate
)
in_flight_wait = ble_work[in_flight_gate:in_flight_exit]
assert "uint32_t deadline_ms = gateway_ble_tx_deadline_ms" in in_flight_wait
assert "uptime_deadline_reached(now_ms, deadline_ms)" in in_flight_wait
assert "gateway_ble_reset_stalled_link(" in in_flight_wait
assert "blocked_generation, deadline_ms, \"notify-timeout\"" in in_flight_wait
assert "deadline_ms == 0u" not in in_flight_wait, (
    "a wrapped zero deadline remains a valid modular uptime deadline"
)

deadline_arm = ble_work.index(
    "gateway_ble_tx_deadline_ms =", in_flight_exit
)
deadline_schedule = ble_work.index(
    "gateway_ble_schedule_in_flight_deadline(", deadline_arm
)
notify_submit = ble_work.index("ret = bt_gatt_notify_cb", deadline_schedule)
assert deadline_arm < deadline_schedule < notify_submit, (
    "the timeout worker must be armed before asynchronous notify submission "
    "can hand controller credit to an untrusted callback path"
)
notify_failure = ble_work.index("if (ret < 0)", notify_submit)
failure_clear = ble_work.index(
    "gateway_ble_tx_deadline_ms = 0u", notify_failure
)
failure_threshold = ble_work.index(
    "GATEWAY_BLE_NOTIFY_FAILURE_RESET_THRESHOLD", failure_clear
)
failure_reset = ble_work.index(
    "gateway_ble_reset_failed_link(", failure_threshold
)
assert notify_failure < failure_clear < failure_threshold < failure_reset

rx_work = function_body(BLE, "gateway_ble_rx_work_handler")
peek_receipt = rx_work.index("k_msgq_peek(&gateway_ble_rx_msgq")
receipt_classification = rx_work.index(
    "gateway_ble_pending_is_host_receipt", peek_receipt
)
result_reservation = rx_work.index(
    "gateway_command_result_reserve_ingress", receipt_classification
)
receipt_dequeue = rx_work.index(
    "k_msgq_get(&gateway_ble_rx_msgq, &pending, K_NO_WAIT)",
    receipt_classification,
)
assert peek_receipt < receipt_classification < receipt_dequeue < result_reservation
receipt_branch = rx_work[receipt_classification:result_reservation]
assert "gateway_handle_ble_frame(pending.frame, pending.len, 0u)" in receipt_branch
assert "gateway_command_result_reserve_ingress" not in receipt_branch

tx_complete = function_body(BLE, "gateway_ble_tx_complete")
assert "gateway_ble_stream_mark_host_notified" in tx_complete
assert "head_send_phase = GATEWAY_BLE_STREAM_HEAD_HOST_NOTIFIED" not in tx_complete
assert "mesh_gateway_host_receipt_ready" not in tx_complete
assert "gateway_ble_host_receipt_timeout_work" in tx_complete
assert "GATEWAY_BLE_HOST_RECEIPT_TIMEOUT_MS" in BLE
cancel_active = function_body(BLE, "gateway_ble_stream_cancel_active")
assert "gateway_ble_stream_rewind_host_notification" in cancel_active
assert "gateway_ble_host_receipt_timeout_work" in cancel_active

receipt = function_body(RESULT_RUNTIME, "gateway_ble_accept_host_receipt")
assert "gateway_host_receipt_packet_validate" in receipt
assert "semantic_digest_sha256(record, record_len, record_digest)" in receipt
assert "gateway_ble_stream_accept_host_receipt" in receipt
assert receipt.index("gateway_ble_stream_accept_host_receipt") < receipt.index(
    "mesh_gateway_host_receipt_ready()"
)
finish = function_body(RESULT_RUNTIME, "gateway_ble_finish_host_delivery")
assert "GATEWAY_BLE_STREAM_HEAD_HOST_ACCEPTED" in finish
assert "GATEWAY_BLE_STREAM_HEAD_HOST_NOTIFIED" not in finish
control = read_composed_source(ROOT / "app/src/app_anchor.c")
handle = function_body(control, "gateway_handle_ble_frame")
assert handle.index("gateway_ble_accept_host_receipt_frame") < handle.index(
    "app_mesh_command_orchestrator_gateway_ingress"
)

delivery = (ROOT / "app/src/app_mesh_report_delivery.inc").read_text(
    encoding="utf-8"
)

# A PREPARE/START result can outlive the volatile survey operation that owned
# it: preflight may admit the exact result and expose it to the host, then
# survey cleanup can retire the owner before the exact host receipt arrives.
# At that point -ENOENT is terminal stale-survey evidence, not a reason to pin
# the singleton gateway delivery forever.  Keep that recovery deliberately
# narrower than the generic semantic-error path.
complete_host_delivery = function_body(
    delivery, "mesh_complete_gateway_host_delivery_locked"
)
semantic_dispatch = complete_host_delivery.index(
    "ret = mesh_gateway_accept_semantic_delivery(pending)"
)
semantic_failure_gate = complete_host_delivery.index(
    "if (ret < 0)", semantic_dispatch
)
classifier_match = re.search(
    r"ret\s*=\s*(mesh_gateway_[A-Za-z0-9_]*post_receipt[A-Za-z0-9_]*)"
    r"\s*\(\s*pending\s*,\s*semantic_ret\s*,\s*ret\s*\)\s*;",
    complete_host_delivery[semantic_dispatch:semantic_failure_gate],
)
assert classifier_match is not None, (
    "post-receipt semantic completion needs an explicit narrow classifier "
    "before the generic retry gate"
)
classifier_name = classifier_match.group(1)
classifier = function_body(delivery, classifier_name)

for required in (
    "preflight_acceptance != APP_GATEWAY_SEMANTIC_ACCEPT_NEW",
    "semantic_ret != -ENOENT",
    "pending->packet.msg_type != MSG_COMMAND_RESULT",
    "gateway_command_extract_id(pending->payload",
    "pending->payload_len",
    "&command_id) != PROTO_OK",
    "CMD_SURVEY_PREPARE_PAIR",
    "CMD_SURVEY_START_PAIR",
    "APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW",
):
    assert required in classifier, (
        "stale post-receipt recovery must require -ENOENT and an exact "
        f"survey PREPARE/START command-result identity: missing {required}"
    )
assert classifier.count("APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW") == 1
assert classifier.count("return semantic_ret;") == 1, (
    "non-ENOENT errors and non-survey command results must retain their "
    "original semantic failure"
)

# The shared extractor is the parser boundary used above.  Pin its unique,
# exact-width decode so a duplicate or malformed command-ID TLV cannot enter
# the stale-survey recovery lane merely because one copy names PREPARE/START.
extract_command_id = function_body(GATEWAY_COMMAND, "gateway_command_extract_id")
for required in (
    "tlv_find_unique(payload, payload_len, TLV_COMMAND_ID",
    "value_len != sizeof(uint16_t)",
    "proto_get_u16_le(value)",
):
    assert required in extract_command_id, (
        "post-receipt stale-survey recovery depends on a unique, exact-width "
        f"command-ID parser: missing {required}"
    )

classifier_call = complete_host_delivery.index(
    classifier_match.group(0), semantic_dispatch, semantic_failure_gate
)
semantic_commit = complete_host_delivery.index(
    "semantic_ret = ret", classifier_call
)
semantic_accepted = complete_host_delivery.index(
    "atomic_set(&mesh_gateway_host_delivery_semantic_accepted_state, 1)",
    semantic_commit,
)
assert (
    semantic_dispatch
    < classifier_call
    < semantic_failure_gate
    < semantic_commit
    < semantic_accepted
), "terminal stale-survey recovery must escape the semantic retry loop"

semantic_finalize = complete_host_delivery.index(
    "gateway_finalize_semantic_delivery", semantic_accepted
)
relay_commit = complete_host_delivery.index(
    "mesh_relay_commit_gateway_delivery", semantic_finalize
)
ack_handoff = complete_host_delivery.index(
    "mesh_handle_result_actions", relay_commit
)
ble_retire = complete_host_delivery.index(
    "gateway_ble_finish_host_delivery", ack_handoff
)
assert (
    "semantic_ret != APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW"
    in complete_host_delivery[semantic_accepted:semantic_finalize]
)
assert semantic_finalize < relay_commit < ack_handoff < ble_retire, (
    "recovered stale survey results must skip semantic mutation but still "
    "commit relay delivery, hand off the gateway ACK, and release BLE custody"
)

drain = function_body(delivery, "mesh_drain_rx_queue_locked")
redrive_gate = drain.index(
    "semantic_ret == APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_REDRIVE"
)
redrive_condition = drain.rfind("if (semantic_ret < 0", 0, redrive_gate)
host_ready = drain.index("host_custody_ready = true", redrive_condition)
stream_reserve = drain.index("gateway_ble_reserve_stream_packet", host_ready)
assert redrive_condition < host_ready < stream_reserve

# Committing a stream reservation exposes the retained record to the BLE
# worker and, transitively, to a host receipt callback.  Arm every piece of
# delivery custody before that boundary so an immediate exact receipt cannot
# be mistaken for a receipt without a pending mesh owner.
stream_custody_branch = drain.rfind(
    "} else if (stream_reservation_acquired)", 0, len(drain)
)
stream_commit = drain.index(
    "gateway_ble_commit_stream_reservation_projection(",
    stream_custody_branch,
)
semantic_arm = drain.index(
    "mesh_gateway_host_delivery_semantic_acceptance = semantic_ret",
    stream_custody_branch,
    stream_commit,
)
pending_arm = drain.index(
    "atomic_set(&mesh_gateway_host_delivery_pending_state, 1)",
    semantic_arm,
    stream_commit,
)
delivery_phase_states = (
    "mesh_gateway_host_receipt_received_state",
    "mesh_gateway_host_delivery_semantic_accepted_state",
    "mesh_gateway_host_delivery_semantic_finalized_state",
    "mesh_gateway_host_delivery_relay_committed_state",
    "mesh_gateway_host_delivery_ack_handoff_state",
)
for state in delivery_phase_states:
    phase_clear = drain.index(
        f"atomic_clear(&{state})",
        semantic_arm,
        pending_arm,
    )
    assert semantic_arm < phase_clear < pending_arm < stream_commit

commit_failure = drain.index("if (ret != 1)", stream_commit)
next_delivery_case = drain.index(
    "} else if (semantic_ret ==",
    commit_failure,
)
commit_rollback = drain[commit_failure:next_delivery_case]
for rollback in (
    "gateway_ble_cancel_stream_reservation()",
    "atomic_clear(&mesh_gateway_host_delivery_pending_state)",
    *(f"atomic_clear(&{state})" for state in delivery_phase_states),
    "mesh_gateway_host_delivery_semantic_acceptance = -EINVAL",
):
    assert rollback in commit_rollback, (
        "a failed BLE stream commit must completely release pre-armed host "
        f"custody: missing {rollback}"
    )

no_host = drain[drain.index("if (!stream_reservation_acquired)", redrive_gate) :]
assert no_host.index("gateway_finalize_semantic_delivery") < no_host.index(
    "mesh_relay_commit_gateway_delivery"
)
common_actions = delivery.index(
    "mesh_handle_result_actions(result, pending->radio_channel"
)
pending_barrier = delivery.rfind(
    "if (atomic_get(&mesh_gateway_host_delivery_pending_state) == 0)",
    0,
    common_actions,
)
assert pending_barrier >= 0
assert common_actions - pending_barrier < 300

tx_reset = function_body(BLE, "gateway_ble_tx_reset_locked")
for reset_statement in (
    "gateway_ble_tx_source = GATEWAY_BLE_TX_NONE",
    "gateway_ble_tx_frame_offset = 0u",
    "gateway_ble_tx_chunk_len = 0u",
    "gateway_ble_tx_in_flight = false",
    "gateway_ble_tx_deadline_ms = 0u",
    "gateway_ble_notify_failure_count = 0u",
    "gateway_ble_direct_queue_cancel",
    "gateway_ble_tx_generation++",
):
    assert reset_statement in tx_reset

reset_link = function_body(BLE, "gateway_ble_reset_link")
reset_lock = reset_link.index("k_spin_lock(&gateway_ble_tx_lock)")
reset_validation = reset_link.index(
    "expected_generation != gateway_ble_tx_generation", reset_lock
)
reset_exact_deadline = reset_link.index(
    "gateway_ble_tx_deadline_ms != expected_deadline_ms", reset_validation
)
reset_expired = reset_link.index(
    "uptime_deadline_reached(now_ms, expected_deadline_ms)",
    reset_exact_deadline,
)
reset_state = reset_link.index("gateway_ble_tx_reset_locked()", reset_expired)
reset_unlock = reset_link.index(
    "k_spin_unlock(&gateway_ble_tx_lock", reset_state
)
reset_stream = reset_link.index(
    "gateway_ble_stream_cancel_active()", reset_unlock
)
reset_disconnect = reset_link.index("bt_conn_disconnect(", reset_stream)
assert (
    reset_lock
    < reset_validation
    < reset_exact_deadline
    < reset_expired
    < reset_state
    < reset_unlock
    < reset_stream
    < reset_disconnect
)
assert "expected_deadline_ms == 0u" not in reset_link, (
    "a zero deadline produced by uptime wrap must not suppress stale-link reset"
)
for failed_submit_guard in (
    "gateway_ble_tx_in_flight",
    "gateway_ble_tx_deadline_ms != 0u",
    "gateway_ble_notify_failure_count <",
):
    assert failed_submit_guard in reset_link

ble_retry = function_body(BLE, "gateway_ble_stream_retry_delay_ms")
assert "GATEWAY_BLE_TX_RETRY_MS << shift" in ble_retry
assert "GATEWAY_BLE_TX_RETRY_MAX_MS" in ble_retry

for helper in (
    "gateway_observability_prepare_reserved_state",
    "gateway_observability_note_enqueue_state",
    "gateway_observability_pending_terminal_state",
    "gateway_observability_snapshot_state",
    "gateway_observability_mark_sent_state",
):
    body = function_body(BLE, helper)
    assert "k_spin_lock(&gateway_command_observability_lock)" in body
    assert "k_spin_unlock(&gateway_command_observability_lock" in body

reserve_identity = function_body(BLE, "gateway_observability_reserve_identity")
assert "gateway_next_broadcast_command_seq()" in reserve_identity
assert "event->event_seq != 0u" in reserve_identity
assert "k_spin_lock(" not in reserve_identity

prepare = function_body(BLE, "gateway_observability_prepare_state")
assert prepare.index("gateway_observability_reserve_identity") < prepare.index(
    "gateway_observability_prepare_reserved_state"
)

observe = function_body(BLE, "gateway_observe_command_event")
assert "gateway_observability_prepare_state" in observe
assert "gateway_command_observability_prepare(" not in observe

flush = function_body(BLE, "gateway_observability_flush")
assert "gateway_observability_pending_terminal_state" in flush
assert "gateway_observability_snapshot_state" in flush

tx_complete = function_body(BLE, "gateway_ble_tx_complete")
assert "gateway_observability_mark_sent_state" in tx_complete

stream_packet = function_body(RESULT_RUNTIME, "gateway_ble_stream_packet")
assert "GATEWAY_COMMAND_EVENT_FLAG_TERMINAL" in stream_packet
assert "gateway_ble_stream_enqueue_retained_packet" in stream_packet
assert stream_packet.index("GATEWAY_COMMAND_EVENT_FLAG_TERMINAL") < (
    stream_packet.index("gateway_ble_stream_enqueue_retained_packet")
), "terminal command events must become non-evictable before queue admission"

observe_available = function_body(
    BLE, "gateway_observe_command_event_if_available"
)
first_capacity_check = observe_available.index(
    "gateway_ble_stream_state.count >= GATEWAY_BLE_STREAM_QUEUE_DEPTH"
)
reserve = observe_available.index(
    "gateway_observability_reserve_identity", first_capacity_check
)
second_capacity_check = observe_available.index(
    "gateway_ble_stream_state.count >= GATEWAY_BLE_STREAM_QUEUE_DEPTH",
    reserve,
)
prepare_reserved = observe_available.index(
    "gateway_observability_prepare_reserved_state", second_capacity_check
)
assert first_capacity_check < reserve < second_capacity_check < prepare_reserved
assert observe_available.count("if (!gateway_ble_stream_ready())") == 2
assert "gateway_next_broadcast_command_seq" not in observe_available, (
    "durable identity allocation must stay outside BLE and stream spinlocks"
)
assert "packet.session_id = event->event_seq" in observe_available
assert "packet.seq = (uint16_t)event->event_seq" in observe_available
assert "gateway_observability_note_enqueue_state" in observe_available

ble_init = function_body(BLE, "gateway_ble_init")
assert ble_init.count("return ret;") >= 2
assert "recovery remains active" in ble_init
for marker in (
    "DBG_GATEWAY_BLE stage=entry",
    "DBG_GATEWAY_BLE stage=bt_enable",
    "DBG_GATEWAY_BLE stage=adv_start",
    "DBG_GATEWAY_BLE event=recovery",
    "DBG_GATEWAY_BLE event=connected",
    "DBG_GATEWAY_BLE event=disconnected",
):
    assert marker in BLE
for marker in (
    "DBG_NODE_COMM_BOOT stage=init",
    "DBG_GATEWAY_BOOT stage=ble_begin",
    "DBG_GATEWAY_BOOT stage=ble_done",
    "DBG_GATEWAY_BOOT stage=ch9_begin",
    "DBG_GATEWAY_BOOT stage=ch9_done",
):
    assert marker in MAIN

print("gateway observability source invariants passed")
