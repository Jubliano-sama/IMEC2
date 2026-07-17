#!/usr/bin/env python3
from pathlib import Path
import re

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
REPORT = read_composed_source(ROOT / "app/src/app_mesh_report.c")
REPORT_HEADER = (ROOT / "app/src/app_mesh_report.h").read_text()
ANCHOR = read_composed_source(ROOT / "app/src/app_anchor.c")
DISCOVERY = (ROOT / "app/src/app_anchor_survey_discovery.c").read_text()
SURVEY_RUNTIME = (ROOT / "app/src/app_anchor_survey_runtime.c").read_text()
NODE_COMM_APP = (ROOT / "app/src/app_node_comm.c").read_text()
CONFIG = (ROOT / "app/src/app_config.h").read_text()
DRIVER = read_composed_source(ROOT / "app/src/dwm3000_driver.c")
PERSISTENCE = (ROOT / "app/src/app_mesh_persistence.c").read_text()


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


def assert_debug_record_fits(
    source: str, marker: str, unsigned_widths: tuple[int, ...]
) -> None:
    match = re.search(
        rf'status_debug_printf\("([^"\n]*{re.escape(marker)}[^"\n]*)"',
        source,
    )
    assert match is not None, f"missing debug record {marker}"
    rendered = match.group(1).replace(r"\n", "\n")
    rendered = re.sub(r"%08x", "f" * 8, rendered)
    widths = iter(unsigned_widths)
    rendered = re.sub(r"%u", lambda unused: "9" * next(widths), rendered)
    try:
        next(widths)
    except StopIteration:
        pass
    else:
        raise AssertionError(f"unused width for debug record {marker}")
    assert "%" not in rendered, f"unmodeled format in debug record {marker}"
    assert len(rendered) <= 127, (
        f"debug record {marker} can exceed status_debug_printf's 127-byte payload"
    )


tracked = function_body(REPORT, "mesh_start_tracked_tx_with_retry")
for match in re.finditer(r"mesh_store_route_waiting_tx\(", tracked):
    prefix = tracked[max(0, match.start() - 100) : match.start()]
    assert "if (store_route_wait)" in prefix, "unguarded generic route-wait store"

owned = function_body(REPORT, "mesh_start_owned_tracked_tx")
assert "APP_MESH_ROUTE_WAIT_TX_OWNER_DURABLE_LOCAL" in owned
assert "rf_sent" in owned

direct = function_body(REPORT, "mesh_send_direct_gateway_payload_and_wait_ack")
assert "anchor_survey_delivery_gateway_confirmed" in direct
assert "&out->packet" in direct

gateway_accept_wrapper = function_body(
    REPORT, "mesh_report_gateway_handle_survey_discovery_report"
)
assert re.search(
    r"\bint\s*\(\*gateway_handle_survey_discovery_report\)", REPORT_HEADER
), "gateway survey callback must report semantic acceptance"
assert "return mesh_report_callbacks->gateway_handle_survey_discovery_report(" in (
    gateway_accept_wrapper
)
assert "return -ENOTSUP;" in gateway_accept_wrapper

gateway_accept = function_body(ANCHOR, "gateway_handle_survey_discovery_report")
pair_accept = function_body(ANCHOR, "gateway_note_survey_pair_result")
transport_check = gateway_accept.index("packet->dst_id != DEVICE_ID")
pair_dispatch = gateway_accept.index("gateway_note_survey_pair_result(")
report_decode = gateway_accept.index("survey_extract_reach_report_tlvs(")
assert transport_check < pair_dispatch < report_decode, (
    "both survey report classes must pass the common transport gate"
)
for rejected_transport in (
    "packet->payload_len != payload_len",
    "packet->src_id == DEVICE_ID",
    "radio_channel != UWB_CHANNEL_MESH_PAYLOAD",
    "FLAG_GATEWAY_ACK_REQUIRED",
    "!mesh_id_is_unicast(previous_hop_id)",
    "previous_hop_id == DEVICE_ID",
    "link_quality > 100u",
):
    assert rejected_transport in gateway_accept
