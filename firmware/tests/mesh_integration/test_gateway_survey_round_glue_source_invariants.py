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
        assert index >= 0, f"missing ordered source invariant: {needle}"
        offset = index + len(needle)


def block_end(source: str, opening_brace: int) -> int:
    depth = 0
    for index in range(opening_brace, len(source)):
        depth += source[index] == "{"
        depth -= source[index] == "}"
        if depth == 0:
            return index
    raise AssertionError("unterminated source block")


# PREPARE and START must both carry the exact nonzero runtime batch identity.
prepare = function_body(SURVEY, "gateway_survey_auto_send_prepare")
start = function_body(SURVEY, "gateway_survey_auto_send_start")
for name, body in (("PREPARE", prepare), ("START", start)):
    assert "gateway_survey_round_active()" in body, (
        f"{name} must add a round ID only for synchronized round operation"
    )
    assert body.count("survey_round_id_append_tlv(") == 1, (
        f"{name} must carry exactly one round identity TLV"
    )
    assert "gateway_survey_round.runtime.batch_sequence" in body, (
        f"{name} must use the current runtime batch identity"
    )


# The round dispatcher must serialize PREPARE/START for every live lane before
# it can enter the separate common-GO branch.
drive = function_body(GLUE, "gateway_survey_round_drive")
assert_ordered(
    drive,
    "gateway_survey_round.phase == APP_GATEWAY_SURVEY_ROUND_DISPATCHING",
    "app_gateway_survey_round_current_control(",
    ".pair = control.pair",
    ".stage = control.stage",
    ".command_id = control.command_id",
    ".target_id = control.target_id",
    "gateway_survey_auto_send_action(&action)",
    "app_gateway_survey_round_go_needed(&gateway_survey_round)",
    "gateway_survey_round_submit_go()",
)

# Starting the first GO copy changes the wrapper phase to OBSERVING. Terminal
# custody therefore has to be serviced from the OBSERVING branch as well as the
# original GO_REQUIRED branch, and a taken terminal clears the exact handle.
observing = drive.index(
    "gateway_survey_round.phase == APP_GATEWAY_SURVEY_ROUND_OBSERVING"
)
observing_open = drive.index("{", observing)
observing_end = block_end(drive, observing_open)
observing_body = drive[observing:observing_end]
assert_ordered(
    observing_body,
    "gateway_survey_round_go_delivery_handle != 0u",
    "app_node_comm_take_delivery_event_for(",
    "gateway_survey_round_go_delivery_handle = 0u",
    "gateway_survey_round_finalize_observation()",
)

control_success = function_body(
    ROUND, "app_gateway_survey_round_note_control_success"
)
assert_ordered(
    control_success,
    "SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR",
    "round->dispatch_stage = SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER",
    "SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER",
    "round->dispatch_stage = SURVEY_GATEWAY_AUTO_START_RESPONDER",
    "SURVEY_GATEWAY_AUTO_START_RESPONDER",
    "round->dispatch_stage = SURVEY_GATEWAY_AUTO_START_INITIATOR",
    "SURVEY_GATEWAY_AUTO_START_INITIATOR",
    "app_gateway_survey_round_advance_dispatch(round)",
)

# Every successful round control remains nonterminal until its response ACK
# has had one complete quiet interval. The glue must arm that boundary before
# it asks drive policy to expose the next PREPARE, START, or common GO action.
round_control_result = function_body(
    GLUE, "gateway_survey_round_note_control_result"
)
successful_round_control = round_control_result[
    round_control_result.index(
        "ret = app_gateway_survey_round_note_control_success("
    ) :
]
assert successful_round_control.count(
    "survey_gateway_response_ack_settle_note_result("
) == 1, "each successful round control must arm one response-ACK settle window"
assert_ordered(
    successful_round_control,
    "app_gateway_survey_round_note_control_success(",
    "survey_gateway_transaction_phase_complete(",
    "gateway_survey_round_sync_auto()",
    "survey_gateway_response_ack_settle_note_result(",
    "gateway_survey_schedule_drive()",
)
assert "K_NO_WAIT" not in successful_round_control, (
    "round control success must not bypass response-ACK settle with immediate work"
)

