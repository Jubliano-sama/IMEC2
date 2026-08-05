#!/usr/bin/env python3
"""Lock survey closure to immutable physical receive-time intervals."""

from pathlib import Path
import re

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = read_composed_source(ROOT / "app/src/app_anchor.c")
SURVEY = (ROOT / "app/src/app_anchor_gateway_survey.inc").read_text()
ROUND = (ROOT / "app/src/app_anchor_gateway_survey_round.inc").read_text()
CONTROL = (ROOT / "app/src/app_anchor_gateway_control.inc").read_text()
REPORT = read_composed_source(ROOT / "app/src/app_mesh_report.c")
REPORT_HEADER = (ROOT / "app/src/app_mesh_report.h").read_text()


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


interval = function_body(SURVEY, "gateway_survey_receive_in_interval")
assert "uint64_t received_at_ms" in interval
assert "uint32_t received_at_32 = (uint32_t)received_at_ms" in interval
start_check = interval.index(
    "uptime_deadline_reached(received_at_32, started_at_ms)"
)
deadline_check = interval.index(
    "!uptime_deadline_reached(received_at_32, deadline_ms)"
)
assert start_check < deadline_check, (
    "survey admission must implement the closed-open [start, deadline) interval"
)


# The callback boundary must preserve the radio-captured timestamp all the way
# from queue custody into survey semantic admission.
assert re.search(
    r"gateway_handle_survey_discovery_report\)\("
    r"[^;]*uint64_t received_at_ms",
    REPORT_HEADER,
    re.S,
)
delivery = function_body(REPORT, "mesh_drain_rx_queue_locked")
assert "pending->first_received_at_ms" in delivery[
    delivery.index("mesh_report_gateway_handle_survey_discovery_report(") :
]
gateway_accept = function_body(ANCHOR, "gateway_handle_survey_discovery_report")
pair_dispatch = gateway_accept[
    gateway_accept.index("if (packet->msg_type == MSG_SURVEY_PAIR_RESULT)") :
    gateway_accept.index("survey_extract_reach_report_tlvs(")
]
assert "received_at_ms" in pair_dispatch


# Discovery uses the frozen safety deadline until known-count completion.  The
# early cutoff is caused by the last accepted physical receipt, includes that
# timestamp under a strict upper bound, and can never exceed the safety D.
discovery_deadline = function_body(
    SURVEY, "gateway_survey_collection_receive_deadline_ms"
)
assert "gateway_survey_collection_finalize_cutoff_valid" in discovery_deadline
assert "gateway_survey_collection_deadline_ms" in discovery_deadline
freeze = function_body(
    SURVEY, "gateway_survey_collection_freeze_finalize_cutoff"
)
assert "(uint32_t)received_at_ms + 1u" in freeze
assert "gateway_survey_collection_deadline_ms" in freeze
assert freeze.index("gateway_survey_collection_deadline_ms") < freeze.index(
    "gateway_survey_collection_finalize_cutoff_valid = true"
)
assert "gateway_survey_receive_in_interval(" in gateway_accept
assert "k_uptime_get_32()" not in gateway_accept, (
    "survey report eligibility must not depend on delayed worker time"
)
record = gateway_accept.index(
    "survey_gateway_note_reach_report_with_reverse_hint_status("
)
freeze_call = gateway_accept.index(
    "gateway_survey_collection_freeze_finalize_cutoff(", record
)
assert record < freeze_call
assert "gateway_survey_expected_node_count" in gateway_accept[
    record:freeze_call
]


collection_finalize = function_body(
    CONTROL, "gateway_survey_wait_for_discovery_collection"
)
collection_validation = collection_finalize.index(
    "gateway_protocol_validation_check_interval("
)
collection_clear = collection_finalize.index(
    "gateway_survey_collection_pending = false;", collection_validation
)
assert "gateway_survey_collection_started_at_ms" in collection_finalize[
    collection_validation:collection_clear
]
assert "receive_deadline_ms" in collection_finalize[
    collection_validation:collection_clear
]
assert "GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED" in collection_finalize[
    collection_validation:collection_clear
]
assert "GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED" in collection_finalize[
    collection_validation:collection_clear
]
assert "COMMAND_INTERNAL_ERROR" in collection_finalize[
    collection_validation:collection_clear
]


# Pair-result paths enforce the same strict physical interval, while each
# deadline-driven finalizer waits for an already-armed pre-D receive.
pair_accept = function_body(ANCHOR, "gateway_note_survey_pair_result")
assert "uint64_t received_at_ms" in pair_accept
assert pair_accept.count("gateway_survey_receive_in_interval(") == 1
assert re.search(
    r"gateway_survey_round_note_sample\s*\([^;]*received_at_ms\s*\)",
    pair_accept,
    re.S,
)
serial_finalize = function_body(
    ANCHOR, "gateway_survey_finalize_pair_observation"
)
for required in (
    "gateway_protocol_validation_check_interval(",
    "gateway_survey_pair_observation_started_at_ms",
    "gateway_survey_pair_observation_deadline_ms",
    "GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED",
    "GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED",
    "GATEWAY_SURVEY_PAIR_FINALIZE_VALIDATION_EXPIRED",
):
    assert required in serial_finalize

