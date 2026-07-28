#!/usr/bin/env python3
"""Static ownership boundaries for the gateway survey machine adapter."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = (ROOT / "app/src/app_anchor.c").read_text(encoding="utf-8")
GLUE = (ROOT / "app/src/app_anchor_gateway_survey_round.inc").read_text(
    encoding="utf-8"
)
SURVEY = (ROOT / "app/src/app_anchor_gateway_survey.inc").read_text(
    encoding="utf-8"
)
CONTROL = (ROOT / "app/src/app_anchor_gateway_control.inc").read_text(
    encoding="utf-8"
)
MACHINE = (ROOT / "src/gateway_survey_machine.c").read_text(encoding="utf-8")
ANCHOR_RUNTIME = (ROOT / "app/src/app_anchor_survey_runtime.c").read_text(
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


def assert_ordered(source: str, *needles: str) -> None:
    offset = 0
    for needle in needles:
        index = source.find(needle, offset)
        assert index >= 0, f"missing ordered adapter boundary: {needle}"
        offset = index + len(needle)


# The machine is a pure transition owner. Platform scheduling, radio custody,
# packet construction, and logging remain in the Zephyr adapter.
for forbidden in (
    "#include <zephyr",
    "app_node_comm_",
    "k_work_",
    "k_uptime_",
    "dwm3000",
    "LOG_",
):
    assert forbidden not in MACHINE, (
        f"pure gateway survey machine contains platform dependency {forbidden}"
    )

assert not (ROOT / "app/src/app_gateway_survey_round.c").exists()
assert not (ROOT / "app/src/app_gateway_survey_round.h").exists()
assert ANCHOR.count("struct gateway_survey_machine survey_machine;") == 1
assert '#include "gateway_survey_machine.h"' in ANCHOR


# Retired collection and failed-control state cannot reappear beside the
# machine. The adapter uses public transitions and never mutates private fields.
adapter = "\n".join((ANCHOR, SURVEY, GLUE, CONTROL))
for retired in (
    "gateway_survey_active",
    "gateway_survey_operation_deadline_ms",
    "gateway_survey_discovery_delivery_handle",
    "gateway_survey_collection_deadline_ms",
    "gateway_survey_collection_emission_deadline_ms",
    "gateway_survey_collection_duration_ms",
    "gateway_survey_collection_window_armed",
    "gateway_survey_collection_pending",
    "gateway_survey_round_cleanup_lane_index",
    "gateway_survey_round_cleanup_lane_valid",
):
    assert retired not in adapter, f"retired split survey owner returned: {retired}"

for source in (SURVEY, GLUE, CONTROL):
    assert "survey_machine." not in source, (
        "Zephyr adapter must use gateway_survey_machine transitions/accessors"
    )

report_admission = function_body(
    SURVEY, "gateway_handle_survey_discovery_report"
)
assert_ordered(
    report_admission,
    "uint32_t delivery_token = gateway_survey_machine_discovery_delivery_token(",
    "gateway_survey_machine_admit_report(",
)

for transition in (
    "gateway_survey_machine_begin(",
    "gateway_survey_machine_bind_discovery_delivery(",
    "gateway_survey_machine_note_discovery_terminal(",
    "gateway_survey_machine_admit_report(",
    "gateway_survey_machine_collection_drive(",
    "gateway_survey_machine_round_begin(",
    "gateway_survey_machine_round_note_control_success(",
    "gateway_survey_machine_round_note_control_failure(",
    "gateway_survey_machine_round_note_sample(",
    "gateway_survey_machine_round_advance_batch(",
):
    assert transition in adapter, f"adapter does not delegate through {transition}"

for cleanup_boundary in (
    "gateway_survey_machine_failed_control_cleanup_pending(",
    "gateway_survey_machine_failed_control_cleanup_lane(",
    "gateway_survey_machine_release_failed_control_cleanup(",
):
    assert cleanup_boundary in GLUE


# RF-start evidence remains an adapter fact: observing cannot start before the
# communication owner reports an actual GO attempt.
ensure_observing = function_body(GLUE, "gateway_survey_round_ensure_observing")
assert_ordered(
    ensure_observing,
    "gateway_survey_machine_round_go_needed(",
    "gateway_survey_round_go_delivery_handle == 0u",
    "app_node_comm_delivery_attempts_started(",
    "attempts_started == 0u",
    "gateway_survey_machine_round_mark_observing_after_go(",
)

build_go = function_body(GLUE, "gateway_survey_round_build_go")
for required in (
    "gateway_survey_machine_round_id(",
    "CMD_SCOPE_ALL_HEARD",
    "CMD_RESPONSE_NONE",
    "outbound->packet.dst_id = MESH_BROADCAST_ID",
    "outbound->packet.ttl = FLOOD_EPOCH_GLOBAL_TTL",
):
    assert required in build_go

submit_go = function_body(GLUE, "gateway_survey_round_submit_go")
assert_ordered(
    submit_go,
    "gateway_survey_machine_operation_remaining_ms(",
    "gateway_survey_round_build_go(",
    "app_node_comm_submit_delivery(",
    "NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD",
)


# Anchor-side START remains a reservation until response custody and matching
# GO ownership are both proven; this is the other side of the adapter seam.
delivery_gate = function_body(ANCHOR_RUNTIME, "pair_start_delivery_ready")
assert "delivery_confirmed &&" in delivery_gate
assert "survey_pair_lease_ready_snapshot(&pair_lease, NULL)" in delivery_gate

survey_worker = function_body(ANCHOR_RUNTIME, "survey_work_handler")
assert_ordered(
    survey_worker,
    "if (!pair_start_delivery_ready())",
    "return;",
    "survey_pair_lease_ready_snapshot(&pair_lease, &pair)",
    "app_mesh_radio_owner_try_claim(",
    "APP_MESH_RADIO_CLIENT_SURVEY",
    '"survey pair DS-TWR"',
    "&radio_lease",
    "survey_pair_lease_mark_running(&pair_lease",
    "as_responder = pair.responder_id == DEVICE_ID",
    "run_pair_responder(&pair, pair_round_id)",
    "run_pair_initiator(&pair, pair_round_id)",
)