# A successful START_INITIATOR is the causal pair-start boundary for its exact
# lane. It must enter telemetry custody before ACK settle and before the drive
# scheduler can expose the common GO or a later rerun.
pair_start_guard = successful_round_control.index(
    "if (control.stage == SURVEY_GATEWAY_AUTO_START_INITIATOR)"
)
pair_start_open = successful_round_control.index("{", pair_start_guard)
pair_start_end = block_end(successful_round_control, pair_start_open)
pair_start_body = successful_round_control[
    pair_start_guard : pair_start_end + 1
]
assert_ordered(
    pair_start_body,
    "GATEWAY_COMMAND_EVENT_STAGE_PAIR_START",
    "CMD_SURVEY_START_PAIR",
    "event.pair_initiator_id = control.pair.initiator_id",
    "event.pair_responder_id = control.pair.responder_id",
    "app_gateway_survey_observability_submit_boundary(",
)
after_pair_start = successful_round_control[pair_start_end + 1 :]
assert_ordered(
    after_pair_start,
    "gateway_survey_round_sync_auto()",
    "survey_gateway_response_ack_settle_note_result(",
    "gateway_survey_schedule_drive()",
)
assert successful_round_control.index(
    "app_gateway_survey_observability_submit_boundary("
) < successful_round_control.index(
    "survey_gateway_response_ack_settle_note_result("
), "pair-start custody must precede ACK settle and any next-phase scheduling"

# The scheduler treats only immediately executable round phases as runnable.
# OBSERVING remains represented by pair_observation_active, which selects the
# bounded poll path and prevents a zero-delay workqueue spin.
drive_state = function_body(SURVEY, "gateway_survey_drive_state")
assert_ordered(
    drive_state,
    ".pair_observation_active =",
    "gateway_survey_pair_observation_active",
    ".round_drive_ready =",
    "APP_GATEWAY_SURVEY_ROUND_DISPATCHING",
    "APP_GATEWAY_SURVEY_ROUND_GO_REQUIRED",
    "APP_GATEWAY_SURVEY_ROUND_BATCH_COMPLETE",
    ".round_go_delivery_pending =",
    "gateway_survey_round_go_delivery_handle != 0u",
)
assert "APP_GATEWAY_SURVEY_ROUND_OBSERVING" not in drive_state, (
    "OBSERVING must poll as an external wait rather than busy-spin as runnable"
)


# GO is one broadcast for the complete armed batch, carries that same round
# identity, and requests no per-anchor responses that could serialize lanes.
build_go = function_body(GLUE, "gateway_survey_round_build_go")
assert_ordered(
    build_go,
    ".survey_id = gateway_survey_context.survey_id",
    ".round_id = gateway_survey_round.runtime.batch_sequence",
    "survey_round_go_append_tlvs(",
    "CMD_SCOPE_ALL_HEARD",
    "CMD_RESPONSE_NONE",
    "survey_round_go_init_packet(",
    "outbound->packet.dst_id = MESH_BROADCAST_ID",
)
assert "outbound->packet.ttl = FLOOD_EPOCH_GLOBAL_TTL" in build_go

submit_go = function_body(GLUE, "gateway_survey_round_submit_go")
assert_ordered(
    submit_go,
    "execute_delay_ms = survey_round_go_execute_delay_ms(",
    "now_ms = k_uptime_get_32()",
    "command_seq = gateway_next_command_seq()",
    "uptime_ms_until_deadline(now_ms",
    "gateway_survey_operation_deadline_ms",
    "gateway_survey_round_build_go(",
    "app_node_comm_submit_delivery(",
    "NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD",
    "gateway_survey_round_observation_deadline_ms = now_ms + required_ms",
)
assert submit_go.index(
    "uptime_ms_until_deadline(now_ms"
) < submit_go.index(
    "app_node_comm_submit_delivery("
), "every regenerated GO must be bounded by the live survey operation deadline"


