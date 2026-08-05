#!/usr/bin/env python3
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
RUNTIME = (ROOT / "app/src/app_anchor_survey_runtime.c").read_text(
    encoding="utf-8"
)
LEASE = (ROOT / "src/survey_pair_lease.c").read_text(encoding="utf-8")
COMMANDS = (ROOT / "app/src/app_anchor_commands.inc").read_text(
    encoding="utf-8"
)


def function_body(source: str, name: str) -> str:
    for candidate in re.finditer(rf"\b{name}\s*\(", source):
        paren = source.index("(", candidate.start())
        depth = 0
        for index in range(paren, len(source)):
            depth += source[index] == "("
            depth -= source[index] == ")"
            if depth != 0:
                continue
            next_index = index + 1
            while next_index < len(source) and source[next_index].isspace():
                next_index += 1
            if next_index >= len(source) or source[next_index] != "{":
                break
            line_start = source.rfind("\n", 0, candidate.start()) + 1
            brace_depth = 0
            for end in range(next_index, len(source)):
                brace_depth += source[end] == "{"
                brace_depth -= source[end] == "}"
                if brace_depth == 0:
                    return source[line_start : end + 1]
            raise AssertionError(f"unterminated function {name}")
    raise AssertionError(f"missing function {name}")


# GO cannot authorize a later START because the wire GO has no START command
# identity. The lease rejects that newer command before replacing start_id or
# clearing/reusing any delivery barrier.
lease_start = function_body(LEASE, "survey_pair_lease_start_round_bound")
go_bound_reject = lease_start.index(
    "lease->round_id != SURVEY_LEGACY_ROUND_ID"
)
go_released = lease_start.index("lease->go_released", go_bound_reject)
stale_return = lease_start.index(
    "return SURVEY_PAIR_LEASE_STALE;", go_released
)
replace_start = lease_start.index("lease->start_id = *control_id;", stale_return)
assert go_bound_reject < go_released < stale_return < replace_start


delivery_gate = function_body(RUNTIME, "pair_start_delivery_ready")
delivered = delivery_gate.index(
    "event.reason == NODE_COMM_TERMINAL_DELIVERED"
)
release = delivery_gate.index("survey_pair_lease_release_start(", delivered)
ready_after_release = delivery_gate.index(
    "ready = delivery_confirmed &&", release
)
round_barrier = delivery_gate.index(
    "survey_pair_lease_ready_snapshot(&pair_lease, NULL)", ready_after_release
)
failure_else = delivery_gate.index("} else {", round_barrier)
failure_cancel = delivery_gate.index(
    "pair_start_pending = false", failure_else
)
assert delivered < release < ready_after_release < round_barrier < failure_else
assert "pair_start_pending = false" not in delivery_gate[delivered:failure_else], (
    "a delivered START result must retain START_PENDING while a nonzero "
    "round waits for GO"
)
assert failure_else < failure_cancel, (
    "only a failed START-result delivery may cancel the pending start here"
)
assert "waiting for matching round GO" in delivery_gate

worker = function_body(RUNTIME, "survey_work_handler")
barrier_return = worker.index("if (!pair_start_delivery_ready())")
first_pending_clear = worker.index("pair_start_pending = false", barrier_return)
assert "return;" in worker[barrier_return:first_pending_clear], (
    "the survey worker must return without clearing START_PENDING while either "
    "START delivery or GO is outstanding"
)

go_handler = function_body(
    RUNTIME, "app_anchor_survey_runtime_go_round_from_command"
)
decode_go = go_handler.index("survey_round_go_from_tlvs(")
accept_go = go_handler.index("survey_pair_lease_go_until_bound(", decode_go)
ready_go = go_handler.index(
    "survey_pair_lease_ready_snapshot(&pair_lease, NULL)", accept_go
)
schedule_guard = go_handler.index("if (ready)", ready_go)
schedule = go_handler.index("schedule(K_NO_WAIT)", schedule_guard)
assert decode_go < accept_go < ready_go < schedule_guard < schedule
assert "go.survey_id" in go_handler[accept_go:ready_go]
assert "go.round_id" in go_handler[accept_go:ready_go], (
    "GO must release only the matching survey and nonzero round"
)
assert "execution_deadline_ms" in go_handler[decode_go:ready_go]
assert "uptime_deadline_reached" in go_handler[decode_go:accept_go], (
    "GO must reject a closed local execution deadline before releasing RF"
)
assert "execution_deadline_ms == 0u" not in go_handler, (
    "a wrapped zero GO deadline is valid and must use the lease's explicit "
    "release state rather than a value sentinel"
)
assert "schedule_ret = schedule(K_NO_WAIT)" in go_handler[schedule_guard:], (
    "GO acceptance must observe work-queue admission failure"
)
go_schedule_failure = go_handler.index("if (schedule_ret < 0)", schedule)
go_revoke = go_handler.index(
    "survey_pair_lease_revoke_go_bound(", go_schedule_failure
)
go_failure_return = go_handler.index("return schedule_ret;", go_revoke)
go_failure_block = go_handler[go_schedule_failure:go_failure_return]
assert "decision == SURVEY_PAIR_LEASE_ACCEPTED" in go_failure_block, (
    "a failed new GO admission must revoke only the lease transition made by "
    "this call; an exact duplicate must retain its prior owner"
)
assert schedule < go_schedule_failure < go_revoke < go_failure_return

