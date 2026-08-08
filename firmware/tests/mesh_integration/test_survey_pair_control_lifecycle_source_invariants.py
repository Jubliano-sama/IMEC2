#!/usr/bin/env python3
from pathlib import Path
import re

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = read_composed_source(ROOT / "app/src/app_anchor.c")


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


preflight = function_body(ANCHOR, "gateway_survey_auto_preflight_result")
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
    ANCHOR, "gateway_survey_auto_commit_preflight_result"
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
duplicate_rearm = duplicate_path.index(
    "survey_gateway_response_ack_settle_note_duplicate("
)
duplicate_schedule = duplicate_path.index(
    "gateway_survey_schedule_drive()", duplicate_rearm
)
assert "preparation.received_at_ms" in duplicate_path[
    duplicate_rearm:duplicate_schedule
]
assert "gateway_survey_operation_deadline_ms" in duplicate_path[
    duplicate_rearm:duplicate_schedule
], (
    "an exact PREPARE/START retry must restart one continuous ACK-settle "
    "window from physical receipt without extending the immutable operation"
)

accepted_result = function_body(
    ANCHOR, "gateway_survey_auto_note_command_result"
)
close_call = accepted_result.index("gateway_survey_complete_accepted_delivery()")
eagain = accepted_result.index("if (ret == -EAGAIN)", close_call)
eagain_return = accepted_result.index("return;", eagain)
assert "memset(&gateway_survey_result_preflight" not in accepted_result[
    close_call:eagain_return
], "the accepted latch must survive an active backend cancel/take race"

finish = function_body(ANCHOR, "gateway_survey_auto_finish_status")
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
retry = finish.index("gateway_survey_work_reschedule(", cancel_error)
defer_return = finish.index("return 0;", retry)
assert (
    finish_pending < cleanup_custody < cancel_take < cancel_error <
    retry < defer_return
), (
    "a cancel/take race must retain the frozen terminal outcome and global "
    "cleanup custody while the exact request backend remains active"
)
observation_cancel = finish.index(
    "gateway_survey_pair_observation_active = false", defer_return
)
finish_inactive = finish.index("gateway_survey_active = false")
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
    defer_return < observation_cancel < finish_inactive <
    finish_latch_clear < finish_preparation_clear < begin_cleanup
), (
    "terminal finish must cancel observation and make the survey inactive "
    "before cleanup can publish the frozen terminal outcome"
)

abandon_current = function_body(
    ANCHOR, "gateway_survey_abandon_current"
)
assert "memset(&gateway_survey_result_preflight" in abandon_current
assert "memset(&gateway_survey_result_preparation" in abandon_current

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

worker = function_body(ANCHOR, "gateway_survey_work_handler")
service = worker.index("gateway_survey_service_active_delivery()")
resume = worker.index("if (gateway_survey_finish_pending", service)
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
    "gateway_survey_auto_finish_status(finish_status, finish_reason)",
    resume_clear,
)
resume_return = worker.index("return;", resume_finish)
assert (
    service < resume < terminal_gate < resume_status < resume_reason <
    resume_clear < resume_finish < resume_return
), (
    "deferred finish must resume only after polling the exact request delivery"
)
cleanup_service = worker.index("gateway_survey_service_cleanup()", resume)
assert resume_return < cleanup_service, (
    "the exact deferred terminal outcome must replay before normal survey work"
)

finish_cleanup = function_body(
    ANCHOR, "gateway_survey_finish_cleanup_if_complete"
)
cleanup_complete = finish_cleanup.index(
    "survey_gateway_transaction_note_cleanup_complete("
)
inprogress = finish_cleanup.index("-EINPROGRESS", cleanup_complete)
pair_complete = finish_cleanup.index(
    "survey_gateway_transaction_pair_complete(", cleanup_complete
)
assert cleanup_complete < inprogress < pair_complete, (
    "cleanup completion must preserve the pair while request delivery is nonterminal"
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
deadline = prepare_cleanup.index(
    ".absolute_deadline_ms = cleanup_deadline_ms", initialize
)
active = prepare_cleanup.index(".active = true", deadline)
route = prepare_cleanup.index(
    "gateway_survey_prepare_pair_control(&route", active
)
assert initialize < deadline < active < route, (
    "cleanup identity and its frozen deadline must exist before route lookup "
    "can fail"
)

cleanup_service = function_body(ANCHOR, "gateway_survey_service_cleanup")
submitted = cleanup_service.index("if (cleanup->submitted)")
take_terminal = cleanup_service.index(
    "app_node_comm_take_delivery_event_for(cleanup->handle, &event)",
    submitted,
)
deadline_gate = cleanup_service.index(
    "if (now_ms < cleanup->absolute_deadline_ms)", take_terminal
)
abandon = cleanup_service.index(
    "app_node_comm_abandon_delivery(cleanup->handle)", deadline_gate
)
abandon_failure = cleanup_service.index(
    "ret < 0 && ret != -ENOENT && ret != -EALREADY", abandon
)
abandon_fail_closed = cleanup_service.index(
    "app_watchdog_stop_feeding()", abandon_failure
)
abandon_failure_return = cleanup_service.index(
    "return;", abandon_fail_closed
)
completion_ready = cleanup_service.index(
    "cleanup->completion_ready = true", abandon_failure_return
)
reschedule = cleanup_service.index(
    "gateway_survey_work_reschedule(", completion_ready
)
assert (
    submitted < take_terminal < deadline_gate < abandon <
    abandon_failure < abandon_fail_closed < abandon_failure_return <
    completion_ready < reschedule
), (
    "a submitted cleanup must preserve its exact handle and fail closed when "
    "abandonment fails; it may advance after the deadline only when transport "
    "custody is retired"
)

manual_control = function_body(
    ANCHOR, "gateway_route_survey_pair_control"
)
manual_state_create = manual_control.index(
    "gateway_manual_survey_pair_state ="
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

pre_submit = manual_control[manual_state_created:manual_send]
assert pre_submit.count("if (created_state)") >= 4
assert pre_submit.count("gateway_manual_survey_pair_reset()") >= 4, (
    "every failure after creating manual state but before possible RF must "
    "release that newly-created state"
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
auto_reap = manual_control.index(
    "app_node_comm_auto_reap_delivery(delivery_handle)", prepare_possible
)
assert send_success < prepare_masks < prepare_possible < auto_reap, (
    "successful PREPARE submission must record possible remote state before "
    "auto-reap can fail, so exact ABORT remains admissible"
)
auto_reap_failure = manual_control.index("if (ret < 0)", auto_reap)
auto_reap_abandon = manual_control.index(
    "app_node_comm_abandon_delivery(delivery_handle)", auto_reap_failure
)
auto_reap_cleanup = manual_control.index(
    "gateway_manual_survey_pair_begin_cleanup()", auto_reap_abandon
)
assert auto_reap_failure < auto_reap_abandon < auto_reap_cleanup, (
    "a backend custody failure after RF admission must abandon the exact "
    "delivery and transfer every possible PREPARE side effect to cleanup"
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
    "gateway_survey_work_reschedule(0u)", mark_cleanup
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
