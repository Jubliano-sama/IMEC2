#!/usr/bin/env python3
from pathlib import Path
import re

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = read_composed_source(ROOT / "app/src/app_anchor.c")
BLE = (ROOT / "app/src/app_gateway_ble.c").read_text()
STREAM = (ROOT / "app/src/app_gateway_ble_stream.h").read_text()
PUBLISHER = (ROOT / "app/src/app_gateway_assignment_publisher.c").read_text()
ASSIGNMENT = (ROOT / "include/discovery_assignment.h").read_text()


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
response = function_body(ANCHOR, "anchor_send_discovery_response")
assert "anchor_discovery_gateway_hop_count()" in schedule
assert "anchor_discovery_gateway_hop_count()" in retry

# Discovery CLAIM and table-ACK responses can become ready while the gateway is
# still completing its required four-copy control flood.  Keep their reliable
# protocol-response custody on the assignment-specific deadline, and preserve
# compile-time proof that this deadline covers the complete retry horizon.
submit = response[response.index("app_node_comm_submit_protocol_response(") :]
assert re.search(
    r"pending->absolute_deadline_ms",
    submit,
)
delay_position = schedule.index("delay_ms = anchor_discovery_response_delay_ms(")
deadline_position = schedule.index(
    "discovery_assignment_response_deadline_ms("
)
assert delay_position < deadline_position
assert "absolute_deadline_ms" in schedule
assert "DBG_DISCOVERY_SLOT_RESPONSE_ADMISSION_RETRY" in retry
assert "GATEWAY_COMMAND_RESULT_TIMEOUT_MS" not in response
assert "DISCOVERY_ASSIGNMENT_RESPONSE_CUSTODY_MAX_MS" in ANCHOR

# Repeated RF envelopes retain one response custody item. CLAIM retention is
# operation epoch plus phase; ACK retention also binds the table generation,
# while the outer packet sequence is deliberately irrelevant.
same_phase = schedule.index("discovery_assignment_response_custody_matches(")
same_phase_end = schedule.index(")) {", same_phase) + len(")) {")
same_phase_gate = schedule[same_phase:same_phase_end]
assert "anchor_discovery_claim_pending.active" in same_phase_gate
assert "anchor_discovery_claim_pending.epoch" in same_phase_gate
assert "anchor_discovery_claim_pending.phase" in same_phase_gate
assert "command->session_id" in same_phase_gate
assert "command->seq" not in same_phase_gate

# Invalid phase/epoch/slot input is rejected before custody can change.  A
# valid new phase or epoch falls through the same-phase return and may abandon
# the old handle only before atomically replacing and rescheduling its state.
validation = schedule[:same_phase]
assert "epoch == 0u" in validation
assert "phase != DISCOVERY_ASSIGNMENT_PHASE_CLAIM" in validation
assert "phase != DISCOVERY_ASSIGNMENT_PHASE_ACK" in validation
assert "slot_count == 0u" in validation
assert "slot >= slot_count" in validation
supersede = schedule[same_phase_end:]
abandon = supersede.index("app_node_comm_abandon_delivery(")
retain = supersede.index("return 0;")
replace_command = supersede.index("anchor_discovery_claim_pending.command = *command")
replace_epoch = supersede.index("anchor_discovery_claim_pending.epoch = epoch")
replace_phase = supersede.index("anchor_discovery_claim_pending.phase = phase")
reschedule = supersede.index("mesh_route_work_reschedule(")
assert retain < abandon
assert abandon < replace_command < reschedule
assert abandon < replace_epoch < reschedule
assert abandon < replace_phase < reschedule

