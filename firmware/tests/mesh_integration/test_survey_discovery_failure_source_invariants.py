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
assert run.count("send_local_survey_probe(") == 1
assert "dwm3000_driver_send_frame(" not in run
assert "while (deferred_mask != 0u" not in run
assert "schedule_survey_probe_retry(" not in run
assert "survey_discovery_probe_real_attempt_count(" not in run
assert "COMMAND_RADIO_ERROR" not in run
assert re.search(
    r"for\s*\(uint8_t opportunity\s*=\s*0u;\s*"
    r"opportunity\s*<\s*config->round_count",
    run,
), "the runtime profile must own the exact announce/listen round count"
send_index = run.index("send_local_survey_probe(")
ensure_index = run.index("dwm3000_driver_ensure_wake_mode(", send_index)
listen_index = run.index("receive_survey_probes_until(", ensure_index)
assert send_index < ensure_index < listen_index
abort_index = run.index("if (abort_requested())")
abort_return = run.index("return -ECANCELED", abort_index)
success_report_index = run.rindex("prepare_discovery_report(")
assert abort_index < abort_return < success_report_index
assert re.search(
    r"prepare_discovery_report\s*\(\s*config->survey_id\s*,\s*"
    r"entries\s*,\s*entry_count\s*,.*?COMMAND_OK\s*\)",
    run[success_report_index:],
    re.S,
), "every non-aborted completed window must stage its useful peer set"

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

discovery_claim_index = worker.index("app_mesh_radio_owner_try_claim(")
discovery_claim_end = worker.index("if (ret < 0)", discovery_claim_index)
discovery_claim = worker[discovery_claim_index:discovery_claim_end]
assert "APP_MESH_RADIO_CLIENT_SURVEY" in discovery_claim
assert '"survey discovery"' in discovery_claim
assert "&radio_lease" in discovery_claim
discovery_defer_index = discovery_claim_end
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

pair_claim_index = worker.index(
    "app_mesh_radio_owner_try_claim(", discovery_claim_end
)
pair_claim_end = worker.index("if (ret < 0)", pair_claim_index)
pair_claim = worker[pair_claim_index:pair_claim_end]
assert "APP_MESH_RADIO_CLIENT_SURVEY" in pair_claim
assert '"survey pair DS-TWR"' in pair_claim
assert "&radio_lease" in pair_claim
pair_defer_index = pair_claim_end
pair_defer_block = braced_block_at(worker, pair_defer_index)
assert "schedule_pair_rf_retry(" in pair_defer_block
assert "REPORT_TX_RETRY_DELAY_MS" not in pair_defer_block
claim = re.search(
    r"survey_pair_lease_mark_running\s*\(\s*&pair_lease\s*,\s*"
    r"&pair\s*,\s*&pair_round_id\s*\)",
    worker,
)
assert claim is not None, (
    "RUNNING must atomically return the final pair and synchronized round"
)
role_index = worker.index("as_responder = pair.responder_id == DEVICE_ID")
assert pair_claim_index < pair_defer_index < claim.start() < role_index
assert "as_responder =" not in worker[pair_claim_index : claim.start()], (
    "the RF role must not be derived from a pre-claim lease snapshot"
)
assert "pair_round_id = pair_lease.round_id" not in worker, (
    "the round generation must come from the atomic RUNNING transition"
)
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
