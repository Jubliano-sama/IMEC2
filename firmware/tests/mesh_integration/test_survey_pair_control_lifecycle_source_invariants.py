#!/usr/bin/env python3
from pathlib import Path
import re

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = read_composed_source(ROOT / "app/src/app_anchor.c")
GATEWAY_BLE = read_composed_source(ROOT / "app/src/app_gateway_ble.c")


def function_body(source: str, name: str) -> str:
    match = None
    brace = None
    for candidate in re.finditer(rf"\b{name}\s*\(", source):
        paren = source.index("(", candidate.start())
        depth = 0
        for index in range(paren, len(source)):
            depth += source[index] == "("
            depth -= source[index] == ")"
            if depth == 0:
                next_index = index + 1
                while source[next_index].isspace():
                    next_index += 1
                if source[next_index] == "{":
                    match = candidate
                    brace = next_index
                break
        if match is not None:
            break
    assert match is not None and brace is not None, f"missing function {name}"
    line_start = source.rfind("\n", 0, match.start()) + 1
    depth = 0
    for index in range(brace, len(source)):
        depth += source[index] == "{"
        depth -= source[index] == "}"
        if depth == 0:
            return source[line_start : index + 1]
    raise AssertionError(f"unterminated function {name}")


def block_end(source: str, opening_brace: int) -> int:
    depth = 0
    for index in range(opening_brace, len(source)):
        depth += source[index] == "{"
        depth -= source[index] == "}"
        if depth == 0:
            return index
    raise AssertionError("unterminated source block")


preflight = function_body(ANCHOR, "gateway_survey_preflight_result")
decode = preflight.index("app_mesh_gateway_command_flow_decode_result(")
decode_reject = preflight.index("if (ret != PROTO_OK)", decode)
malformed_return = preflight.index(
    "return GATEWAY_SURVEY_RESULT_PREFLIGHT_UNRECOGNIZED;",
    decode_reject,
)
reconcile = preflight.index("survey_gateway_transaction_reconcile_result(")
assert decode < decode_reject < malformed_return < reconcile, (
    "malformed pair results must be rejected before transaction reconciliation"
)
assert "gateway_survey_result_preflight =" not in preflight[:reconcile], (
    "malformed input must not populate the accepted-result latch"
)
assert "&transaction_candidate" in preflight[reconcile : reconcile + 300], (
    "preflight must reconcile an isolated transaction candidate"
)
assert "&gateway_survey_transaction" not in preflight[reconcile : reconcile + 300], (
    "preflight must not mutate authoritative survey state before terminal claim"
)
preparation = preflight.index(
    "gateway_survey_result_preparation.key = key", reconcile
)
assert reconcile < preparation
for required in (
    "gateway_survey_result_preparation.result = transaction_result",
    "gateway_survey_result_preparation.received_at_ms = received_at_ms",
    "gateway_survey_result_preparation.result_digest",
    "gateway_survey_result_preparation.result_token = result_token",
    "gateway_survey_result_preparation.command_id = command_id",
    "gateway_survey_result_preparation.status = status",
    "gateway_survey_result_preparation.reason = reason",
    "gateway_survey_result_preparation.valid = true",
):
    assert required in preflight[preparation:], (
        "preflight preparation must preserve the complete reconciled result: "
        f"{required}"
    )
assert "gateway_survey_result_preflight =" not in preflight[reconcile:], (
    "preflight must not populate the accepted-result latch before commit"
)
assert "GATEWAY_SURVEY_RESULT_PREFLIGHT_ACCEPTED" in preflight[preparation:]
assert "GATEWAY_SURVEY_RESULT_PREFLIGHT_RECONCILED" in preflight[preparation:]

commit = function_body(
    ANCHOR, "gateway_survey_commit_preflight_result"
)
authoritative_commit = commit.index(
    "survey_gateway_transaction_reconcile_result("
)
authoritative_call = commit[
    authoritative_commit : authoritative_commit + 500
]
assert "&gateway_survey_transaction" in authoritative_call, (
    "only commit may reconcile authoritative survey state"
)
latch_assignment = commit.index(
    "gateway_survey_result_preflight =", authoritative_commit
)
latch_guard = commit[
    authoritative_commit:latch_assignment
]
assert "SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK" in latch_guard
assert "SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_FAILURE" in latch_guard
assert "!gateway_survey_result_preflight.valid" in latch_guard, (
    "the first committed accepted result must own an immutable latch"
)

accepted_return = commit.index("return;", latch_assignment)
duplicate_branch = commit.index(
    "SURVEY_GATEWAY_TRANSACTION_RESULT_DUPLICATE", accepted_return
)
duplicate_path = commit[duplicate_branch:]
assert "gateway_survey_result_preflight =" not in duplicate_path
assert "memset(&gateway_survey_result_preflight" not in duplicate_path, (
    "a duplicate during cancel/take deferral must not overwrite the accepted latch"
)
assert "gateway_survey_duplicate_count++" in duplicate_path
assert "response_ack_settle" not in duplicate_path, (
    "a duplicate result cannot create a timer-owned phase transition"
)