publish = function_body(ANCHOR, "gateway_discovery_assignment_publish_table")
assert "app_gateway_assignment_publisher_stage_sorted_ids(" not in publish
assert "app_gateway_assignment_publisher_stage_table_ready(" not in publish
assert "gateway_observe_command_event(&event, false)" not in publish
delivery = function_body(
    ANCHOR, "gateway_discovery_assignment_service_delivery"
)
assert "event.reason == NODE_COMM_TERMINAL_DELIVERED" in delivery
assert "kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE" in delivery
assert "app_gateway_assignment_publisher_stage_table_ready(" not in delivery
assert "gateway_discovery_assignment_expected_claims_complete_locked()" in delivery
assert "gateway_discovery_assignment_missing_ack_count_locked() == 0u" in delivery
assert re.search(r"K_MSEC\s*\(\s*wait_ms\s*\)", delivery)
semantic_policy = delivery.index(
    "app_discovery_assignment_semantic_terminal_success("
)
semantic_marker = delivery.index(
    "DBG_DISCOVERY_SLOT_FLOOD_SEMANTIC_OVERRIDE"
)
strict_deadline_failure = delivery.index(
    "event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED"
)
assert semantic_policy < semantic_marker < strict_deadline_failure
assert "event.attempts_started" in delivery[semantic_policy:semantic_marker]
assert "operation_deadline_ms" in delivery[semantic_policy:semantic_marker]
assert "event.reason == NODE_COMM_TERMINAL_CANCELLED" in delivery[
    semantic_policy:semantic_marker
]
assert "effective_delivered" in delivery[semantic_policy:strict_deadline_failure]

submit_flood = function_body(
    ANCHOR, "gateway_discovery_assignment_submit_control_flood_locked"
)
assert "discovery_assignment_control_flood_deadline_ms(" in submit_flood
submit_call = submit_flood[submit_flood.index("app_node_comm_submit_delivery(") :]
assert "absolute_deadline_ms" in submit_call
assert re.search(
    r"#define\s+DISCOVERY_ASSIGNMENT_CONTROL_FLOOD_DEADLINE_MS\s+10000u",
    ASSIGNMENT,
)
assert re.search(r"DISCOVERY_ASSIGNMENT_CLAIM_MAX_ROUNDS\s+1u", ASSIGNMENT)
assert re.search(r"DISCOVERY_ASSIGNMENT_TABLE_MAX_ROUNDS\s+1u", ASSIGNMENT)

note_claim = function_body(ANCHOR, "gateway_discovery_assignment_note_claim")
assert "gateway_discovery_assignment_expected_claims_complete_locked()" in note_claim
assert "discovery_assignment_response_ack_settle_deadline_ms(" in note_claim
assert "K_NO_WAIT" in note_claim
complete_success = function_body(
    ANCHOR, "gateway_discovery_assignment_complete_success_locked"
)
assert "gateway_discovery_assignment_missing_ack_count_locked() != 0u" in complete_success
assert "discovery_assignment_response_ack_settle_pending(" in complete_success
membership = complete_success.index("gateway_set_registered_membership_roster(")
stage_batch = complete_success.index(
    "app_gateway_assignment_publisher_stage_sorted_ids("
)
stage_table = complete_success.index(
    "app_gateway_assignment_publisher_stage_table_ready("
)
terminal = complete_success.index("gateway_observe_command_event(&event, true)")
host_result = complete_success.index("gateway_emit_host_command_result(")
success = complete_success.index("COMMAND_OK", host_result)
assert membership < stage_batch < stage_table < host_result < success < terminal
membership_failure = complete_success[membership:stage_batch]
assert "gateway_discovery_assignment_fail_locked(" in membership_failure
assert "COMMAND_INTERNAL_ERROR" in membership_failure
assert "discovery_assignment_membership_epoch(" in complete_success
assert function_body(PUBLISHER, "app_gateway_assignment_publisher_stage_batch")
assert function_body(PUBLISHER, "app_gateway_assignment_publisher_stage_sorted_ids")

window = function_body(ANCHOR, "gateway_discovery_assignment_window_ms_locked")
assert "app_discovery_assignment_table_windows_remaining(" in window
assert "return remaining_ms;" not in window
finalize = function_body(
    ANCHOR, "gateway_discovery_assignment_finalize_work_handler"
)
assert "discovery_assignment_response_ack_settle_pending(" in finalize
assert "response_ack_settle_deadline_ms" in finalize
assert "app_discovery_assignment_table_retry_backoff_required(" in finalize
assert "discovery_assignment_retry_backoff_ms(" in finalize
assert "DBG_DISCOVERY_SLOT_TABLE_BACKOFF" in finalize
publish_work = function_body(
    ANCHOR, "gateway_discovery_assignment_publish_work_handler"
)
assert "expected_claim_count" in publish_work
assert "gateway_discovery_assignment_fail_locked(" in publish_work
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