start_handler = function_body(
    RUNTIME, "app_anchor_survey_runtime_start_pair_from_command"
)
round_decode = start_handler.index("survey_round_id_extract_tlv(")
round_start = start_handler.index(
    "survey_pair_lease_start_round_bound(", round_decode
)
assert "round_id" in start_handler[round_start : round_start + 300], (
    "START must retain the round identity installed by PREPARE"
)
superseded_admission = start_handler.index(
    "decision != SURVEY_PAIR_LEASE_SUPERSEDED", round_start
)
superseded_branch = start_handler.index(
    "decision == SURVEY_PAIR_LEASE_SUPERSEDED", superseded_admission
)
capture_old_delivery = start_handler.index(
    "superseded_delivery_handle = pair_start_delivery_handle",
    superseded_branch,
)
detach_old_delivery = start_handler.index(
    "pair_start_delivery_handle = 0u", capture_old_delivery
)
unlock_after_detach = start_handler.index(
    "k_spin_unlock(&survey_lock, key)", detach_old_delivery
)
abandon_old_delivery = start_handler.index(
    "abandon_pair_start_delivery(", unlock_after_detach
)
replacement_section = start_handler[superseded_branch:unlock_after_detach]
assert (
    superseded_admission
    < superseded_branch
    < capture_old_delivery
    < detach_old_delivery
    < unlock_after_detach
    < abandon_old_delivery
), "newer START identity must detach old result custody atomically before abandon"
assert "decision == SURVEY_PAIR_LEASE_DUPLICATE" not in replacement_section, (
    "an exact START duplicate must keep its already-bound result custody"
)

delivery_bind = function_body(
    RUNTIME, "app_anchor_survey_runtime_bind_pair_start_delivery"
)
assert "pair_lease.start_id.session_id == command->session_id" in delivery_bind
assert "pair_lease.start_id.command_seq == command->seq" in delivery_bind
assert "pair_start_delivery_handle == 0u" in delivery_bind, (
    "the superseding START result may bind only after old custody is detached"
)
primary_schedule = delivery_bind.index("ret = schedule(K_NO_WAIT)")
fallback_identity = delivery_bind.index(
    "pair_start_kick_id = control_id", primary_schedule
)
fallback_handle = delivery_bind.index(
    "pair_start_kick_delivery_handle = delivery_handle", fallback_identity
)
fallback_schedule = delivery_bind.index(
    "k_work_reschedule(&pair_start_kick_work", fallback_handle
)
fallback_success = delivery_bind.index(
    "if (fallback_ret >= 0)", fallback_schedule
)
fallback_rollback = delivery_bind.index(
    "rolled_back = survey_pair_lease_abort(&pair_lease)", fallback_success
)
fallback_error = delivery_bind.rindex("return fallback_ret;")
assert (
    primary_schedule
    < fallback_identity
    < fallback_handle
    < fallback_schedule
    < fallback_success
    < fallback_rollback
    < fallback_error
), (
    "a rejected private-queue bind must publish an exact identity-bound "
    "system-workqueue owner, and total scheduling failure must roll back the "
    "pair before returning an error to the command-result owner"
)
assert "return 0;" in delivery_bind[fallback_success:fallback_rollback], (
    "bind may report success only after the fallback work owner is live"
)
assert "app_watchdog_stop_feeding()" in delivery_bind[
    fallback_rollback:fallback_error
], "a rollback race must fail closed through watchdog recovery"

bind_fallback = function_body(RUNTIME, "pair_start_kick_work_handler")
for exact_guard in (
    "pair_start_kick_delivery_handle == delivery_handle",
    "pair_start_kick_id.session_id == control_id->session_id",
    "pair_start_kick_id.command_seq == control_id->command_seq",
    "pair_start_delivery_handle == delivery_handle",
    "pair_lease.start_id.session_id == control_id->session_id",
    "pair_lease.start_id.command_seq == control_id->command_seq",
):
    assert exact_guard in RUNTIME, f"missing START bind fallback guard: {exact_guard}"
