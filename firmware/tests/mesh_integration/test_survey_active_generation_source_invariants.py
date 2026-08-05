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
PERSISTENCE = (ROOT / "app/src/app_mesh_persistence.c").read_text()
PAIR_LEASE = (ROOT / "src/survey_pair_lease.c").read_text()


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
generation_mutex = admit.index(
    "k_mutex_lock(&survey_generation_admission_mutex, K_FOREVER)"
)
generation_admit = admit.index(
    "survey_generation_admit_locked(", generation_mutex
)
lock_index = admit.index("k_spin_lock(&survey_lock)")
inactive_index = admit.index("!discovery_generation_active", lock_index)
claim_index = admit.index("discovery_generation_active = true", inactive_index)
same_generation_index = admit.index(
    "discovery_config.operation_generation ==", claim_index
)
same_index = admit.index(
    "discovery_config.survey_id == config->survey_id", same_generation_index
)
assert inactive_index < claim_index < same_index, (
    "an inactive runtime must claim ownership before active generations are compared"
)
assert generation_mutex < generation_admit < lock_index < claim_index, (
    "durable generation advancement and successor-config installation must "
    "share one admission mutex"
)
accepted_guard = admit[lock_index:claim_index]
assert "advanced ||" in accepted_guard and "!survey_running" in accepted_guard, (
    "a durably advanced successor must be retained even while the superseded "
    "survey worker is still running"
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
mutex_unlock = admit.rindex(
    "k_mutex_unlock(&survey_generation_admission_mutex)"
)
assert (
    admit.rindex("k_spin_unlock(&survey_lock") <
    mutex_unlock <
    admit.rindex("return admission")
), (
    "the admission decision must be complete before releasing the generation lock"
)
assert "discovery_config.operation_generation ==" in admit
assert "discovery_config.survey_id == config->survey_id" in admit, (
    "an active runtime must distinguish an idempotent copy from a new generation"
)

# Operation generations deliberately never wrap. The persistent producer must
# fail at UINT64_MAX and skip low-32-bit zero session IDs before raw ordering in
# every consumer can be considered sound.
reserve_generation = function_body(
    PERSISTENCE, "app_mesh_persistence_reserve_gateway_survey_generation"
)
overflow_gate = reserve_generation.index(
    "snapshot.generation == UINT64_MAX"
)
overflow_error = reserve_generation.index("ret = -EOVERFLOW", overflow_gate)
increment = reserve_generation.index(
    "next_generation = snapshot.generation + 1u", overflow_error
)
reserved_session = reserve_generation.index(
    "(uint32_t)next_generation == 0u", increment
)
reserved_skip = reserve_generation.index(
    "next_generation++", reserved_session
)
assert (
    overflow_gate < overflow_error < increment < reserved_session <
    reserved_skip
), (
    "the sole generation producer must prove no-wrap monotonic ordering and "
    "skip reserved zero session IDs"
)
advance_generation = function_body(
    PERSISTENCE, "app_mesh_persistence_advance_anchor_survey_generation"
)
assert "generation < snapshot.generation" in advance_generation
admit_locked = function_body(RUNTIME, "survey_generation_admit_locked")
assert "generation < survey_generation_high_watermark" in admit_locked
report_custody = function_body(
    DISCOVERY, "app_anchor_survey_discovery_report_custody_status"
)
assert report_custody.count(
    "operation_generation == owned_generation"
) == 2
assert "app_mesh_local_delivery_discard_failed(" not in report_custody
prepare_lease = function_body(
    PAIR_LEASE, "survey_pair_lease_prepare_round_bound"
)
assert "pair->operation_generation <" in prepare_lease
assert "pair->operation_generation <=" in prepare_lease, (
    "raw consumer ordering is valid only while the persistent producer "
    "continues to reject generation wrap"
)

queue = function_body(RUNTIME, "app_anchor_survey_runtime_queue_discovery")
assert re.search(
    r"int\s+app_anchor_survey_runtime_queue_discovery\s*\(",
    RUNTIME_HEADER,
), "queue admission must report whether it acquired a delayed-work owner"
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
commit_unlock = queue.index("k_spin_unlock(&survey_lock", config_write_index)
schedule_index = queue.index("ret = schedule(K_MSEC(delay_ms))", commit_unlock)
assert queue.index("discovery_pending = true") < commit_unlock < schedule_index, (
    "queue commit must install config, start time, and pending state atomically "
    "before it asks Zephyr for the delayed-work owner"
)
assert "atomic_set(&abort_requested, 0)" not in queue, (
    "queueing a successor while old RF is running must retain the abort latch "
    "until the private worker takes the successor"
)

# Regression: inject a negative scheduler result immediately after admission.
# The exact pending generation must be rolled back under the runtime lock, and
# ordinary scan ownership must be restored before the error is returned.
schedule_failure = queue.index("if (ret >= 0)", schedule_index)
rollback_lock = queue.index("k_spin_lock(&survey_lock)", schedule_failure)
rollback_generation = queue.index(
    "discovery_generation_active && discovery_pending", rollback_lock
)
rollback_identity = queue.index(
    "discovery_config.operation_generation ==", rollback_generation
)
assert "config->operation_generation" in queue[
    rollback_identity:queue.index("discovery_pending = false", rollback_identity)
]
rollback_pending = queue.index("discovery_pending = false", rollback_identity)
rollback_active = queue.index(
    "discovery_generation_active = false", rollback_pending
)
rollback_unlock = queue.index("k_spin_unlock(&survey_lock", rollback_active)
restart_role_scan = queue.index(
    "app_node_comm_restart_role_scan()", rollback_unlock
)
restart_uwb_scan = queue.index("runtime_ops.start_uwb_scan()", restart_role_scan)
error_return = queue.rindex("return ret")
assert (
    schedule_index
    < schedule_failure
    < rollback_lock
    < rollback_generation
    < rollback_identity
    < rollback_pending
    < rollback_active
    < rollback_unlock
    < restart_role_scan
    < restart_uwb_scan
    < error_return
), (
    "a scheduler rejection after admission must release exact generation "
    "ownership and restore both normal scan owners before returning"
)
assert "discovery_config = (struct survey_discovery_config) {0}" in queue[
    rollback_identity:rollback_unlock
], "rollback must erase the rejected generation identity"
assert "discovery_start_ms = 0u" in queue[
    rollback_identity:rollback_unlock
], "rollback must erase the rejected generation timing"
assert queue.index("survey_id = config->survey_id") < queue_lock, (
    "the rollback identity must be frozen before the retained config can be "
    "cleared"
)

start = function_body(DISCOVERY, "app_anchor_survey_discovery_handle_start")
admit_index = start.index("discovery_ops.admit_start(&config)")
queue_index = start.index("ret = discovery_ops.queue_start(&config")
abort_index = start.index("discovery_ops.abort_pair()")
preempt_index = start.index("discovery_ops.preempt_radio(config.survey_id)")
assert admit_index < abort_index < preempt_index < queue_index, (
    "durable runtime admission must precede preemption, and queue commit must "
    "follow the preemption handoff"
)
assert "app_anchor_survey_discovery_report_custody_status(" not in start, (
    "the runtime generation admission mutex owns the report-custody check, so "
    "START parsing cannot race durable generation advancement"
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
queue_call = start[queue_index:start.index(";", queue_index) + 1]
assert "start_at_ms" in queue_call and "schedule_delay_ms" in queue_call, (
    "the runtime must receive both the immutable start and its worker delay so "
    "pending-state commit and scheduler admission remain one transaction"
)
queue_failure = start.index("if (ret < 0)", queue_index)
queue_failure_return = start.index("return;", queue_failure)
scheduled_marker = start.index("DBG_SURVEY_DISCOVERY_SCHEDULED", queue_index)
assert queue_index < queue_failure < queue_failure_return < scheduled_marker, (
    "a failure after admission must terminate before claiming the discovery "
    "was scheduled"
)
assert "schedule_work_ms(schedule_delay_ms)" not in start, (
    "the discovery handler must not split pending-state commit from scheduler "
    "admission"
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
retry_stage_exact_generation = worker.rfind(
    "discovery_config.operation_generation ==",
    retry_stage_durable,
    retry_stage_clear_pending,
)
assert retry_stage_exact_generation >= retry_stage_durable, (
    "old report completion must match the full generation before it can "
    "release a successor that intentionally reuses the survey ID"
)
retry_stage_else = worker.index("} else {", retry_stage_clear_generation)
retry_stage_reschedule = worker.index(
    "schedule_discovery_if_current(",
    retry_stage_else,
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
custody_check = worker.index(
    "app_anchor_survey_discovery_report_custody_status(",
    pending_take,
)
already_durable = worker.index("if (ret == -EALREADY)", custody_check)
already_exact_generation = worker.index(
    "discovery_config.operation_generation ==",
    already_durable,
)
already_pending_clear = worker.index(
    "discovery_pending = false", already_exact_generation
)
already_generation_clear = worker.index(
    "discovery_generation_active = false", already_pending_clear
)
conflict = worker.index("if (ret < 0)", already_generation_clear)
conflict_pending_clear = worker.index(
    "discovery_pending = false", conflict
)
conflict_generation_clear = worker.index(
    "discovery_generation_active = false", conflict_pending_clear
)
conflict_return = worker.index("return;", conflict_generation_clear)
pending_clear = worker.index(
    "discovery_pending = false", conflict_return
)
running_set = worker.index("survey_running = true", pending_clear)
abort_clear = worker.index(
    "atomic_set(&abort_requested, 0)", running_set
)
run_call = worker.index("app_anchor_survey_discovery_run(", running_set)
running_clear = worker.index("survey_running = false", run_call)
assert (
    pending_take < custody_check < already_durable <
    already_exact_generation < already_pending_clear <
    already_generation_clear < conflict < conflict_pending_clear <
    conflict_generation_clear < conflict_return < pending_clear < running_set <
    abort_clear < run_call < running_clear
), (
    "the single private worker must recognize same-generation durable custody, "
    "reject different-generation custody without deleting it, and only then "
    "take an unblocked pending generation"
)
conflict_block = worker[conflict:conflict_return]
assert "DBG_SURVEY_DISCOVERY_REPORT_CUSTODY_BLOCKED" in conflict_block
for destructive_api in (
    "app_node_comm_abandon_delivery(",
    "app_mesh_local_delivery_note_failed(",
    "app_mesh_local_delivery_discard_failed(",
):
    assert destructive_api not in conflict_block, (
        "a pending G+1 worker must not mutate durable report G custody"
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
assert "schedule_pair_unless_discovery_pending(" in worker, (
    "stale pair backpressure must not overwrite the queued successor's work deadline"
)

prepare = function_body(
    RUNTIME, "app_anchor_survey_runtime_handle_pair_prepare"
)
prepare_mutex = prepare.index(
    "k_mutex_lock(&survey_generation_admission_mutex, K_FOREVER)"
)
prepare_copy = prepare.index("preflight_lease = pair_lease", prepare_mutex)
prepare_dry_run = prepare.index(
    "survey_pair_lease_prepare_round_bound(", prepare_copy
)
prepare_invalid = prepare.index(
    "SURVEY_PAIR_LEASE_INVALID_ARGUMENT", prepare_dry_run
)
prepare_generation = prepare.index(
    "survey_generation_admit_locked(", prepare_invalid
)
prepare_commit = prepare.index(
    "survey_pair_lease_prepare_round_bound(", prepare_generation
)
prepare_schedule = prepare.index(
    "k_work_reschedule(", prepare_commit
)
prepare_mutex_unlock = prepare.index(
    "k_mutex_unlock(&survey_generation_admission_mutex)", prepare_schedule
)
assert (
    prepare_mutex < prepare_copy < prepare_dry_run < prepare_invalid <
    prepare_generation < prepare_commit < prepare_schedule <
    prepare_mutex_unlock
), (
    "pair PREPARE must reject malformed lease semantics before persistent "
    "generation supersession, then publish its exact lease and expiry owner "
    "under the same generation mutex"
)
assert "atomic_set(&abort_requested, 0)" not in prepare, (
    "PREPARE ingress must not clear cancellation while superseded RF is exiting"
)
start_pair = function_body(
    RUNTIME, "app_anchor_survey_runtime_start_pair_from_command"
)
assert "atomic_set(&abort_requested, 0)" not in start_pair, (
    "START ingress must not clear cancellation while superseded RF is exiting"
)
pair_snapshot = worker.index(
    "survey_pair_lease_ready_snapshot(&pair_lease, &pair)"
)
pair_worker_abort_clear = worker.index(
    "atomic_set(&abort_requested, 0)", pair_snapshot
)
pair_worker_radio = worker.index(
    'radio_guard_uwb_start("survey pair DS-TWR")', pair_worker_abort_clear
)
assert pair_snapshot < pair_worker_abort_clear < pair_worker_radio, (
    "only the serialized private worker may clear the predecessor abort latch "
    "after taking the exact successor pair and before RF admission"
)
abandon_start = function_body(
    RUNTIME, "app_anchor_survey_runtime_abandon_pair_start_delivery"
)
abandon_call = abandon_start.index("app_node_comm_abandon_delivery(")
abandon_failure = abandon_start.index(
    "ret < 0 && ret != -ENOENT && ret != -EALREADY", abandon_call
)
retain_handle = abandon_start.index(
    "pair_start_failed_abandon_handle = delivery_handle", abandon_failure
)
stop_watchdog = abandon_start.index(
    "app_watchdog_stop_feeding()", retain_handle
)
failure_return = abandon_start.index("return ret;", stop_watchdog)
assert (
    abandon_call < abandon_failure < retain_handle < stop_watchdog <
    failure_return
), (
    "a failed START-result abandonment must retain its exact handle and fail "
    "closed instead of erasing live delivery custody"
)
assert "pair_start_failed_abandon_handle != 0u" in admit_locked, (
    "no later generation may proceed while failed START-result abandonment "
    "custody remains live"
)

assert re.search(
    r"int\s+app_anchor_survey_discovery_report_custody_status\s*\(",
    DISCOVERY_HEADER,
), "generation admission needs a read-only exact report-custody gate"
custody_status = function_body(
    DISCOVERY,
    "app_anchor_survey_discovery_report_custody_status",
)
assert "app_mesh_local_delivery_occupied(delivery)" in custody_status
assert "operation_generation == owned_generation ?" in custody_status
for destructive_api in (
    "app_node_comm_abandon_delivery(",
    "app_mesh_local_delivery_note_failed(",
    "app_mesh_local_delivery_discard_failed(",
    "app_mesh_local_delivery_cleanup_ack(",
):
    assert destructive_api not in custody_status

generation_custody = admit_locked.index(
    "app_anchor_survey_discovery_report_custody_status("
)
generation_persist = admit_locked.index(
    "app_mesh_persistence_advance_anchor_survey_generation("
)
assert generation_custody < generation_persist, (
    "restored or live report G custody must reject G+1 before its durable "
    "generation cursor advances"
)

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
no_radio_role_scan = no_radio.index(
    "app_node_comm_restart_role_scan()", no_radio_generation
)
no_radio_uwb_scan = no_radio.index(
    "runtime_ops.start_uwb_scan()", no_radio_role_scan
)
assert empty_report < no_radio_pending < no_radio_generation, (
    "deadline or terminal radio deferral must keep generation ownership exactly "
    "when its explicit empty report could not enter durable custody"
)
assert no_radio_generation < no_radio_role_scan < no_radio_uwb_scan, (
    "every terminal discovery path must restore both normal scan owners after "
    "the survey generation has released radio ownership"
)
retired_before_report = no_radio.index("if (!current)")
conditional_release = no_radio.index(
    "discovery_generation_active = false", retired_before_report
)
assert retired_before_report < conditional_release < empty_report, (
    "an aborted old run may release only its matching generation before any "
    "empty report is staged; a queued successor remains untouched"
)

print("survey active-generation source invariants passed")
