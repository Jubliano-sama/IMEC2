#!/usr/bin/env python3
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
REPORT = (ROOT / "app/src/app_mesh_report.c").read_text()
REPORT_HEADER = (ROOT / "app/src/app_mesh_report.h").read_text()
ANCHOR = (ROOT / "app/src/app_anchor.c").read_text()
DISCOVERY = (ROOT / "app/src/app_anchor_survey_discovery.c").read_text()
SURVEY_RUNTIME = (ROOT / "app/src/app_anchor_survey_runtime.c").read_text()
NODE_COMM_APP = (ROOT / "app/src/app_node_comm.c").read_text()
CONFIG = (ROOT / "app/src/app_config.h").read_text()
DRIVER = (ROOT / "app/src/dwm3000_driver.c").read_text()
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
assert retry.index("mesh_owned_tracked_tx_preflight") < retry.index(
    "app_mesh_local_delivery_begin_attempt"
), "route preflight must avoid STARTING/refund NVS churn while disconnected"
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
assert "node_comm_lease_defer_pre_rf(" not in delivery_service

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
supersede_index = start.index("app_mesh_local_delivery_supersede(")
assert supersede_index < queue_index, (
    "obsolete report custody must terminate before the next survey is queued"
)
assert "schedule_work_ms(0u)" in start, (
    "a duplicate survey start must re-kick its packet-exact pending delivery"
)
assert "DBG_SURVEY_REPORT_SUPERSEDED" in start

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

gateway_rx_worker = function_body(REPORT, "mesh_uwb_rx_work_handler")
assert gateway_rx_worker.count("mesh_rx_radio_start(") == 2
assert gateway_rx_worker.count("mesh_rx_radio_stop(") == 2
assert "mesh_transport_radio_start(" not in gateway_rx_worker

rx_schedule = function_body(REPORT, "mesh_schedule_uwb_rx")
assert "mesh_rx_handoff_scan_rearm_allowed()" in rx_schedule, (
    "continuous RX must not rearm while bounded control owns the radio"
)

print("survey delivery source invariants passed")
