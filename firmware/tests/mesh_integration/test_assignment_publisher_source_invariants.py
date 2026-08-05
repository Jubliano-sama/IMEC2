#!/usr/bin/env python3
from pathlib import Path
import re

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = read_composed_source(ROOT / "app/src/app_anchor.c")
BLE = read_composed_source(ROOT / "app/src/app_gateway_ble.c")
STREAM = (ROOT / "app/src/app_gateway_ble_stream.h").read_text()
PUBLISHER = (ROOT / "app/src/app_gateway_assignment_publisher.c").read_text()
PERSISTENCE = (ROOT / "app/src/app_mesh_persistence.c").read_text()
ASSIGNMENT = (ROOT / "include/discovery_assignment.h").read_text()
ASSIGNMENT_DEFINES = re.sub(r"\\\s*\n\s*", " ", ASSIGNMENT)
ANCHOR_DEFINES = re.sub(r"\\\s*\n\s*", " ", ANCHOR)


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


next_broadcast_sequence = function_body(
    BLE, "gateway_next_broadcast_command_seq"
)
sequence_lock = next_broadcast_sequence.index(
    "k_mutex_lock(&gateway_broadcast_command_sequence_mutex"
)
sequence_reserve = next_broadcast_sequence.index(
    "gateway_broadcast_command_sequence_reserve_locked(",
    sequence_lock,
)
sequence_consume = next_broadcast_sequence.index(
    "sequence = gateway_broadcast_command_sequence_next", sequence_reserve
)
sequence_unlock = next_broadcast_sequence.index(
    "k_mutex_unlock(&gateway_broadcast_command_sequence_mutex",
    sequence_consume,
)
assert sequence_lock < sequence_reserve < sequence_consume < sequence_unlock, (
    "block reservation and last-sequence consumption must share one mutex "
    "transaction so two boundary callers cannot return zero"
)
sequence_reserve_locked = function_body(
    BLE, "gateway_broadcast_command_sequence_reserve_locked"
)
assert "app_mesh_persistence_reserve_gateway_command_sequences(" in (
    sequence_reserve_locked
), "the locked reservation helper must durably reserve each sequence block"


assert re.search(r"#define GATEWAY_BLE_STREAM_QUEUE_DEPTH\s+3u", STREAM)
publisher_state = PUBLISHER[
    PUBLISHER.index("struct app_gateway_assignment_publisher_state") :
    PUBLISHER.index("_Static_assert(")
]
assert "anchor_ids[APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES]" in publisher_state
assert "slots[APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES]" not in publisher_state
assert "pending_event_seq" in publisher_state
assert "publisher.anchor_ids[slots[i]] = anchor_ids[i]" in PUBLISHER
assert "APP_GATEWAY_ASSIGNMENT_PUBLISHER_RAM_BUDGET_BYTES" in PUBLISHER
publisher_next = function_body(PUBLISHER, "build_next_event")
publisher_slot = publisher_next.index("mapping_slot_for_ordinal(")
publisher_slot_guard = publisher_next.index(
    "slot == GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE",
    publisher_slot,
)
publisher_slot_shift = publisher_next.index(
    "UINT64_C(1) << slot",
    publisher_slot,
)
assert publisher_slot < publisher_slot_guard < publisher_slot_shift, (
    "sparse assignment publication must reject an unavailable slot before "
    "using it as a shift count"
)

