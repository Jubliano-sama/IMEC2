#!/usr/bin/env python3
"""Guard survey-start generation ownership across queue, run, and report custody."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
DISCOVERY = (ROOT / "app/src/app_anchor_survey_discovery.c").read_text()
DISCOVERY_HEADER = (
    ROOT / "app/src/app_anchor_survey_discovery.h"
).read_text()
RUNTIME = (ROOT / "app/src/app_anchor_survey_runtime.c").read_text()
RUNTIME_HEADER = (ROOT / "app/src/app_anchor_survey_runtime.h").read_text()


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


RESULT_ENUM = "app_anchor_survey_discovery_admission"
ACCEPTED = "APP_ANCHOR_SURVEY_DISCOVERY_ACCEPTED"
DUPLICATE = "APP_ANCHOR_SURVEY_DISCOVERY_DUPLICATE"
BUSY = "APP_ANCHOR_SURVEY_DISCOVERY_BUSY"

enum_match = re.search(
    rf"enum\s+{RESULT_ENUM}\s*\{{(?P<body>.*?)\}}\s*;",
    DISCOVERY_HEADER,
    re.S,
)
assert enum_match is not None, (
    "survey-start admission needs one atomic runtime result for queued, "
    "same-generation duplicate, and different-generation busy"
)
enum_body = enum_match.group("body")
for result in (ACCEPTED, DUPLICATE, BUSY):
    assert result in enum_body, f"missing survey queue result {result}"

assert re.search(
    rf"enum\s+{RESULT_ENUM}\s+app_anchor_survey_runtime_admit_discovery\s*\(",
    RUNTIME_HEADER,
), "runtime admission API must atomically claim the survey generation"
assert "(*admit_start)" in DISCOVERY_HEADER, (
    "survey-start dispatch must ask the runtime to claim generation ownership"
)

admit = function_body(RUNTIME, "app_anchor_survey_runtime_admit_discovery")
lock_index = admit.index("k_spin_lock(&survey_lock)")
inactive_index = admit.index("!discovery_generation_active", lock_index)
claim_index = admit.index("discovery_generation_active = true", inactive_index)
same_index = admit.index("discovery_config.survey_id == survey_id", claim_index)
assert inactive_index < claim_index < same_index, (
    "an inactive runtime must claim ownership before active generations are compared"
)
assert DUPLICATE in admit[same_index:] and BUSY in admit[same_index:], (
    "same-survey copies must return duplicate and another survey must return busy"
)
assert "discovery_config = (struct survey_discovery_config)" in admit[
    claim_index:same_index
], (
    "generation admission must retain the accepted survey identity before queue commit"
)
assert ACCEPTED in admit[claim_index:same_index], (
    "only an inactive runtime may atomically accept a new generation"
)
assert admit.count("discovery_generation_active = true") == 1
assert admit.rindex("k_spin_unlock(&survey_lock") < admit.rindex("return admission"), (
    "the admission decision must be complete before releasing the generation lock"
)
assert "discovery_config.survey_id == survey_id" in admit, (
    "an active runtime must distinguish an idempotent copy from a new generation"
)

queue = function_body(RUNTIME, "app_anchor_survey_runtime_queue_discovery")
queue_lock = queue.index("k_spin_lock(&survey_lock)")
generation_check = queue.index("discovery_generation_active", queue_lock)
id_check = queue.index("discovery_config.survey_id", generation_check)
config_write_index = queue.index("discovery_config = *config", id_check)
assert generation_check < id_check < config_write_index, (
    "queue commit must reject a stale or unadmitted generation before mutation"
)
assert queue.count("discovery_config = *config") == 1
assert config_write_index < queue.index("discovery_start_ms = start_ms")
assert config_write_index < queue.index("discovery_pending = true")
assert queue.index("discovery_pending = true") < queue.rindex(
    "k_spin_unlock(&survey_lock"
), "queue commit must install config, start time, and pending state atomically"

start = function_body(DISCOVERY, "app_anchor_survey_discovery_handle_start")
report_gate_index = start.index("app_mesh_local_delivery_active(delivery)")
admit_index = start.index("discovery_ops.admit_start(config.survey_id)")
queue_index = start.index("discovery_ops.queue_start(&config, start_at_ms)")
abort_index = start.index("discovery_ops.abort_pair()")
preempt_index = start.index("discovery_ops.preempt_radio(config.survey_id)")
assert report_gate_index < admit_index < abort_index < preempt_index < queue_index, (
    "report custody and atomic runtime admission must succeed before preemption, "
    "and queue commit must follow the preemption handoff"
)
admission_prefix = start[admit_index:abort_index]
assert DUPLICATE in admission_prefix, (
    "a same-generation duplicate must terminate before abort/preempt"
)
assert re.search(
    rf"admission\s*!=\s*{ACCEPTED}", admission_prefix
), "every nonaccepted generation, including busy, must terminate before preemption"
assert admission_prefix.count("return;") >= 2, (
    "same-generation and different-generation starts must both avoid a second run"
)
assert ACCEPTED in admission_prefix, (
    "the handler must explicitly continue only for a newly admitted generation"
)

worker = function_body(RUNTIME, "survey_work_handler")
retry_stage_guard = worker.index("if (discovery_report_stage_pending)")
retry_stage_call = worker.index(
    "app_anchor_survey_discovery_retry_report()", retry_stage_guard
)
retry_stage_durable = worker.index(
    "app_anchor_survey_discovery_report_staged(", retry_stage_call
)
retry_stage_clear_pending = worker.index(
    "discovery_report_stage_pending = false", retry_stage_durable
)
retry_stage_clear_generation = worker.index(
    "discovery_generation_active = false", retry_stage_clear_pending
)
retry_stage_else = worker.index("} else {", retry_stage_clear_generation)
retry_stage_reschedule = worker.index(
    "schedule(K_MSEC(SURVEY_NON_RF_SERVICE_POLL_MS))", retry_stage_else
)
assert (
    retry_stage_guard
    < retry_stage_call
    < retry_stage_durable
    < retry_stage_clear_pending
    < retry_stage_clear_generation
    < retry_stage_else
    < retry_stage_reschedule
), (
    "a failed report stage must retain generation ownership and retry the exact "
    "report until durable custody is observable"
)
retry_stage_block = worker[retry_stage_guard:worker.index("return;", retry_stage_else)]
assert retry_stage_block.count("discovery_generation_active = false") == 1, (
    "the report-stage retry path may release the generation only after exact "
    "durable report custody is observed"
)
pending_take = worker.index("if (discovery_pending)")
pending_clear = worker.index("discovery_pending = false", pending_take)
running_set = worker.index("survey_running = true", pending_clear)
run_call = worker.index("app_anchor_survey_discovery_run(", running_set)
running_clear = worker.index("survey_running = false", run_call)
assert pending_take < pending_clear < running_set < run_call < running_clear, (
    "runtime ownership must transfer from queued to running without an inactive gap"
)
retry_requeue = worker.index("discovery_pending = true", running_set)
retry_requeue_lock = worker.rfind("k_spin_lock(&survey_lock)", running_set,
                                  retry_requeue)
retry_requeue_unlock = worker.index("k_spin_unlock(&survey_lock", retry_requeue)
retry_transition = worker[retry_requeue_lock:retry_requeue_unlock]
assert "survey_running = false" in retry_transition
assert "discovery_generation_active = false" not in retry_transition, (
    "a pre-RF retry may move running work back to pending but must retain the "
    "same active generation"
)
failsafe_report = worker.index(
    "app_anchor_survey_discovery_stage_empty_report(", run_call
)
report_durable_stage_result = worker.index(
    "report_durable = report_ret == 0", failsafe_report
)
report_staged_probe = worker.index(
    "app_anchor_survey_discovery_report_staged(", report_durable_stage_result
)
generation_pending = worker.index(
    "discovery_report_stage_pending = ret < 0 && !report_durable",
    report_staged_probe,
)
generation_assignment = worker.index(
    "discovery_generation_active = discovery_report_stage_pending",
    generation_pending,
)
assert (
    run_call
    < failsafe_report
    < report_durable_stage_result
    < report_staged_probe
    < generation_pending
    < generation_assignment
), (
    "a failed active run must prove its exact report is durable before runtime "
    "generation ownership can be released"
)
assert running_clear <= generation_assignment, (
    "runtime generation ownership must remain coherent until the run phase ends"
)
assert "discovery_generation_active = false" not in worker[
    failsafe_report:generation_assignment
], "a failed run must not unconditionally release its generation"

run = function_body(DISCOVERY, "app_anchor_survey_discovery_run")
report_stage = run.index("prepare_discovery_report(")
success_return = run.rindex("return 0;")
assert report_stage < success_return, (
    "a successful run must establish journal report custody before returning to "
    "the runtime that clears survey_running"
)

no_radio = function_body(RUNTIME, "finish_discovery_without_radio")
empty_report = no_radio.index("app_anchor_survey_discovery_stage_empty_report(")
no_radio_pending = no_radio.index(
    "discovery_report_stage_pending = report_ret < 0", empty_report
)
no_radio_generation = no_radio.index(
    "discovery_generation_active = report_ret < 0", no_radio_pending
)
assert empty_report < no_radio_pending < no_radio_generation, (
    "deadline or terminal radio deferral must keep generation ownership exactly "
    "when its explicit empty report could not enter durable custody"
)
assert "discovery_generation_active = false" not in no_radio, (
    "a failed empty-report stage must never look like a completed generation"
)

print("survey active-generation source invariants passed")
