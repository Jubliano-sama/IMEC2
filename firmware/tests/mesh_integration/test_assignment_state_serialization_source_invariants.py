#!/usr/bin/env python3

from pathlib import Path
import re

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
ANCHOR = read_composed_source(ROOT / "app/src/app_anchor.c")
STATE = "gateway_discovery_assignment_state."
LOCK = "k_mutex_lock(&gateway_discovery_assignment_mutex"
UNLOCK = "k_mutex_unlock(&gateway_discovery_assignment_mutex)"


def function_definitions(source: str) -> dict[str, str]:
    definitions: dict[str, str] = {}
    pattern = re.compile(
        r"(?m)^(?:static\s+)?(?:[A-Za-z_]\w*(?:\s+|\s*\*\s*))+"
        r"(?P<name>gateway_(?:start_discovery_assignment|"
        r"send_discovery_assignment_[A-Za-z0-9_]+|"
        r"discovery_assignment_[A-Za-z0-9_]+))\s*\("
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

print("assignment state serialization source invariants passed")