# Both bounded control-flood terminal events are consumed by a polled gateway
# worker. Reserve one complete poll interval per phase in the shared operation
# budget, and keep the app poll cadence tied to the shared timing contract.
assert re.search(
    r"#define\s+DISCOVERY_ASSIGNMENT_DELIVERY_TERMINAL_POLL_MS\s+5u",
    ASSIGNMENT_DEFINES,
)
assert re.search(
    r"#define\s+DISCOVERY_ASSIGNMENT_CONTROL_PHASE_COUNT\s+2u",
    ASSIGNMENT_DEFINES,
)
assert re.search(
    r"#define\s+DISCOVERY_ASSIGNMENT_OPERATION_TERMINAL_SCHEDULING_GUARD_MS\s+"
    r"\(DISCOVERY_ASSIGNMENT_CONTROL_PHASE_COUNT\s*\*\s*"
    r"DISCOVERY_ASSIGNMENT_DELIVERY_TERMINAL_POLL_MS\)",
    ASSIGNMENT_DEFINES,
)
assert re.search(
    r"#define\s+DISCOVERY_ASSIGNMENT_OPERATION_MIN_BUDGET_MS\s+"
    r"DISCOVERY_ASSIGNMENT_OPERATION_REQUIRED_BUDGET_MS\(\s*"
    r"DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MIN_MS\)",
    ASSIGNMENT_DEFINES,
)
assert re.search(
    r"#define\s+DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS\s+"
    r"DISCOVERY_ASSIGNMENT_OPERATION_REQUIRED_BUDGET_MS\(\s*"
    r"DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS\)",
    ASSIGNMENT_DEFINES,
)
assert re.search(
    r"#define\s+GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_POLL_MS\s+"
    r"DISCOVERY_ASSIGNMENT_DELIVERY_TERMINAL_POLL_MS",
    ANCHOR_DEFINES,
)
assert re.search(
    r"BUILD_ASSERT\(DISCOVERY_ASSIGNMENT_OPERATION_TERMINAL_SCHEDULING_GUARD_MS\s*"
    r">=\s*DISCOVERY_ASSIGNMENT_CONTROL_PHASE_COUNT\s*\*\s*"
    r"GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_POLL_MS\s*,",
    ANCHOR_DEFINES,
)

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
delay_position = schedule.index("anchor_discovery_response_delay_ms(")
deadline_position = schedule.index(
    "discovery_assignment_response_deadline_ms("
)
assert delay_position < deadline_position
assert "absolute_deadline_ms" in schedule
assert "DBG_DISCOVERY_SLOT_RESPONSE_ADMISSION_RETRY" in retry
assert "GATEWAY_COMMAND_RESULT_TIMEOUT_MS" not in response
assert "DISCOVERY_ASSIGNMENT_RESPONSE_CUSTODY_MAX_MS" in ANCHOR
orphan_cleanup = function_body(
    ANCHOR, "anchor_discovery_claim_service_failed_abandon"
)
assert "app_node_comm_abandon_delivery(handle)" in orphan_cleanup
assert "ret != -ENOENT && ret != -EALREADY" in orphan_cleanup
assert "app_watchdog_stop_feeding()" in orphan_cleanup
assert retry.index(
    "anchor_discovery_claim_service_failed_abandon()"
) < retry.index("anchor_discovery_claim_snapshot(&pending)")
stale_abandon = retry.index(
    "app_node_comm_abandon_delivery(delivery_handle)"
)
stale_retain = retry.index(
    "anchor_discovery_claim_failed_abandon_handle =",
    stale_abandon,
)
assert stale_abandon < stale_retain
assert "app_watchdog_stop_feeding()" in retry[stale_retain:]

assignment_start = function_body(ANCHOR, "gateway_start_discovery_assignment")
start_prepare = assignment_start.index("app_operation_policy_prepare_payload(")
start_budget = assignment_start.index(
    "operation_policy_assignment_required_budget_ms("
)
start_commit = assignment_start.index(
    "app_operation_policy_commit_prepared("
)
start_state = assignment_start.index(
    "gateway_discovery_assignment_state.active = true"
)
assert start_prepare < start_budget < start_commit < start_state
assert "app_operation_policy_install(" not in assignment_start

assignment_apply = function_body(
    ANCHOR, "anchor_apply_discovery_assignment_command"
)
assert assignment_apply.index("app_operation_policy_prepare_payload(") < (
    assignment_apply.index("local_anchor_discovery_assignment_note_claim(")
)
assert "app_operation_policy_install(" not in assignment_apply
assert "app_operation_policy_commit_prepared(" in assignment_apply
replay_start = assignment_apply.index(
    "APP_DISCOVERY_ASSIGNMENT_TABLE_REPLAY"
)
replay_end = assignment_apply.index(
    "\n    for (size_t i = 0u; i < entry_count; i++) {",
    replay_start,
)
replay_branch = assignment_apply[replay_start:replay_end]
assert replay_branch.count("app_operation_policy_commit_prepared(") == 3, (
    "pending, committed-assigned, and persisted-unassigned exact replays "
    "must restore the accepted RAM-only assignment profile"
)