discard = function_body(ANCHOR, "gateway_survey_discard_preflight_result")
assert "memset(&gateway_survey_result_preparation" in discard
assert "gateway_survey_result_preflight" not in discard, (
    "discard retires only the uncommitted preparation; the first accepted "
    "result latch remains owned until exact terminal processing"
)

owns_pending = function_body(ANCHOR, "gateway_survey_owns_pending_control")
for exact_owner_field in (
    "gateway_survey_round_active()",
    "gateway_survey_pending_command_valid",
    "gateway_survey_transaction.active.state == NODE_TRANSACTION_EMPTY",
    "gateway_survey_transaction.pair_loaded",
    "app_gateway_survey_round_current_control(",
    "gateway_survey_pair_matches_transaction(&control.pair)",
    "active_key->requester_id == command->src_id",
    "active_key->responder_id == command->dst_id",
    "active_key->session_id == command->session_id",
    "active_key->transaction_id == command->seq",
    "active_key->operation_id == (uint16_t)command_id",
):
    assert exact_owner_field in owns_pending, (
        "survey result ownership must be reconstructed from the persistent "
        f"round plus exact transaction identity: missing {exact_owner_field}"
    )

accepted_result = function_body(
    ANCHOR, "gateway_survey_note_command_result"
)
assert "gateway_survey_owns_pending_control(command, command_id)" in accepted_result
close_call = accepted_result.index("gateway_survey_complete_accepted_delivery()")
eagain = accepted_result.index("if (ret == -EAGAIN)", close_call)
eagain_return = accepted_result.index("return;", eagain)
assert "memset(&gateway_survey_result_preflight" not in accepted_result[
    close_call:eagain_return
], "the accepted latch must survive an active backend cancel/take race"

finish = function_body(ANCHOR, "gateway_survey_finish_status")
assert "gateway_survey_finalize_pair_observation()" not in finish, (
    "terminal finish must not run normal pair finalization, which can schedule "
    "a rerun or wait for telemetry before ABORT cleanup owns every remote lane"
)
finish_status = finish.index("gateway_survey_finish_pending_status = status")
finish_reason = finish.index("gateway_survey_finish_pending_reason = reason")
finish_pending = finish.index("gateway_survey_finish_pending = true")
cleanup_custody = finish.index("survey_gateway_transaction_require_cleanup(")
round_termination = finish.index(
    "app_gateway_survey_round_begin_termination(", cleanup_custody
)
assert (
    finish_status < finish_reason < finish_pending < cleanup_custody <
    round_termination
), (
    "terminal finish must freeze the exact outcome before installing global "
    "cleanup custody"
)

cancel_take = finish.index("gateway_survey_cancel_take_active_delivery(")
cancel_error = finish.index("if (ret < 0)", cancel_take)
retry = finish.index("gateway_survey_work_schedule(", cancel_error)
defer_return = finish.index("return 0;", retry)
assert (
    finish_pending < cleanup_custody < cancel_take < cancel_error <
    retry < defer_return
), (
    "a cancel/take race must retain the frozen terminal outcome and global "
    "cleanup custody while the exact request backend remains active"
)
finish_inactive = finish.index("gateway_survey_active = false")
observation_cancel = finish.index(
    "survey_gateway_observation_origin_reset(", finish_inactive
)
finish_latch_clear = finish.index(
    "memset(&gateway_survey_result_preflight", finish_inactive
)
finish_preparation_clear = finish.index(
    "memset(&gateway_survey_result_preparation", finish_latch_clear
)
assert finish_inactive < finish_latch_clear < finish_preparation_clear, (
    "terminal survey cancellation must clear both result replay latches"
)
begin_cleanup = finish.index("gateway_survey_begin_cleanup()", finish_preparation_clear)
assert (
    defer_return < finish_inactive < observation_cancel <
    finish_latch_clear < finish_preparation_clear < begin_cleanup
), (
    "terminal finish must make the survey inactive and retire the round "
    "observation origin "
    "before cleanup can publish the frozen terminal outcome"
)

abandon_current = function_body(
    ANCHOR, "gateway_survey_abandon_current"
)
abandon_close = abandon_current.index(
    "survey_gateway_transaction_request_close("
)
abandon_cancel = abandon_current.index(
    "gateway_survey_cancel_take_active_delivery(", abandon_close
)
abandon_eagain = abandon_current.index("if (ret == -EAGAIN)", abandon_cancel)
abandon_retry = abandon_current.index(
    "gateway_survey_work_schedule(", abandon_eagain
)
abandon_defer = abandon_current.index("return;", abandon_retry)
abandon_complete = abandon_current.index(
    "gateway_survey_complete_close_request()", abandon_defer
)
assert (
    abandon_close < abandon_cancel < abandon_eagain < abandon_retry <
    abandon_defer < abandon_complete
), (
    "abandonment must retain close intent while cancel/take waits for the "
    "exact delivery terminal"
)
assert "memset(&gateway_survey_result_preflight" not in abandon_current
assert "memset(&gateway_survey_result_preparation" not in abandon_current, (
    "an in-flight close must retain its exact result latches until terminal "
    "custody is consumed"
)

