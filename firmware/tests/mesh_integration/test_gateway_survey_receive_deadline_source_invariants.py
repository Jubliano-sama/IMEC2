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
assert "survey_gateway_receive_in_interval(" in interval


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

ack_callback = function_body(REPORT, "mesh_report_gateway_note_ack_confirm")
assert "uint64_t first_received_at_ms" in ack_callback
assert re.search(
    r"gateway_note_ack_confirm\s*\([^;]*first_received_at_ms\s*\)",
    ack_callback,
    re.S,
)
ack_delivery = function_body(REPORT, "mesh_gateway_accept_semantic_delivery")
ack_branch = ack_delivery[
    ack_delivery.index("case MSG_GATEWAY_ACK_CONFIRM:") :
    ack_delivery.index("case MSG_CLICK_REPORT:")
]
assert re.search(
    r"mesh_report_gateway_note_ack_confirm\s*\("
    r"[^;]*pending->first_received_at_ms\s*\)",
    ack_branch,
    re.S,
)


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
assert "gateway_survey_receive_in_interval(" not in pair_accept
assert re.search(
    r"gateway_survey_round_note_sample\s*\([^;]*received_at_ms\s*\)",
    pair_accept,
    re.S,
)
assert re.search(
    r"gateway_manual_survey_pair_note_sample\s*\([^;]*received_at_ms\s*\)",
    pair_accept,
    re.S,
)

round_accept = function_body(ROUND, "gateway_survey_round_note_sample")
assert "uint64_t received_at_ms" in round_accept
assert "gateway_survey_receive_in_interval(" in round_accept
round_finalize = function_body(
    ROUND, "gateway_survey_round_finalize_observation"
)
for required in (
    "gateway_protocol_validation_check_interval(",
    "gateway_survey_round_observation_origin.valid",
    "gateway_survey_round_observation_origin.started_at_ms",
    "gateway_survey_round_observation_deadline_ms",
    "GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED",
    "GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED",
    "COMMAND_INTERNAL_ERROR",
):
    assert required in round_finalize


# Result delivery is durable upstream and may legitimately spend most of the
# command budget repairing a one-hop route. The gateway must not replace that
# immutable operation horizon with the old BLE-service estimate or the short
# physical ranging window while responder samples are still in custody.
round_commitment = function_body(ROUND, "gateway_survey_round_commitment")
assert (
    "gateway_survey_operation_deadline_ms -" in round_commitment
    and "gateway_survey_operation_started_at_ms" in round_commitment
)
assert "gateway_survey_round_longest_run_ms" not in ROUND

round_start = function_body(SURVEY, "gateway_survey_send_start")
round_observation = round_start[
    round_start.index("survey_gateway_observation_origin_freeze(") :
]
assert (
    "gateway_survey_round_observation_deadline_ms =\n"
    "            gateway_survey_operation_deadline_ms;"
    in round_observation
)
assert "SURVEY_PAIR_RESULT_DELIVERY_TIMEOUT_MS" not in round_observation

survey_work = function_body(CONTROL, "gateway_survey_work_handler")
operation_gate = survey_work[
    survey_work.index("gateway_survey_operation_deadline_ms") :
    survey_work.index("gateway_survey_flush_boundary_event")
]
timely_proof = operation_gate.index(
    "app_gateway_survey_round_control_confirmation_received_in_interval("
)
apply_proof = operation_gate.index(
    "gateway_survey_round_apply_control_confirmation()", timely_proof
)
validation_gate = operation_gate.index(
    "gateway_protocol_validation_check_interval(", apply_proof
)
assert timely_proof < apply_proof < validation_gate
for required in (
    "gateway_protocol_validation_check_interval(",
    "gateway_survey_operation_started_at_ms",
    "GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED",
    "GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED",
    "COMMAND_INTERNAL_ERROR",
):
    assert required in operation_gate


# An accepted parallel-round result may retire its delivery immediately, but
# only the exact terminal ACK_CONFIRM may advance the next phase. This avoids
# replacing the operation deadline with a three-second proxy wake.
round_result = function_body(ROUND, "gateway_survey_round_note_control_result")
capture = round_result.index("app_gateway_survey_round_capture_control_result(")
retire = round_result.index(
    "survey_gateway_transaction_phase_complete(", capture
)
assert capture < retire
assert "response_ack_settle" not in round_result
ack_confirm = function_body(SURVEY, "gateway_note_survey_ack_confirm")
assert "uint64_t first_received_at_ms" in ack_confirm
assert ".first_received_at_ms = first_received_at_ms" in ack_confirm
assert "app_gateway_survey_round_note_control_ack_confirm(" in ack_confirm
assert "gateway_survey_work_schedule(" in ack_confirm
assert "SURVEY_GATEWAY_DUE_CONTROL_DELIVERY, 0u" in ack_confirm

# The deadline worker must service every immutable phase clock before a
# boundary, settle interval, or operation confirmation can gate later protocol
# progress. Delivery-terminal reconciliation may precede those clocks because
# it cannot advance the phase and is needed by termination.
worker_delivery = survey_work.index(
    "gateway_survey_service_active_delivery()"
)
worker_active = survey_work.index("if (!gateway_survey_active)")
worker_deadline = survey_work.index(
    "gateway_survey_operation_deadline_ms", worker_active
)
worker_cleanup = survey_work.index(
    "gateway_survey_service_cleanup()", worker_deadline
)
worker_collection = survey_work.index(
    "gateway_survey_wait_for_discovery_collection()", worker_cleanup
)
worker_boundary = survey_work.index(
    "gateway_survey_flush_boundary_event()", worker_collection
)
worker_round = survey_work.index(
    "gateway_survey_round_drive(", worker_boundary
)
assert (
    worker_delivery < worker_active < worker_deadline < worker_cleanup <
    worker_collection < worker_boundary < worker_round
), (
    "the worker must preserve timeout and immutable phase owners, then let "
    "the round consume exact terminal proof before any successor control"
)
for retired in (
    "gateway_survey_auto",
    "survey_gateway_auto_next_action",
    "survey_gateway_auto_",
    "gateway_survey_finalize_pair_observation",
):
    assert retired not in survey_work

print("gateway survey receive deadline source invariants passed")