# Repeated RF envelopes first find the response owner by epoch plus phase.
# Exact operation identity is then checked separately: packet sequence may
# change across a bounded flood retry, but a conflicting command session or
# ACK TABLE commitment is rejected before it can retain or replace custody.
same_phase = schedule.index("discovery_assignment_response_custody_matches(")
same_phase_end = schedule.index(");", same_phase) + len(");")
same_phase_gate = schedule[same_phase:same_phase_end]
assert "anchor_discovery_claim_pending.active" in same_phase_gate
assert "anchor_discovery_claim_pending.epoch" in same_phase_gate
assert "anchor_discovery_claim_pending.phase" in same_phase_gate
assert "command->session_id" not in same_phase_gate
assert "command->seq" not in same_phase_gate
identity_start = schedule.index(
    "app_discovery_assignment_response_identity_matches(",
    same_phase_end,
)
identity_end = schedule.index("return -ESTALE;", identity_start)
identity_gate = schedule[identity_start:identity_end]
assert "anchor_discovery_claim_pending.command.session_id" in identity_gate
assert "command->session_id" in identity_gate
assert "anchor_discovery_claim_pending.table_commitment" in identity_gate
assert "table_commitment" in identity_gate
assert "command->seq" not in identity_gate

# Invalid phase/epoch/slot input is rejected before custody can change.  A
# valid new phase or epoch falls through the same-phase return. Replacement
# first invalidates queued work, abandons the captured exact handle outside the
# response-state mutex, and can restore the complete old owner on failure
# before installing and scheduling the new generation.
validation = schedule[:same_phase]
assert "epoch == 0u" in validation
assert "phase != DISCOVERY_ASSIGNMENT_PHASE_CLAIM" in validation
assert "phase != DISCOVERY_ASSIGNMENT_PHASE_ACK" in validation
assert "slot_count == 0u" in validation
assert "slot >= slot_count" in validation
supersede = schedule[same_phase_end:]
retain = supersede.index("return 0;")
snapshot = supersede.index(
    "replaced_pending = anchor_discovery_claim_pending",
    retain + len("return 0;"),
)
capture = supersede.index(
    "replaced_delivery_handle = replaced_pending.delivery_handle",
    snapshot,
)
invalidate = supersede.index(
    "anchor_discovery_claim_pending.active = false", capture
)
reserved_generation = supersede.index(
    "anchor_discovery_claim_pending.generation = generation", invalidate
)
unlock_before_abandon = supersede.index(
    "k_mutex_unlock(&anchor_discovery_claim_mutex)", reserved_generation
)
abandon = supersede.index(
    "app_node_comm_abandon_delivery(", unlock_before_abandon
)
rollback = supersede.index(
    "anchor_discovery_claim_pending = replaced_pending", abandon
)
generation = supersede.index(
    "anchor_discovery_claim_pending.generation = generation",
    rollback,
)
replace_command = supersede.index("anchor_discovery_claim_pending.command = *command")
replace_epoch = supersede.index("anchor_discovery_claim_pending.epoch = epoch")
replace_phase = supersede.index("anchor_discovery_claim_pending.phase = phase")
reschedule = supersede.index(
    "anchor_discovery_claim_reschedule_locked(",
    generation,
)
assert retain < snapshot < capture < invalidate
assert invalidate < reserved_generation < unlock_before_abandon < abandon
assert abandon < rollback < generation
assert generation < replace_command < reschedule
assert generation < replace_epoch < reschedule
assert generation < replace_phase < reschedule

publish = function_body(ANCHOR, "gateway_discovery_assignment_publish_table")
assert "app_gateway_assignment_publisher_stage_sorted_ids(" not in publish
assert "app_gateway_assignment_publisher_stage_table_ready(" not in publish
assert "gateway_observe_command_event(&event, false)" not in publish
sort_call = publish[
    publish.index("discovery_assignment_order_roster_extension(") :
    publish.index(
        ");", publish.index("discovery_assignment_order_roster_extension(")
    ) + 2
]
assert "prior_anchor_count" in sort_call
assert "claim_count" in sort_call
delivery = function_body(
    ANCHOR, "gateway_discovery_assignment_service_delivery"
)
assert "event.reason == NODE_COMM_TERMINAL_DELIVERED" in delivery
assert "kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE" in delivery
assert "app_gateway_assignment_publisher_stage_table_ready(" not in delivery
assert "gateway_discovery_assignment_expected_claims_complete_locked()" in delivery
assert "gateway_discovery_assignment_missing_ack_count_locked() == 0u" in delivery
assert re.search(r"K_MSEC\s*\(\s*wait_ms\s*\)", delivery)
peek_terminal = delivery.index("app_node_comm_peek_delivery_event_for(")
validation_barrier = delivery.index(
    "gateway_discovery_assignment_boundary_ready_locked(", peek_terminal
)
take_terminal = delivery.index(
    "app_node_comm_take_delivery_event_for(", validation_barrier
)
assert peek_terminal < validation_barrier < take_terminal
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
quorum_arm = note_claim.index(
    "discovery_assignment_ack_quorum_settle_should_arm("
)
quorum_deadline = note_claim.index(
    "discovery_assignment_response_ack_settle_deadline_ms("
)
assert quorum_arm < quorum_deadline
assert "response_ack_settle_armed" in note_claim[quorum_arm:quorum_deadline]
assert "K_NO_WAIT" in note_claim

