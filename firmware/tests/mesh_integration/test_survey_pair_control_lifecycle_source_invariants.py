#!/usr/bin/env python3
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = (ROOT / "app/src/app_anchor.c").read_text()


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
resume_finish = worker.index("gateway_survey_auto_finish_status(", terminal_gate)
assert service < resume < terminal_gate < resume_finish, (
    "deferred finish must resume only after polling the exact request delivery"
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

print("survey pair-control lifecycle source invariants passed")