round_accept = function_body(ROUND, "gateway_survey_round_note_sample")
assert "uint64_t received_at_ms" in round_accept
assert "gateway_survey_receive_in_interval(" in round_accept
round_finalize = function_body(
    ROUND, "gateway_survey_round_finalize_observation"
)
for required in (
    "gateway_protocol_validation_check_interval(",
    "gateway_survey_round_observation_started_at_ms",
    "gateway_survey_round_observation_deadline_ms",
    "GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED",
    "GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED",
    "COMMAND_INTERNAL_ERROR",
):
    assert required in round_finalize


survey_work = function_body(CONTROL, "gateway_survey_work_handler")
assert "GATEWAY_SURVEY_PAIR_FINALIZE_VALIDATION_EXPIRED" in survey_work
operation_gate = survey_work[
    survey_work.index("gateway_survey_operation_deadline_ms") :
    survey_work.index("gateway_survey_flush_boundary_event")
]
for required in (
    "gateway_protocol_validation_check_interval(",
    "gateway_survey_operation_started_at_ms",
    "GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED",
    "GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED",
    "COMMAND_INTERNAL_ERROR",
):
    assert required in operation_gate


# A duplicate PREPARE/START can be physically received at ACK-settle D-1 and
# remain queued for semantic validation when the worker reaches D. The old
# settle interval must remain authoritative until that receive either commits
# the exact duplicate re-arm or leaves validation custody.
settle_barrier = function_body(
    SURVEY, "gateway_survey_response_ack_settle_blocks_progress"
)
settle_expiry = settle_barrier.index(
    "survey_gateway_response_ack_settle_deadline_reached("
)
settle_validation = settle_barrier.index(
    "gateway_protocol_validation_check_interval(", settle_expiry
)
settle_blocked = settle_barrier.index(
    "GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED", settle_validation
)
settle_expired = settle_barrier.index(
    "GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED", settle_blocked
)
settle_complete = settle_barrier.index(
    "survey_gateway_response_ack_settle_complete(", settle_expired
)
assert (
    settle_expiry < settle_validation < settle_blocked < settle_expired <
    settle_complete
), "settle expiry must preserve the causal receive barrier before phase advance"
validation_interval = settle_barrier[settle_validation:settle_blocked]
assert "gateway_survey_response_ack_settle.started_at_ms" in validation_interval
assert "gateway_survey_response_ack_settle.deadline_ms" in validation_interval
blocked_path = settle_barrier[settle_blocked:settle_expired]
assert "gateway_survey_work_reschedule(" in blocked_path
assert "return true;" in blocked_path

schedule_drive = function_body(SURVEY, "gateway_survey_schedule_drive")
schedule_barrier = schedule_drive.index(
    "gateway_survey_response_ack_settle_blocks_progress("
)
drive_decision = schedule_drive.index(
    "survey_gateway_drive_action(&state)", schedule_barrier
)
assert schedule_barrier < drive_decision

# Initial accepted results in both the serial and parallel-round paths must
# clamp their settle wake to the immutable operation deadline. Otherwise a
# result accepted at operation D-1 can replace the operation wake with D+2999.
serial_result = function_body(
    SURVEY, "gateway_survey_auto_note_command_result"
)
serial_settle = serial_result[
    serial_result.index("survey_gateway_response_ack_settle_note_result(") :
    serial_result.index("gateway_survey_schedule_drive()")
]
assert "gateway_survey_operation_deadline_ms" in serial_settle

round_result = function_body(
    ROUND, "gateway_survey_round_note_control_result"
)
round_settle = round_result[
    round_result.index("survey_gateway_response_ack_settle_note_result(") :
    round_result.index("gateway_survey_schedule_drive()")
]
assert "gateway_survey_operation_deadline_ms" in round_settle

# The deadline worker must preserve operation timeout and pending boundary
# ownership first, then run the settle barrier before cleanup, round dispatch,
# or serial next-action progress. Delivery-terminal reconciliation may precede
# the barrier because it cannot advance the phase and is needed by termination.
worker_delivery = survey_work.index(
    "gateway_survey_service_active_delivery()"
)
worker_active = survey_work.index("if (!gateway_survey_active)")
worker_deadline = survey_work.index(
    "gateway_survey_operation_deadline_ms", worker_active
)
worker_boundary = survey_work.index(
    "gateway_survey_flush_boundary_event()", worker_deadline
)
worker_barrier = survey_work.index(
    "gateway_survey_response_ack_settle_blocks_progress(", worker_boundary
)
worker_cleanup = survey_work.index(
    "gateway_survey_service_cleanup()", worker_barrier
)
worker_round = survey_work.index(
    "gateway_survey_round_drive()", worker_cleanup
)
worker_next_action = survey_work.index(
    "survey_gateway_auto_next_action(", worker_round
)
assert (
    worker_delivery < worker_active < worker_deadline < worker_boundary <
    worker_barrier < worker_cleanup <
    worker_round < worker_next_action
), (
    "the worker must preserve timeout and boundary ownership, then retain "
    "D-1 receive custody before protocol progress"
)

print("gateway survey receive deadline source invariants passed")