complete_close = function_body(
    ANCHOR, "gateway_survey_complete_close_request"
)
close_present = complete_close.index(
    "intent == SURVEY_GATEWAY_TRANSACTION_CLOSE_NONE"
)
close_active = complete_close.index(
    "gateway_survey_transaction.active.state == NODE_TRANSACTION_EMPTY",
    close_present,
)
close_terminal = complete_close.index(
    "!gateway_survey_transaction.active.request_delivery_terminal",
    close_active,
)
close_cleanup = complete_close.index(
    "survey_gateway_transaction_require_cleanup(", close_terminal
)
close_clear = complete_close.index(
    "survey_gateway_transaction_clear_close_request(", close_cleanup
)
close_latch_clear = complete_close.index(
    "memset(&gateway_survey_result_preflight", close_clear
)
close_preparation_clear = complete_close.index(
    "memset(&gateway_survey_result_preparation", close_latch_clear
)
close_round_fail = complete_close.index(
    "gateway_survey_round_fail_current_control(", close_preparation_clear
)
close_begin_cleanup = complete_close.index(
    "gateway_survey_begin_cleanup()", close_round_fail
)
assert (
    close_present < close_active < close_terminal < close_cleanup <
    close_clear < close_latch_clear < close_preparation_clear <
    close_round_fail < close_begin_cleanup
), (
    "the exact terminal must be present before one close intent can clear "
    "its latches, fail the round control, and transfer cleanup custody"
)
assert complete_close.count("gateway_survey_round_fail_current_control(") == 1

delivery_service = function_body(
    ANCHOR, "gateway_survey_service_active_delivery"
)
peek = delivery_service.index("app_node_comm_peek_delivery_event_for(")
failure = delivery_service.index(
    "event.reason != NODE_COMM_TERMINAL_DELIVERED", peek
)
validation = delivery_service.index(
    "gateway_protocol_validation_check_interval(", failure
)
blocked = delivery_service.index(
    "GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED", validation
)
take = delivery_service.index("app_node_comm_take_delivery_event_for(", blocked)
terminal_commit = delivery_service.index(
    "survey_gateway_transaction_note_delivery_terminal(", take
)
assert peek < failure < validation < blocked < take < terminal_commit, (
    "a failed PREPARE/START delivery terminal must remain unconsumed until "
    "pre-deadline result validation finishes"
)
assert "gateway_survey_transaction.active_started_at_ms" in delivery_service[
    validation:blocked
]
close_snapshot = delivery_service.index(
    "close_intent = survey_gateway_transaction_close_requested("
)
redrive_close_gate = delivery_service.index(
    "close_intent == SURVEY_GATEWAY_TRANSACTION_CLOSE_NONE", close_snapshot
)
validation_close_gate = delivery_service.index(
    "close_intent == SURVEY_GATEWAY_TRANSACTION_CLOSE_NONE",
    redrive_close_gate + 1,
)
close_after_terminal = delivery_service.index(
    "survey_gateway_transaction_close_requested(", terminal_commit
)
complete_after_terminal = delivery_service.index(
    "gateway_survey_complete_close_request()", close_after_terminal
)
close_return = delivery_service.index("return;", complete_after_terminal)
assert (
    close_snapshot < redrive_close_gate < validation_close_gate < take <
    terminal_commit < close_after_terminal < complete_after_terminal <
    close_return
), (
    "a retained close intent must suppress redrive and result-validation "
    "paths, consume the exact terminal, and complete before normal failure "
    "processing"
)
assert "gateway_survey_round_fail_current_control(" not in delivery_service

timeout_close = function_body(
    ANCHOR, "gateway_survey_note_command_timeout"
)
timeout_request = timeout_close.index(
    "survey_gateway_transaction_request_close("
)
timeout_intent = timeout_close.index(
    "SURVEY_GATEWAY_TRANSACTION_CLOSE_TIMEOUT", timeout_request
)
timeout_cancel = timeout_close.index(
    "gateway_survey_cancel_take_active_delivery(", timeout_intent
)
timeout_eagain = timeout_close.index(
    "if (delivery_ret == -EAGAIN)", timeout_cancel
)
timeout_retry = timeout_close.index(
    "gateway_survey_work_schedule(", timeout_eagain
)
timeout_defer = timeout_close.index("return;", timeout_retry)
timeout_complete = timeout_close.index(
    "gateway_survey_complete_close_request()", timeout_defer
)
assert (
    timeout_request < timeout_intent < timeout_cancel < timeout_eagain <
    timeout_retry < timeout_defer < timeout_complete
), (
    "a command-result timeout must retain its typed close intent across "
    "cancel/take contention and may advance only after the exact terminal"
)
assert "gateway_survey_round_fail_current_control(" not in timeout_close, (
    "the timeout callback must not advance the round independently of the "
    "single terminal close owner"
)

