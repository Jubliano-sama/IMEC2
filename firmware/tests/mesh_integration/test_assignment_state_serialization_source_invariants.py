#!/usr/bin/env python3

from pathlib import Path
import re

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = read_composed_source(ROOT / "app/src/app_anchor.c")
MAIN = (ROOT / "app/src/main.c").read_text(encoding="utf-8")
PERSISTENCE_HEADER = (
    ROOT / "app/src/app_mesh_persistence.h"
).read_text(encoding="utf-8")
PERSISTENCE = (
    ROOT / "app/src/app_mesh_persistence.c"
).read_text(encoding="utf-8")
APP_STATE = (ROOT / "app/src/app_state.c").read_text(encoding="utf-8")
STATE = "gateway_discovery_assignment_state."
LOCK = "k_mutex_lock(&gateway_discovery_assignment_mutex"
UNLOCK = "k_mutex_unlock(&gateway_discovery_assignment_mutex)"


def function_definitions(source: str) -> dict[str, str]:
    definitions: dict[str, str] = {}
    pattern = re.compile(
        r"(?m)^(?:static\s+)?(?:[A-Za-z_]\w*(?:\s+|\s*\*\s*))+"
        r"(?P<name>(?:gateway_(?:start_discovery_assignment|"
        r"send_discovery_assignment_[A-Za-z0-9_]+|"
        r"discovery_assignment_[A-Za-z0-9_]+)|"
        r"anchor_(?:schedule_discovery_response|"
        r"schedule_late_discovery_claim|"
        r"cancel_discovery_response|"
        r"apply_discovery_assignment_command_serialized|"
        r"promote_discovery_assignment_after_ack(?:_locked)?|"
        r"settle_ack_before_newer_assignment|"
        r"persist_discovery_assignment_ack_retry_round|"
        r"resume_pending_discovery_assignment_ack|"
        r"discovery_(?:claim|ack)_[A-Za-z0-9_]+)))\s*\("
    )
    for candidate in pattern.finditer(source):
        name = candidate.group("name")
        paren = source.index("(", candidate.start())
        depth = 0
        brace = None
        for index in range(paren, len(source)):
            depth += source[index] == "("
            depth -= source[index] == ")"
            if depth != 0:
                continue
            next_index = index + 1
            while next_index < len(source) and source[next_index].isspace():
                next_index += 1
            if next_index < len(source) and source[next_index] == "{":
                brace = next_index
            break
        if brace is None:
            continue
        depth = 0
        for end in range(brace, len(source)):
            depth += source[end] == "{"
            depth -= source[end] == "}"
            if depth == 0:
                # Keep the production gateway definition when the source also
                # contains a non-gateway -ENOTSUP stub under the #else branch.
                definitions.setdefault(name, source[candidate.start() : end + 1])
                break
        else:
            raise AssertionError(f"unterminated function {name}")
    return definitions


def assert_direct_owner_covers_state(name: str, body: str) -> None:
    lock = body.find(LOCK)
    assert lock >= 0, f"{name} does not acquire the assignment mutex"
    first_state = body.find(STATE)
    assert first_state < 0 or lock < first_state, (
        f"{name} reads assignment state before acquiring its mutex"
    )
    last_state = body.rfind(STATE)
    last_unlock = body.rfind(UNLOCK)
    owned_return = re.search(
        r"#define\s+(GATEWAY_ASSIGNMENT_[A-Z_]*RETURN)\([^\n]*\).*?"
        r"k_mutex_unlock\(&gateway_discovery_assignment_mutex\)",
        body,
        re.DOTALL,
    )
    macro_unlocks_after_state = (
        owned_return is not None and
        body.rfind(owned_return.group(1)) > last_state
    )
    assert last_unlock > last_state or macro_unlocks_after_state, (
        f"{name} can leave assignment state ownership without an unlock"
    )


functions = function_definitions(ANCHOR)

# These functions are entered by independent execution contexts: host command
# ingress, mesh RX, the gateway-priority workqueue, and delayable finalization.
# Each boundary owns the mutex for its complete state decision, so claim/ACK
# updates cannot race table construction or completion.
entrypoints = (
    "gateway_start_discovery_assignment",
    "gateway_discovery_assignment_note_claim",
    "gateway_discovery_assignment_publish_work_handler",
    "gateway_discovery_assignment_finalize_work_handler",
)
for entrypoint in entrypoints:
    assert entrypoint in functions, f"missing assignment entrypoint {entrypoint}"
    assert_direct_owner_covers_state(entrypoint, functions[entrypoint])