# A two-hop child can still be completing the gateway-ACK bubble and its
# shared-relay timing cleanup when its accepted CLAIM completes the roster.
# Keep the one logical table flood behind a hop-aware quiet interval. A valid
# duplicate proves the prior ACK did not finish, so it must restart that same
# interval while the transaction still collects CLAIMs.
claim_settle_deadline = "discovery_assignment_claim_ack_settle_deadline_ms("
arm_claim_settle = function_body(
    ANCHOR, "gateway_discovery_assignment_arm_claim_ack_settle_locked"
)
assert "GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS" in arm_claim_settle
assert "gateway_discovery_assignment_expected_claims_complete_locked()" in (
    arm_claim_settle
)
assert claim_settle_deadline in arm_claim_settle
assert "gateway_discovery_assignment_state.max_hop_count" in arm_claim_settle[
    arm_claim_settle.index(claim_settle_deadline):
]
assert re.search(r"claim\w*_settle_armed\s*=\s*true", arm_claim_settle)
assert re.search(r"claim\w*_settle_deadline_ms\s*=", arm_claim_settle)

claim_settle_remaining = function_body(
    ANCHOR, "gateway_discovery_assignment_claim_ack_settle_remaining_locked"
)
assert "discovery_assignment_response_ack_settle_pending(" in (
    claim_settle_remaining
)
assert re.search(r"claim\w*_settle_deadline_ms", claim_settle_remaining)
assignment_reschedule = function_body(
    ANCHOR, "gateway_discovery_assignment_reschedule"
)
assert "mesh_route_owner_work_reschedule_timeout(" in assignment_reschedule
assert "&gateway_discovery_assignment_finalize_work" in assignment_reschedule
assert "app_watchdog_stop_feeding()" in assignment_reschedule
duplicate_claim = note_claim.index("if (anchor_index != SIZE_MAX)")
new_claim = note_claim.index(
    "gateway_discovery_assignment_state.anchor_ids[", duplicate_claim
)
duplicate_path = note_claim[duplicate_claim:new_claim]
assert "gateway_discovery_assignment_arm_claim_ack_settle_locked(" in (
    duplicate_path
)
assert "gateway_discovery_assignment_state.round_open" in duplicate_path
assert "gateway_discovery_assignment_reschedule(" in duplicate_path
assert '"duplicate-claim-settle"' in duplicate_path
assert "k_work_reschedule(" not in duplicate_path

