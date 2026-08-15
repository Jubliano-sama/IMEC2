#!/usr/bin/env python3
"""Guard the app-level survey failure paths that native core tests cannot link."""

from pathlib import Path
import re

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
DISCOVERY = (ROOT / "app/src/app_anchor_survey_discovery.c").read_text()
RESULT_DELIVERY = (
    ROOT / "app/src/app_anchor_survey_result_delivery.c"
).read_text()
RUNTIME = (ROOT / "app/src/app_anchor_survey_runtime.c").read_text()
RUNTIME_HEADER = (ROOT / "app/src/app_anchor_survey_runtime.h").read_text()
ANCHOR = read_composed_source(ROOT / "app/src/app_anchor.c")
MESH_REPORT = read_composed_source(ROOT / "app/src/app_mesh_report.c")
DRIVER = read_composed_source(ROOT / "app/src/dwm3000_driver.c")
DRIVER_HEADER = (ROOT / "app/src/dwm3000_driver.h").read_text()
CORE_SURVEY = (ROOT / "src/survey.c").read_text()
STACK_BUDGET = (ROOT / "include/stack_budget.h").read_text()


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


def braced_block_at(source: str, marker_index: int) -> str:
    brace = source.index("{", marker_index)
    depth = 0
    for index in range(brace, len(source)):
        depth += source[index] == "{"
        depth -= source[index] == "}"
        if depth == 0:
            return source[brace : index + 1]
    raise AssertionError("unterminated block")


for source, helper_name in (
    (DISCOVERY, "schedule_work_ms"),
    (RESULT_DELIVERY, "result_delivery_schedule"),
):
    schedule_helper = function_body(source, helper_name)
    assert "return ret < 0 ? ret : 0;" in schedule_helper, (
        f"{helper_name} must not expose Zephyr's positive accepted-work "
        "status through terminal-cleanup callback APIs"
    )


run = function_body(DISCOVERY, "app_anchor_survey_discovery_run")
assert run.count("send_local_survey_probe(") == 1
assert "dwm3000_driver_send_frame(" not in run
assert "while (deferred_mask != 0u" not in run
assert "schedule_survey_probe_retry(" not in run
assert "survey_discovery_probe_real_attempt_count(" not in run
assert "COMMAND_RADIO_ERROR" not in run
assert re.search(
    r"for\s*\(uint8_t opportunity\s*=\s*0u;\s*"
    r"opportunity\s*<\s*config->round_count",
    run,
), "the runtime profile must own the exact announce/listen round count"
send_index = run.index("send_local_survey_probe(")
ensure_index = run.index("dwm3000_driver_ensure_wake_mode(", send_index)
listen_index = run.index("receive_survey_probes_until(", ensure_index)
assert send_index < ensure_index < listen_index
abort_index = run.index("if (abort_requested())")
abort_return = run.index("return -ECANCELED", abort_index)
success_report_index = run.rindex("prepare_discovery_report(")
assert abort_index < abort_return < success_report_index
assert re.search(
    r"prepare_discovery_report\s*\(\s*config->operation_generation\s*,\s*"
    r"config->survey_id\s*,\s*"
    r"entries\s*,\s*entry_count\s*,.*?COMMAND_OK\s*\)",
    run[success_report_index:],
    re.S,
), "every non-aborted completed window must stage its useful peer set"

probe_send = function_body(DISCOVERY, "send_local_survey_probe")
assert "dwm3000_driver_send_frame_tracked_until(" in probe_send
assert "rf_started" in probe_send
assert "absolute_tx_deadline_ms" in probe_send
assert "dwm3000_driver_send_frame_tracked_until" in DRIVER_HEADER

handle_start = function_body(
    DISCOVERY, "app_anchor_survey_discovery_handle_start"
)
assert "packet->flags != FLAG_DIAGNOSTIC" in handle_start, (
    "survey discovery admission must reject extra or missing mode flags even "
    "when called outside the mesh relay"
)

