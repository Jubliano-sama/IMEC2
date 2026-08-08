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
            if depth:
                continue
            brace = index + 1
            while brace < len(source) and source[brace].isspace():
                brace += 1
            if brace >= len(source) or source[brace] != "{":
                break
            line_start = source.rfind("\n", 0, candidate.start()) + 1
            brace_depth = 0
            for end in range(brace, len(source)):
                brace_depth += source[end] == "{"
                brace_depth -= source[end] == "}"
                if brace_depth == 0:
                    return source[line_start : end + 1]
            raise AssertionError(f"unterminated function {name}")
    raise AssertionError(f"missing function {name}")


def assert_ordered(source: str, *needles: str) -> None:
    offset = 0
    for needle in needles:
        index = source.find(needle, offset)
        assert index >= 0, f"missing ordered source invariant: {needle}"
        offset = index + len(needle)


# START is the only execution release. It must bind one bounded future local
# deadline while accepting the exact generation, round, commitment, and command
# identity prepared for that endpoint.
start_lease = function_body(LEASE, "survey_pair_lease_start_round_bound_at")
assert_ordered(
    start_lease,
    "!round_binding_valid(pair, round_id, round_commitment)",
    "(uint32_t)(execution_deadline_ms - now_ms)",
    "!round_binding_equal(lease, round_commitment)",
    "lease->round_id != round_id",
    "lease->start_id = *control_id",
    "lease->start_execution_deadline_ms = execution_deadline_ms",
    "lease->start_execution_armed = true",
)

release = function_body(LEASE, "survey_pair_lease_release_start")
assert "control_id_equal(&lease->start_id, control_id)" in release
assert "lease->start_released = true" in release

mark_running = function_body(LEASE, "survey_pair_lease_mark_running_at")
assert_ordered(
    mark_running,
    "survey_pair_lease_expire(lease, now_ms)",
    "!lease->start_execution_armed",
    "!deadline_reached(now_ms, lease->start_execution_deadline_ms)",
    "survey_pair_lease_mark_running(lease, pair, round_id)",
)

# The wire START delay is canonical, packet age shortens the remaining delay,
# and a closed or wider-than-signed deadline is rejected before lease mutation.
start_handler = function_body(
    RUNTIME, "app_anchor_survey_runtime_start_pair_from_command"
)
assert_ordered(
    start_handler,
    "gateway_command_extract_options(",
    "command_options.execute_delay_ms !=",
    "SURVEY_ROUND_START_EXECUTE_DELAY_MS",
    "packet->message_age_ms >= command_options.execute_delay_ms",
    "execution_remaining_ms = command_options.execute_delay_ms -",
    "packet->message_age_ms",
    "execution_remaining_ms > (uint32_t)INT32_MAX",
    "execution_deadline_ms = now_ms + execution_remaining_ms",
    "survey_pair_lease_start_round_bound_at(",
)

# Exact START-result delivery remains a second barrier. A delivered result
# releases custody, but the worker schedules the remaining delay and cannot
# claim RUNNING until the synchronized deadline is reached.
delivery_gate = function_body(RUNTIME, "pair_start_delivery_ready")
assert_ordered(
    delivery_gate,
    "event.reason == NODE_COMM_TERMINAL_DELIVERED",
    "survey_pair_lease_release_start(&pair_lease",
    "survey_pair_lease_execution_remaining_ms(",
    "schedule_owned(K_MSEC(release_remaining_ms)",
)
worker = function_body(RUNTIME, "survey_work_handler")
assert_ordered(
    worker,
    "if (!pair_start_delivery_ready())",
    "survey_pair_lease_mark_running_at(&pair_lease",
)

# The retired broadcast GO command has no runtime handler or delayed-command
# priority branch. A decoder may retain only the explicit retired wire ID.
runtime_source = RUNTIME + COMMANDS
for retired_runtime_symbol in (
    "CMD_SURVEY_GO",
    "survey_round_go_",
    "app_anchor_survey_runtime_go_round_from_command",
):
    assert retired_runtime_symbol not in runtime_source

# Generation-bound ABORT remains exact and cannot cancel another round.
abort_handler = function_body(
    RUNTIME, "app_anchor_survey_runtime_abort_pair_matching_round"
)
assert "survey_pair_lease_abort_matching_round_bound(" in abort_handler
assert "round_commitment" in abort_handler

print("anchor survey START barrier source invariants passed")