assert "packet->session_id != survey_id" in gateway_accept
assert "packet->src_id != anchor_id" in gateway_accept
assert "return -ESTALE;" in gateway_accept
duplicate_report = gateway_accept[
    gateway_accept.index("if (duplicate_report)") :
    gateway_accept.index("reverse_hint =")
]
assert "return APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;" in duplicate_report, (
    "a valid previously recorded reach report must remain ACK-eligible"
)
assert gateway_accept.index("if (duplicate_report)") < gateway_accept.index(
    "if (gateway_survey_auto.running)"
), "an accepted report duplicate must survive orchestration phase advance"
plan_tail = gateway_accept[
    gateway_accept.index("ret = survey_gateway_plan_pairs(") :
]
assert re.search(
    r"if \(ret != PROTO_OK\).*?return APP_GATEWAY_SEMANTIC_ACCEPT_NEW;",
    plan_tail,
    re.S,
), (
    "pair-planning failure after report storage must not revoke acceptance"
)

for required_sample_field in (
    "TLV_SURVEY_ID",
    "TLV_INITIATOR_ID",
    "TLV_RESPONDER_ID",
    "TLV_SAMPLE_COUNT",
    "TLV_SAMPLE_INDEX",
    "TLV_DISTANCE_MM",
    "TLV_QUALITY",
    "TLV_RANGE_STATUS",
):
    assert required_sample_field in pair_accept
assert "survey_sample_validate(&sample)" in pair_accept
assert "packet->session_id != sample.pair.survey_id" in pair_accept
assert "packet->src_id != sample.pair.initiator_id" in pair_accept
assert "packet->src_id != sample.pair.responder_id" in pair_accept
duplicate_sample = pair_accept[
    pair_accept.index("gateway_survey_pair_result_mask & sample_bit") :
    pair_accept.index("gateway_survey_pair_result_mask |= sample_bit")
]
assert "return APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;" in duplicate_sample, (
    "a valid previously recorded pair sample must remain ACK-eligible"
)

relay_ack = function_body(REPORT, "mesh_handle_result_actions")
assert "anchor_survey_delivery_gateway_confirmed" in relay_ack
assert "confirmed_packet" in relay_ack

preempt = function_body(REPORT, "mesh_preempt_clear_outbox")
assert "released_packet" in preempt
assert re.search(
    r"anchor_survey_delivery_transport_released\s*\(\s*released_packet,\s*true\)",
    preempt,
)

assert ".anchor_survey_delivery_gateway_confirmed =" in ANCHOR
assert ".anchor_survey_delivery_transport_released =" in ANCHOR
assert "SURVEY_DELIVERY_LOCK()" in DISCOVERY
assert "app_mesh_local_delivery_recover" in DISCOVERY
retry = function_body(DISCOVERY, "app_anchor_survey_discovery_retry_report")
assert retry.count("app_node_comm_submit_delivery(") == 1
assert "NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK" in retry
assert "outbound = *app_mesh_local_delivery_outbound(delivery);" in retry, (
    "the communication facade must receive the exact persisted survey envelope"
)
assert "survey_delivery_deadline_ms(&outbound)" in retry
assert "&delivery_handle" in retry
assert "survey_delivery_handle = delivery_handle" in retry
assert "app_node_comm_abandon_delivery(stale_handle)" in retry, (
    "a submit that races with supersede must release its accepted handle"
)
for synchronous_or_inline_api in (
    "app_node_comm_start_delivery(",
    "app_node_comm_start_owned_delivery(",
    "app_node_comm_request_path(",
    "app_node_comm_service_deliveries(",
    "mesh_start_tracked_tx(",
    "mesh_start_owned_tracked_tx(",
    "mesh_request_route(",
    "dwm3000_driver_",
):
    assert synchronous_or_inline_api not in retry, (
        "survey retry must submit immutable work without running RF inline: "
        f"{synchronous_or_inline_api}"
    )
submit_index = retry.index("app_node_comm_submit_delivery(")
assert retry.rfind("SURVEY_DELIVERY_UNLOCK();", 0, submit_index) >= 0, (
    "submission must not run while the persistent-custody lock is held"
)
assert submit_index < retry.index("survey_delivery_handle = delivery_handle")

poll = function_body(DISCOVERY, "survey_delivery_poll_comm_result")
assert "app_node_comm_take_delivery_event_for(handle, &event)" in poll
assert "survey_delivery_handle != handle" in poll, (
    "a stale terminal event must not mutate current survey custody"
)
assert "outbound = *app_mesh_local_delivery_outbound(delivery);" in poll
assert re.search(
    r"event\.reason\s*==\s*NODE_COMM_TERMINAL_DELIVERED.*?"
    r"app_anchor_survey_delivery_gateway_confirmed\(&outbound\.packet\)",
    poll,
    re.S,
), "delivery success must commit the exact persisted packet ACK"
failure_index = poll.index("app_mesh_local_delivery_note_failed(delivery)")
release_index = poll.index("app_mesh_local_delivery_discard_failed(delivery)")
assert failure_index < release_index, (
    "terminal communication failure must first persist FAILED then release custody"
)