# Assignment finalization publishes the committed 50-entry membership roster
# consumed by route-owned collection code. It must execute on that same owner
# queue, so a reader can see the complete old or complete new snapshot only.
assignment_reschedule = functions[
    "gateway_discovery_assignment_reschedule"
]
assert "mesh_route_owner_work_reschedule_timeout(" in assignment_reschedule
assert "k_work_reschedule(" not in assignment_reschedule

# Every other assignment function that touches the shared transaction must
# either acquire the mutex itself or advertise that its caller must hold it.
# The explicit `_locked` suffix makes ownership reviewable and prevents a new
# workqueue/RX caller from accidentally reusing an unguarded helper.
unguarded = []
direct_owners = set(entrypoints)
for name, body in functions.items():
    if STATE not in body or name in entrypoints:
        continue
    if LOCK in body:
        assert_direct_owner_covers_state(name, body)
        direct_owners.add(name)
        continue
    if not name.endswith("_locked"):
        unguarded.append(name)
assert not unguarded, (
    "assignment state helpers must lock directly or be explicit _locked "
    f"helpers: {', '.join(sorted(unguarded))}"
)

# A `_locked` helper is only valid when every call site appears inside a
# lexical mutex-owned region. This catches a future direct call from RX or a
# new work item without requiring a runtime race to reproduce it.
for helper, helper_body in functions.items():
    if not helper.endswith("_locked") or STATE not in helper_body:
        continue
    call_pattern = re.compile(rf"\b{re.escape(helper)}\s*\(")
    callers = []
    for caller, caller_body in functions.items():
        if caller == helper:
            continue
        for call in call_pattern.finditer(caller_body):
            if caller.endswith("_locked") or caller in direct_owners:
                continue
            prefix = caller_body[: call.start()]
            if prefix.rfind(LOCK) <= prefix.rfind(UNLOCK):
                callers.append(caller)
    assert not callers, (
        f"{helper} is called without assignment mutex ownership by "
        f"{', '.join(sorted(set(callers)))}"
    )

# The 64-bit ACK word must remain inside the same serialized transaction; on
# the 32-bit nRF target an unlocked read can tear even when writers use `|=`.
ack_users = [
    name
    for name, body in functions.items()
    if "ack_mask" in body or "expected_ack_mask" in body
]
assert ack_users, "assignment ACK state has no audited users"
for name in ack_users:
    body = functions[name]
    assert LOCK in body or name.endswith("_locked"), (
        f"64-bit assignment ACK state is unguarded in {name}"
    )

# A gateway epoch is a durable reservation, not an in-memory transaction ID.
# The write must succeed before policy/state/RF activation so an NVS failure
# cannot create an epoch that another gateway boot could reuse.
start = functions["gateway_start_discovery_assignment"]
reserve = start.find("gateway_discovery_assignment_reserve_epoch(&reserved_epoch)")
reserve_claim_sequence = start.find(
    "claim_command_seq = gateway_next_broadcast_command_seq()"
)
commit_policy = start.find("app_operation_policy_commit_prepared")
reset_state = start.find("memset(&gateway_discovery_assignment_state")
activate = start.find("gateway_discovery_assignment_state.active = true")
schedule_first_claim = start.find(
    'gateway_discovery_assignment_reschedule(\n'
    '        K_NO_WAIT, "assignment-start")'
)
assert -1 not in (
    reserve,
    reserve_claim_sequence,
    commit_policy,
    reset_state,
    activate,
    schedule_first_claim,
), (
    "assignment start is missing durable identity reservation, activation, "
    "or its asynchronous first-CLAIM handoff"
)
assert (
    reserve < reserve_claim_sequence < commit_policy <
    reset_state < activate < schedule_first_claim
), (
    "assignment epoch and CLAIM identity must reserve before "
    "policy/state activation and the first-CLAIM work handoff"
)
assert "gateway_discovery_assignment_open_claim_round_locked" not in start, (
    "host command ingress must not synchronously retain the CLAIM flood and "
    "node-communication stack chain"
)
reservation_failure = start[reserve:commit_policy]
assert "if (ret < 0)" in reservation_failure
assert "GATEWAY_ASSIGNMENT_START_RETURN(ret)" in reservation_failure
claim_sequence_failure = start[
    reserve_claim_sequence:commit_policy
]
assert "if (claim_command_seq == 0u)" in claim_sequence_failure
assert "GATEWAY_ASSIGNMENT_START_RETURN(-EIO)" in claim_sequence_failure
for forbidden_mutation in (
    "app_operation_policy_commit_prepared",
    "memset(&gateway_discovery_assignment_state",
    "gateway_discovery_assignment_state.active = true",
    "gateway_discovery_assignment_open_claim_round_locked",
):
    assert forbidden_mutation not in claim_sequence_failure, (
        "CLAIM sequence reservation failure can mutate active "
        f"policy/state/RF through {forbidden_mutation}"
    )
