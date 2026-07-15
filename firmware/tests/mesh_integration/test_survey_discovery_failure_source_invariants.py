#!/usr/bin/env python3
"""Guard the app-level survey failure paths that native core tests cannot link."""

from pathlib import Path
import re

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
DISCOVERY = (ROOT / "app/src/app_anchor_survey_discovery.c").read_text()
RUNTIME = (ROOT / "app/src/app_anchor_survey_runtime.c").read_text()
DRIVER = read_composed_source(ROOT / "app/src/dwm3000_driver.c")
DRIVER_HEADER = (ROOT / "app/src/dwm3000_driver.h").read_text()
CORE_SURVEY = (ROOT / "src/survey.c").read_text()


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
assert run.count("send_local_survey_probe(") == 2
assert run.count("if (rf_started)") == 2
for match in re.finditer(r"if \(rf_started\)", run):
    rf_started_block = braced_block_at(run, match.start())
    assert "survey_discovery_probe_attempt_note_rf_started(" in rf_started_block, (
        "every accepted RF start must consume exactly one probe opportunity"
    )
assert "dwm3000_driver_send_frame(" not in run
assert "while (deferred_mask != 0u" in run
assert run.count("schedule_survey_probe_retry(") == 2
assert "return -EBUSY" not in run, (
    "a second pre-RF refusal must remain pending until the shared deadline"
)
assert "survey_discovery_probe_real_attempt_count(" in run
deficit_index = run.index(
    "survey_discovery_probe_real_attempt_count("
)
deficit_guard_index = run.rfind("if (", 0, deficit_index)
deficit_block = braced_block_at(run, deficit_guard_index)
deficit_condition = run[
    deficit_guard_index:run.index("{", deficit_guard_index)
]
assert "survey_discovery_probe_real_attempt_count(" in deficit_condition
assert "SURVEY_DISCOVERY_OPPORTUNITY_COUNT" in deficit_condition
assert "prepare_discovery_report(" in deficit_block, (
    "deadline exhaustion must still stage a report containing heard peers"
)
assert re.search(
    r"prepare_discovery_report\s*\(\s*config->survey_id\s*,\s*"
    r"entries\s*,\s*entry_count\s*,.*?COMMAND_RADIO_ERROR\s*\)",
    deficit_block,
    re.S,
), (
    "fewer than four real RF opportunities must stage the exact heard-peer "
    "report with an explicit radio-error status"
)
assert "return ret < 0 ? ret : -ETIMEDOUT;" in deficit_block, (
    "an incomplete survey must remain terminally incomplete even after its "
    "explicit status report enters durable custody"
)
success_report_index = run.rindex("prepare_discovery_report(")
assert deficit_block.find("COMMAND_RADIO_ERROR") >= 0
assert deficit_guard_index < success_report_index
assert "COMMAND_OK" in run[success_report_index:], (
    "only the complete four-opportunity path may publish a successful report"
)

probe_retry = function_body(DISCOVERY, "schedule_survey_probe_retry")
assert "survey_discovery_probe_attempt_defer(" in probe_retry
assert "attempt->due_ms - retry_origin_ms" in probe_retry

core_probe_retry = function_body(
    CORE_SURVEY, "survey_discovery_probe_attempt_defer"
)
assert "node_comm_retry_backoff_ms(" in core_probe_retry
assert "NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE" in core_probe_retry
assert "attempt->retry_round++" in core_probe_retry
assert "retry_origin_ms - absolute_deadline_ms" in core_probe_retry
assert "attempt->pending = true" in core_probe_retry

core_rf_started = function_body(
    CORE_SURVEY, "survey_discovery_probe_attempt_note_rf_started"
)
assert "attempt->rf_started" in core_rf_started
assert "return PROTO_ERR_STALE" in core_rf_started
assert "attempt->pending = false" in core_rf_started

probe_send = function_body(DISCOVERY, "send_local_survey_probe")
assert "dwm3000_driver_send_frame_tracked(" in probe_send
assert "rf_started" in probe_send
assert "dwm3000_driver_send_frame_tracked" in DRIVER_HEADER

tracked_send = function_body(DRIVER, "dwm3000_driver_send_frame_tracked")
clear_index = tracked_send.index("*rf_started = false")
start_call_index = tracked_send.index("send_range_frame(")
started_index = tracked_send.index("*rf_started = true")
completion_index = tracked_send.index("wait_tx_complete(")
assert clear_index < start_call_index < started_index < completion_index, (
    "RF-start accounting must be committed after immediate TX acceptance and "
    "before completion polling"
)
legacy_send = function_body(DRIVER, "dwm3000_driver_send_frame")
assert "dwm3000_driver_send_frame_tracked(" in legacy_send

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

retry_helper = function_body(RUNTIME, "survey_rf_retry_delay_ms")
assert "app_node_comm_retry_identity_backoff_ms(" in retry_helper
assert "state->retry_round++" in retry_helper
assert "uptime_deadline_reached(now_ms, absolute_deadline_ms)" in retry_helper
assert "uptime_ms_until_deadline(now_ms, absolute_deadline_ms)" in retry_helper
assert "*delay_ms_out = remaining_ms" in retry_helper

discovery_guard_index = worker.index(
    'radio_guard_uwb_start("survey discovery")'
)
discovery_defer_index = worker.index("if (ret < 0)", discovery_guard_index)
discovery_defer_block = braced_block_at(worker, discovery_defer_index)
assert "survey_rf_retry_delay_ms(" in discovery_defer_block
assert "app_node_comm_restart_role_scan()" in discovery_defer_block
assert "REPORT_TX_RETRY_DELAY_MS" not in discovery_defer_block

pair_owner_busy_index = worker.index("if (anchor_uwb_window_active()")
pair_owner_busy_block = braced_block_at(worker, pair_owner_busy_index)
assert "schedule_pair_rf_retry(" in pair_owner_busy_block
assert "REPORT_TX_RETRY_DELAY_MS" not in pair_owner_busy_block

report_queue_index = worker.index("if (runtime_ops.report_queue_used()")
report_queue_block = braced_block_at(worker, report_queue_index)
assert "SURVEY_NON_RF_SERVICE_POLL_MS" in report_queue_block
assert "schedule_pair_rf_retry(" not in report_queue_block

pair_guard_index = worker.index(
    'radio_guard_uwb_start(as_responder ? "survey responder DS-TWR"'
)
pair_defer_index = worker.index("if (ret < 0)", pair_guard_index)
pair_defer_block = braced_block_at(worker, pair_defer_index)
assert "schedule_pair_rf_retry(" in pair_defer_block
assert "REPORT_TX_RETRY_DELAY_MS" not in pair_defer_block
assert worker.count("schedule_pair_rf_retry(") == 2
assert "REPORT_TX_RETRY_DELAY_MS" not in worker
assert worker.count("SURVEY_NON_RF_SERVICE_POLL_MS") == 3, (
    "report-stage custody retry, post-run custody retry, and report-queue pressure "
    "must use non-RF service polling without consuming radio retry rounds"
)

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