new_claim_path = note_claim[new_claim:]
quorum = new_claim_path.index(
    "gateway_discovery_assignment_expected_claims_complete_locked()"
)
assert "gateway_discovery_assignment_arm_claim_ack_settle_locked(" in (
    new_claim_path[quorum:]
)
complete_success = function_body(
    ANCHOR, "gateway_discovery_assignment_complete_success_locked"
)
# Ordinary assignment publishes any useful ACKed subset. Missing ACKs remain
# explicit failures, but they do not revoke the anchors that committed.
assert "gateway_discovery_assignment_missing_ack_count_locked() != 0u" not in complete_success
assert "discovery_assignment_response_ack_settle_pending(" in complete_success
assert "gateway_discovery_assignment_state.ack_mask" in complete_success
membership = complete_success.index("gateway_set_registered_membership_roster(")
prepare_batch = complete_success.index(
    "app_gateway_assignment_publisher_prepare_table("
)
commit_batch = complete_success.index(
    "app_gateway_assignment_publisher_commit_prepared_batch("
)
stage_table = complete_success.index(
    "app_gateway_assignment_publisher_stage_table_ready("
)
terminal = complete_success.index("gateway_observe_command_event(&event, true)")
host_result = complete_success.index(
    "gateway_emit_host_command_result_reserved("
)
success = complete_success.index("COMMAND_OK", host_result)
assert (
    prepare_batch < membership < commit_batch <
    stage_table < host_result < success < terminal
)
membership_failure = complete_success[membership:commit_batch]
assert "gateway_discovery_assignment_fail_locked(" in membership_failure
assert "COMMAND_INTERNAL_ERROR" in membership_failure
assert "app_gateway_assignment_publisher_abort_prepared_batch(" in (
    membership_failure
)
assert "discovery_assignment_membership_epoch(" in complete_success
membership_call = complete_success[
    membership : complete_success.index(");", membership) + 2
]
prepare_batch_call = complete_success[
    prepare_batch : complete_success.index(");", prepare_batch) + 2
]
assert "committed_anchor_ids" in membership_call
assert "committed_count" in membership_call
assert "gateway_discovery_assignment_state.anchor_ids" in prepare_batch_call
assert "gateway_discovery_assignment_state.anchor_slots" in prepare_batch_call
assert "gateway_discovery_assignment_state.claim_count" in prepare_batch_call
assert "acknowledged_slot_mask" in prepare_batch_call
assert "publication.claimed_node_ids[slot]" in complete_success
assert "publication.committed_mask = committed_slot_mask" in complete_success
assert (
    "publication.acknowledged_mask = acknowledged_slot_mask"
    in complete_success
)
assert "failure_count" in complete_success
assert function_body(
    PUBLISHER, "app_gateway_assignment_publisher_prepare_table"
)
assert function_body(
    PUBLISHER, "app_gateway_assignment_publisher_commit_prepared_batch"
)
assert function_body(
    PUBLISHER, "app_gateway_assignment_publisher_abort_prepared_batch"
)
assert function_body(PUBLISHER, "app_gateway_assignment_publisher_stage_table")
assert function_body(PUBLISHER, "app_gateway_assignment_publisher_stage_sorted_ids")

resume_publication = function_body(
    ANCHOR, "gateway_resume_pending_assignment_publication"
)
resume_commit = resume_publication.index(
    "app_gateway_assignment_publisher_commit_prepared_batch("
)
resume_result = resume_publication.index(
    "gateway_emit_host_command_result_reserved("
)
resume_commit_failure = resume_publication[
    resume_commit:resume_result
]
assert resume_commit < resume_result, (
    "reboot publication must activate the prepared batch before reporting "
    "host success"
)
assert "gateway_command_result_release_reserved(" in resume_commit_failure, (
    "failed publication activation must release its bound result reservation"
)

window = function_body(ANCHOR, "gateway_discovery_assignment_window_ms_locked")
assert "app_discovery_assignment_table_windows_remaining(" in window
assert "app_discovery_assignment_collection_hop_count(" in window
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
# Expected count is an early-completion hint and host qualification target. A
# useful partial claim set must still advance to table publication when its
# conservative collection window closes.
strict_expected_failure = re.search(
    r"expected_claim_count\s*!=\s*0u\s*&&\s*"
    r"gateway_discovery_assignment_state\.claim_count\s*<\s*"
    r"gateway_discovery_assignment_state\.expected_claim_count\s*\)\s*\{\s*"
    r"gateway_discovery_assignment_fail_locked\(",
    publish_work,
    re.S,
)
assert strict_expected_failure is None
assert "DBG_DISCOVERY_SLOT_CLAIM_BACKOFF" in publish_work
assert "discovery_assignment_retry_backoff_ms(" in publish_work
table_publish = publish_work.index("gateway_discovery_assignment_publish_table()")
claim_settle_gate = publish_work.index(
    "gateway_discovery_assignment_claim_ack_settle_remaining_locked("
)
assert claim_settle_gate < table_publish
assert re.search(
    r"claim\w*_settle_deadline_ms", publish_work[claim_settle_gate:table_publish]
)
claim_settle_path = publish_work[claim_settle_gate:table_publish]
assert "gateway_discovery_assignment_reschedule(" in claim_settle_path
assert '"claim-ack-settle"' in claim_settle_path
assert "k_work_reschedule(" not in claim_settle_path

