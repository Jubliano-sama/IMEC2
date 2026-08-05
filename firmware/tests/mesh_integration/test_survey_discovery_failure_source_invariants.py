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
    r"prepare_discovery_report\s*\(\s*config->operation_generation\s*,\s*"
    r"config->survey_id\s*,\s*"
    r"entries\s*,\s*entry_count\s*,.*?COMMAND_OK\s*\)",
    run[success_report_index:],
    re.S,
), "every non-aborted completed window must stage its useful peer set"

probe_send = function_body(DISCOVERY, "send_local_survey_probe")
assert "dwm3000_driver_send_frame_tracked_until(" in probe_send
assert "rf_started" in probe_send
assert "absolute_tx_deadline_ms" in probe_send
assert "dwm3000_driver_send_frame_tracked_until" in DRIVER_HEADER

handle_start = function_body(
    DISCOVERY, "app_anchor_survey_discovery_handle_start"
)
assert "packet->flags != FLAG_DIAGNOSTIC" in handle_start, (
    "survey discovery admission must reject extra or missing mode flags even "
    "when called outside the mesh relay"
)

tracked_send = function_body(DRIVER, "dwm3000_driver_send_frame_tracked")
assert "dwm3000_driver_send_frame_tracked_until(" in tracked_send
assert "*rf_started = observation.rf_started" in tracked_send
tracked_until = function_body(
    DRIVER, "dwm3000_driver_send_frame_tracked_until"
)
gate_index = tracked_until.index("now_ms >= absolute_deadline_ms")
start_call_index = tracked_until.index("send_range_frame_until(")
completion_index = tracked_until.index("wait_tx_complete_observed(")
assert gate_index < start_call_index < completion_index, (
    "an already-expired absolute deadline must be rejected before frame staging, "
    "before completion polling"
)
assert "&result.rf_started" in tracked_until[
    start_call_index:completion_index
]
assert "&result.rf_started_at_ms" in tracked_until[
    start_call_index:completion_index
], "the low-level command boundary must return conservative RF accounting"
start_failure_index = tracked_until.index("if (ret < 0)", start_call_index)
start_failure_return = tracked_until.index("return ret", start_failure_index)
assert "*observation = result" in tracked_until[
    start_failure_index:start_failure_return
], "an ambiguous command-transfer failure must preserve possible-RF accounting"
send_until = function_body(DRIVER, "send_range_frame_until")
staging_index = send_until.index("write_tx_frame(")
device_time_index = send_until.index("dwt_readsystimestamphi32()")
deadline_gate_index = send_until.index(
    "absolute_deadline_ms - host_now_ms <= lead_ms"
)
program_index = send_until.index("dwt_setdelayedtrxtime(")
possible_index = send_until.index("*rf_start_possible = true")
rf_start_index = send_until.index("start_ret = dwt_starttx(")
assert (
    staging_index
    < device_time_index
    < deadline_gate_index
    < program_index
    < possible_index
    < rf_start_index
), (
    "deadline-bound sends must stage first, program an in-window hardware TX, "
    "and account possible RF before transferring the command"
)
assert "effective_tx_mode |= DWT_START_TX_DELAYED" in send_until
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

pair_retry = function_body(RUNTIME, "schedule_pair_rf_retry")
pair_retry_expiry = pair_retry.index("survey_rf_retry_delay_ms(")
pair_retry_capture = pair_retry.index(
    "delivery_handle = pair_start_delivery_handle", pair_retry_expiry
)
pair_retry_detach = pair_retry.index(
    "pair_start_delivery_handle = 0u", pair_retry_capture
)
pair_retry_unlock = pair_retry.index(
    "k_spin_unlock(&survey_lock, key)", pair_retry_detach
)
pair_retry_abandon = pair_retry.index(
    "app_anchor_survey_runtime_abandon_pair_start_delivery(",
    pair_retry_unlock,
)
assert (
    pair_retry_expiry < pair_retry_capture < pair_retry_detach <
    pair_retry_unlock < pair_retry_abandon
), (
    "an expired pair RF retry must detach the exact START-result handle under "
    "the lease lock and abandon it only after unlocking"
)

discovery_guard_index = worker.index(
    'radio_guard_uwb_start("survey discovery")'
)
discovery_defer_index = worker.index("if (ret < 0)", discovery_guard_index)
discovery_defer_block = braced_block_at(worker, discovery_defer_index)
assert "survey_rf_retry_delay_ms(" in discovery_defer_block
assert "app_node_comm_restart_role_scan()" in discovery_defer_block
assert "REPORT_TX_RETRY_DELAY_MS" not in discovery_defer_block
abort_clear = discovery_defer_block.index(
    "discovery_generation_active = false"
)
abort_scan = discovery_defer_block.index(
    "runtime_ops.start_uwb_scan()", abort_clear
)
assert abort_clear < abort_scan, (
    "an aborted discovery whose low-duty scan was preempted must restore that "
    "scan even though no discovery radio run reaches the normal exit"
)

