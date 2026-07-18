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


reject = function_body(ANCHOR, "gateway_reject_survey_request")
assert reject.count("gateway_emit_host_command_result(") == 1
assert reject.count("gateway_observe_host_terminal(") == 1

reachability = function_body(ANCHOR, "gateway_route_survey_reachability")
assert "gateway_command_survey_sample_admission(" in reachability
assert (
    "uint32_t command_budget_ms = "
    "SURVEY_GATEWAY_OPERATION_DEFAULT_BUDGET_MS;"
) in reachability, "survey must retain its dedicated 600-second default"
assert re.search(
    r"gateway_command_extract_budget_ms\s*\([^;]*"
    r"SURVEY_GATEWAY_OPERATION_DEFAULT_BUDGET_MS\s*,",
    reachability,
    re.DOTALL,
), "survey budget extraction must use its protocol-specific default"
assert "GATEWAY_COMMAND_BUDGET_MAX_MS" not in reachability, (
    "raising the explicit command maximum must not silently extend survey default"
)
assert re.search(
    r"gateway_command_budget_window_ms\s*\(\s*true\s*,\s*"
    r"command_budget_ms\s*,\s*1u\s*,\s*collection_delay_ms\s*\)",
    reachability,
), "survey collection must remain one indivisible phase under an explicit budget"

control_timeout = function_body(ANCHOR, "gateway_survey_control_timeout_ms")
natural_timeout = function_body(
    ANCHOR, "gateway_survey_natural_control_timeout_ms"
)
assert "gateway_survey_remaining_control_phases" not in ANCHOR, (
    "survey control deadlines must not be divided across future pair phases"
)
assert "survey_gateway_reverse_hint_for_target(" in natural_timeout
assert "survey_pair_control_timeout_ms(reverse_hint.hop_count)" in natural_timeout
assert "SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS" in natural_timeout, (
    "unknown route depth must keep the established 90-second fallback"
)
assert re.search(
    r"gateway_command_budget_window_ms\s*\(\s*true\s*,\s*"
    r"remaining_ms\s*,\s*1u\s*,\s*"
    r"natural_timeout_ms\s*\)",
    control_timeout,
), "each survey control must use the natural timeout clipped by the global deadline"

report_accept = function_body(ANCHOR, "gateway_handle_survey_discovery_report")
assert "survey_gateway_hop_count_from_report_ttl(packet->ttl)" in report_accept

cleanup = function_body(ANCHOR, "gateway_survey_prepare_cleanup_delivery")
assert "gateway_survey_natural_control_timeout_ms(target_id)" in cleanup
assert "gateway_survey_operation_deadline_ms" not in cleanup, (
    "cleanup must retain its bounded natural deadline after the host deadline"
)

assert reachability.count("gateway_emit_host_command_result(") == 1
assert re.search(
    r"gateway_emit_host_command_result\s*\(\s*host_packet,\s*"
    r"CMD_SURVEY_REACHABILITY,\s*COMMAND_OK,\s*0u\s*\)",
    reachability,
), "only the accepted survey path may emit its result outside the rejection owner"
assert "gateway_observe_host_terminal(" not in reachability
for expression in re.findall(r"\breturn\s+(.+?);", reachability, re.DOTALL):
    normalized = " ".join(expression.split())
    assert normalized == "0" or normalized.startswith(
        "gateway_reject_survey_request("
    ), f"survey rejection bypasses its terminal owner: {normalized}"

worker = function_body(ANCHOR, "gateway_host_command_work_handler")
assert re.search(
    r"kind\s*==\s*GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY\s*&&\s*"
    r"item\.command_id\s*!=\s*CMD_SURVEY_REACHABILITY",
    worker,
), "worker must not synthesize a second reachability terminal"

survey_worker = function_body(ANCHOR, "gateway_survey_work_handler")
deadline_check = survey_worker.index("gateway_survey_operation_deadline_ms")
pair_planning = survey_worker.index("survey_gateway_plan_pairs(")
assert deadline_check < pair_planning, (
    "an explicit operation deadline must win before collection can be closed "
    "or pairs planned"
)
assert re.search(
    r"if\s*\(\s*ret\s*==\s*-ETIMEDOUT\s*&&\s*"
    r"gateway_survey_budget_explicit\s*\)\s*\{.*?"
    r"gateway_survey_auto_finish_status\s*\(\s*COMMAND_TIMEOUT\s*,\s*"
    r"GATEWAY_COMMAND_EVENT_REASON_TIMEOUT\s*\)",
    survey_worker,
    re.DOTALL,
), "an exhausted global budget must terminate the survey as a global timeout"

print("survey command source invariants passed")