worker = function_body(ANCHOR, "gateway_survey_work_handler")
manual_service = worker.index("gateway_manual_survey_control_service()")
service = worker.index("gateway_survey_service_active_delivery()")
resume = worker.index(
    "if (gateway_survey_active && gateway_survey_finish_pending", service
)
terminal_gate = worker.index("request_delivery_terminal", resume)
resume_status = worker.index(
    "gateway_survey_finish_pending_status", terminal_gate
)
resume_reason = worker.index(
    "gateway_survey_finish_pending_reason", resume_status
)
resume_clear = worker.index(
    "gateway_survey_finish_pending = false", resume_reason
)
resume_finish = worker.index(
    "gateway_survey_finish_status(finish_status, finish_reason)",
    resume_clear,
)
resume_out = worker.index("goto out;", resume_finish)
assert (
    service < resume < terminal_gate < resume_status < resume_reason <
    resume_clear < resume_finish < resume_out
), (
    "deferred finish must resume only after polling the exact request delivery"
)
cleanup_service = worker.index("gateway_survey_service_cleanup()", resume)
assert resume_out < cleanup_service, (
    "the exact deferred terminal outcome must replay before normal survey work"
)
inactive_gate = worker.index("if (!gateway_survey_active)", resume_out)
inactive_cleanup_pending = worker.index(
    "gateway_survey_cleanup_pending()", inactive_gate
)
inactive_cleanup_custody = worker.index(
    "gateway_survey_cleanup_custody_owned()", inactive_cleanup_pending
)
inactive_cleanup = worker.index(
    "gateway_survey_service_cleanup()", inactive_cleanup_custody
)
inactive_out = worker.index("goto out;", inactive_cleanup)
drive_label = worker.index("\nout:", inactive_out)
schedule_drive = worker.index("gateway_survey_schedule_drive()", drive_label)
rearm_due = worker.index("gateway_survey_work_rearm_due()", schedule_drive)
assert (
    manual_service < service < resume_out < inactive_gate <
    inactive_cleanup_pending < inactive_cleanup_custody < inactive_cleanup <
    inactive_out <
    drive_label < schedule_drive < rearm_due
), (
    "manual polling must run first, while an inactive automatic survey may "
    "enter cleanup only with exact retained cleanup custody before rearming "
    "the nearest due owner"
)
assert cleanup_service == inactive_cleanup

manual_service_body = function_body(
    ANCHOR, "gateway_manual_survey_control_service"
)
for foreign_finalizer in (
    "gateway_survey_service_cleanup(",
    "gateway_survey_finish_cleanup_if_complete(",
    "gateway_operation_owner_release(",
):
    assert foreign_finalizer not in manual_service_body, (
        "ordinary manual delivery polling must not enter the automatic "
        f"survey finalizer: found {foreign_finalizer}"
    )

cleanup_custody_owner = function_body(
    ANCHOR, "gateway_survey_cleanup_custody_owned"
)
assert "APP_GATEWAY_OPERATION_OWNER_AUTO_SURVEY" in cleanup_custody_owner
assert "gateway_manual_survey_cleanup_matches_transaction()" in (
    cleanup_custody_owner
), (
    "manual work may share cleanup only after exact pair cleanup custody, "
    "not merely because a manual operation exists"
)

finish_cleanup = function_body(
    ANCHOR, "gateway_survey_finish_cleanup_if_complete"
)
cleanup_complete = finish_cleanup.index(
    "survey_gateway_transaction_note_cleanup_complete("
)
inprogress = finish_cleanup.index("-EINPROGRESS", cleanup_complete)
empty_owner_gate = finish_cleanup.index(
    "gateway_survey_transaction.active.state !=",
    inprogress,
)
pair_complete = finish_cleanup.index(
    "survey_gateway_transaction_pair_complete(", empty_owner_gate
)
assert cleanup_complete < inprogress < empty_owner_gate < pair_complete, (
    "cleanup completion must retire an abandoning request and must preserve "
    "the pair while any other request delivery owner remains nonempty"
)
round_cleanup = finish_cleanup.index(
    "gateway_survey_start_next_round_termination_cleanup(", pair_complete
)
inactive = finish_cleanup.index("if (!gateway_survey_active)", round_cleanup)
terminal = finish_cleanup.index("gateway_observe_survey_terminal(", inactive)
pending_clear = finish_cleanup.index(
    "gateway_survey_finish_pending = false", terminal
)
owner_release = finish_cleanup.index(
    "gateway_operation_owner_release(", pending_clear
)
assert (
    pair_complete < round_cleanup < inactive < terminal <
    pending_clear < owner_release
), (
    "terminal observability and owner release must wait until every retained "
    "round cleanup lane has retired"
)

prepare_cleanup = function_body(
    ANCHOR, "gateway_survey_prepare_cleanup_delivery"
)
initialize = prepare_cleanup.index(
    "*cleanup = (struct gateway_survey_cleanup_delivery)"
)
active = prepare_cleanup.index(".active = true", initialize)
route = prepare_cleanup.index(
    "gateway_survey_prepare_pair_control(&route", active
)
request_timeout = prepare_cleanup.index(
    "survey_pair_control_timeout_ms(hop_count)", route
)
result_timeout = prepare_cleanup.index(
    "SURVEY_PAIR_ABORT_RESULT_TIMEOUT_MS", request_timeout
)
request_deadline = prepare_cleanup.index(
    "cleanup->request_deadline_ms =", result_timeout
)
semantic_deadline = prepare_cleanup.index(
    "cleanup->absolute_deadline_ms =", request_deadline
)
prepared = prepare_cleanup.index("cleanup->prepared = true", semantic_deadline)
assert (
    initialize < active < route < request_timeout < result_timeout <
    request_deadline < semantic_deadline < prepared
), (
    "cleanup must retain its exact identity across route retries, then derive "
    "separate route-aware request T and semantic T+R deadlines before submit"
)