assert re.search(
    r"expected_claim_count\s*(?:<|<=)\s*prior_anchor_count|"
    r"prior_anchor_count\s*(?:>|>=)\s*expected_claim_count",
    start,
) is None, (
    "a retained roster larger than the current expected responder count must "
    "not reject enumeration admission"
)

reserve_body = functions["gateway_discovery_assignment_reserve_epoch"]
retry_restore = reserve_body.find(
    "gateway_discovery_assignment_restore_epoch_cursor()"
)
persist_epoch = reserve_body.find(
    "app_mesh_persistence_save_gateway_assignment_epoch(next_epoch)"
)
cursor_commit = reserve_body.find(
    "gateway_discovery_assignment_epoch_cursor = next_epoch"
)
assert -1 not in (retry_restore, persist_epoch, cursor_commit)
assert retry_restore < persist_epoch < cursor_commit, (
    "gateway assignment admission does not retry restore and persist before "
    "advancing its cursor"
)

restore_body = functions["gateway_discovery_assignment_restore_epoch_cursor"]
reconcile = restore_body.find(
    "app_mesh_persistence_reconcile_gateway_assignment_epoch(&epoch)"
)
clear_cursor = restore_body.find(
    "gateway_discovery_assignment_epoch_cursor = 0u", reconcile
)
mark_not_ready = restore_body.find(
    "gateway_discovery_assignment_epoch_ready = false", clear_cursor
)
commit_cursor = restore_body.find(
    "gateway_discovery_assignment_epoch_cursor = epoch", mark_not_ready
)
mark_ready = restore_body.find(
    "gateway_discovery_assignment_epoch_ready = true", commit_cursor
)
assert -1 not in (
    reconcile,
    clear_cursor,
    mark_not_ready,
    commit_cursor,
    mark_ready,
), "gateway epoch restore is missing cross-record reconciliation or readiness"
assert (
    reconcile < clear_cursor < mark_not_ready < commit_cursor < mark_ready
), (
    "gateway epoch readiness can publish before the standalone cursor and "
    "membership proof have reconciled"
)

membership_restore_start = PERSISTENCE.index(
    "int app_mesh_persistence_restore_gateway_membership("
)
membership_restore_end = PERSISTENCE.index(
    "\nint app_mesh_persistence_restore_gateway_assignment_publication(",
    membership_restore_start,
)
membership_restore = PERSISTENCE[
    membership_restore_start:membership_restore_end
]
assert "app_mesh_persistence_clear_gateway_membership" not in membership_restore, (
    "membership restore must preserve corrupt evidence for repeated "
    "fail-closed recovery"
)
baseline_start = PERSISTENCE.index(
    "int app_mesh_persistence_restore_gateway_assignment_baseline("
)
baseline_end = PERSISTENCE.index(
    "\nint app_mesh_persistence_reconcile_gateway_assignment_epoch(",
    baseline_start,
)
baseline_restore = PERSISTENCE[baseline_start:baseline_end]
assert "nvs_delete" not in baseline_restore
assert "app_mesh_persistence_clear_gateway_membership" not in baseline_restore
assert "gateway_membership_restore_snapshot" in baseline_restore
assert "gateway_membership_restore_v2" in baseline_restore

reconcile_start = baseline_end + 1
reconcile_end = PERSISTENCE.index(
    "\nint app_mesh_persistence_reserve_gateway_command_sequences(",
    reconcile_start,
)
reconcile_restore = PERSISTENCE[reconcile_start:reconcile_end]
cursor_read = reconcile_restore.index(
    "app_mesh_persistence_restore_gateway_assignment_epoch("
)
proof_read = reconcile_restore.index(
    "app_mesh_persistence_restore_gateway_assignment_baseline(",
    cursor_read,
)
order = reconcile_restore.index(
    "discovery_assignment_reconcile_epoch_baseline(", proof_read
)
repair = reconcile_restore.index(
    "app_mesh_persistence_save_gateway_assignment_epoch(", order
)
publish = reconcile_restore.index("*epoch = resolved_epoch", repair)
assert cursor_read < proof_read < order < repair < publish, (
    "gateway epoch recovery must validate both records and durably repair a "
    "newer proof before publishing the cursor"
)

