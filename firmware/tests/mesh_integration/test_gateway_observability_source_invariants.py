#!/usr/bin/env python3
from pathlib import Path
import re

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = read_composed_source(ROOT / "app/src/app_anchor.c")
REPORT_RX = (ROOT / "app/src/app_mesh_report_rx.inc").read_text(encoding="utf-8")
BLE = read_composed_source(ROOT / "app/src/app_gateway_ble.c")
RESULT_RUNTIME = (ROOT / "app/src/app_gateway_result_runtime.inc").read_text(
    encoding="utf-8"
)
MAIN = (ROOT / "app/src/main.c").read_text(encoding="utf-8")
SURVEY = (ROOT / "app/src/app_gateway_survey_observability.c").read_text(
    encoding="utf-8"
)
SURVEY_ROUND = (ROOT / "app/src/app_gateway_survey_round.c").read_text(
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


def closing_brace_index(source: str, opening_brace: int) -> int:
    depth = 0
    for index in range(opening_brace, len(source)):
        depth += source[index] == "{"
        depth -= source[index] == "}"
        if depth == 0:
            return index
    raise AssertionError("unterminated braced block")

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

# A retryable local BUSY belongs to the serialized command worker, not to the
# host-visible terminal stream.  Keep the exact reservation alive while its
# dispatch token is published; the worker emits BACKOFF and owns the sole
# terminal conversion after success or bounded exhaustion.
emit_result = function_body(RESULT_RUNTIME, "gateway_emit_host_command_result")
busy_result_gate = emit_result.index("status == COMMAND_BUSY")
busy_result_token = emit_result.index(
    "gateway_command_result_dispatch_token != 0u", busy_result_gate
)
reserved_conversion = emit_result.index(
    "gateway_emit_host_command_result_reserved(", busy_result_token
)
assert busy_result_gate < busy_result_token < reserved_conversion

host_terminal = function_body(ANCHOR, "gateway_observe_host_terminal")
assert "status == COMMAND_BUSY" in host_terminal
assert "gateway_command_result_get_dispatch_token() != 0u" in host_terminal

host_worker = function_body(ANCHOR, "gateway_host_command_work_handler")
dispatch_publish = host_worker.index("gateway_command_result_set_dispatch_token(")
route_call = host_worker.index("gateway_route_host_packet(", dispatch_publish)
dispatch_clear = host_worker.index(
    "gateway_command_result_set_dispatch_token(0u)", route_call
)
retry_gate = host_worker.index(
    "app_gateway_command_ingress_contention_retryable(ret)", dispatch_clear
)
backoff = host_worker.index(
    "GATEWAY_COMMAND_EVENT_STAGE_BACKOFF", retry_gate
)
retry_exhaustion = host_worker.index(
    "GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED", retry_gate
)
assert (
    dispatch_publish < route_call < dispatch_clear < retry_gate <
    retry_exhaustion < backoff
)

survey_work = function_body(ANCHOR, "gateway_survey_work_handler")
boundary_gate = survey_work.index(
    "if (!gateway_survey_flush_boundary_event())"
)
boundary_flush = survey_work.index(
    "gateway_survey_flush_boundary_event", boundary_gate
)
active_gate = survey_work.index("if (!gateway_survey_active)")
deadline_gate = survey_work.index("if (uptime_deadline_reached")
cleanup_service = survey_work.index(
    "gateway_survey_service_cleanup()", deadline_gate
)
collection_owner = survey_work.index(
    "gateway_survey_wait_for_discovery_collection()", cleanup_service
)
round_drive = survey_work.index(
    "gateway_survey_round_drive(", boundary_flush
)
assert (
    active_gate < deadline_gate < cleanup_service < collection_owner <
    boundary_flush < round_drive
), (
    "the immutable operation deadline, cleanup owner, collection owner, and "
    "backpressured boundary must run before the round can expose a successor"
)
for retired_owner in (
    "gateway_survey_auto",
    "survey_gateway_auto_next_action",
    "survey_gateway_auto_",
):
    assert retired_owner not in survey_work, (
        "the gateway worker must read only the round and exact transaction "
        f"owners; found retired mirror {retired_owner}"
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
command_event_finish = receipt.index(
    "return gateway_command_event_finish_host_receipt(&head_packet)"
)
local_result_branch = receipt.index(
    "local_command_result = gateway_local_command_result_packet("
)
malformed_local_result = receipt.index(
    "if (self_addressed_command_result && !local_command_result)",
    local_result_branch,
)
local_result_finish = receipt.index(
    "gateway_ble_finish_host_delivery(&head_packet)", local_result_branch
)
mesh_completion = receipt.index("mesh_gateway_host_receipt_ready()")
assert (
    local_result_branch
    < malformed_local_result
    < command_event_finish
    < local_result_finish
    < mesh_completion
), (
    "self-addressed command results must fail closed before mesh completion, "
    "while reliable publisher events and local results each retire their "
    "own BLE custody before the distinct mesh-result branch"
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

# A delayed pair result can arrive after its automatic survey batch has
# advanced, including after the runtime has loaded a different planned pair.
# That live operation's immutable plan plus a strictly newer batch is terminal
# proof for recovered-raw custody.  The same batch is also retired once its
# exact lane is in CLEANUP: an orphan transit result must not prevent the ABORT
# which advances that batch.  A generic -ESTALE also covers no owner, an
# expired manual owner, a current live lane, and an obsolete operation; none of
# those classifications may fabricate host acceptance or a gateway ACK.
pair_preflight = function_body(ANCHOR, "gateway_survey_preflight_pair_result")
retired_round = function_body(
    ANCHOR, "gateway_survey_pair_result_is_retired_automatic_round"
)
exact_round_preflight = function_body(
    SURVEY_ROUND, "app_gateway_survey_round_preflight_sample"
)
round_pair_equal = function_body(
    SURVEY_ROUND, "app_gateway_survey_round_pair_equal"
)
payload_validation = pair_preflight.index("survey_pair_result_payload_validate")
malformed_return = pair_preflight.index("return -EBADMSG", payload_validation)
identity_validation = pair_preflight.index(
    "sample.pair.operation_generation == 0u", malformed_return
)
identity_return = pair_preflight.index("return -EPROTO", identity_validation)
transport_validation = pair_preflight.index(
    "survey_pair_result_transport_sequence", identity_return
)
transport_return = pair_preflight.index("return -EPROTO", transport_validation)
round_preflight = pair_preflight.index(
    "ret = gateway_survey_round_preflight_sample", transport_return
)
manual_preflight = pair_preflight.index(
    "ret = gateway_manual_survey_pair_preflight_sample", round_preflight
)
stale_recovery_gate = pair_preflight.index("if (ret == -ESTALE &&", manual_preflight)
retired_round_gate = pair_preflight.index(
    "gateway_survey_pair_result_is_retired_automatic_round", stale_recovery_gate
)
recovered_raw = pair_preflight.index(
    "return APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW", retired_round_gate
)
ordinary_return = pair_preflight.index("return ret", recovered_raw)
assert (
    payload_validation
    < malformed_return
    < identity_validation
    < identity_return
    < transport_validation
    < transport_return
    < round_preflight
    < manual_preflight
    < stale_recovery_gate
    < retired_round_gate
    < recovered_raw
    < ordinary_return
), (
    "only a structurally valid result proven to belong to a retired batch of "
    "the live automatic operation may enter recovered-raw host custody"
)
assert pair_preflight.count("APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW") == 1
for required in (
    "gateway_survey_round_active()",
    "gateway_survey_round.runtime.active",
    "gateway_survey_round.runtime.batch_sequence",
    "sample->round_id",
    "context->pair_count > SURVEY_GATEWAY_MAX_PAIRS",
    "survey_gateway_pair_at(context",
    "planned_pair.operation_generation ==",
    "sample->pair.operation_generation",
    "planned_pair.survey_id == sample->pair.survey_id",
    "planned_pair.initiator_id == sample->pair.initiator_id",
    "planned_pair.responder_id == sample->pair.responder_id",
    "planned_pair.sample_count == sample->pair.sample_count",
    "app_gateway_survey_round_preflight_sample(",
    "SURVEY_PAIR_ROUND_LANE_CLEANUP",
):
    assert required in retired_round, (
        "retired pair-result recovery must bind the older round to one exact "
        f"pair in the live automatic plan: missing {required}"
    )
assert re.search(
    r"sample->round_id\s*>\s*"
    r"gateway_survey_round\.runtime\.batch_sequence",
    retired_round,
), "a future round must remain ordinary stale rejection"
current_round_branch = retired_round.index(
    "sample->round_id == gateway_survey_round.runtime.batch_sequence"
)
older_plan_lookup = retired_round.index(
    "survey_gateway_pair_at(context", current_round_branch
)
assert current_round_branch < older_plan_lookup, (
    "after future rounds fail and the current round gets exact CLEANUP-only "
    "classification, older rounds must retain immutable-plan recovery"
)
cleanup_state = retired_round.index("SURVEY_PAIR_ROUND_LANE_CLEANUP")
exact_cleanup_call = retired_round.index(
    "app_gateway_survey_round_preflight_sample("
)
assert exact_cleanup_call < cleanup_state
assert retired_round.count("SURVEY_PAIR_ROUND_LANE_CLEANUP") == 1
for forbidden_state in (
    "SURVEY_PAIR_ROUND_LANE_READY",
    "SURVEY_PAIR_ROUND_LANE_ARMING",
    "SURVEY_PAIR_ROUND_LANE_ARMED",
    "SURVEY_PAIR_ROUND_LANE_OBSERVING",
    "SURVEY_PAIR_ROUND_LANE_SUCCEEDED",
    "SURVEY_PAIR_ROUND_LANE_FAILED",
    "SURVEY_PAIR_ROUND_LANE_RERUN_QUEUED",
):
    assert forbidden_state not in retired_round, (
        "current-batch recovered raw must name only the exact CLEANUP lane: "
        f"found {forbidden_state}"
    )
for field in (
    "operation_generation",
    "survey_id",
    "initiator_id",
    "responder_id",
    "sample_count",
):
    assert f"left->{field} == right->{field}" in round_pair_equal
assert "SURVEY_PAIR_ROUND_LANE_CLEANUP" in exact_round_preflight
assert "candidate->state != admissible_lane_state" in exact_round_preflight
assert "app_gateway_survey_round_pair_equal(" in exact_round_preflight
assert "matched != NULL" in exact_round_preflight
assert "matched == NULL" in exact_round_preflight
assert "gateway_manual_survey_pair_state" not in retired_round
assert "APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW" not in retired_round
for current_preflight in (
    "gateway_survey_round_preflight_sample",
    "gateway_manual_survey_pair_preflight_sample",
):
    body = function_body(ANCHOR, current_preflight)
    assert "APP_GATEWAY_SEMANTIC_ACCEPT_NEW" in body
    assert "APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE" in body

# A host-gated gateway delivery can complete long after the RF turn and after
# route maintenance has changed the downlink cache.  The immutable queued RX
# item must retain the physical ingress peer, and both immediate and delayed
# commit paths must pass that exact peer back to the relay ACK builder.
queue_rx = function_body(REPORT_RX, "mesh_queue_from_frame_at_internal")
capture_previous_hop = queue_rx.index(
    "pending.previous_hop_id = context.previous_hop_id"
)
enqueue_rx = queue_rx.index("k_msgq_put(&mesh_rx_msgq", capture_previous_hop)
assert capture_previous_hop < enqueue_rx

# A survey result/report can outlive the volatile survey operation that owned
# it: preflight may admit the exact result and expose it to the host, then
# survey cleanup can retire the owner before the exact host receipt arrives.
# At that point the type-specific -ENOENT/-ESTALE is terminal stale-survey
# evidence, not a reason to pin the singleton gateway delivery forever.  Keep
# that recovery deliberately narrower than the generic semantic-error path.
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
    "semantic_ret == -ESTALE",
    "pending->packet.msg_type == MSG_SURVEY_DISCOVERY_REPORT",
    "pending->packet.msg_type == MSG_SURVEY_PAIR_RESULT",
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
        "stale post-receipt recovery must require exact survey report/result "
        f"identity and terminal stale evidence: missing {required}"
    )
assert classifier.count("APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW") == 2
assert classifier.count("return semantic_ret;") >= 1, (
    "non-stale errors and unrelated packet types must retain their original "
    "semantic failure"
)
survey_recovery = classifier.index("semantic_ret == -ESTALE")
survey_recovery_end = classifier.index(
    "APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW", survey_recovery
)
survey_recovery_branch = classifier[survey_recovery:survey_recovery_end]
for packet_type in (
    "MSG_SURVEY_DISCOVERY_REPORT",
    "MSG_SURVEY_PAIR_RESULT",
):
    assert packet_type in survey_recovery_branch
assert "MSG_COMMAND_RESULT" not in survey_recovery_branch

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
delayed_commit = complete_host_delivery[relay_commit:]
assert re.search(
    r"mesh_relay_commit_gateway_delivery\s*\([^;]*?"
    r"pending->previous_hop_id\s*,",
    delayed_commit,
    re.DOTALL,
), "delayed host receipt must preserve the captured physical ingress peer"
ack_handoff = complete_host_delivery.index(
    "mesh_handle_result_actions", relay_commit
)
ble_retire = complete_host_delivery.index(
    "gateway_ble_finish_host_delivery", ack_handoff
)
collection_recovery_relay_bypass = complete_host_delivery.index(
    "semantic_ret != APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECOVERY",
    relay_commit,
)
assert relay_commit < collection_recovery_relay_bypass < ack_handoff, (
    "only the reboot-recovery EACK path may bypass normal relay/ACK handoff; "
    "recovered stale survey records still need their captured physical custody "
    "committed"
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
assert re.search(
    r"mesh_relay_commit_gateway_delivery\s*\([^;]*?"
    r"pending->previous_hop_id\s*,",
    no_host,
    re.DOTALL,
), "immediate and duplicate commits must use the current queued ingress peer"
common_actions = delivery.index(
    "mesh_handle_result_actions(result, pending->radio_channel"
)
pending_barrier = delivery.rfind(
    "if (atomic_get(&mesh_gateway_host_delivery_pending_state) == 0)",
    0,
    common_actions,
)
assert pending_barrier >= 0
abort_retirement = delivery.index(
    "mesh_retire_stale_transit_survey_result_for_abort(",
    pending_barrier,
    common_actions,
)
assert pending_barrier < abort_retirement < common_actions
assert "app_watchdog_stop_feeding();" in delivery[
    abort_retirement:common_actions
]

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
assert "app_gateway_control_sequence_next_receiptable(&event_seq)" in reserve_identity
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
unretained_event = tx_complete.index("unretained_command_event =")
host_custody_gate = tx_complete.index("host_custody_supported =")
host_notified = tx_complete.index(
    "gateway_ble_stream_mark_host_notified", host_custody_gate
)
assert host_custody_gate < host_notified
assert "gateway_observability_mark_sent_state(" not in tx_complete[unretained_event:], (
    "command observability may retire only after an exact GUI receipt"
)
assert "unretained-command-observability" in tx_complete[unretained_event:]

finish_command_event = function_body(
    BLE, "gateway_command_event_finish_host_receipt"
)
assert "gateway_observability_mark_sent_state(event.event_seq)" in finish_command_event
assert "packet->flags != FLAG_GATEWAY_ACK_REQUIRED" in finish_command_event
assert "app_gateway_assignment_publisher_event_is_reliable(" in finish_command_event
publisher_advance = finish_command_event.index(
    "app_gateway_assignment_publisher_note_host_receipt("
)
stream_retire = finish_command_event.index(
    "gateway_ble_stream_mark_sent", publisher_advance
)
assert publisher_advance < stream_retire, (
    "publisher semantic advancement must succeed before its exact retained "
    "BLE head can retire"
)
retirement_complete = finish_command_event.index(
    "gateway_observability_mark_sent_state(event.event_seq)", stream_retire
)
publisher_pump = finish_command_event.index(
    "app_gateway_assignment_publisher_pump();", retirement_complete
)
publisher_owned_fallback = finish_command_event.index(
    "if (publisher_owned && publisher_ret > 0)", retirement_complete
)
persistence_fallback_end = closing_brace_index(
    finish_command_event,
    finish_command_event.index("{", publisher_owned_fallback),
)
persistence_retry = finish_command_event.index(
    "gateway_schedule_persistence_retry(", publisher_owned_fallback
)
assert retirement_complete < publisher_owned_fallback < persistence_retry, (
    "persistence fallback scheduling must remain conditional on an exact "
    "publisher-owned receipt"
)
assert persistence_retry < persistence_fallback_end
assert retirement_complete < publisher_pump
assert not (
    publisher_owned_fallback < publisher_pump < persistence_fallback_end
), (
    "every accepted and retired host receipt must pump a prepared assignment "
    "publisher, including mapping receipts that are not publisher-owned"
)
assert finish_command_event.count(
    "app_gateway_assignment_publisher_pump();"
) == 1

stream_packet = function_body(RESULT_RUNTIME, "gateway_ble_stream_packet")
assert "MSG_GATEWAY_COMMAND_EVENT" in stream_packet
assert "gateway_ble_stream_enqueue_retained_packet" in stream_packet
assert "if (packet->flags == 0u)" in stream_packet
reliable_marker = stream_packet.index(
    "packet->flags == FLAG_GATEWAY_ACK_REQUIRED"
)
reliable_shape = stream_packet.index(
    "app_gateway_assignment_publisher_event_is_reliable(", reliable_marker
)
retained_enqueue = stream_packet.index(
    "gateway_ble_stream_enqueue_retained_packet", reliable_shape
)
best_effort_enqueue = stream_packet.index(
    "gateway_ble_stream_enqueue_packet", retained_enqueue
)
assert reliable_marker < reliable_shape < retained_enqueue < best_effort_enqueue, (
    "ACK_REQUIRED is reserved for a validated durable publisher event; "
    "flags-zero command telemetry remains an ordinary evictable stream item"
)

publisher_emit = function_body(BLE, "gateway_publish_assignment_event_if_available")
assert "app_gateway_assignment_publisher_event_is_reliable(event)" in publisher_emit
assert "packet.flags = FLAG_GATEWAY_ACK_REQUIRED" in publisher_emit
assert "gateway_ble_stream_packet(&packet" in publisher_emit

observe_available = function_body(
    BLE, "gateway_observe_command_event_if_available"
)
assert "gateway_observability_prepare_state(event, terminal)" in observe_available
assert "gateway_observability_enqueue_prepared(event)" in observe_available
assert "return 0;" in observe_available
assert "gateway_ble_stream_ready()" not in observe_available, (
    "BLE disconnect must not stall or abort accepted RF command ownership"
)

enqueue_prepared = function_body(BLE, "gateway_observability_enqueue_prepared")
assert "packet.flags = FLAG_GATEWAY_ACK_REQUIRED" in enqueue_prepared
assert "gateway_ble_stream_enqueue_retained_packet(" in enqueue_prepared
assert "gateway_observability_note_enqueue_state" in enqueue_prepared

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