confirmed = function_body(
    DISCOVERY, "app_anchor_survey_delivery_gateway_confirmed"
)
assert "app_mesh_local_delivery_note_ack(delivery, packet)" in confirmed
assert confirmed.index("app_mesh_local_delivery_note_ack(delivery, packet)") < (
    confirmed.index("app_stack_workload_diag_anchor_survey_release(packet")
), "delivered custody clears transactionally before terminal diagnostics"

assert "app_node_comm_retry_backoff_ms(" in DISCOVERY
assert "NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK" in DISCOVERY
assert "DBG_SURVEY_REPORT_BACKOFF" in DISCOVERY
released = function_body(
    DISCOVERY, "app_anchor_survey_delivery_transport_released"
)
assert "survey_delivery_next_retry_delay_ms(" in released
assert "schedule_work_ms(0u)" not in released, (
    "a released RF attempt must use randomized exponential backoff"
)
delivery_service = function_body(
    NODE_COMM_APP, "app_node_comm_service_deliveries"
)
assert delivery_service.count("node_comm_lease_defer_pre_rf_retry(") == 2
assert delivery_service.count("node_comm_lease_defer_pre_rf(") == 1
assert delivery_service.count("node_comm_lease_wait_resource(") == 1
single_flight_wait = delivery_service.index(
    "node_comm_lease_wait_resource("
)
assert delivery_service.index(
    "node_comm_reliable_uplink_inflight_handle != 0u"
) < single_flight_wait
assert "node_comm_release_resource_wait(" in NODE_COMM_APP, (
    "single-flight occupancy is a resource wait, so owner release must wake "
    "blocked protocol and uplink deliveries without consuming a retry"
)
assert re.search(
    r"if\s*\(\s*scheduled_retry_delay_ms\s*>\s*0u\s*\).*?"
    r"node_comm_lease_defer_pre_rf\s*\(.*?not_before_ms",
    delivery_service,
    re.S,
), (
    "only an exact backend-scheduled radio boundary may use deterministic "
    "pre-RF deferral"
)
assert delivery_service.index("node_comm_lease_defer_pre_rf(") < (
    delivery_service.index("node_comm_lease_defer_pre_rf_retry(",
                           delivery_service.index(
                               "node_comm_lease_defer_pre_rf("))
), "unscheduled retryable deferrals must retain randomized retry policy"

survey_rx = function_body(DISCOVERY, "receive_survey_probes_until")
assert "dwm3000_driver_receive_frame_continuous(" in survey_rx, (
    "survey discovery must keep RX armed for the complete listen interval"
)
assert "dwm3000_driver_receive_frame(" not in survey_rx, (
    "the short preamble-hunt API collapses the survey listen interval"
)

measured = re.search(
    r"#define SURVEY_DISCOVERY_PHY_PREP_MEASURED_MAX_MS\s+(\d+)u", CONFIG
)
margin = re.search(
    r"#define SURVEY_DISCOVERY_PHY_PREP_MARGIN_MS\s+(\d+)u", CONFIG
)
assert measured is not None and int(measured.group(1)) == 63
assert margin is not None and int(margin.group(1)) >= 40
assert "SURVEY_DISCOVERY_PHY_PREP_MEASURED_MAX_MS +" in CONFIG
assert "default survey start delay must allow early PHY preparation" in CONFIG

start = function_body(DISCOVERY, "app_anchor_survey_discovery_handle_start")
timing_index = start.index("survey_discovery_timing_from_age(")
start_at_index = start.index("survey_discovery_start_at_ms(")
queue_index = start.index("discovery_ops.queue_start(")
assert timing_index < start_at_index < queue_index, (
    "survey timing must be populated before reconstructing the absolute start"
)
assert "discovery_ops.queue_start(&config, start_at_ms)" in start
assert "uptime_ms_until_deadline(now_ms, start_at_ms)" in start
reject_index = start.index(
    "survey discovery start rejected while earlier report custody is pending"
)
assert reject_index < queue_index, (
    "pending report custody must reject a later survey before it can be queued"
)
assert "app_mesh_local_delivery_supersede(" not in start
assert "app_node_comm_abandon_delivery(" not in start
assert "schedule_work_ms(0u)" in start, (
    "a duplicate survey start must re-kick its packet-exact pending delivery"
)