tracked_send = function_body(DRIVER, "dwm3000_driver_send_frame_tracked")
assert "dwm3000_driver_send_frame_tracked_until(" in tracked_send
assert "*rf_started = observation.rf_started" in tracked_send
tracked_until = function_body(
    DRIVER, "dwm3000_driver_send_frame_tracked_until"
)
gate_index = tracked_until.index("now_ms >= absolute_deadline_ms")
start_call_index = tracked_until.index("send_range_frame_until(")
completion_index = tracked_until.index("wait_tx_complete_observed(")
assert gate_index < start_call_index < completion_index, (
    "an already-expired absolute deadline must be rejected before frame staging, "
    "before completion polling"
)
assert "&result.rf_started" in tracked_until[
    start_call_index:completion_index
]
assert "&result.rf_started_at_ms" in tracked_until[
    start_call_index:completion_index
], "the low-level command boundary must return conservative RF accounting"
start_failure_index = tracked_until.index("if (ret < 0)", start_call_index)
start_failure_return = tracked_until.index("return ret", start_failure_index)
assert "*observation = result" in tracked_until[
    start_failure_index:start_failure_return
], "an ambiguous command-transfer failure must preserve possible-RF accounting"
send_until = function_body(DRIVER, "send_range_frame_until")
staging_index = send_until.index("write_tx_frame(")
device_time_index = send_until.index("dwt_readsystimestamphi32()")
deadline_gate_index = send_until.index(
    "absolute_deadline_ms - host_now_ms <= lead_ms"
)
program_index = send_until.index("dwt_setdelayedtrxtime(")
possible_index = send_until.index("*rf_start_possible = true")
rf_start_index = send_until.index("start_ret = dwt_starttx(")
assert (
    staging_index
    < device_time_index
    < deadline_gate_index
    < program_index
    < possible_index
    < rf_start_index
), (
    "deadline-bound sends must stage first, program an in-window hardware TX, "
    "and account possible RF before transferring the command"
)
assert "effective_tx_mode |= DWT_START_TX_DELAYED" in send_until
legacy_send = function_body(DRIVER, "dwm3000_driver_send_frame")
assert "dwm3000_driver_send_frame_tracked(" in legacy_send

worker = function_body(RUNTIME, "survey_work_handler")
run_call_index = worker.index("app_anchor_survey_discovery_run(")
failure_guard_index = worker.index("if (ret < 0 &&", run_call_index)
abort_guard_index = worker.index(
    "!app_anchor_survey_runtime_abort_requested()", failure_guard_index
)
empty_report_index = worker.index(
    "app_anchor_survey_discovery_stage_empty_report(", abort_guard_index
)
failure_block = braced_block_at(worker, failure_guard_index)
assert run_call_index < failure_guard_index < abort_guard_index < empty_report_index
assert worker.count("app_anchor_survey_discovery_stage_empty_report(") == 1, (
    "one failed, non-aborted discovery run must stage exactly one fail-safe report"
)
assert failure_block.count("app_anchor_survey_discovery_stage_empty_report(") == 1

retry_helper = function_body(RUNTIME, "survey_rf_retry_delay_ms")
assert "app_node_comm_retry_identity_backoff_ms(" in retry_helper
assert "state->retry_round++" in retry_helper
assert "uptime_deadline_reached(now_ms, absolute_deadline_ms)" in retry_helper
assert "uptime_ms_until_deadline(now_ms, absolute_deadline_ms)" in retry_helper
assert "*delay_ms_out = remaining_ms" in retry_helper