restore_expression = "ret = gateway_discovery_assignment_restore_epoch_cursor()"
reserve_restore_call = ANCHOR.find(restore_expression)
restore_call = ANCHOR.find(
    restore_expression, reserve_restore_call + len(restore_expression)
)
assert restore_call >= 0, "gateway init does not restore its assignment epoch"
restore_failure = ANCHOR[restore_call : restore_call + 500]
assert "if (ret < 0)" in restore_failure
assert "return ret;" not in restore_failure, (
    "gateway cursor restore failure leaves the runtime partially initialized"
)
assert "gateway assignment epoch restore deferred" in restore_failure, (
    "gateway cursor restore failure is not visibly admission-gated"
)

# Anchor snapshot I/O errors cannot be converted into erased/unprovisioned
# policy, because doing so would discard the ordered freshness watermark.
anchor_restore = ANCHOR.find(
    "restore_ret = app_mesh_persistence_restore_discovery_assignment"
)
assert anchor_restore >= 0
anchor_restore_failure = ANCHOR[anchor_restore : anchor_restore + 6500]
assert "return restore_ret;" in anchor_restore_failure, (
    "anchor assignment snapshot I/O failure does not fail closed"
)
main_init = MAIN.find("ret = app_anchor_init()")
assert main_init >= 0
main_failure = MAIN[main_init : main_init + 400]
assert "if (ret < 0)" in main_failure
assert "k_panic()" in main_failure and "return ret;" in main_failure, (
    "main can continue after partial anchor/gateway initialization"
)

# Result telemetry may be parsed before the mutex, but it cannot alter the
# active transaction until phase, session, and roster membership are accepted.
# In particular, a late CLAIM cannot reopen TABLE collection.
note_result = functions["gateway_discovery_assignment_note_claim"]
ack_guard = note_result.find(
    "gateway_discovery_assignment_state.stage !=\n"
    "                GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS"
)
ack_session = note_result.find(
    "gateway_discovery_assignment_state.table_command_seq"
)
ack_member = note_result.find("anchor_index == SIZE_MAX")
claim_guard = note_result.find(
    "GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS"
)
claim_session = note_result.find(
    "gateway_discovery_assignment_state.claim_command_seq"
)
rf_started = note_result.find(
    "if (!gateway_discovery_assignment_rf_started_locked",
    claim_session,
)
capacity_guard = note_result.find(
    "gateway_discovery_assignment_state.claim_count >=\n"
    "            ARRAY_SIZE(gateway_discovery_assignment_state.anchor_ids)"
)
hop_normalization = note_result.find(
    "hop_count > DISCOVERY_ASSIGNMENT_MAX_HOPS"
)
hop_mutation = note_result.find(
    "gateway_discovery_assignment_state.max_hop_count = hop_count"
)
ack_mutation = note_result.find(
    "gateway_discovery_assignment_state.ack_mask |="
)
roster_mutation = note_result.find(
    "gateway_discovery_assignment_state.anchor_ids[",
    hop_mutation,
)
assert -1 not in (
    ack_guard,
    ack_session,
    ack_member,
    claim_guard,
    claim_session,
    rf_started,
    capacity_guard,
    hop_normalization,
    hop_mutation,
    ack_mutation,
    roster_mutation,
), "assignment result acceptance guards are incomplete"
assert max(ack_guard, ack_session, ack_member, claim_guard, claim_session) < (
    rf_started
) < capacity_guard < hop_normalization < hop_mutation < min(
    ack_mutation, roster_mutation
), (
    "assignment response state mutates before stage/session/member/RF acceptance"
)
assert re.search(
    r"gateway_discovery_assignment_state\.stage\s*=\s*"
    r"GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS",
    note_result,
) is None, "a late CLAIM can reopen the TABLE phase"
assert "gateway_discovery_assignment_state.table_round = 0u" not in note_result
assert "gateway_discovery_assignment_state.claim_round = 0u" not in note_result

# Anchor CLAIM/ACK response state is shared by inline mesh delivery, delayed
# command execution, and route-owned response work. Every direct access is
# serialized, while node-communication and workqueue backends operate on a
# frozen generation snapshot outside the mutex.
anchor_response_functions = {
    name: body
    for name, body in functions.items()
    if name.startswith("anchor_") and
    "anchor_discovery_claim_pending." in body
}
assert anchor_response_functions, "anchor response ownership has no audited users"
for name, body in anchor_response_functions.items():
    assert (
        "anchor_discovery_claim_mutex" in body or name.endswith("_locked")
    ), (
        f"{name} accesses anchor response state without its mutex"
    )

