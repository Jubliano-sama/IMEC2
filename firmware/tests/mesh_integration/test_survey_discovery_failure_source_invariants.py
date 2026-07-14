#!/usr/bin/env python3
"""Guard the app-level survey failure paths that native core tests cannot link."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
DISCOVERY = (ROOT / "app/src/app_anchor_survey_discovery.c").read_text()
RUNTIME = (ROOT / "app/src/app_anchor_survey_runtime.c").read_text()


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


def braced_block_at(source: str, marker_index: int) -> str:
    brace = source.index("{", marker_index)
    depth = 0
    for index in range(brace, len(source)):
        depth += source[index] == "{"
        depth -= source[index] == "}"
        if depth == 0:
            return source[brace : index + 1]
    raise AssertionError("unterminated block")


run = function_body(DISCOVERY, "app_anchor_survey_discovery_run")
missed_window_index = run.index("if (should_tx &&")
missed_window_block = braced_block_at(run, missed_window_index)
tx_guard_index = run.index("if (should_tx) {", missed_window_index)
tx_guard_block = braced_block_at(run, tx_guard_index)
send_index = run.index("dwm3000_driver_send_frame(")
attempt_index = run.index("actual_attempt_count++", send_index)
assert "should_tx = false" in missed_window_block
assert "actual_attempt_count++" not in missed_window_block, (
    "a missed or busy nominal window must not consume a discovery attempt"
)
assert "dwm3000_driver_send_frame(" in tx_guard_block
assert "actual_attempt_count++" in tx_guard_block
assert send_index < attempt_index
assert run.count("actual_attempt_count++") == 1, (
    "discovery attempts must be counted at one explicit RF outcome boundary"
)
assert "actual_attempt_count != SURVEY_DISCOVERY_OPPORTUNITY_COUNT" in run

worker = function_body(RUNTIME, "survey_work_handler")
run_call_index = worker.index("app_anchor_survey_discovery_run(")
failure_guard_index = worker.index("if (ret < 0 &&", run_call_index)
abort_guard_index = worker.index(
    "!app_anchor_survey_runtime_abort_requested()", failure_guard_index
)
empty_report_index = worker.index(
    "app_anchor_survey_discovery_stage_empty_report(", abort_guard_index
)
failure_block = braced_block_at(worker, failure_guard_index)
assert run_call_index < failure_guard_index < abort_guard_index < empty_report_index
assert worker.count("app_anchor_survey_discovery_stage_empty_report(") == 1, (
    "one failed, non-aborted discovery run must stage exactly one fail-safe report"
)
assert failure_block.count("app_anchor_survey_discovery_stage_empty_report(") == 1

empty_report = function_body(
    DISCOVERY, "app_anchor_survey_discovery_stage_empty_report"
)
assert empty_report.count("prepare_discovery_report(") == 1
assert re.search(
    r"prepare_discovery_report\s*\(\s*config->survey_id\s*,\s*"
    r"NULL\s*,\s*0u\s*,",
    empty_report,
), "the fail-safe report must contain a valid zero-peer reachability list"

print("survey discovery failure source invariants passed")
