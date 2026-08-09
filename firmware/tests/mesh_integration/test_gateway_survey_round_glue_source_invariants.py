#!/usr/bin/env python3
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
GLUE = (ROOT / "app/src/app_anchor_gateway_survey_round.inc").read_text(
    encoding="utf-8"
)
SURVEY = (ROOT / "app/src/app_anchor_gateway_survey.inc").read_text(
    encoding="utf-8"
)
ROUND = (ROOT / "app/src/app_gateway_survey_round.c").read_text(
    encoding="utf-8"
)
ROUND_HEADER = (ROOT / "app/src/app_gateway_survey_round.h").read_text(
    encoding="utf-8"
)
ANCHOR = (ROOT / "app/src/app_anchor.c").read_text(encoding="utf-8")
SURVEY_CORE = (ROOT / "src/survey.c").read_text(encoding="utf-8")
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


# Exact sample bytes remain duplicate/conflict authority; a bounded hash cannot
# decide equality. The wrapper retains all 25 disjoint runtime lanes.
assert "survey_sample_semantic_fingerprint" not in SURVEY_CORE
assert "struct survey_sample_observation_identity sample_identities" in ROUND_HEADER
assert "SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES == 25u" in ROUND
preflight = function_body(ROUND, "app_gateway_survey_round_preflight_sample")
assert_ordered(
    preflight,
    "survey_sample_observation_identity_capture(",
    "survey_sample_observation_identity_valid(existing_identity)",
    "!survey_sample_observation_identity_equal(",
)

# Each lane uses the contract's complete four-control sequence. START initiator
# makes that lane observable immediately, while later disjoint lanes may still
# be dispatching.
details = function_body(ROUND, "app_gateway_survey_round_stage_details")
assert_ordered(
    details,
    "SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR",
    "SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER",
    "SURVEY_GATEWAY_AUTO_START_RESPONDER",
    "SURVEY_GATEWAY_AUTO_START_INITIATOR",
)
success = function_body(ROUND, "app_gateway_survey_round_note_control_success")
assert_ordered(
    success,
    "case SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR:",
    "round->dispatch_stage = SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER",
    "case SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER:",
    "round->dispatch_stage = SURVEY_GATEWAY_AUTO_START_RESPONDER",
    "case SURVEY_GATEWAY_AUTO_START_RESPONDER:",
    "round->dispatch_stage = SURVEY_GATEWAY_AUTO_START_INITIATOR",
    "case SURVEY_GATEWAY_AUTO_START_INITIATOR:",
    "survey_pair_round_runtime_mark_observing(",
    "app_gateway_survey_round_advance_dispatch(round)",
)
note_sample = function_body(ROUND, "app_gateway_survey_round_note_sample")
finalize_lane = function_body(ROUND, "app_gateway_survey_round_finalize_lane")
for mixed_phase_handler in (note_sample, finalize_lane):
    assert "APP_GATEWAY_SURVEY_ROUND_DISPATCHING" in mixed_phase_handler
    assert "APP_GATEWAY_SURVEY_ROUND_OBSERVING" in mixed_phase_handler

# The responder START freezes the shared time origin. The initiator START uses
# elapsed packet age from the same origin, both carry the canonical delay, and
# the observation window starts at the future release rather than send time.
send_start = function_body(SURVEY, "gateway_survey_auto_send_start")
assert_ordered(
    send_start,
    "action->stage == SURVEY_GATEWAY_AUTO_START_RESPONDER",
    ".started_at_ms = now_ms",
    "message_age_ms = 0u",
    "message_age_ms = now_ms -",
    "gateway_survey_start_release.started_at_ms",
    "message_age_ms >= SURVEY_ROUND_START_EXECUTE_DELAY_MS",
    "TLV_EXECUTE_DELAY_MS",
    "SURVEY_ROUND_START_EXECUTE_DELAY_MS",
    "outbound.packet.message_age_ms = message_age_ms",
    "release_ms =",
    "gateway_survey_start_release.started_at_ms +",
    "SURVEY_ROUND_START_EXECUTE_DELAY_MS",
    "gateway_survey_round_observation_started_at_ms =",
    "release_ms",
)

# Result ACK quiet time still separates every successful control, so responder
# and initiator START are not collapsed into an unsafe immediate burst.
result = function_body(GLUE, "gateway_survey_round_note_control_result")
assert_ordered(
    result,
    "app_gateway_survey_round_note_control_success(",
    "survey_gateway_response_ack_settle_note_result(",
    "gateway_survey_schedule_drive()",
)

# Once the last START result settles, the parallel round has no serial-auto
# wait bit to keep the worker alive.  Its OBSERVING phase must therefore feed
# the same bounded observation-poll policy as the legacy single-pair owner.
drive_state = function_body(SURVEY, "gateway_survey_drive_state")
observation_mapping = drive_state[
    drive_state.index(".pair_observation_active =") :
    drive_state.index(".round_drive_ready =")
]
assert "gateway_survey_pair_observation_active" in observation_mapping
assert "gateway_survey_round.phase" in observation_mapping
assert "APP_GATEWAY_SURVEY_ROUND_OBSERVING" in observation_mapping

# Observation waiting is a deadline poll, never an immediate self-resubmit.
drive_schedule = function_body(SURVEY, "gateway_survey_schedule_drive")
poll_wait_start = drive_schedule.index(
    "action == SURVEY_GATEWAY_DRIVE_POLL_WAIT"
)
poll_wait_end = drive_schedule.index(
    "action == SURVEY_GATEWAY_DRIVE_RETRY_BOUNDARY",
    poll_wait_start,
)
poll_wait_branch = drive_schedule[poll_wait_start:poll_wait_end]
assert "GATEWAY_SURVEY_TRANSACTION_POLL_MS" in poll_wait_branch
assert "gateway_survey_work_reschedule(0u)" not in poll_wait_branch

# Failed controls own cleanup only for their lane, and final observation cannot
# erase cleanup debt before both endpoint outcomes settle.
failure = function_body(ROUND, "app_gateway_survey_round_note_control_failure")
assert "survey_pair_round_runtime_require_cleanup(" in failure
assert "memset(round" not in failure
finalize = function_body(GLUE, "gateway_survey_round_finalize_observation")
assert "app_gateway_survey_round_finalize_lane(" in finalize
cleanup = function_body(GLUE, "gateway_survey_round_note_cleanup_peer")
assert "app_gateway_survey_round_note_cleanup_complete(" in cleanup

# Anchors require both the exact START-result terminal and synchronized release
# deadline before they claim the private UWB worker.
delivery_gate = function_body(ANCHOR_RUNTIME, "pair_start_delivery_ready")
assert "survey_pair_lease_release_start(&pair_lease" in delivery_gate
assert "survey_pair_lease_execution_remaining_ms(" in delivery_gate
survey_worker = function_body(ANCHOR_RUNTIME, "survey_work_handler")
assert "survey_pair_lease_mark_running_at(&pair_lease" in survey_worker

# No production runtime retains a separate GO phase, codec, delivery handle, or
# broadcast submit path. Only the explicit retired wire ID may remain elsewhere.
runtime_source = GLUE + SURVEY + ROUND + ROUND_HEADER + ANCHOR + ANCHOR_RUNTIME
for retired_runtime_symbol in (
    "CMD_SURVEY_GO",
    "survey_round_go_",
    "GO_REQUIRED",
    "round_go_delivery",
):
    assert retired_runtime_symbol not in runtime_source

print("gateway survey synchronized-START glue source invariants passed")