assert "uptime_deadline_reached(" in bind_fallback
assert "ret = schedule(K_NO_WAIT)" in bind_fallback
assert "k_work_reschedule(&pair_start_kick_work" in bind_fallback
assert (
    "app_anchor_survey_runtime_abandon_pair_start_delivery("
    in bind_fallback
)
assert "app_watchdog_stop_feeding()" in bind_fallback, (
    "the bounded fallback must stop watchdog feeds if it loses its own sole "
    "reschedule edge"
)

stale_terminal = function_body(RUNTIME, "pair_start_delivery_ready")
for exact_identity_guard in (
    "pair_start_delivery_handle == delivery_handle",
    "pair_lease.start_id.session_id == control_id.session_id",
    "pair_lease.start_id.command_seq == control_id.command_seq",
):
    assert exact_identity_guard in stale_terminal
assert stale_terminal.index("still_current =") < stale_terminal.index(
    "survey_pair_lease_release_start("
), "a terminal event must prove exact current custody before releasing START"

local_command = function_body(COMMANDS, "anchor_handle_local_command_locked")
late_guard = local_command.index("command_id == CMD_SURVEY_GO")
late_boundary = local_command.index(
    "packet->message_age_ms >= command_options.execute_delay_ms", late_guard
)
late_return = local_command.index("return ret == 0 ? 1 : 0;", late_boundary)
remaining_delay = local_command.index(
    "gateway_command_execute_delay_remaining_ms", late_return
)
ordinary_finish = local_command.index(
    "anchor_finish_broadcast_command(", remaining_delay
)
guard = local_command[late_guard:late_return]
assert "command_options.execute_delay_ms == 0u" in guard
assert late_guard < late_boundary < late_return < remaining_delay < ordinary_finish, (
    "a GO first received at or after its execute delay must be rejected before "
    "the generic path can execute it immediately"
)
schedule = local_command.index(
    "anchor_schedule_broadcast_command_execution(", remaining_delay
)
schedule_failure = local_command.index("if (ret < 0)", schedule)
schedule_failure_return = local_command.index(
    "return ret == 0 ? 1 : 0;", schedule_failure
)
schedule_success_return = local_command.index(
    "return 0;", schedule_failure_return
)
assert schedule < schedule_failure < schedule_failure_return < schedule_success_return
assert "anchor_commit_broadcast_command_replay(" not in local_command[
    schedule_failure_return:schedule_success_return
], (
    "scheduler custody alone must not commit semantic replay; an exact copy "
    "remains locally retryable until delayed execution has a terminal owner"
)

schedule_delayed = function_body(
    COMMANDS, "anchor_schedule_broadcast_command_execution"
)
assert (
    "anchor_pending_command_execution.options.execute_delay_ms = "
    "options->execute_delay_ms;"
) in schedule_delayed, "the delayed-command snapshot must retain execute_delay_ms"
assert "anchor_pending_command_execution.execute_at_ms =" in schedule_delayed
assert "anchor_pending_command_execution.execute_deadline_ms =" in schedule_delayed
assert "(void)mesh_route_work_reschedule(" not in schedule_delayed, (
    "a failed delayed-command work admission must not leave inert active state"
)
schedule_submit = schedule_delayed.index(
    "ret = mesh_route_work_reschedule("
)
schedule_submit_failure = schedule_delayed.index("if (ret < 0)", schedule_submit)
schedule_submit_return = schedule_delayed.index(
    "return ret;", schedule_submit_failure
)
schedule_failure_block = schedule_delayed[
    schedule_submit_failure:schedule_submit_return
]
assert (
    "memset(&anchor_pending_command_execution" in schedule_failure_block or
    "anchor_pending_command_execution.active = false" in schedule_failure_block
), "failed work admission must roll back the pending-command owner"

restore_options = function_body(COMMANDS, "anchor_pending_options_to_gateway")
assert "options->execute_delay_ms = pending->execute_delay_ms;" in restore_options, (
    "delayed execution must restore the original execute_delay_ms policy"
)

