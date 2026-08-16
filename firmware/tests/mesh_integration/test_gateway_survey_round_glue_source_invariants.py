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
COORDINATION = (ROOT / "app/src/app_mesh_report_coordination.inc").read_text(
    encoding="utf-8"
)
ANCHOR = (ROOT / "app/src/app_anchor.c").read_text(encoding="utf-8")
SURVEY_CORE = (ROOT / "src/survey.c").read_text(encoding="utf-8")
ANCHOR_RUNTIME = (ROOT / "app/src/app_anchor_survey_runtime.c").read_text(
    encoding="utf-8"
)
ANCHOR_COMMANDS = (ROOT / "app/src/app_anchor_commands.inc").read_text(
    encoding="utf-8"
)
APP_CONFIG = (ROOT / "app/src/app_config.h").read_text(encoding="utf-8")
MESH_REPORT = (ROOT / "app/src/app_mesh_report.c").read_text(encoding="utf-8")
NODE_COMM = (ROOT / "src/node_comm.c").read_text(encoding="utf-8")
OPERATION_POLICY = (ROOT / "include/operation_policy.h").read_text(
    encoding="utf-8"
)
SURVEY_HEADER = (ROOT / "include/survey.h").read_text(encoding="utf-8")
GUI_OPERATION_POLICY = (
    ROOT.parent / "tools/gateway_gui/operation_policy.py"
).read_text(encoding="utf-8")


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
    "APP_GATEWAY_SURVEY_CONTROL_PREPARE_INITIATOR",
    "APP_GATEWAY_SURVEY_CONTROL_PREPARE_RESPONDER",
    "APP_GATEWAY_SURVEY_CONTROL_START_RESPONDER",
    "APP_GATEWAY_SURVEY_CONTROL_START_INITIATOR",
)
success = function_body(ROUND, "app_gateway_survey_round_note_control_success")
assert_ordered(
    success,
    "case APP_GATEWAY_SURVEY_CONTROL_PREPARE_INITIATOR:",
    "round->dispatch_stage = APP_GATEWAY_SURVEY_CONTROL_PREPARE_RESPONDER",
    "case APP_GATEWAY_SURVEY_CONTROL_PREPARE_RESPONDER:",
    "round->dispatch_stage = APP_GATEWAY_SURVEY_CONTROL_START_RESPONDER",
    "case APP_GATEWAY_SURVEY_CONTROL_START_RESPONDER:",
    "round->dispatch_stage = APP_GATEWAY_SURVEY_CONTROL_START_INITIATOR",
    "case APP_GATEWAY_SURVEY_CONTROL_START_INITIATOR:",
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
# START initiator is also submitted immediately so hop-2/3 ACK_CONFIRM cannot
# consume the 15 s barrier before the second START is on the air.
send_start = function_body(SURVEY, "gateway_survey_send_start")
assert_ordered(
    send_start,
    "control->stage == APP_GATEWAY_SURVEY_CONTROL_START_RESPONDER",
    ".started_at_ms = now_ms",
    "message_age_ms = 0u",
    "message_age_ms = now_ms -",
    "gateway_survey_start_release.started_at_ms",
    "survey_round_start_initiator_send_allowed(",
    "gateway_survey_start_release.initiator_submitted",
    "TLV_EXECUTE_DELAY_MS",
    "SURVEY_ROUND_START_EXECUTE_DELAY_MS",
    "outbound.packet.message_age_ms = message_age_ms",
    "outbound.queued_at_ms = now_ms",
    "outbound.queued_at_valid = true",
    "gateway_survey_send_outbound(&outbound",
    "release_ms =",
    "gateway_survey_start_release.started_at_ms +",
    "SURVEY_ROUND_START_EXECUTE_DELAY_MS",
    "survey_gateway_observation_origin_freeze(",
    "&gateway_survey_round_observation_origin, release_ms",
    "outbound.packet.dst_id = control->pair.initiator_id",
    "gateway_survey_send_pair_control(",
    "initiator_submitted = true",
)

# An accepted control result retires only its request-delivery handle. The
# persistent round owner retains the complete semantic identity and advances
# exactly once when the matching terminal ACK_CONFIRM reaches the gateway.
result = function_body(GLUE, "gateway_survey_round_note_control_result")
assert_ordered(
    result,
    "app_gateway_survey_round_capture_control_result(",
    "survey_gateway_transaction_phase_complete(",
)
assert "app_gateway_survey_round_note_control_success(" not in result
assert "response_ack_settle" not in result
confirmation = function_body(
    GLUE, "gateway_survey_round_apply_control_confirmation"
)
assert_ordered(
    confirmation,
    "app_gateway_survey_round_control_confirmation_ready(",
    "app_gateway_survey_round_note_control_success(",
    "app_gateway_survey_round_clear_control_confirmation(",
    "gateway_survey_schedule_drive()",
)
assert "gateway_survey_round_fail_current_control(" in confirmation
ack_confirm = function_body(SURVEY, "gateway_note_survey_ack_confirm")
assert "app_gateway_survey_round_note_control_ack_confirm(" in ack_confirm
assert "gateway_survey_work_schedule(" in ack_confirm
assert "SURVEY_GATEWAY_DUE_CONTROL_DELIVERY, 0u" in ack_confirm
ack_commit = function_body(COORDINATION, "mesh_gateway_accept_semantic_delivery")
assert_ordered(
    ack_commit,
    "mesh_relay_gateway_ack_confirm_history_match(",
    "mesh_report_gateway_note_ack_confirm(",
)
preflight_result = function_body(SURVEY, "gateway_survey_preflight_result")
assert "mesh_packet_semantic_digest(" in preflight_result
failure = function_body(GLUE, "gateway_survey_round_fail_current_control")
failure_call = failure.index("app_gateway_survey_round_note_control_failure(")
failure_arguments = failure[failure_call : failure.index("&lane_index", failure_call)]
assert "operation_session_id" in failure_arguments
assert "gateway_survey_context.survey_id" not in failure_arguments

round_drive = function_body(GLUE, "gateway_survey_round_drive")
assert_ordered(
    round_drive,
    "gateway_survey_round_apply_control_confirmation()",
    "app_gateway_survey_round_control_confirmation_pending(",
    "gateway_survey_control_inflight()",
    "app_gateway_survey_round_current_control(",
    "gateway_survey_send_control(&control)",
)
cleanup_service = function_body(SURVEY, "gateway_survey_service_cleanup")
assert "gateway_survey_round.control_confirmation.valid" in cleanup_service
assert "APP_GATEWAY_SURVEY_ROUND_DISPATCHING" in cleanup_service
assert "gateway_survey_transaction.pair_loaded" in cleanup_service

# Once the last START result is confirmed, the round's OBSERVING phase feeds
# the bounded observation-poll policy directly.
drive_state = function_body(SURVEY, "gateway_survey_drive_state")
observation_mapping = drive_state[
    drive_state.index(".round_observing =") :
    drive_state.index(".round_drive_ready =")
]
assert "gateway_survey_round.phase" in observation_mapping
assert "APP_GATEWAY_SURVEY_ROUND_OBSERVING" in observation_mapping
assert ".control_inflight = gateway_survey_control_inflight()" in drive_state

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
assert "SURVEY_GATEWAY_DUE_BOUNDARY_POLL" in poll_wait_branch
assert "gateway_survey_work_schedule(\n            SURVEY_GATEWAY_DUE_BOUNDARY_POLL, 0u" not in poll_wait_branch

# Failed controls own cleanup only for their lane, and final observation cannot
# erase cleanup debt before both endpoint outcomes settle.
failure = function_body(ROUND, "app_gateway_survey_round_note_control_failure")
assert "survey_pair_round_runtime_require_cleanup(" in failure
assert "memset(round" not in failure
finalize = function_body(GLUE, "gateway_survey_round_finalize_observation")
assert "app_gateway_survey_round_finalize_lane(" in finalize
cleanup = function_body(GLUE, "gateway_survey_round_note_cleanup_peer")
assert "app_gateway_survey_round_note_cleanup_complete(" in cleanup

# The old sequential finalizer and its parallel masks are deleted. Keeping
# them beside the round would recreate two owners for one pair observation.
for retired in (
    "gateway_survey_finalize_pair_observation",
    "enum gateway_survey_pair_finalize_result",
    "gateway_survey_pair_responder_usable_mask",
    "gateway_survey_pair_observation_active",
):
    assert retired not in SURVEY

# Anchors arm from exact local START acceptance and leave the status packet in
# independent auto-reap custody; only the synchronized release deadline gates
# the private UWB worker.
delivery_gate = function_body(ANCHOR_RUNTIME, "pair_start_delivery_ready")
assert "survey_pair_lease_release_start(&pair_lease" in delivery_gate
result_submit = function_body(ANCHOR_COMMANDS, "anchor_submit_command_result")
assert "app_node_comm_commit_protocol_response_auto_reap(" in result_submit
assert "app_node_comm_auto_reap_delivery" not in delivery_gate
assert "app_node_comm_take_delivery_event_for" not in delivery_gate
assert "survey_pair_lease_execution_remaining_for_role_ms(" in delivery_gate
survey_worker = function_body(ANCHOR_RUNTIME, "survey_work_handler")
assert "survey_pair_lease_mark_running_for_role_at(&pair_lease" in survey_worker

# Discovery START is a timed broadcast, so every relay must forward it before
# claiming its own discovery RF window. A retryable forward remains retained
# in the eight-retry C5 lane, but the preceding Here-I-Am and exact assignment
# have already proven the route. Four fixed-origin redrives occur at 2, 4, 6,
# and 8 seconds before the 20-second execution boundary. A node that exhausts
# the full retry lane is reported absent instead of extending every survey:
#
#   one relay = 1400 ms flood wave + 40 ms initial failed turnaround
#             + (300 + 600 + 1200 + 5 * 2400) ms retry backoff
#             + 7 * (400 ms wake train + 40 ms failed turnaround)
#             + 400 ms final wake + 40 ms turnaround + 3 * 40 ms repeats
#             = 19,180 ms
#   three relays + 1,103 ms transport/PHY preparation = 58,643 ms.
assert "#define MESH_C5_DEFERRED_MAX_RETRIES 8u" in MESH_REPORT
bounded_control = NODE_COMM[
    NODE_COMM.index("[NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD]") :
    NODE_COMM.index("[NODE_COMM_PROFILE_RELIABLE_UPLINK]")
]
assert ".retry_delay_ms = 200u" in bounded_control
assert ".retry_backoff_shift_cap = 3u" in bounded_control
retry_maxima_ms = (300, 600, 1200, 2400, 2400, 2400, 2400, 2400)
per_relay_ms = 1400 + 40 + sum(retry_maxima_ms) + 7 * (400 + 40) + 400 + 40 + 3 * 40
transport_preempt_budget_ms = 1000
phy_setup_and_margin_ms = 63 + 40
phy_prep_budget_ms = transport_preempt_budget_ms + phy_setup_and_margin_ms
full_ttl_lead_ms = 3 * per_relay_ms + phy_prep_budget_ms
assert per_relay_ms == 19_180
assert phy_prep_budget_ms == 1_103
assert full_ttl_lead_ms == 58_643
origin_redrive_count = 4
control_hop_budget_ms = 2_000
default_start_delay_ms = 20_000
origin_redrive_offsets_ms = [
    wave * control_hop_budget_ms
    for wave in range(1, origin_redrive_count + 1)
]
assert origin_redrive_offsets_ms == [2_000, 4_000, 6_000, 8_000]
assert (
    origin_redrive_offsets_ms[-1]
    + 4 * control_hop_budget_ms
    + phy_prep_budget_ms
    < default_start_delay_ms
)
assert full_ttl_lead_ms > default_start_delay_ms
assert "#define SURVEY_DISCOVERY_ORIGIN_REDRIVE_COUNT 4u" in SURVEY_HEADER
assert "OPERATION_POLICY_DISCOVERY_START_DELAY_MIN_MS 20000u" in OPERATION_POLICY
assert "OPERATION_POLICY_DISCOVERY_DEFAULT_START_DELAY_MS 20000u" in OPERATION_POLICY
assert "OPERATION_POLICY_DISCOVERY_START_DELAY_MAX_MS 20000u" in OPERATION_POLICY
assert re.search(
    r"#define\s+SURVEY_DISCOVERY_START_DELAY_MS\s+\\?\s*"
    r"OPERATION_POLICY_DISCOVERY_DEFAULT_START_DELAY_MS",
    APP_CONFIG,
)
assert "DISCOVERY_START_DELAY_MIN_MS = 20_000" in GUI_OPERATION_POLICY
assert "DISCOVERY_START_DELAY_MAX_MS = 20_000" in GUI_OPERATION_POLICY
assert "DISCOVERY_DEFAULT_START_DELAY_MS = 20_000" in GUI_OPERATION_POLICY

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