cleanup_service = function_body(ANCHOR, "gateway_survey_service_cleanup")
inactive_slot = cleanup_service.index("if (!cleanup->active)")
inactive_begin = cleanup_service.index(
    "gateway_survey_begin_cleanup()", inactive_slot
)
inactive_finish = cleanup_service.index(
    "gateway_survey_finish_cleanup_if_complete(now_ms)", inactive_begin
)
inactive_return = cleanup_service.index("return;", inactive_finish)
assert inactive_slot < inactive_begin < inactive_finish < inactive_return, (
    "an empty cleanup slot may poll the common finalizer, whose transaction "
    "owner gate must distinguish no debt from a completed live request"
)
submitted = cleanup_service.index("if (cleanup->submitted)")
take_terminal = cleanup_service.index(
    "app_node_comm_take_delivery_event_for(cleanup->handle, &event)",
    submitted,
)
request_terminal = cleanup_service.index(
    "cleanup->request_terminal = true", take_terminal
)
zero_rf_failure = cleanup_service.index(
    "event.reason != NODE_COMM_TERMINAL_DELIVERED", request_terminal
)
zero_attempts = cleanup_service.index(
    "event.attempts_started == 0u", zero_rf_failure
)
zero_rf_unavailable = cleanup_service.index(
    "cleanup->peer_unavailable = true", zero_attempts
)
result_terminal = cleanup_service.index(
    "if (cleanup->result_terminal)", zero_rf_unavailable
)
assert "app_mesh_report_gateway_delivery_confirmation_pending(" not in (
    cleanup_service[result_terminal:]
), (
    "exact COMMAND_OK proves ABORT execution; lost ACK_CONFIRM may retain the "
    "result owner independently but must not extend frozen cleanup"
)
result_abandon = cleanup_service.index(
    "app_node_comm_abandon_delivery(cleanup->handle)", result_terminal
)
result_ok = cleanup_service.index("cleanup->result_ok", result_abandon)
result_completion = cleanup_service.index(
    "cleanup->completion_ready = true", result_ok
)
deadline_gate = cleanup_service.index(
    "if (now_ms < cleanup->absolute_deadline_ms)", result_completion
)
abandon = cleanup_service.index(
    "app_node_comm_abandon_delivery(cleanup->handle)", deadline_gate
)
abandon_failure = cleanup_service.index(
    "ret < 0 && ret != -ENOENT && ret != -EALREADY", abandon
)
abandon_quarantine = cleanup_service.index(
    "gateway_survey_quarantine_cleanup(", abandon_failure
)
abandon_failure_return = cleanup_service.index(
    "return;", abandon_quarantine
)
completion_ready = cleanup_service.index(
    "cleanup->completion_ready = true", abandon_failure_return
)
reschedule = cleanup_service.index(
    "gateway_survey_work_schedule(", completion_ready
)
assert (
    submitted < take_terminal < request_terminal < zero_rf_failure <
    zero_attempts < zero_rf_unavailable < result_terminal < result_abandon <
    result_ok < result_completion < deadline_gate < abandon <
    abandon_failure < abandon_quarantine < abandon_failure_return <
    completion_ready < reschedule
), (
    "transport delivery alone must not prove ABORT execution: only a zero-RF "
    "failure, an exact semantic result, or the semantic deadline may finish "
    "cleanup, and a failed local abandonment must enter bounded quarantine "
    "instead of claiming completion or depending on reset"
)

cleanup_wait = cleanup_service.index(
    "gateway_begin_command_result_wait_until("
)
cleanup_result_deadline = cleanup_service.index(
    "(uint32_t)cleanup->absolute_deadline_ms", cleanup_wait
)
cleanup_submit = cleanup_service.index(
    "app_node_comm_submit_delivery(", cleanup_result_deadline
)
cleanup_request_deadline = cleanup_service.index(
    "cleanup->request_deadline_ms", cleanup_submit
)
assert (
    cleanup_wait < cleanup_result_deadline < cleanup_submit <
    cleanup_request_deadline
), (
    "cleanup must reserve its exact semantic ABORT result through T+R before "
    "submitting transport with the shorter one-way request deadline T"
)

cleanup_match = function_body(
    ANCHOR, "gateway_survey_cleanup_command_matches"
)
for exact_identity in (
    "command_id == CMD_SURVEY_ABORT",
    "command->src_id == DEVICE_ID",
    "command->dst_id == cleanup->target_id",
    "command->session_id == survey_operation_session_id(",
    "command->seq == cleanup->sequence",
):
    assert exact_identity in cleanup_match