pair_retry = function_body(RUNTIME, "schedule_pair_rf_retry")
pair_retry_expiry = pair_retry.index("survey_rf_retry_delay_ms(")
pair_retry_capture = pair_retry.index(
    "delivery_handle = pair_start_delivery_handle", pair_retry_expiry
)
pair_retry_detach = pair_retry.index(
    "pair_start_delivery_handle = 0u", pair_retry_capture
)
pair_retry_unlock = pair_retry.index(
    "k_spin_unlock(&survey_lock, key)", pair_retry_detach
)
pair_retry_abandon = pair_retry.index(
    "app_anchor_survey_runtime_abandon_pair_start_delivery(",
    pair_retry_unlock,
)
assert (
    pair_retry_expiry < pair_retry_capture < pair_retry_detach <
    pair_retry_unlock < pair_retry_abandon
), (
    "an expired pair RF retry must detach the exact START-result handle under "
    "the lease lock and abandon it only after unlocking"
)

discovery_run_index = worker.index("if (run_discovery)")
discovery_run_block = braced_block_at(worker, discovery_run_index)
discovery_guard_index = discovery_run_block.index("radio_guard_uwb_claim(")
discovery_defer_index = discovery_run_block.index(
    "if (ret < 0)", discovery_guard_index
)
discovery_defer_block = braced_block_at(
    discovery_run_block, discovery_defer_index
)
assert "survey_rf_retry_delay_ms(" in discovery_defer_block
assert "app_node_comm_restart_role_scan()" in discovery_defer_block
assert "REPORT_TX_RETRY_DELAY_MS" not in discovery_defer_block
abort_clear = discovery_defer_block.index(
    "discovery_generation_active = false"
)
abort_scan = discovery_defer_block.index(
    "runtime_ops.start_uwb_scan()", abort_clear
)
assert abort_clear < abort_scan, (
    "an aborted discovery whose low-duty scan was preempted must restore that "
    "scan even though no discovery radio run reaches the normal exit"
)

pair_owner_busy_index = worker.index("if (anchor_uwb_window_active()")
pair_owner_busy_block = braced_block_at(worker, pair_owner_busy_index)
assert "schedule_pair_rf_retry(" in pair_owner_busy_block
assert "REPORT_TX_RETRY_DELAY_MS" not in pair_owner_busy_block

result_custody_index = worker.index(
    "if (as_responder &&\n"
    "        app_anchor_survey_result_delivery_occupied_count() > 0u)"
)
result_custody_block = braced_block_at(worker, result_custody_index)
assert "app_anchor_survey_result_delivery_service()" in result_custody_block
assert "SURVEY_NON_RF_SERVICE_POLL_MS" in result_custody_block
assert "schedule_pair_rf_retry(" not in result_custody_block

reserve_results_index = worker.index(
    "app_node_comm_reserve_durable_reliable_uplinks(",
    result_custody_index,
)
reserve_failure_index = worker.index("if (ret < 0)", reserve_results_index)
reserve_failure_block = braced_block_at(worker, reserve_failure_index)
assert "app_anchor_survey_result_delivery_service()" in reserve_failure_block
assert "SURVEY_NON_RF_SERVICE_POLL_MS" in reserve_failure_block
assert "schedule_pair_rf_retry(" not in reserve_failure_block
reserve_owner_index = worker.rfind("if (as_responder)",
                                   result_custody_index,
                                   reserve_results_index)
assert reserve_owner_index >= 0, (
    "only the responder may reserve the five durable pair-result owners"
)