backend_calls = (
    "app_node_comm_take_delivery_event_for(",
    "app_node_comm_abandon_delivery(",
    "anchor_send_discovery_response(",
)
for name, body in anchor_response_functions.items():
    for backend in backend_calls:
        offset = 0
        while True:
            call = body.find(backend, offset)
            if call < 0:
                break
            prefix = body[:call]
            assert prefix.rfind(
                "k_mutex_lock(&anchor_discovery_claim_mutex"
            ) <= prefix.rfind(
                "k_mutex_unlock(&anchor_discovery_claim_mutex)"
            ), f"{name} calls {backend} while holding response mutex"
            offset = call + len(backend)

# A generation check followed by an unlocked reschedule is still racy: the
# stale call can overwrite a newer generation's timer after the check.  Keep
# the absolute not-before watermark and timer replacement under the same
# response mutex, and gate every submit on that watermark.
reschedule_owner = functions[
    "anchor_discovery_claim_reschedule_locked"
]
assert (
    "anchor_discovery_claim_pending.generation != generation" in
    reschedule_owner
)
not_before_store = reschedule_owner.find(
    "anchor_discovery_claim_pending.next_attempt_not_before_ms = not_before_ms"
)
timer_replace = reschedule_owner.find("mesh_route_work_reschedule(")
assert 0 <= not_before_store < timer_replace
assert (
    reschedule_owner.find(
        "k_mutex_unlock(&anchor_discovery_claim_mutex)"
    ) < 0
), "generation-bound reschedule must stay inside its caller's critical section"
for name, body in anchor_response_functions.items():
    if name == "anchor_discovery_claim_reschedule_locked":
        continue
    assert "mesh_route_work_reschedule(" not in body, (
        f"{name} bypasses the generation-bound response scheduler"
    )
    for call in re.finditer(
        r"\banchor_discovery_claim_reschedule_locked\s*\(",
        body,
    ):
        prefix = body[: call.start()]
        if name == "anchor_schedule_discovery_response":
            assert (
                body.find("k_mutex_lock(&anchor_discovery_claim_mutex") <
                call.start() <
                body.rfind("k_mutex_unlock(&anchor_discovery_claim_mutex)")
            ), "initial response scheduling is outside its ownership region"
            continue
        if name == "anchor_discovery_ack_liveness_work_handler":
            assert (
                body.find("k_mutex_lock(&anchor_discovery_claim_mutex") <
                call.start() <
                body.rfind("k_mutex_unlock(&anchor_discovery_claim_mutex)")
            ), "ACK liveness publication is outside response ownership"
            continue
        if name == "anchor_discovery_claim_work_handler":
            assert (
                "anchor_discovery_assignment_transaction_mutex" in body and
                body.find("k_mutex_lock(&anchor_discovery_claim_mutex") <
                call.start() <
                body.rfind("k_mutex_unlock(&anchor_discovery_claim_mutex)")
            ), "ACK terminal retry is outside assignment/response ownership"
            continue
        assert prefix.rfind(
            "k_mutex_lock(&anchor_discovery_claim_mutex"
        ) > prefix.rfind(
            "k_mutex_unlock(&anchor_discovery_claim_mutex)"
        ), f"{name} calls the response scheduler without owning its mutex"

ready = functions["anchor_discovery_claim_ready_for_submit"]
assert (
    "anchor_discovery_claim_pending.next_attempt_not_before_ms ==" in ready
)
assert "(uint64_t)k_uptime_get() >= not_before_ms" in ready
handler = functions["anchor_discovery_claim_work_handler"]
assert handler.find("anchor_discovery_claim_ready_for_submit(") < handler.find(
    "anchor_send_discovery_response("
), "response submit is not guarded by current generation and not-before time"
submit_success = handler.find("if (ret == 0)")
terminal_poll = handler.find(
    "anchor_discovery_claim_reschedule_locked(",
    submit_success,
)
assert submit_success >= 0 and terminal_poll > submit_success, (
    "successful response admission does not schedule terminal-event polling"
)