admit = function_body(BLE, "gateway_observe_command_event_if_available")
first_capacity = admit.index("gateway_ble_stream_state.count >=")
reserve_identity = admit.index(
    "gateway_observability_reserve_identity(", first_capacity
)
second_capacity = admit.index(
    "gateway_ble_stream_state.count >=", reserve_identity
)
prepare = admit.index(
    "gateway_observability_prepare_reserved_state(", second_capacity
)
assert first_capacity < reserve_identity < second_capacity < prepare
assert "!gateway_ble_stream_ready()" in admit
assert "gateway_ble_tx_in_flight" not in admit
assert "items[i].priority == 0u" not in admit
assert "retain_until_sent = true" in admit

reserve = function_body(BLE, "gateway_observability_reserve_identity")
assert "gateway_next_broadcast_command_seq()" in reserve
assert "k_spin_lock(" not in reserve

prepare_state = function_body(BLE, "gateway_observability_prepare_state")
assert prepare_state.index("gateway_observability_reserve_identity(") < (
    prepare_state.index("gateway_observability_prepare_reserved_state(")
)

prepare_reserved = function_body(
    BLE, "gateway_observability_prepare_reserved_state"
)
state_lock = prepare_reserved.index(
    "k_spin_lock(&gateway_command_observability_lock)"
)
state_prepare = prepare_reserved.index(
    "gateway_command_observability_prepare_with_sequence(", state_lock
)
state_unlock = prepare_reserved.index(
    "k_spin_unlock(&gateway_command_observability_lock", state_prepare
)
assert state_lock < state_prepare < state_unlock

publisher_pump = function_body(
    PUBLISHER, "app_gateway_assignment_publisher_pump"
)
assert "publisher.pending_event_seq = event.event_seq" in publisher_pump
assert "publisher.pending_event_seq = 0u" in publisher_pump

observe = function_body(BLE, "gateway_observe_command_event")
assert observe.index("app_gateway_assignment_publisher_capture_terminal(") < observe.index(
    "gateway_observability_prepare_state("
)

complete = function_body(BLE, "gateway_ble_tx_complete")
assert "app_gateway_assignment_publisher_note_sent(" in complete
assert "app_gateway_assignment_publisher_work_pending()" in complete
assert "app_gateway_assignment_publisher_pump();" not in complete
assert "gateway_schedule_persistence_retry(" in complete

publisher_note_sent = function_body(
    PUBLISHER, "app_gateway_assignment_publisher_note_sent"
)
assert "app_gateway_assignment_publisher_pump(" not in publisher_note_sent
assert "batch_completed" not in publisher_note_sent
assert "publisher.completion_pending = true" in publisher_note_sent

publisher_pump = function_body(
    PUBLISHER, "app_gateway_assignment_publisher_pump"
)
assert "batch_completed" not in publisher_pump
assert "publisher.completion_pending" in publisher_pump

publisher_complete = function_body(
    PUBLISHER, "app_gateway_assignment_publisher_complete_pending"
)
assert "publisher.ops.batch_completed" in publisher_complete
assert "publisher.completion_pending" in publisher_complete

persistence_retry = function_body(
    BLE, "gateway_persistence_retry_work_handler"
)
complete_debt = persistence_retry.index(
    "app_gateway_assignment_publisher_complete_pending("
)
next_event = persistence_retry.index(
    "app_gateway_assignment_publisher_pump();", complete_debt
)
assert complete_debt < next_event
assert persistence_retry.index(
    "gateway_schedule_persistence_retry(",
    complete_debt,
) < next_event

publication_complete = function_body(
    PERSISTENCE,
    "app_mesh_persistence_complete_gateway_assignment_publication",
)
assert "struct gateway_membership_snapshot completed" not in publication_complete
assert "table_commitment = stored.current.assignment_table_commitment" in (
    publication_complete
)
assert re.search(
    r"gateway_membership_export_assignment_snapshot\s*\("
    r".*?&stored\.current\s*\)",
    publication_complete,
    re.DOTALL,
), "publication completion must rewrite the validated NVS buffer in place"

ble_init = function_body(BLE, "gateway_ble_init")
publication_pending = ble_init.index("gateway_assignment_publication_pending()")
publication_handoff = ble_init.index(
    '"assignment-publication-init"', publication_pending
)
assert publication_pending < publication_handoff
assert "gateway_resume_pending_assignment_publication()" not in (
    ble_init[publication_pending:publication_handoff]
), "boot/main must not retain the full publication reconstruction stack"

print("assignment publisher source invariants passed")