# A failed control owns only its lane. It may queue that lane for cleanup or a
# rerun, but cannot reset or terminalize the rest of the active batch.
control_failure = function_body(
    ROUND, "app_gateway_survey_round_note_control_failure"
)
assert_ordered(
    control_failure,
    "app_gateway_survey_round_current_control(round, &control)",
    "survey_pair_round_runtime_require_cleanup(&round->runtime",
    "control.lane_index",
    "*lane_index = control.lane_index",
    "app_gateway_survey_round_advance_dispatch(round)",
)
assert "memset(round" not in control_failure
assert "survey_pair_round_runtime_complete(" not in control_failure

fail_glue = function_body(GLUE, "gateway_survey_round_fail_current_control")
assert_ordered(
    fail_glue,
    "app_gateway_survey_round_note_control_failure(",
    "&lane_index",
    "gateway_survey_round_cleanup_lane_index = lane_index",
)

finalize = function_body(GLUE, "gateway_survey_round_finalize_observation")
assert finalize.count("app_gateway_survey_round_finalize_lane(") == 2, (
    "success and failure must each finalize only the current lane"
)
assert_ordered(
    finalize,
    "for (size_t i = 0u; i < lane_count; i++)",
    "app_gateway_survey_round_finalize_lane(",
    "if (!app_gateway_survey_round_batch_complete(&gateway_survey_round))",
    "app_gateway_survey_round_advance_batch(",
)
assert_ordered(
    finalize,
    "app_gateway_survey_round_advance_batch(",
    "gateway_survey_round_release_go_delivery()",
)
assert "gateway_survey_round_go_delivery_handle = 0u" not in finalize, (
    "batch advance must release rather than silently erase an unreaped GO handle"
)

release_go = function_body(GLUE, "gateway_survey_round_release_go_delivery")
assert_ordered(
    release_go,
    "gateway_survey_round_go_delivery_handle == 0u",
    "return;",
    "app_node_comm_abandon_delivery(",
    "gateway_survey_round_go_delivery_handle",
    "gateway_survey_round_go_delivery_handle = 0u",
)

reset_round = function_body(GLUE, "gateway_survey_round_reset")
assert_ordered(
    reset_round,
    "gateway_survey_round_release_go_delivery()",
    "memset(&gateway_survey_round",
)


# The gateway may observe a batch only after the common GO has really started
# RF. On each anchor, START remains a reservation until the lease reports both
# START-result delivery and the matching GO; only then can DS-TWR start.
ensure_observing = function_body(GLUE, "gateway_survey_round_ensure_observing")
assert_ordered(
    ensure_observing,
    "app_gateway_survey_round_go_needed(&gateway_survey_round)",
    "gateway_survey_round_go_delivery_handle == 0u",
    "app_node_comm_delivery_attempts_started(",
    "attempts_started == 0u",
    "app_gateway_survey_round_mark_observing_after_go(",
)

# GO remains live through transient facade admission pressure. A retryable
# submission cannot enter the terminal survey branch, and the delayed retry
# must rebuild against a later current time and fresh command sequence.
go_branch_start = drive.index(
    "if (app_gateway_survey_round_go_needed(&gateway_survey_round))"
)
go_branch_open = drive.index("{", go_branch_start)
go_branch_end = block_end(drive, go_branch_open)
go_branch = drive[go_branch_start : go_branch_end + 1]
submit_failure_start = go_branch.index("if (ret < 0)")
submit_failure_open = go_branch.index("{", submit_failure_start)
submit_failure_end = block_end(go_branch, submit_failure_open)
submit_failure = go_branch[
    submit_failure_start : submit_failure_end + 1
]
submit_retry_start = submit_failure.index(
    "if (app_gateway_survey_round_go_submit_retryable(ret))"
)
submit_retry_open = submit_failure.index("{", submit_retry_start)
submit_retry_end = block_end(submit_failure, submit_retry_open)
submit_retry = submit_failure[
    submit_retry_start : submit_retry_end + 1
]
assert_ordered(
    submit_retry,
    "app_gateway_survey_round_go_submit_retryable(ret)",
    "k_work_reschedule(",
    "GATEWAY_SURVEY_TRANSACTION_POLL_MS",
)
assert "gateway_survey_auto_finish_status(" not in submit_retry, (
    "transient GO admission pressure must retain the active round"
)
assert submit_retry_end < submit_failure.index(
    "gateway_survey_auto_finish_status("
), "only nonretryable GO submission failures may terminalize the survey"