assert (
    "anchor_discovery_claim_pending.active = false;" not in
    ANCHOR[
        ANCHOR.find("static int anchor_apply_discovery_assignment_command") :
        ANCHOR.find("static void anchor_finish_broadcast_command")
    ]
), "TABLE apply bypasses generation-aware response cancellation"
assert (
    "mesh_route_work_reschedule(\n"
    "        &anchor_command_execute_work" in ANCHOR
), "delayed assignment application is not route-owner scheduled"

# Snapshot v8 carries committed and candidate SHA-256 identities independently. A
# pending replacement can therefore survive reset without suppressing the
# committed slot used by normal click/range behavior.
assert "APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION 8u" in PERSISTENCE_HEADER
for field in (
    "pending_epoch",
    "pending_table_command_seq",
    "pending_table_commitment",
    "pending_slot",
    "pending_slot_count",
    "pending_valid",
    "ack_retry_round",
):
    assert field in PERSISTENCE_HEADER, f"snapshot is missing {field}"

apply_assignment = ANCHOR[
    ANCHOR.find("static int anchor_apply_discovery_assignment_command") :
    ANCHOR.find("static void anchor_execute_command_side_effects")
]
pending_save = apply_assignment.find("snapshot.pending_epoch = epoch")
pending_retain = apply_assignment.find(
    "DBG_DISCOVERY_SLOT_TABLE_RETAIN"
)
pending_schedule = apply_assignment.find(
    "DISCOVERY_ASSIGNMENT_PHASE_ACK", pending_save
)
assert 0 <= pending_retain < pending_save < pending_schedule
assert "anchor_resume_pending_discovery_assignment_ack(" in (
    apply_assignment[:pending_save]
), "exact pending TABLE replay replaces ACK custody"
assigned_branch = apply_assignment[
    pending_save :
    apply_assignment.find(
        "DBG_DISCOVERY_SLOT_ASSIGNED", pending_schedule
    )
]
assert "snapshot.provisioned = false" not in assigned_branch
assert "local_anchor_mark_discovery_assignment_unprovisioned" not in (
    assigned_branch
), "pending TABLE revokes the committed assignment before proof"

# A TABLE-correlated late CLAIM requests an exact semantic TABLE redrive. It
# must retain the TABLE generation/session and outer sequence instead of
# guessing a private CLAIM identity.
late_claim = functions["anchor_schedule_late_discovery_claim"]
assert "claim_command = *table_command;" in late_claim
assert "claim_command.session_id = epoch;" not in late_claim
assert "anchor_schedule_discovery_claim(" in late_claim
assert apply_assignment.count("anchor_schedule_late_discovery_claim(") >= 2

# DEADLINE_EXPIRED closes one bounded delivery handle, not durable ACK custody.
# The next attempt gets a fresh absolute deadline, and a non-radio liveness
# work item republishes it onto the serialized communication queue if paused.
terminal_retry = handler[
    handler.find("retry = pending.phase") :
    handler.find("if (retry)", handler.find("retry = pending.phase"))
]
assert "NODE_COMM_TERMINAL_CANCELLED" in terminal_retry
assert "NODE_COMM_TERMINAL_DEADLINE_EXPIRED" not in terminal_retry
assert "anchor_discovery_ack_refresh_attempt_deadline_locked(" in handler
liveness = functions["anchor_discovery_ack_liveness_work_handler"]
assert "anchor_discovery_claim_reschedule_locked(" in liveness
assert "anchor_discovery_ack_liveness_generation" in liveness
assert "anchor_send_discovery_response(" not in liveness
reschedule = functions["anchor_discovery_claim_reschedule_locked"]
assert "anchor_discovery_ack_liveness_work" in reschedule