initiator = function_body(RUNTIME, "run_pair_initiator")
responder = function_body(RUNTIME, "run_pair_responder")
assert "queue_sample_result" not in initiator, (
    "the initiator performs RF exchanges but must not create result custody"
)
assert "delivery_reservation_leases" not in initiator
assert "queue_sample_result" in responder
assert "delivery_reservation_leases" in responder
assert "&delivery_reservation_leases[" in responder
assert "memset(&delivery_reservation_leases[sample_index]" in responder
assert (
    "const struct app_node_comm_reservation_lease *delivery_reservation"
    in RUNTIME_HEADER
), "the runtime callback must transfer the complete reservation capability"
assert (
    "const struct app_node_comm_reservation_lease *delivery_reservation"
    in ANCHOR
), "the anchor adapter must stage through the exact reservation capability"
assert (
    "delivery_reservation->owner_generation != pair->operation_generation"
    in ANCHOR
), "the anchor adapter must reject a lease from another survey generation"
stage_reserved = function_body(
    RESULT_DELIVERY, "app_anchor_survey_result_delivery_stage_reserved"
)
assert "APP_NODE_COMM_RESERVATION_OWNER_SURVEY_RESULT" in stage_reserved
assert "sample.pair.operation_generation !=\n            delivery_reservation->owner_generation" in stage_reserved
assert "app_node_comm_commit_durable_reliable_uplink_reservation(\n        delivery_reservation" in stage_reserved
reserve_call = worker[reserve_results_index:reserve_failure_index]
assert "pair.operation_generation" in reserve_call
assert reserve_call.index("pair.operation_generation") < reserve_call.index(
    "pair.sample_count"
), "survey reservations must belong to the pair operation generation"

pair_guard_index = worker.index(
    "radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_ANCHOR_SURVEY,",
    discovery_guard_index + 1,
)
pair_defer_index = worker.index("if (ret < 0)", pair_guard_index)
pair_defer_block = braced_block_at(worker, pair_defer_index)
assert "schedule_pair_rf_retry(" in pair_defer_block
assert "REPORT_TX_RETRY_DELAY_MS" not in pair_defer_block
pair_poison_index = pair_defer_block.index("if (radio_guard_uwb_poisoned())")
pair_poison_block = braced_block_at(pair_defer_block, pair_poison_index)
assert "app_anchor_survey_result_delivery_cancel_reservations(" in pair_poison_block
assert "delivery_reservation_leases" in pair_poison_block
assert "return;" in pair_poison_block, (
    "a poisoned radio guard must release the exact outstanding survey "
    "reservations before it suppresses retry/rearm"
)
claim = re.search(
    r"survey_pair_lease_mark_running_for_role_at\s*\(\s*&pair_lease\s*,\s*"
    r"k_uptime_get_32\s*\(\s*\)\s*,\s*"
    r"as_responder\s*,\s*"
    r"&pair\s*,\s*&pair_round_id\s*\)",
    worker,
)
assert claim is not None, (
    "RUNNING must validate its execution deadline and atomically return the "
    "final pair and synchronized round"
)
assert "survey_pair_lease_mark_running(&pair_lease" not in worker
role_index = worker.index("as_responder = pair.responder_id == DEVICE_ID")
assert (
    pair_owner_busy_index
    < role_index
    < result_custody_index
    < reserve_results_index
    < pair_guard_index
    < pair_defer_index
    < claim.start()
)
assert "as_responder != (pair.responder_id == DEVICE_ID)" in worker[
    claim.start():
], (
    "the atomic RUNNING transition must revalidate the role used for result "
    "capacity admission"
)
assert "pair_round_id = pair_lease.round_id" not in worker, (
    "the round generation must come from the atomic RUNNING transition"
)
assert worker.count("schedule_pair_rf_retry(") == 2
assert "REPORT_TX_RETRY_DELAY_MS" not in worker
assert worker.count("SURVEY_NON_RF_SERVICE_POLL_MS") == 4, (
    "discovery report-stage retries plus pair-result custody and five-record "
    "admission pressure must use non-RF service polling without consuming "
    "radio retry rounds"
)

empty_report = function_body(
    DISCOVERY, "app_anchor_survey_discovery_stage_empty_report"
)
assert empty_report.count("prepare_discovery_report(") == 1
assert re.search(
    r"prepare_discovery_report\s*\(\s*"
    r"config->operation_generation\s*,\s*config->survey_id\s*,\s*"
    r"NULL\s*,\s*0u\s*,",
    empty_report,
), "the fail-safe report must contain a valid zero-peer reachability list"