cleanup_terminal = function_body(
    ANCHOR, "gateway_survey_cleanup_note_command_terminal"
)
assert "gateway_survey_cleanup_command_matches(command, command_id)" in (
    cleanup_terminal
)
assert "gateway_survey_cleanup.result_terminal = true" in cleanup_terminal
assert "gateway_survey_cleanup.result_ok = status == COMMAND_OK" in (
    cleanup_terminal
)

timeout_side_effect = function_body(
    ANCHOR, "gateway_command_timeout_side_effects"
)
result_side_effect = function_body(
    ANCHOR, "gateway_command_result_side_effects"
)
assert "gateway_survey_cleanup_note_command_terminal(" in timeout_side_effect
assert "command, command_id, COMMAND_TIMEOUT" in timeout_side_effect
assert "gateway_survey_cleanup_note_command_terminal(" in result_side_effect
assert "command, command_id, status" in result_side_effect

round_control_failure = function_body(
    ANCHOR, "gateway_survey_round_fail_current_control"
)
zero_cleanup = round_control_failure.index("if (cleanup_mask == 0u)")
zero_cleanup_finish = round_control_failure.index(
    "gateway_survey_finish_cleanup_if_complete(", zero_cleanup
)
assert "survey_gateway_transaction_pair_complete(" not in (
    round_control_failure[zero_cleanup:]
), (
    "zero-debt round failure must use the owner-aware cleanup finalizer; "
    "direct pair completion can strand an ABANDONED zero-RF request"
)
assert zero_cleanup < zero_cleanup_finish

round_control_result = function_body(
    ANCHOR, "gateway_survey_round_note_control_result"
)
phase_complete = round_control_result.index(
    "survey_gateway_transaction_phase_complete("
)
assert "survey_gateway_transaction_pair_complete(" not in round_control_result
round_confirmation = function_body(
    ANCHOR, "gateway_survey_round_apply_control_confirmation"
)
proof_ready = round_confirmation.index(
    "app_gateway_survey_round_control_confirmation_ready("
)
successful_pair_release = round_confirmation.index(
    "survey_gateway_transaction_pair_complete(", proof_ready
)
assert proof_ready < successful_pair_release, (
    "successful START_INITIATOR may release pair identity only after the "
    "retired result has exact terminal ACK_CONFIRM proof"
)

manual_control = function_body(
    ANCHOR, "gateway_route_survey_pair_control"
)
manual_first_prepare = manual_control.index(
    "if (!gateway_manual_survey_pair_state.valid)"
)
manual_owner_claim = manual_control.index(
    "gateway_operation_owner_claim(", manual_first_prepare
)
manual_capacity_reserve = manual_control.index(
    "app_node_comm_reserve_bounded_control(", manual_owner_claim
)
manual_pending_reserve = manual_control.index(
    "gateway_command_result_wait_reserve(", manual_capacity_reserve
)
manual_physical_route = manual_control.index(
    "gateway_survey_prepare_pair_control(&outbound, NULL)",
    manual_pending_reserve,
)
manual_durable_generation = manual_control.index(
    "gateway_durable_reserve_survey_generation(", manual_physical_route
)
manual_state_create = manual_control.index(
    "gateway_manual_survey_pair_state =", manual_durable_generation
)
assert (
    manual_first_prepare < manual_owner_claim < manual_capacity_reserve <
    manual_pending_reserve < manual_physical_route <
    manual_durable_generation < manual_state_create
), (
    "the first manual PREPARE must claim its operation, bounded-control "
    "capacity, singleton result wait, and physical route before consuming a "
    "durable survey generation"
)
manual_nonzero_round = manual_control.index(
    ".round_id = 1u", manual_state_create
)
manual_state_created = manual_control.index(
    "created_state = true", manual_nonzero_round
)
manual_append_pair = manual_control.index(
    "survey_append_pair_tlvs(", manual_state_created
)
manual_append_round = manual_control.index(
    "survey_round_id_append_tlv(", manual_append_pair
)
manual_append_commitment = manual_control.index(
    "survey_round_commitment_append_tlv(", manual_append_round
)
manual_send = manual_control.index(
    "gateway_survey_send_pair_control(", manual_append_commitment
)
assert (
    manual_state_create < manual_nonzero_round < manual_state_created <
    manual_append_pair < manual_append_round < manual_append_commitment <
    manual_send
), (
    "manual PREPARE and START must carry one gateway-owned nonzero round "
    "identity and its matching commitment"
)
assert (
    "gateway_manual_survey_pair_state.round_id"
    in manual_control[manual_append_round:manual_append_commitment]
), "manual controls must serialize the retained round identity"

host_rebind = manual_control.index(
    "gateway_command_result_rebind_command(", manual_append_commitment
)
reserved_wait_commit = manual_control.index(
    "gateway_command_result_wait_commit(", host_rebind
)
existing_wait_gate = manual_control.index(
    "} else {", reserved_wait_commit
)
assert (
    host_rebind < reserved_wait_commit < existing_wait_gate
), (
    "the first PREPARE must rebind host custody, then commit its nonsemantic "
    "result-wait reservation before later controls use ordinary admission"
)