# Snapshot RMW, terminal-event ownership, and live policy transition share one
# transaction owner. Exact pending identity is revalidated before an ACK
# promotion writes NVS, while a newer command first drains a queued DELIVERED
# outcome so its proof cannot be lost by response replacement.
assert "K_MUTEX_DEFINE(anchor_discovery_assignment_transaction_mutex)" in ANCHOR
serialized_apply = functions[
    "anchor_apply_discovery_assignment_command_serialized"
]
assert (
    serialized_apply.find(
        "k_mutex_lock(&anchor_discovery_assignment_transaction_mutex"
    ) <
    serialized_apply.find("anchor_apply_discovery_assignment_command(") <
    serialized_apply.find(
        "k_mutex_unlock(&anchor_discovery_assignment_transaction_mutex"
    )
), "TABLE snapshot RMW is not transaction-owned"
promote = functions["anchor_promote_discovery_assignment_after_ack"]
assert (
    promote.find(
        "k_mutex_lock(&anchor_discovery_assignment_transaction_mutex"
    ) <
    promote.find("anchor_promote_discovery_assignment_after_ack_locked(") <
    promote.find(
        "k_mutex_unlock(&anchor_discovery_assignment_transaction_mutex"
    )
), "delivered ACK promotion is not transaction-owned"
promotion_locked = functions[
    "anchor_promote_discovery_assignment_after_ack_locked"
]
assert (
    "local_anchor_discovery_assignment_project_pending_commit(" in
    promotion_locked
)
project_commit = APP_STATE[
    APP_STATE.find(
        "bool local_anchor_discovery_assignment_project_pending_commit"
    ) :
    APP_STATE.find(
        "bool local_anchor_discovery_assignment_export_retired_epochs"
    )
]
for identity in (
    "joining_epoch == next_epoch",
    "joining_table_seq == table_seq",
    "discovery_assignment_table_commitment_equal(",
    "claim_observed",
):
    assert identity in project_commit, (
        f"promotion live-policy identity omits {identity}"
    )
settle = functions["anchor_settle_ack_before_newer_assignment"]
assert "app_node_comm_cancel_delivery(" in settle
assert "app_node_comm_take_delivery_event_for(" in settle
assert "NODE_COMM_TERMINAL_DELIVERED" in settle
assert "anchor_promote_discovery_assignment_after_ack(" in settle
assert "anchor_retire_superseded_discovery_ack(" in settle
retire_pending = ANCHOR[
    ANCHOR.find("static int anchor_retire_superseded_discovery_ack(") :
    ANCHOR.find(
        "static void anchor_discovery_claim_work_handler",
        ANCHOR.find("static int anchor_retire_superseded_discovery_ack("),
    )
]
for required in (
    "if (snapshot.provisioned != 0u)",
    "snapshot.ack_pending = 0u",
    "snapshot.pending_valid = 0u",
    "app_mesh_persistence_save_discovery_assignment(&snapshot)",
    "app_mesh_persistence_clear_discovery_assignment_checked()",
    "anchor_resume_pending_discovery_assignment_ack(false)",
    "anchor_restore_live_discovery_assignment_snapshot(&snapshot)",
):
    assert required in retire_pending, (
        f"superseded ACK retirement omits {required}"
    )

assignment_init_start = ANCHOR.index(
    "if (anchor_discovery_assignment_required())"
)
assignment_init_end = ANCHOR.index(
    "ret = uwb_anchor_session_init", assignment_init_start
)
assignment_init = ANCHOR[assignment_init_start:assignment_init_end]
assert "app_mesh_persistence_clear_discovery_assignment();" not in (
    assignment_init
)
assert assignment_init.count(
    "app_mesh_persistence_clear_discovery_assignment_checked()"
) == 2
for retirement_reason in (
    "invalid discovery assignment retirement failed closed",
    "mismatched discovery assignment retirement failed closed",
):
    failure = assignment_init.index(retirement_reason)
    guard = assignment_init.rfind("if (ret < 0)", 0, failure)
    returned = assignment_init.index("return ret;", failure)
    assert guard >= 0 and guard < failure < returned

claim_defer = apply_assignment.find(
    "DBG_DISCOVERY_SLOT_CLAIM_DEFERRED"
)
claim_note = apply_assignment.find(
    "local_anchor_discovery_assignment_note_claim(epoch)"
)
claim_phase = apply_assignment.find(
    "if (phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM)"
)
claim_pending_guard = apply_assignment.find(
    "if (snapshot.valid && snapshot.pending_valid != 0u",
    claim_phase,
)
claim_resume = apply_assignment.find(
    "anchor_resume_pending_discovery_assignment_ack(false)",
    claim_pending_guard,
)
claim_settle = apply_assignment.find(
    "anchor_settle_ack_before_newer_assignment(epoch)",
    claim_resume,
)
table_settle = apply_assignment.find(
    "anchor_settle_ack_before_newer_assignment(epoch)",
    claim_settle + 1,
)
table_first_assignment_retirement = apply_assignment.find(
    "bool retired_first_assignment", table_settle - 1600
)
table_invalid_after_retirement = apply_assignment.find(
    "(!snapshot.valid && !retired_first_assignment)",
    table_settle,
)
table_note = apply_assignment.find(
    "local_anchor_discovery_assignment_note_table("
)
assert 0 <= claim_defer < claim_note, (
    "newer CLAIM can supersede an older durable ACK owner"
)
assert 0 <= claim_phase < claim_pending_guard < claim_resume < claim_defer
claim_pending_condition = apply_assignment[
    claim_pending_guard:claim_resume
]
for required in (
    "epoch == snapshot.pending_epoch",
    "discovery_assignment_epoch_strictly_newer(",
    "snapshot.provisioned != 0u",
):
    assert required in claim_pending_condition, (
        f"pending ACK CLAIM gate omits {required}"
    )