delayed_worker = function_body(
    COMMANDS, "anchor_command_execute_work_handler_locked"
)
age_update = delayed_worker.index("packet_age_add_elapsed(")
restore = delayed_worker.index("anchor_pending_options_to_gateway(", age_update)
closed_go = delayed_worker.index(
    "uptime_deadline_reached(now_ms,\n"
    "                                    pending.execute_deadline_ms)",
    restore,
)
execute = delayed_worker.index("anchor_finish_broadcast_command(", restore)
assert age_update < restore < closed_go < execute
assert "pending.execute_at_ms == 0u" not in delayed_worker
assert "pending.execute_deadline_ms == 0u" not in delayed_worker, (
    "the active delayed-command owner must retain valid execute/deadline "
    "timestamps that wrap to zero"
)
assert "terminal_without_execution = true" in delayed_worker[closed_go:execute]
execute_failure = delayed_worker.index("if (ret < 0)", execute)
result_owner = delayed_worker.index(
    "anchor_collection_result_matches_broadcast_command(",
    execute_failure,
)
execution_clear = delayed_worker.index(
    "memset(&anchor_pending_command_execution", result_owner
)
owner_return = delayed_worker.index("return;", execution_clear)
retry_submit = delayed_worker.index(
    "anchor_command_execute_reschedule(", owner_return
)
assert (
    execute < execute_failure < result_owner <
    execution_clear < owner_return < retry_submit
), (
    "a delayed collection result owner must retire the execution owner before "
    "generic retry can repeat command side effects"
)
assert "(void)mesh_route_work_reschedule(" not in delayed_worker, (
    "every delayed-command retry must observe scheduling failure"
)
delayed_reschedule = function_body(
    COMMANDS, "anchor_command_execute_reschedule"
)
normal_admission = delayed_reschedule.index("mesh_route_work_reschedule(")
paused_fallback = delayed_reschedule.index(
    "if (ret == -ESHUTDOWN)", normal_admission
)
owner_admission = delayed_reschedule.index(
    "mesh_route_owner_work_reschedule(", paused_fallback
)
watchdog_recovery = delayed_reschedule.index(
    "app_watchdog_stop_feeding()", owner_admission
)
assert normal_admission < paused_fallback < owner_admission < watchdog_recovery
side_effect_recovery = delayed_worker.index(
    "if (pending.side_effect_completed ||"
)
first_execute = delayed_worker.index(
    "anchor_finish_broadcast_command("
)
mark_side_effect = delayed_worker.index(
    "anchor_pending_command_execution.side_effect_completed = true",
    first_execute,
)
retry_after_side_effect = delayed_worker.index(
    "anchor_command_execute_reschedule(", mark_side_effect
)
assert side_effect_recovery < first_execute < mark_side_effect < (
    retry_after_side_effect
), "a completed SURVEY_GO side effect must retry replay without re-execution"

command_side_effects = function_body(
    COMMANDS, "anchor_execute_command_side_effects"
)
abort_branch = command_side_effects[
    command_side_effects.index("command_id == CMD_SURVEY_ABORT") :
]
round_extract = abort_branch.index("survey_round_id_extract_tlv(")
commitment_extract = abort_branch.index(
    "survey_round_commitment_extract_tlv(", round_extract
)
round_abort = abort_branch.index(
    "app_anchor_survey_runtime_abort_pair_matching_round(",
    commitment_extract,
)
assert round_extract < commitment_extract < round_abort, (
    "round cleanup must decode the exact commitment before mutating the lease"
)
assert "pair.operation_generation != 0u" in abort_branch[:round_abort]
assert "app_anchor_survey_runtime_abort_pair_matching(" not in abort_branch, (
    "a broad pair-only ABORT could cancel a newer incarnation"
)

round_abort_runtime = function_body(
    RUNTIME, "app_anchor_survey_runtime_abort_pair_matching_round"
)
assert "pair->operation_generation == 0u" in round_abort_runtime
assert "survey_pair_lease_abort_matching_round_bound(" in round_abort_runtime

go_scheduler = function_body(
    COMMANDS, "anchor_schedule_broadcast_command_execution"
)
decode_command = go_scheduler.index("gateway_command_extract_id(")
decode_go = go_scheduler.index("survey_round_go_from_tlvs(", decode_command)
session_gate = go_scheduler.index(
    "survey_operation_session_id(", decode_go
)
generation_gate = go_scheduler.index(
    "app_anchor_survey_runtime_operation_generation_active(", session_gate
)
preempt_gate = go_scheduler.index(
    "if (anchor_pending_command_execution.active)", generation_gate
)
park_old = go_scheduler.index(
    "anchor_deferred_command_execution = previous", preempt_gate
)
publish_go = go_scheduler.index(
    "anchor_pending_command_execution.command = *packet", park_old
)
assert (
    decode_command
    < decode_go
    < session_gate
    < generation_gate
    < preempt_gate
    < park_old
    < publish_go
), (
    "a valid current-generation GO must preempt one generic delayed command "
    "only after its complete identity has been validated"
)

delayed_wrapper = function_body(
    COMMANDS, "anchor_command_execute_work_handler"
)
run_primary = delayed_wrapper.index(
    "anchor_command_execute_work_handler_locked(work)"
)
promote_deferred = delayed_wrapper.index(
    "anchor_deferred_command_execution.active", run_primary
)
restore_deferred = delayed_wrapper.index(
    "anchor_pending_command_execution =",
    promote_deferred,
)
reschedule_deferred = delayed_wrapper.index(
    "anchor_command_execute_reschedule(delay_ms)", restore_deferred
)
assert run_primary < promote_deferred < restore_deferred < reschedule_deferred, (
    "GO priority must preserve and resume the displaced command at its "
    "original absolute execution instant"
)