wait_reserve = function_body(
    GATEWAY_BLE, "gateway_command_result_wait_reserve"
)
reserve_lock = wait_reserve.index("gateway_pending_command_lock()")
reserve_pending_guard = wait_reserve.index(
    "gateway_command_pending_state.active", reserve_lock
)
reserve_timeout_guard = wait_reserve.index(
    "gateway_command_timeout_dispatch_state.pending", reserve_pending_guard
)
reserve_exact_guard = wait_reserve.index(
    "gateway_command_wait_reservation_active != 0u", reserve_timeout_guard
)
reserve_publish = wait_reserve.index(
    "gateway_command_wait_reservation_active =", reserve_exact_guard
)
assert (
    reserve_lock < reserve_pending_guard < reserve_timeout_guard <
    reserve_exact_guard < reserve_publish
)
assert "gateway_command_pending_start" not in wait_reserve
assert "gateway_command_result_timeout_arm" not in wait_reserve, (
    "the pre-NVS result-wait reservation must not publish a semantic command "
    "or arm its timeout"
)

wait_commit = function_body(
    GATEWAY_BLE, "gateway_command_result_wait_commit"
)
commit_lock = wait_commit.index("gateway_pending_command_lock()")
commit_exact = wait_commit.index(
    "gateway_command_wait_reservation_active != reservation_token",
    commit_lock,
)
commit_pending = wait_commit.index(
    "gateway_command_pending_start_until(", commit_exact
)
commit_consume = wait_commit.index(
    "gateway_command_wait_reservation_active = 0u", commit_pending
)
commit_timeout = wait_commit.index(
    "gateway_command_result_timeout_arm(", commit_consume
)
assert (
    commit_lock < commit_exact < commit_pending < commit_consume <
    commit_timeout
), (
    "only the exact reservation may publish the semantic pending identity and "
    "arm its retained timeout"
)

pre_submit = manual_control[manual_state_created:manual_send]
assert pre_submit.count("if (created_state)") >= 4
assert pre_submit.count("gateway_manual_survey_pair_reset()") >= 4, (
    "every failure after creating manual state but before possible RF must "
    "release that newly-created state"
)

reserved_commit = manual_control.index(
    "app_node_comm_commit_bounded_control_reservation(",
    reserved_wait_commit,
)
reserved_commit_call = manual_control[
    reserved_commit : reserved_commit + 500
]
reservation_consumed = manual_control.index(
    "memset(&control_reservation, 0, sizeof(control_reservation))",
    reserved_commit,
)
assert "&control_reservation" in reserved_commit_call
assert "&outbound" in reserved_commit_call
assert "request_deadline_ms" in reserved_commit_call
assert "&delivery_handle" in reserved_commit_call
assert (
    reserved_wait_commit < reserved_commit < reservation_consumed
), (
    "the rebound first PREPARE must commit the exact bounded-control lease "
    "before the local reservation token is retired"
)

send_success = manual_control.index("if (ret < 0)", manual_send)
prepare_masks = manual_control.index(
    "gateway_manual_survey_pair_state.prepare_submitted_mask |=",
    send_success,
)
prepare_possible = manual_control.index(
    "gateway_manual_survey_pair_state.prepare_possible_mask |=",
    prepare_masks,
)
retain_command = manual_control.index(
    "gateway_manual_survey_pair_state.control_command = outbound.packet",
    prepare_possible,
)
retain_deadline = manual_control.index(
    "gateway_manual_survey_pair_state.control_transaction_deadline_ms =",
    retain_command,
)
retain_handle = manual_control.index(
    "gateway_manual_survey_pair_state.control_delivery_handle = delivery_handle",
    retain_deadline,
)
publish_owner = manual_control.index(
    "gateway_manual_survey_pair_state.control_delivery_active = true",
    retain_handle,
)
schedule_owner = manual_control.index(
    "gateway_survey_work_schedule(", publish_owner
)
assert (
    send_success < prepare_masks < prepare_possible < retain_command <
    retain_deadline < retain_handle < publish_owner < schedule_owner
), (
    "successful PREPARE submission must record possible remote state before "
    "publishing the exact retained transport owner"
)
assert "app_node_comm_auto_reap_delivery" not in manual_control, (
    "manual semantic controls must retain their handle for same-identity "
    "redrive instead of discarding it after the first RF wave"
)
schedule_failure = manual_control.index("if (ret < 0)", schedule_owner)
release_owner = manual_control.index(
    "gateway_manual_survey_control_release()", schedule_failure
)
release_guard = manual_control.index("if (cleanup_ret < 0)", release_owner)
schedule_cleanup = manual_control.index(
    "gateway_manual_survey_pair_begin_cleanup()", release_guard
)
assert schedule_failure < release_owner < release_guard < schedule_cleanup, (
    "a scheduling failure after RF admission must release the exact retained "
    "owner before transferring every possible PREPARE side effect to cleanup"
)