failed_abandon = function_body(
    DISCOVERY, "survey_delivery_service_failed_abandon"
)
assert "survey_delivery_failed_abandon_handle" in failed_abandon
assert "app_node_comm_abandon_delivery(handle)" in failed_abandon
assert "ret != -ENOENT && ret != -EALREADY" in failed_abandon
assert failed_abandon.index("app_watchdog_stop_feeding()") < (
    failed_abandon.index("return ret")
), "unexpected orphan-cleanup failures must force bounded reset recovery"

report_retry = function_body(
    DISCOVERY, "app_anchor_survey_discovery_retry_report"
)
service_index = report_retry.index(
    "survey_delivery_service_failed_abandon()"
)
poll_index = report_retry.index("survey_delivery_poll_comm_result()")
assert service_index < poll_index, (
    "an unresolved stale submission must be abandoned before another report "
    "delivery is polled or submitted"
)
stale_abandon_index = report_retry.index(
    "app_node_comm_abandon_delivery(stale_handle)"
)
retain_index = report_retry.index(
    "survey_delivery_failed_abandon_handle = stale_handle",
    stale_abandon_index,
)
stop_index = report_retry.index(
    "app_watchdog_stop_feeding()", retain_index
)
assert stale_abandon_index < retain_index < stop_index
assert "ret != -ENOENT && ret != -EALREADY" in report_retry[
    stale_abandon_index:retain_index
], "already-terminal handles are successful cleanup, not fatal abandonment"

# A retained discovery command can be queued well before its synchronized RF
# start.  That future timer is protocol state, not current radio ownership:
# the relay must remain able to forward the same discovery start during the
# lead interval so a forced-hop child can schedule the synchronized round.
assert "app_anchor_survey_runtime_radio_active" in RUNTIME_HEADER
discovery_radio_active = function_body(
    RUNTIME, "app_anchor_survey_runtime_radio_active"
)
assert "survey_running" in discovery_radio_active
assert "discovery_pending" not in discovery_radio_active, (
    "the mesh arbitration predicate must describe a due/running RF owner, "
    "not a future discovery timer"
)

queue_discovery = function_body(
    RUNTIME, "app_anchor_survey_runtime_queue_discovery"
)
assert "discovery_pending = true" in queue_discovery
assert "survey_running = true" not in queue_discovery, (
    "queueing a future synchronized discovery must not claim the radio"
)
take_pending = worker.index("discovery_pending = false")
claim_running = worker.index("survey_running = true", take_pending)
claim_radio = worker.index(
    "radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_ANCHOR_SURVEY,",
    claim_running,
)
assert take_pending < claim_running < claim_radio, (
    "the discovery RF predicate must become active only when the due worker "
    "takes pending ownership and approaches the radio guard"
)

assert "app_anchor_survey_runtime_discovery_is_pending()" not in ANCHOR, (
    "heartbeat and low-duty scan arbitration must not treat a future survey "
    "timer as current RF ownership"
)
assert (
    ".anchor_survey_radio_active =\n"
    "        app_anchor_survey_runtime_radio_active"
) in ANCHOR, (
    "the mesh callback boundary must publish the RF-owner predicate"
)
assert "mesh_report_anchor_survey_discovery_is_pending" not in MESH_REPORT
assert "mesh_report_anchor_survey_radio_active" in MESH_REPORT
coordinator = function_body(
    MESH_REPORT, "mesh_coordinator_decide_for_c5_intent"
)
assert "mesh_report_anchor_survey_radio_active()" in coordinator
c5_defer = function_body(MESH_REPORT, "mesh_c5_flood_defer_active_cb")
assert "mesh_report_anchor_survey_radio_active()" in c5_defer
assert "mesh_report_anchor_survey_discovery_is_pending" not in c5_defer
assert "anchor_survey_discovery_is_pending" not in STACK_BUDGET, (
    "the stack-root table must not retain the superseded callback identity"
)
assert "app_anchor_survey_runtime_radio_active" in STACK_BUDGET, (
    "the address-taken survey RF-owner callback needs an explicit stack root"
)

