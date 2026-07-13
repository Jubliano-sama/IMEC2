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

publish = function_body(ANCHOR, "gateway_discovery_assignment_publish_table")
assert "app_gateway_assignment_publisher_stage_batch(" in publish
assert "app_gateway_assignment_publisher_stage_table_ready(" in publish
assert "gateway_observe_command_event(&event, false)" not in publish

admit = function_body(BLE, "gateway_observe_command_event_if_available")
prepare = admit.index("gateway_command_observability_prepare(")
assert admit.index("gateway_ble_stream_state.count >=") < prepare
assert admit.index("gateway_ble_tx_in_flight") < prepare
assert admit.index("items[i].priority == 0u") < prepare
assert "retain_until_sent = true" in admit

observe = function_body(BLE, "gateway_observe_command_event")
assert observe.index("app_gateway_assignment_publisher_capture_terminal(") < observe.index(
    "gateway_command_observability_prepare("
)

complete = function_body(BLE, "gateway_ble_tx_complete")
assert "app_gateway_assignment_publisher_note_sent(" in complete
assert "app_gateway_assignment_publisher_pump();" in complete

print("assignment publisher source invariants passed")