manual_terminal = function_body(
    ANCHOR, "gateway_manual_survey_pair_note_terminal"
)
start_case = manual_terminal.index("case CMD_SURVEY_START_PAIR:")
started = manual_terminal.index(
    "gateway_manual_survey_pair_state.started_mask |= target_mask",
    start_case,
)
arm_start = manual_terminal.index(
    "gateway_manual_survey_pair_arm_start_release()", started
)
abort_case = manual_terminal.index("case CMD_SURVEY_ABORT:", arm_start)
assert start_case < started < arm_start < abort_case, (
    "only an accepted START terminal may contribute to the synchronized "
    "START release"
)

manual_arm_start = function_body(
    ANCHOR, "gateway_manual_survey_pair_arm_start_release"
)
both_started = manual_arm_start.index(
    "gateway_manual_survey_pair_state.started_mask != both_endpoints"
)
nonzero_round = manual_arm_start.index(
    "gateway_manual_survey_pair_state.round_id ==", both_started
)
release_started = manual_arm_start.index(
    "gateway_manual_survey_pair_state.start_release_started_at_ms =",
    nonzero_round,
)
release_armed = manual_arm_start.index(
    "gateway_manual_survey_pair_state.start_release_armed = true",
    release_started,
)
assert (
    both_started < nonzero_round < release_started < release_armed
), (
    "both START results must complete before the manual pair records one "
    "future synchronized START release"
)

manual_match = function_body(
    ANCHOR, "gateway_manual_survey_pair_matches_sample"
)
assert "!manual->cleanup_requested" in manual_match
assert "manual->start_release_armed" in manual_match
assert "manual->round_id != SURVEY_LEGACY_ROUND_ID" in manual_match
assert "sample, &manual->pair, manual->round_id" in manual_match, (
    "manual results must retain their exact round binding"
)

manual_preflight = function_body(
    ANCHOR, "gateway_manual_survey_pair_preflight_sample"
)
assert "start_release_started_at_ms +" in manual_preflight
assert "SURVEY_ROUND_START_EXECUTE_DELAY_MS" in manual_preflight
assert "uptime_deadline_reached(" in manual_preflight, (
    "manual results must remain inadmissible until the synchronized START "
    "release instant"
)

for retired_runtime_symbol in (
    "CMD_SURVEY_GO",
    "survey_round_go_",
    "gateway_manual_survey_pair_service_go",
    "go_rf_started",
):
    assert retired_runtime_symbol not in ANCHOR

manual_cleanup = function_body(
    ANCHOR, "gateway_manual_survey_pair_begin_cleanup"
)
capture_possible = manual_cleanup.index(
    "possible_prepare_mask =\n"
    "        gateway_manual_survey_pair_state.prepare_possible_mask"
)
load_pair = manual_cleanup.index(
    "survey_gateway_transaction_load_pair(", capture_possible
)
transfer_possible = manual_cleanup.index(
    "gateway_survey_transaction.possible_prepare_mask =",
    load_pair,
)
require_cleanup = manual_cleanup.index(
    "survey_gateway_transaction_require_cleanup(", transfer_possible
)
mark_cleanup = manual_cleanup.index(
    "gateway_manual_survey_pair_state.cleanup_requested = true",
    require_cleanup,
)
schedule_cleanup = manual_cleanup.index(
    "gateway_survey_work_schedule(SURVEY_GATEWAY_DUE_CLEANUP, 0u)",
    mark_cleanup,
)
assert (
    capture_possible < load_pair < transfer_possible < require_cleanup <
    mark_cleanup < schedule_cleanup
), (
    "manual cancellation must transfer exact possible-PREPARE ownership into "
    "the bounded survey transaction before it schedules cleanup"
)

manual_expire = function_body(
    ANCHOR, "gateway_manual_survey_pair_expire"
)
assert "gateway_manual_survey_pair_begin_cleanup()" in manual_expire
assert "gateway_manual_survey_pair_reset()" not in manual_expire, (
    "lease expiry must not erase possible remote PREPARE state"
)

local_abort = function_body(
    ANCHOR, "gateway_handle_local_survey_abort"
)
abort_cleanup = local_abort.index(
    "gateway_manual_survey_pair_begin_cleanup()"
)
abort_success = local_abort.index(
    "gateway_emit_host_command_result_reserved(", abort_cleanup
)
assert abort_cleanup < abort_success, (
    "local host ABORT may report success only after remote cleanup custody "
    "has been installed"
)

cleanup_identity = function_body(
    ANCHOR, "gateway_survey_prepare_cleanup_delivery"
)
assert "gateway_manual_survey_cleanup_matches_transaction()" in cleanup_identity
assert "gateway_manual_survey_pair_state.round_commitment" in cleanup_identity

cleanup_finish = function_body(
    ANCHOR, "gateway_survey_finish_cleanup_if_complete"
)
pair_retired = cleanup_finish.index(
    "survey_gateway_transaction_pair_complete("
)
manual_reset = cleanup_finish.index(
    "gateway_manual_survey_pair_reset()", pair_retired
)
assert pair_retired < manual_reset, (
    "manual operation ownership must remain live until its cleanup "
    "transaction is retired"
)

print("survey pair-control lifecycle source invariants passed")