drain = function_body(REPORT, "mesh_drain_rx_queue_locked")
actions_index = drain.index("mesh_handle_result_actions(")
delivery_index = drain.index("mesh_report_anchor_handle_survey_discovery_start(")
refresh_indices = [
    match.start()
    for match in re.finditer(r"mesh_rx_pending_refresh_age\(", drain)
]
assert any(actions_index < index < delivery_index for index in refresh_indices), (
    "local delivery must refresh message age after a potentially long relay action"
)

run = function_body(DISCOVERY, "app_anchor_survey_discovery_run")
assert run.count("dwm3000_driver_configure_wake_mode(") == 1, (
    "each discovery run must perform exactly one deliberate full wake-PHY configure"
)
assert "dwm3000_driver_idle(" in run and "sleep_until_ms(start_ms)" in run
assert "sleep_with_uwb_standby_until_ms(" not in run
send_index = run.index("send_local_survey_probe(")
ensure_index = run.index("dwm3000_driver_ensure_wake_mode(", send_index)
post_tx_rx_index = run.index("receive_survey_probes_until(", ensure_index)
assert send_index < ensure_index < post_tx_rx_index
probe_send = function_body(DISCOVERY, "send_local_survey_probe")
assert "dwm3000_driver_send_frame_tracked(" in probe_send
assert run.count("receive_survey_probes_until(") >= 2, (
    "discovery must listen both before and after its own probe airtime"
)

ensure_wake = function_body(DRIVER, "dwm3000_driver_ensure_wake_mode")
assert "ensure_phy_mode(DWM3000_PHY_WAKE)" in ensure_wake
assert "configure_radio_from_reset(" not in ensure_wake

receive_response = function_body(DRIVER, "receive_response")
assert "read_rx_diagnostics(" not in receive_response
assert "capture_rx_diag_raw(" not in receive_response
initiator = function_body(DRIVER, "dwm3000_driver_range_initiator")
delayed_final = initiator.index("DWT_START_TX_DELAYED")
assert "request->capture_rsl" not in initiator[:delayed_final], (
    "optional diagnostics must stay out of the RESP-to-delayed-FINAL path"
)
response_wait = initiator.index("receive_response(")
poll_timestamp = initiator.index("capture_completed_tx_timestamp(")
assert poll_timestamp < response_wait, (
    "the poll TX timestamp must be captured during the poll-to-RESP interval"
)
final_prestage = initiator.index('take_port_error("final-prestage")')
assert poll_timestamp < final_prestage < response_wait, (
    "the invariant FINAL bytes must be staged before waiting for RESP"
)
final_build = initiator.index("final_tx_time =", response_wait)
critical_path = initiator[final_build:delayed_final]
assert "patch_tx_frame(" in critical_path
assert "start_prepared_range_frame(" in critical_path
assert "send_range_frame(" not in critical_path, (
    "the full FINAL frame write must stay out of delayed-TX arm headroom"
)
assert "clear_status(" not in critical_path, (
    "receive_response already clears TXFRS before FINAL preparation"
)
assert "dwt_setpreambledetecttimeout(" not in critical_path, (
    "the unchanged delayed-RX preamble timeout must not be rewritten"
)
assert "read_tx_timestamp_u64(" not in critical_path, (
    "timestamp SPI reads must stay out of the RESP-to-delayed-FINAL path"
)
assert "status_debug_printf(" not in critical_path, (
    "RTT formatting/output must stay out of the RESP-to-delayed-FINAL path"
)
delayed_final_arm = initiator.index("start_prepared_range_frame(", final_build)
post_arm_diagnostics = initiator.index("if (request->capture_rsl)", delayed_final_arm)
assert delayed_final_arm < post_arm_diagnostics
assert "read_rx_diagnostics(" in initiator[post_arm_diagnostics:]
assert "capture_rx_diag_raw(" in initiator[post_arm_diagnostics:]
assert_debug_record_fits(initiator, "stage=final-armed", (10, 3, 3, 1))
assert_debug_record_fits(initiator, "stage=final-raw", (3,))
responder = function_body(DRIVER, "responder_poll_once")
response_arm = responder.index("DWT_START_TX_DELAYED")
matched_poll = responder.index("reply_delay_uus = request_reply_delay_uus(")
responder_critical_path = responder[matched_poll:response_arm]
assert "status_debug_printf(" not in responder_critical_path, (
    "RTT formatting/output must stay out of the POLL-to-delayed-RESP path"
)
assert_debug_record_fits(responder, "stage=resp-armed", (10, 3, 3))
assert_debug_record_fits(responder, "stage=resp-raw", (3, 5))
survey_initiator = function_body(SURVEY_RUNTIME, "run_pair_initiator")
assert "request.capture_rsl = false" in survey_initiator

