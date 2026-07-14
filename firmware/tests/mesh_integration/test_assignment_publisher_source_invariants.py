#!/usr/bin/env python3
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = (ROOT / "app/src/app_anchor.c").read_text()
BLE = (ROOT / "app/src/app_gateway_ble.c").read_text()
STREAM = (ROOT / "app/src/app_gateway_ble_stream.h").read_text()
PUBLISHER = (ROOT / "app/src/app_gateway_assignment_publisher.c").read_text()


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
    depth = 0
    for index in range(brace, len(source)):
        depth += source[index] == "{"
        depth -= source[index] == "}"
        if depth == 0:
            return source[match.start() : index + 1]
    raise AssertionError(f"unterminated function {name}")


assert re.search(r"#define GATEWAY_BLE_STREAM_QUEUE_DEPTH\s+3u", STREAM)
assert "anchor_ids[APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES]" in PUBLISHER
assert "slots[APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES]" in PUBLISHER
assert "APP_GATEWAY_ASSIGNMENT_PUBLISHER_RAM_BUDGET_BYTES" in PUBLISHER

hop_report = function_body(ANCHOR, "anchor_discovery_gateway_hop_count")
assert "route_selected(&mesh_runtime.upstream)" in hop_report
assert re.search(
    r"selected\s*==\s*NULL\s*\|\|\s*selected->hop_count\s*==\s*UINT8_MAX",
    hop_report,
)
assert re.search(r"return\s+0u\s*;", hop_report)
assert re.search(r"return\s+selected->hop_count\s*\+\s*1u\s*;", hop_report)

schedule = function_body(ANCHOR, "anchor_schedule_discovery_response")
retry = function_body(ANCHOR, "anchor_discovery_claim_work_handler")
assert "anchor_discovery_gateway_hop_count()" in schedule
assert "anchor_discovery_gateway_hop_count()" in retry

publish = function_body(ANCHOR, "gateway_discovery_assignment_publish_table")
assert "app_gateway_assignment_publisher_stage_sorted_ids(" in publish
assert "app_gateway_assignment_publisher_stage_table_ready(" in publish
assert "gateway_observe_command_event(&event, false)" not in publish
assert function_body(PUBLISHER, "app_gateway_assignment_publisher_stage_batch")
assert function_body(PUBLISHER, "app_gateway_assignment_publisher_stage_sorted_ids")

window = function_body(ANCHOR, "gateway_discovery_assignment_window_ms")
assert "app_discovery_assignment_table_windows_remaining(" in window
assert "return remaining_ms;" not in window
finalize = function_body(
    ANCHOR, "gateway_discovery_assignment_finalize_work_handler"
)
assert "app_discovery_assignment_table_retry_backoff_required(" in finalize
assert "discovery_assignment_retry_backoff_ms(" in finalize
assert "DBG_DISCOVERY_SLOT_TABLE_BACKOFF" in finalize
publish_work = function_body(
    ANCHOR, "gateway_discovery_assignment_publish_work_handler"
)
assert "DBG_DISCOVERY_SLOT_CLAIM_BACKOFF" in publish_work
assert "discovery_assignment_retry_backoff_ms(" in publish_work

admit = function_body(BLE, "gateway_observe_command_event_if_available")
prepare = admit.index("gateway_command_observability_prepare(")
assert admit.index("gateway_ble_stream_state.count >=") < prepare
assert "!gateway_ble_stream_ready()" in admit
assert "gateway_ble_tx_in_flight" not in admit
assert "items[i].priority == 0u" not in admit
assert "retain_until_sent = true" in admit

observe = function_body(BLE, "gateway_observe_command_event")
assert observe.index("app_gateway_assignment_publisher_capture_terminal(") < observe.index(
    "gateway_command_observability_prepare("
)

complete = function_body(BLE, "gateway_ble_tx_complete")
assert "app_gateway_assignment_publisher_note_sent(" in complete
assert "app_gateway_assignment_publisher_pump();" in complete

print("assignment publisher source invariants passed")