# Only the responder creates or satisfies pair-result custody. The initiator
# still performs all five DS-TWR exchanges, but it cannot consume durable
# delivery slots or become a semantic reporter at any later layer.
result_constructor = function_body(
    CORE_SURVEY, "survey_init_result_packet_from_reporter"
)
default_constructor = function_body(
    CORE_SURVEY, "survey_init_result_packet"
)
mask_admission = function_body(CORE_SURVEY, "survey_pair_note_sample_masks")
assert "reporter_id != sample->pair.responder_id" in result_constructor
assert "sample->pair.responder_id" in default_constructor
assert "sample->pair.initiator_id" not in default_constructor
assert "reporter_id != sample->pair.responder_id" in mask_admission

gateway_preflight = function_body(
    ANCHOR, "gateway_survey_preflight_pair_result"
)
gateway_apply = function_body(ANCHOR, "gateway_note_survey_pair_result")
manual_match = function_body(
    ANCHOR, "gateway_manual_survey_pair_matches_sample"
)
manual_note = function_body(ANCHOR, "gateway_manual_survey_pair_note_sample")
for boundary in (gateway_preflight, gateway_apply, manual_match):
    assert "responder_id" in boundary
    assert "initiator_id &&" not in boundary
assert "responder_result_mask" in manual_note
assert "initiator_result_mask ==" not in manual_note

# Exact ABORT owns stale local and transit result retirement even after the RF
# lease ended. Pair-result wire identity is the operation generation, survey,
# pair, sample count, round tuple, and RAM-retained round commitment. Different
# tuples and a
# gateway-accepted forwarded ACK remain untouched.
abort_retire = function_body(
    RESULT_DELIVERY, "app_anchor_survey_result_delivery_abort_round"
)
abort_match = function_body(
    RESULT_DELIVERY, "result_delivery_matches_round_abort"
)
abort_service = function_body(
    RESULT_DELIVERY, "result_delivery_service_slot"
)
runtime_abort = function_body(
    RUNTIME, "app_anchor_survey_runtime_abort_pair_matching_round"
)
transit_abort = function_body(
    MESH_REPORT, "mesh_retire_stale_transit_survey_result_for_abort"
)
transit_match = function_body(
    MESH_REPORT, "mesh_survey_abort_matches_transit_result"
)
assert "survey_sample_matches_pair_run" in abort_match
assert "survey_round_commitment_extract_tlv" not in abort_match
assert "result_delivery_tombstone_equal" in abort_match
assert abort_retire.index("result_abort_tombstone.abort_requested = true") < \
       abort_retire.index("retirement_in_progress = true")
assert abort_service.index("if (slot->retirement_in_progress)") < \
       abort_service.index("app_node_comm_abandon_delivery(handle)")
assert "app_anchor_survey_result_delivery_abort_round" in runtime_abort
assert runtime_abort.index("survey_pair_lease_abort_matching_round_bound") < \
       runtime_abort.index("app_anchor_survey_result_delivery_abort_round")
assert "CMD_SURVEY_ABORT" in transit_match
assert "survey_round_commitment_extract_tlv" in transit_match
assert "result_packet->src_id == pair.responder_id" in transit_match
assert "survey_sample_matches_pair_run" in transit_match
assert "result_commitment" not in transit_match
assert "!mesh_runtime.pending.gateway_ack_forward_pending" in transit_abort
assert "MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD" in transit_abort
assert "mesh_relay_cancel_tx" in transit_abort
assert "mesh_route_waiting_tx_valid = false" in transit_abort
assert "mesh_save_outbox_durable" not in transit_abort
assert "mesh_deferred_outbox_pending" not in transit_abort
assert MESH_REPORT.index(
    "mesh_retire_stale_transit_survey_result_for_abort("
) < MESH_REPORT.rindex("mesh_handle_result_actions(result")

print("survey discovery failure source invariants passed")