# Taking the terminal event releases node-communication custody. If no RF
# attempt started for a retryable reason, retire the local handle and schedule
# another drive without marking the round observing or emitting a terminal.
zero_rf_retry_start = go_branch.index(
    "if (app_gateway_survey_round_go_terminal_retryable("
)
zero_rf_retry_open = go_branch.index("{", zero_rf_retry_start)
zero_rf_retry_end = block_end(go_branch, zero_rf_retry_open)
zero_rf_retry = go_branch[
    zero_rf_retry_start : zero_rf_retry_end + 1
]
assert go_branch.index(
    "app_node_comm_take_delivery_event_for("
) < zero_rf_retry_start, (
    "zero-RF retry may release its local handle only after taking the exact "
    "terminal event"
)
assert_ordered(
    zero_rf_retry,
    "app_gateway_survey_round_go_terminal_retryable(",
    "event.reason",
    "event.attempts_started",
    "gateway_survey_round_go_delivery_handle = 0u",
    "k_work_reschedule(",
    "GATEWAY_SURVEY_TRANSACTION_POLL_MS",
    "return true;",
)
assert "gateway_survey_auto_finish_status(" not in zero_rf_retry
assert "app_gateway_survey_round_mark_observing_after_go(" not in zero_rf_retry
assert zero_rf_retry_end < go_branch.index(
    "if (event.attempts_started == 0u)"
), "retryable zero-RF terminals must be regenerated before terminal fallback"

delivery_gate = function_body(ANCHOR_RUNTIME, "pair_start_delivery_ready")
assert "delivery_confirmed &&" in delivery_gate
assert "survey_pair_lease_ready_snapshot(&pair_lease, NULL)" in delivery_gate

survey_worker = function_body(ANCHOR_RUNTIME, "survey_work_handler")
running_claim = re.search(
    r"survey_pair_lease_mark_running\s*\(\s*&pair_lease\s*,\s*"
    r"&pair\s*,\s*&pair_round_id\s*\)",
    survey_worker,
)
assert running_claim is not None, (
    "the RUNNING transition must return pair and round as one ownership snapshot"
)
assert_ordered(
    survey_worker,
    "if (!pair_start_delivery_ready())",
    "return;",
    "survey_pair_lease_ready_snapshot(&pair_lease, &pair)",
    'radio_guard_uwb_start("survey pair DS-TWR")',
    "survey_pair_lease_mark_running(&pair_lease",
    "as_responder = pair.responder_id == DEVICE_ID",
    "run_pair_responder(&pair, pair_round_id)",
    "run_pair_initiator(&pair, pair_round_id)",
)
assert "pair_round_id = pair_lease.round_id" not in survey_worker, (
    "a pre-claim round snapshot could mismatch the operation that enters RUNNING"
)

for run_name in ("run_pair_initiator", "run_pair_responder"):
    run_pair = function_body(ANCHOR_RUNTIME, run_name)
    declaration = run_pair[: run_pair.index("{")]
    assert "uint16_t round_id" in declaration
    assert_ordered(
        run_pair,
        "runtime_ops.queue_sample_result(pair",
        "round_id",
        "sample_index",
    )