restore = function_body(PERSISTENCE, "app_mesh_persistence_restore_local_delivery")
assert "app_mesh_local_delivery_snapshot_valid(snapshot)" in restore
assert "return -EBADMSG" in restore

prepare_report = function_body(DISCOVERY, "prepare_discovery_report")
stage_call = prepare_report.index("app_mesh_local_delivery_stage(delivery, &outbound")
inactive_guard = prepare_report.index(
    "!app_mesh_local_delivery_active(delivery)", stage_call
)
retry_copy = prepare_report.index(
    "survey_report_stage_retry_outbound = outbound", inactive_guard
)
retry_generation = prepare_report.index(
    "survey_report_stage_retry_generation = survey_id", retry_copy
)
retry_valid = prepare_report.index(
    "survey_report_stage_retry_valid = true", retry_generation
)
assert stage_call < inactive_guard < retry_copy < retry_generation < retry_valid, (
    "a failed first journal write must retain the exact encoded report and its "
    "survey generation before returning pressure to the runtime"
)

retry_report = function_body(
    DISCOVERY, "app_anchor_survey_discovery_retry_report"
)
retry_guard = retry_report.index("if (survey_report_stage_retry_valid &&")
retry_inactive = retry_report.index(
    "!app_mesh_local_delivery_active(delivery)", retry_guard
)
retry_stage = retry_report.index(
    "app_mesh_local_delivery_stage(", retry_inactive
)
retry_outbound = retry_report.index(
    "&survey_report_stage_retry_outbound", retry_stage
)
retry_generation_arg = retry_report.index(
    "survey_report_stage_retry_generation", retry_outbound
)
retry_success = retry_report.index("if (ret == 0)", retry_generation_arg)
retry_clear_outbound = retry_report.index(
    "memset(&survey_report_stage_retry_outbound", retry_success
)
retry_clear_generation = retry_report.index(
    "survey_report_stage_retry_generation = 0u", retry_clear_outbound
)
retry_clear_valid = retry_report.index(
    "survey_report_stage_retry_valid = false", retry_clear_generation
)
retry_failure = retry_report.index("} else {", retry_clear_valid)
retry_reschedule = retry_report.index(
    "schedule_work_ms(REPORT_TX_RETRY_DELAY_MS)", retry_failure
)
assert (
    retry_guard
    < retry_inactive
    < retry_stage
    < retry_outbound
    < retry_generation_arg
    < retry_success
    < retry_clear_outbound
    < retry_clear_generation
    < retry_clear_valid
    < retry_failure
    < retry_reschedule
), (
    "stage retry must replay the exact cached packet and may clear it only after "
    "durable journal admission; pressure must leave custody intact and reschedule"
)
retry_failure_slice = retry_report[retry_failure:retry_reschedule]
for forbidden_clear in (
    "memset(&survey_report_stage_retry_outbound",
    "survey_report_stage_retry_generation = 0u",
    "survey_report_stage_retry_valid = false",
):
    assert forbidden_clear not in retry_failure_slice, (
        "stage pressure must not discard cached exact-report custody"
    )

staged_probe = function_body(
    DISCOVERY, "app_anchor_survey_discovery_report_staged"
)
assert "app_mesh_local_delivery_active(delivery)" in staged_probe
assert "delivery->snapshot.generation == survey_id" in staged_probe, (
    "runtime release must observe durable custody for the exact survey generation"
)

discovery_restore = function_body(
    DISCOVERY, "app_anchor_survey_discovery_restore"
)
assert "app_mesh_local_delivery_rebase_after_boot(" in discovery_restore, (
    "persisted boot-relative report timing must be rebased after reset"
)
assert "discovery_ops.seed_sequence(outbound->packet.seq)" in discovery_restore, (
    "restored report identity must advance the next local sequence"
)

