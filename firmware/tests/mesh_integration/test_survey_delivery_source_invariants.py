#!/usr/bin/env python3
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
REPORT = (ROOT / "app/src/app_mesh_report.c").read_text()
ANCHOR = (ROOT / "app/src/app_anchor.c").read_text()
DISCOVERY = (ROOT / "app/src/app_anchor_survey_discovery.c").read_text()
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

worker_delay = function_body(DISCOVERY, "discovery_worker_delay_ms")
assert "timing->wait_ms <= SURVEY_DISCOVERY_PHY_PREP_BUDGET_MS" in worker_delay
assert "timing->wait_ms - SURVEY_DISCOVERY_PHY_PREP_BUDGET_MS" in worker_delay

start = function_body(DISCOVERY, "app_anchor_survey_discovery_handle_start")
assert "schedule_delay_ms = discovery_worker_delay_ms(&timing)" in start
assert re.search(
    r"timing\.pending\s*\?\s*now_ms \+ timing\.wait_ms\s*:\s*"
    r"now_ms - timing\.elapsed_ms",
    start,
), "early worker scheduling must preserve the original absolute survey start"

run = function_body(DISCOVERY, "app_anchor_survey_discovery_run")
assert run.count("dwm3000_driver_configure_wake_mode(") == 1, (
    "each discovery run must perform exactly one deliberate full wake-PHY configure"
)
assert "dwm3000_driver_idle(" in run and "sleep_until_ms(start_ms)" in run
assert "sleep_with_uwb_standby_until_ms(" not in run
send_index = run.index("dwm3000_driver_send_frame(")
ensure_index = run.index("dwm3000_driver_ensure_wake_mode(", send_index)
post_tx_rx_index = run.index("receive_survey_probes_until(", ensure_index)
assert send_index < ensure_index < post_tx_rx_index
assert run.count("receive_survey_probes_until(") >= 2, (
    "discovery must listen both before and after its own probe airtime"
)

ensure_wake = function_body(DRIVER, "dwm3000_driver_ensure_wake_mode")
assert "ensure_phy_mode(DWM3000_PHY_WAKE)" in ensure_wake
assert "configure_radio_from_reset(" not in ensure_wake

restore = function_body(PERSISTENCE, "app_mesh_persistence_restore_local_delivery")
assert "app_mesh_local_delivery_snapshot_valid(snapshot)" in restore
assert "return -EBADMSG" in restore

print("survey delivery source invariants passed")