pair_owner_busy_index = worker.index("if (anchor_uwb_window_active()")
pair_owner_busy_block = braced_block_at(worker, pair_owner_busy_index)
assert "schedule_pair_rf_retry(" in pair_owner_busy_block
assert "REPORT_TX_RETRY_DELAY_MS" not in pair_owner_busy_block

result_custody_index = worker.index(
    "if (app_anchor_survey_result_delivery_occupied_count() > 0u)"
)
result_custody_block = braced_block_at(worker, result_custody_index)
assert "app_anchor_survey_result_delivery_service()" in result_custody_block
assert "SURVEY_NON_RF_SERVICE_POLL_MS" in result_custody_block
assert "schedule_pair_rf_retry(" not in result_custody_block

reserve_results_index = worker.index(
    "app_node_comm_reserve_durable_reliable_uplinks(",
    result_custody_index,
)
reserve_failure_index = worker.index("if (ret < 0)", reserve_results_index)
reserve_failure_block = braced_block_at(worker, reserve_failure_index)
assert "app_anchor_survey_result_delivery_service()" in reserve_failure_block
assert "SURVEY_NON_RF_SERVICE_POLL_MS" in reserve_failure_block
assert "schedule_pair_rf_retry(" not in reserve_failure_block

pair_guard_index = worker.index(
    'radio_guard_uwb_start("survey pair DS-TWR")'
)
pair_defer_index = worker.index("if (ret < 0)", pair_guard_index)
pair_defer_block = braced_block_at(worker, pair_defer_index)
assert "schedule_pair_rf_retry(" in pair_defer_block
assert "REPORT_TX_RETRY_DELAY_MS" not in pair_defer_block
claim = re.search(
    r"survey_pair_lease_mark_running_at\s*\(\s*&pair_lease\s*,\s*"
    r"k_uptime_get_32\s*\(\s*\)\s*,\s*"
    r"&pair\s*,\s*&pair_round_id\s*\)",
    worker,
)
assert claim is not None, (
    "RUNNING must validate its execution deadline and atomically return the "
    "final pair and synchronized round"
)
assert "survey_pair_lease_mark_running(&pair_lease" not in worker
role_index = worker.index("as_responder = pair.responder_id == DEVICE_ID")
assert (
    pair_owner_busy_index
    < result_custody_index
    < reserve_results_index
    < pair_guard_index
    < pair_defer_index
    < claim.start()
    < role_index
)
assert "as_responder =" not in worker[pair_guard_index : claim.start()], (
    "the RF role must not be derived from a pre-claim lease snapshot"
)
assert "pair_round_id = pair_lease.round_id" not in worker, (
    "the round generation must come from the atomic RUNNING transition"
)
assert worker.count("schedule_pair_rf_retry(") == 2
assert "REPORT_TX_RETRY_DELAY_MS" not in worker
assert worker.count("SURVEY_NON_RF_SERVICE_POLL_MS") == 4, (
    "discovery report-stage retries plus pair-result custody and four-record "
    "admission pressure must use non-RF service polling without consuming "
    "radio retry rounds"
)

empty_report = function_body(
    DISCOVERY, "app_anchor_survey_discovery_stage_empty_report"
)
assert empty_report.count("prepare_discovery_report(") == 1
assert re.search(
    r"prepare_discovery_report\s*\(\s*"
    r"config->operation_generation\s*,\s*config->survey_id\s*,\s*"
    r"NULL\s*,\s*0u\s*,",
    empty_report,
), "the fail-safe report must contain a valid zero-peer reachability list"

failed_abandon = function_body(
    DISCOVERY, "survey_delivery_service_failed_abandon"
)
assert "survey_delivery_failed_abandon_handle" in failed_abandon
assert "app_node_comm_abandon_delivery(handle)" in failed_abandon
assert "ret != -ENOENT && ret != -EALREADY" in failed_abandon
assert failed_abandon.index("app_watchdog_stop_feeding()") < (
    failed_abandon.index("return ret")
), "unexpected orphan-cleanup failures must force bounded reset recovery"

report_retry = function_body(
    DISCOVERY, "app_anchor_survey_discovery_retry_report"
)
service_index = report_retry.index(
    "survey_delivery_service_failed_abandon()"
)
poll_index = report_retry.index("survey_delivery_poll_comm_result()")
assert service_index < poll_index, (
    "an unresolved stale submission must be abandoned before another report "
    "delivery is polled or submitted"
)
stale_abandon_index = report_retry.index(
    "app_node_comm_abandon_delivery(stale_handle)"
)
retain_index = report_retry.index(
    "survey_delivery_failed_abandon_handle = stale_handle",
    stale_abandon_index,
)
stop_index = report_retry.index(
    "app_watchdog_stop_feeding()", retain_index
)
assert stale_abandon_index < retain_index < stop_index
assert "ret != -ENOENT && ret != -EALREADY" in report_retry[
    stale_abandon_index:retain_index
], "already-terminal handles are successful cleanup, not fatal abandonment"

print("survey discovery failure source invariants passed")