bounded_control = function_body(REPORT, "mesh_try_send_c5_flood_view")
handoff_begin = bounded_control.index("mesh_rx_handoff_begin_control(")
handoff_wait = bounded_control.index("mesh_rx_handoff_wait_for_control(")
control_send = bounded_control.index("mesh_send_c5_flood_now(")
handoff_end = bounded_control.index("mesh_rx_handoff_end_control(")
scan_restart = bounded_control.index("mesh_restart_role_scan(")
assert handoff_begin < handoff_wait < control_send < handoff_end < scan_restart, (
    "bounded gateway control must own the RX handoff through its complete send"
)
assert re.search(
    r"mesh_send_c5_flood_now\s*\([^;]+?\btrue\s*,\s*NULL\s*,\s*NULL\s*,\s*rf_started\s*\)",
    bounded_control,
    re.S,
), "node-communication control attempts must request one lower-layer opportunity"

control_backend = function_body(NODE_COMM_APP, "app_node_comm_service_deliveries")
assert "mesh_try_send_c5_flood_view(" in control_backend
assert "app_mesh_flood_send_bounded(" not in control_backend

command_result = function_body(ANCHOR, "anchor_submit_command_result")
assert "command_id == CMD_SURVEY_PREPARE_PAIR" in command_result
assert "command_id == CMD_SURVEY_START_PAIR" in command_result
assert "SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS" in command_result
assert "GATEWAY_COMMAND_RESULT_TIMEOUT_MS" in command_result
assert re.search(
    r"app_node_comm_submit_protocol_response\s*\(\s*&outbound\s*,\s*"
    r"absolute_deadline_ms",
    command_result,
    re.S,
), (
    "survey command results must carry the selected end-to-end deadline into "
    "protocol-response custody"
)

pair_failure = function_body(ANCHOR, "gateway_survey_auto_log_skipped_pair")
assert "failed_command_id" in pair_failure
assert "event.anchor_id = failed_target_id" in pair_failure
assert "event.progress_count" in pair_failure
assert "event.total_count" in pair_failure
assert "event.attempt" in pair_failure
survey_worker = function_body(ANCHOR, "gateway_survey_work_handler")
assert re.search(
    r"gateway_survey_auto_log_skipped_pair\s*\(\s*\"send-failed\".*?"
    r"action\.command_id\s*,\s*action\.target_id\s*\)",
    survey_worker,
    re.S,
), "send admission failures must identify the exact survey phase and target"

gateway_rx_worker = function_body(REPORT, "mesh_uwb_rx_work_handler")
assert gateway_rx_worker.count("mesh_rx_radio_start(") == 2
assert gateway_rx_worker.count("mesh_rx_radio_stop(") == 2
assert "mesh_transport_radio_start(" not in gateway_rx_worker

rx_schedule = function_body(REPORT, "mesh_schedule_uwb_rx")
assert "mesh_rx_handoff_scan_rearm_allowed()" in rx_schedule, (
    "continuous RX must not rearm while bounded control owns the radio"
)

accepted_result = function_body(
    ANCHOR, "gateway_survey_auto_note_command_result"
)
close_call = accepted_result.index("ret = gateway_survey_complete_accepted_delivery()")
eagain = accepted_result.index("if (ret == -EAGAIN)")
abandon = accepted_result.index("gateway_survey_abandon_current(", eagain)
assert "k_work_reschedule(&gateway_survey_work" in accepted_result[eagain:abandon], (
    "an accepted early result must wait for an active backend call to return"
)
assert "memset(&gateway_survey_result_preflight" not in accepted_result[close_call:eagain], (
    "accepted result identity must remain cached while terminal take is deferred"
)
delivery_poll = function_body(ANCHOR, "gateway_survey_service_active_delivery")
assert "gateway_survey_result_preflight.valid" in delivery_poll
assert "gateway_survey_auto_note_command_result(" in delivery_poll, (
    "terminal polling must resume the already accepted semantic result"
)

anchor_start = function_body(ANCHOR, "app_anchor_start_anchor_role")
restore_journal = anchor_start.index("app_anchor_survey_discovery_restore(")
bind_journal = anchor_start.index("app_anchor_survey_discovery_retry_report(")
resume_outbox = anchor_start.index("mesh_report_resume_restored_outbox(")
assert restore_journal < bind_journal < resume_outbox, (
    "restored survey custody must bind to node communication before outbox RF resumes"
)
assert "if (!delivery_restored || delivery_bind_ret == 0)" in anchor_start

print("survey delivery source invariants passed")