assert "return 0;" in apply_assignment[claim_resume:claim_note], (
    "same-epoch or committed-slot pending ACK does not retain response custody"
)
assert claim_defer < claim_settle < claim_note, (
    "unprovisioned newer CLAIM can replace ACK custody before draining its "
    "queued terminal outcome"
)
assert (
    0 <= table_first_assignment_retirement < table_settle <
    table_invalid_after_retirement < table_note
), (
    "a checked deletion of an unprovisioned superseded ACK must let the same "
    "newer TABLE continue into late-claim recovery"
)
claim_settle_result = apply_assignment[claim_settle:claim_note]
assert "if (ret < 0)" in claim_settle_result
assert "return ret;" in claim_settle_result, (
    "unclassified old ACK terminal does not defer the newer CLAIM"
)

schedule_response = functions["anchor_schedule_discovery_response"]
ack_owner_start = schedule_response.find(
    "if (anchor_discovery_claim_pending.active"
)
ack_owner_end = schedule_response.find(
    "replaced_delivery_handle =",
    ack_owner_start,
)
assert 0 <= ack_owner_start < ack_owner_end
ack_owner_guard = schedule_response[ack_owner_start:ack_owner_end]
assert (
    "!discovery_assignment_epoch_strictly_newer(" in ack_owner_guard and
    "epoch, anchor_discovery_claim_pending.epoch" in ack_owner_guard
), (
    "response scheduler lets an obsolete unprovisioned ACK block a newer CLAIM"
)
assert 0 <= table_settle < table_note, (
    "new TABLE mutates live policy before draining queued ACK delivery"
)
table_settle_guard_start = apply_assignment.rfind(
    "if ((table_decision", 0, table_settle
)
table_settle_guard = apply_assignment[table_settle_guard_start:table_settle]
assert "APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY" in table_settle_guard
assert "APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM" in table_settle_guard, (
    "TABLE-before-CLAIM can replace an old ACK owner without draining it"
)
late_start = apply_assignment.find(
    "APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM"
)
late_end = apply_assignment.find(
    "APP_DISCOVERY_ASSIGNMENT_TABLE_REPLAY", late_start
)
late_branch = apply_assignment[late_start:late_end]
assert (
    late_branch.find("local_anchor_discovery_assignment_note_claim(epoch)") <
    late_branch.find("anchor_schedule_late_discovery_claim(")
), "late CLAIM scheduling does not advance local causal state"
assert "anchor_restore_live_discovery_assignment_snapshot(" in late_branch
assert "anchor_rearm_discovery_ack_after_table_rollback(" in late_branch

# Fast retry handles give a D-1 receipt time to obtain proof after D. A
# persisted retry round then moves into capped, minute-to-hour probes, so a
# partial roster cannot consume the queue continuously and lost return ACKs
# still converge after a late route recovery without another TABLE.
assert "#define ANCHOR_DISCOVERY_ACK_FAST_HANDLE_RETRIES 3u" in ANCHOR
assert (
    "#define ANCHOR_DISCOVERY_ACK_LOW_DUTY_RETRY_BASE_MS 60000u" in
    ANCHOR
)
assert (
    "#define ANCHOR_DISCOVERY_ACK_LOW_DUTY_RETRY_MAX_MS 3600000u" in
    ANCHOR
)
low_duty = functions["anchor_discovery_ack_low_duty_retry_delay_ms"]
assert "ANCHOR_DISCOVERY_ACK_LOW_DUTY_RETRY_MAX_MS" in low_duty
assert "DBG_DISCOVERY_SLOT_ACK_LOW_DUTY" in handler
assert "DBG_DISCOVERY_SLOT_ACK_DORMANT" not in handler
persist_round = functions[
    "anchor_persist_discovery_assignment_ack_retry_round"
]
assert "snapshot.ack_retry_round = retry_round;" in persist_round
resume = functions["anchor_resume_pending_discovery_assignment_ack"]
assert "snapshot.ack_retry_round" in resume
assert "explicit_table_replay" in resume

print("assignment state serialization source invariants passed")
