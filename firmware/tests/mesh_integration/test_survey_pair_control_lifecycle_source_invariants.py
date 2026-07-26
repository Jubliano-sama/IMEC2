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
malformed_return = preflight.index("return false;", decode_reject)
reconcile = preflight.index("survey_gateway_transaction_reconcile_result(")
assert decode < decode_reject < malformed_return < reconcile, (
    "malformed pair results must be rejected before transaction reconciliation"
)
assert "gateway_survey_result_preflight =" not in preflight[:reconcile], (
    "malformed input must not populate the accepted-result latch"
)

latch_assignment = preflight.index("gateway_survey_result_preflight =", reconcile)
latch_guard = preflight[max(reconcile, latch_assignment - 500) : latch_assignment]
assert "SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK" in latch_guard
assert "SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_FAILURE" in latch_guard
assert "!gateway_survey_result_preflight.valid" in latch_guard, (
    "the first accepted result must own an immutable latch"
)

duplicate_branch = preflight.index(
    "SURVEY_GATEWAY_TRANSACTION_RESULT_DUPLICATE", latch_assignment
)
duplicate_end = preflight.index("return true;", duplicate_branch)
duplicate_path = preflight[duplicate_branch:duplicate_end]
assert "gateway_survey_result_preflight =" not in duplicate_path
assert "memset(&gateway_survey_result_preflight" not in duplicate_path, (
    "a duplicate during cancel/take deferral must not overwrite the accepted latch"
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
finalize_call = finish.index("gateway_survey_finalize_pair_observation()")
finalize_retry = finish.index(
    "if (finalize_result == GATEWAY_SURVEY_PAIR_FINALIZE_RETRY)",
    finalize_call,
)
finalize_retry_open = finish.index("{", finalize_retry)
finalize_retry_end = block_end(finish, finalize_retry_open)
finalize_retry_body = finish[finalize_retry : finalize_retry_end + 1]
retry_status = finalize_retry_body.index(
    "gateway_survey_finish_pending_status = status"
)
retry_reason = finalize_retry_body.index(
    "gateway_survey_finish_pending_reason = reason"
)
retry_pending = finalize_retry_body.index(
    "gateway_survey_finish_pending = true"
)
retry_schedule = finalize_retry_body.index("k_work_reschedule(")
retry_return = finalize_retry_body.index("return;", retry_schedule)
assert (
    retry_status < retry_reason < retry_pending < retry_schedule < retry_return
), "pair-finalize backpressure must retain the caller's exact outcome"
assert "gateway_observe_survey_terminal(" not in finalize_retry_body
assert "gateway_survey_begin_cleanup(" not in finalize_retry_body

cancel_take = finish.index("gateway_survey_cancel_take_active_delivery(")
cancel_error = finish.index("if (ret < 0)", cancel_take)
pending = finish.index("gateway_survey_finish_pending = true", cancel_error)
retry = finish.index("k_work_reschedule(", pending)
defer_return = finish.index("return;", retry)
terminal_observation = finish.index("gateway_observe_survey_terminal(")
cleanup = finish.index("survey_gateway_transaction_require_cleanup(")
assert cancel_take < cancel_error < pending < retry < defer_return
assert defer_return < terminal_observation < cleanup, (
    "finish must defer terminal reporting and cleanup while the request backend is active"
)

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
completion_ready = cleanup_service.index(
    "cleanup->completion_ready = true", abandon
)
reschedule = cleanup_service.index("k_work_reschedule(", completion_ready)
assert (
    submitted < take_terminal < deadline_gate < abandon <
    completion_ready < reschedule
), (
    "a submitted cleanup with a missing terminal must abandon its exact "
    "delivery handle and advance after the absolute deadline"
)

print("survey pair-control lifecycle source invariants passed")
