#!/usr/bin/env python3
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
RUNTIME = (ROOT / "app/src/app_anchor_survey_runtime.c").read_text(
    encoding="utf-8"
)
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
accept_go = go_handler.index("survey_pair_lease_go(", decode_go)
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

start_handler = function_body(
    RUNTIME, "app_anchor_survey_runtime_start_pair_from_command"
)
round_decode = start_handler.index("survey_round_id_extract_tlv(")
round_start = start_handler.index("survey_pair_lease_start_round(", round_decode)
assert "round_id" in start_handler[round_start : round_start + 300], (
    "START must retain the round identity installed by PREPARE"
)

local_command = function_body(COMMANDS, "anchor_handle_local_command")
late_guard = local_command.index("command_id == CMD_SURVEY_GO")
late_boundary = local_command.index(
    "packet->message_age_ms >= command_options.execute_delay_ms", late_guard
)
late_return = local_command.index("return;", late_boundary)
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

schedule_delayed = function_body(
    COMMANDS, "anchor_schedule_broadcast_command_execution"
)
assert (
    "anchor_pending_command_execution.options.execute_delay_ms = "
    "options->execute_delay_ms;"
) in schedule_delayed, "the delayed-command snapshot must retain execute_delay_ms"

restore_options = function_body(COMMANDS, "anchor_pending_options_to_gateway")
assert "options->execute_delay_ms = pending->execute_delay_ms;" in restore_options, (
    "delayed execution must restore the original execute_delay_ms policy"
)

delayed_worker = function_body(COMMANDS, "anchor_command_execute_work_handler")
age_update = delayed_worker.index("packet_age_add_elapsed(")
restore = delayed_worker.index("anchor_pending_options_to_gateway(", age_update)
execute = delayed_worker.index("anchor_finish_broadcast_command(", restore)
assert age_update < restore < execute
