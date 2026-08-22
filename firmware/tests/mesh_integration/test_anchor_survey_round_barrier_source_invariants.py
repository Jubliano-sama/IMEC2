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
CONFIG = (ROOT / "app/src/app_config.h").read_text(encoding="utf-8")
SURVEY = (ROOT / "include/survey.h").read_text(encoding="utf-8")


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


def caller_and_direct_helpers(source: str, caller_name: str) -> str:
    caller = function_body(source, caller_name)
    bodies = [caller]
    visited = {caller_name}

    for called_name in re.findall(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", caller):
        if called_name in visited:
            continue
        visited.add(called_name)
        try:
            bodies.append(function_body(source, called_name))
        except AssertionError:
            continue
    return "\n".join(bodies)


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

mark_running = function_body(
    LEASE, "survey_pair_lease_mark_running_for_role_at"
)
assert_ordered(
    mark_running,
    "survey_pair_lease_expire(lease, now_ms)",
    "!lease->start_execution_armed",
    "role_start_ms = lease->start_execution_deadline_ms",
    "if (as_responder)",
    "role_start_ms -= SURVEY_PAIR_START_SKEW_MARGIN_MS",
    "!deadline_reached(now_ms, role_start_ms)",
    "latest_start_ms = lease->start_execution_deadline_ms +",
    "SURVEY_PAIR_START_SKEW_MARGIN_MS",
    "deadline_reached(now_ms, latest_start_ms)",
    "clear_active(lease)",
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

# A responder cannot accept a new immutable START while an older pair result
# still owns delivery custody.  The BUSY path services that owner and returns
# before touching the prepared lease or arming the later RF-preemption path;
# initiators deliberately bypass the responder-only gate.
responder_role = start_handler.index(
    "as_responder = pair.responder_id == DEVICE_ID"
)
custody_count = start_handler.index(
    "app_anchor_survey_result_delivery_occupied_count()",
    responder_role,
)
custody_gate = start_handler.index(
    "if (as_responder && occupied_result_count > 0u)", custody_count
)
radio_gate = start_handler.index("if (anchor_uwb_window_active())", custody_gate)
lease_mutation = start_handler.index(
    "survey_pair_lease_start_round_bound_at(", radio_gate
)
custody_busy = start_handler[custody_gate:radio_gate]
assert_ordered(
    custody_busy,
    "if (as_responder && occupied_result_count > 0u)",
    "app_anchor_survey_result_delivery_service()",
    "*status = COMMAND_BUSY",
    "return -EBUSY",
)
for forbidden_busy_mutation in (
    "k_spin_lock(&survey_lock)",
    "survey_pair_lease_",
    "pair_start_pending",
    "pair_start_delivery_handle",
    "deadline_schedule",
    "survey_transport_preempt_begin",
    "app_operation_policy_commit_prepared",
):
    assert forbidden_busy_mutation not in custody_busy
assert responder_role < custody_count < custody_gate < radio_gate < lease_mutation
assert "survey_transport_preempt_begin" not in start_handler

# Local START acceptance arms the synchronized run immediately. The status
# packet keeps independent reliable custody, so its ACK_CONFIRM path cannot
# suppress DS-TWR; only the immutable shared execution deadline remains a gate.
delivery_gate = function_body(RUNTIME, "pair_start_delivery_ready")
assert_ordered(
    delivery_gate,
    "survey_pair_lease_release_start(&pair_lease",
    "survey_pair_lease_execution_remaining_for_role_ms(",
    "SURVEY_ANCHOR_DEADLINE_PHASE_SAFETY",
    "release_remaining_ms",
)
result_submit = function_body(COMMANDS, "anchor_submit_command_result")
assert "app_node_comm_commit_protocol_response_auto_reap(" in result_submit
assert "app_node_comm_submit_protocol_response_auto_reap(" in result_submit
assert "app_node_comm_auto_reap_delivery" not in delivery_gate
assert "app_node_comm_take_delivery_event_for" not in delivery_gate
assert "NODE_COMM_TERMINAL_DELIVERED" not in delivery_gate
worker = function_body(RUNTIME, "survey_work_handler")
assert_ordered(
    worker,
    "if (!pair_start_delivery_ready())",
    "survey_pair_lease_mark_running_for_role_at(&pair_lease",
)

# A failed exchange may return before its 150 ms receive timeout. Both roles
# therefore derive every sequence from the immutable scheduled execution grid:
# the initiator skips an expired cell and caps RF to the cell end, while the
# responder keeps START skew only in sample zero and uses absolute later cells.
timing = CONFIG + SURVEY
assert re.search(
    r"#define\s+SURVEY_PAIR_SAMPLE_CELL_MS\s+"
    r"(?:\\\n\s*)?\(?SURVEY_PAIR_INITIATOR_TIMEOUT_MS\s*\+\s*"
    r"SURVEY_PAIR_SAMPLE_GAP_MS\)?",
    timing,
), "survey sample cell must equal initiator timeout plus sample gap"
assert re.search(
    r"#define\s+SURVEY_PAIR_BATCH_WINDOW_MS\s+"
    r"(?:\\\n\s*)?\(?SURVEY_PAIR_RESPONDER_WINDOW_MS\s*\+[^#]*"
    r"SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT\s*-\s*1u[^#]*"
    r"SURVEY_PAIR_SAMPLE_CELL_MS",
    timing,
), "survey batch window must contain the initial skew window and later cells"

initiator = function_body(RUNTIME, "run_pair_initiator")
initiator_surface = caller_and_direct_helpers(RUNTIME, "run_pair_initiator")
assert "k_msleep(SURVEY_PAIR_SAMPLE_GAP_MS)" not in initiator
assert "pair_execution_ms" in initiator
assert "SURVEY_PAIR_SAMPLE_CELL_MS" in initiator_surface
assert "SURVEY_PAIR_INITIATOR_TIMEOUT_MS" in initiator_surface
assert "k_msleep" in initiator_surface
assert re.search(
    r"pair_execution_ms\s*\+[^;]*sample_index[^;]*"
    r"SURVEY_PAIR_SAMPLE_CELL_MS",
    initiator_surface,
), "initiator must derive each sample start from the immutable execution grid"
assert_ordered(
    initiator,
    "if (!uptime_deadline_reached(now_ms, sample_start_ms))",
    "k_msleep(uptime_ms_until_deadline(now_ms, sample_start_ms))",
    "if (uptime_deadline_reached(now_ms, sample_deadline_ms))",
    "continue",
    "remaining_ms = uptime_ms_until_deadline(now_ms",
    "request.timeout_ms =",
    "dwm3000_driver_range_initiator(&request, &result)",
)
assert "request.timeout_ms = SURVEY_PAIR_INITIATOR_TIMEOUT_MS;" not in initiator

responder = function_body(RUNTIME, "run_pair_responder")
responder_surface = caller_and_direct_helpers(RUNTIME, "run_pair_responder")
assert "pair_execution_ms" in responder
assert re.search(
    r"survey_pair_responder_sample_deadline_ms\(\s*"
    r"pair_execution_ms\s*,\s*sample_index\s*\)",
    responder,
)
assert "SURVEY_PAIR_SAMPLE_CELL_MS" in responder_surface
assert re.search(
    r"pair_execution_ms\s*-\s*SURVEY_PAIR_START_SKEW_MARGIN_MS",
    responder_surface,
), "responder deadlines must retain the immutable scheduled role origin"
assert re.search(
    r"responder_batch_origin_ms\s*\+\s*"
    r"SURVEY_PAIR_RESPONDER_WINDOW_MS[^;]*sample_index[^;]*"
    r"SURVEY_PAIR_SAMPLE_CELL_MS",
    responder_surface,
), "responder must use one skew window followed by absolute sample cells"
assert not re.search(
    r"deadline_ms\s*=\s*k_uptime_get\(\)\s*\+\s*"
    r"SURVEY_PAIR_RESPONDER_WINDOW_MS",
    responder,
), "responder must not reopen the full skew window for every sample"

assert_ordered(
    worker,
    "pair_execution_ms = pair_lease.start_execution_deadline_ms",
    "survey_pair_lease_mark_running_for_role_at(&pair_lease",
    "run_pair_responder(&pair",
)
assert re.search(
    r"run_pair_responder\s*\([^;]*pair_execution_ms",
    worker,
), "worker must pass the preserved scheduled execution to the responder"
assert re.search(
    r"run_pair_initiator\s*\([^;]*pair_execution_ms",
    worker,
), "worker must pass the preserved scheduled execution to the initiator"

# The responder executes START one skew window early so its RX is already open
# at the immutable initiator timestamp. The packet's original delay remains
# intact, so both endpoints still derive the same shared deadline.
schedule_delay = function_body(
    COMMANDS, "anchor_broadcast_command_schedule_delay_ms"
)
assert_ordered(
    schedule_delay,
    "command_id != CMD_SURVEY_START_PAIR",
    "survey_extract_pair_tlvs(payload, payload_len, &pair)",
    "pair.responder_id != DEVICE_ID",
    "execution_remaining_ms - SURVEY_PAIR_START_SKEW_MARGIN_MS",
)
delayed_command = function_body(
    COMMANDS, "anchor_schedule_broadcast_command_execution"
)
assert_ordered(
    delayed_command,
    "anchor_broadcast_command_schedule_delay_ms(",
    "execute_at_ms =",
    "received_at_ms + schedule_delay_ms",
    "execute_deadline_ms =",
    "received_at_ms + delay_ms +",
    "mesh_route_work_reschedule(",
    "schedule_delay_ms == 0u ? 1u : schedule_delay_ms",
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
