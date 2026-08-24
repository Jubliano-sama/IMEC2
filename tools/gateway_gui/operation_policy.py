"""Versioned host-configurable operation policy shared with mesh firmware."""

from __future__ import annotations

from dataclasses import dataclass
import struct
from typing import Any, ClassVar


OPERATION_POLICY_VERSION = 1
OPERATION_POLICY_FLAGS_NONE = 0
OPERATION_POLICY_ASSIGNMENT_FLAG_RAM_ONLY_ITERATION = 1 << 0
OPERATION_POLICY_ASSIGNMENT_FLAGS_MASK = (
    OPERATION_POLICY_ASSIGNMENT_FLAG_RAM_ONLY_ITERATION
)
OPERATION_POLICY_FAMILY_ASSIGNMENT = 1

COMMAND_BUDGET_MIN_MS = 1_000
COMMAND_BUDGET_MAX_MS = 1_800_000
EXPECTED_ANCHOR_COUNT_MAX = 50
ASSIGNMENT_RESPONSE_SPREAD_MIN_MS = 20
ASSIGNMENT_RESPONSE_SPREAD_MAX_MS = 10_000

ASSIGNMENT_CONTROL_PHASE_COUNT = 2
ASSIGNMENT_CONTROL_FLOOD_DEADLINE_MS = 10_000
ASSIGNMENT_RESPONSE_CUSTODY_MAX_MS = 100_000
ASSIGNMENT_RESPONSE_BASE_MS = 100
ASSIGNMENT_RESPONSE_PRIOR_HOP_CUSTODY_MAX_MS = 420_000
ASSIGNMENT_CLAIM_FAST_HANDLE_RETRIES = 2
ASSIGNMENT_CLAIM_FAST_RETRY_BACKOFF_MAX_MS = 598
ASSIGNMENT_ACK_FAST_HANDLE_RETRIES = 3
ASSIGNMENT_ACK_FAST_RETRY_BACKOFF_MAX_MS = 1_397
ASSIGNMENT_CLAIM_ACK_SETTLE_MAX_MS = 10_000
ASSIGNMENT_RESPONSE_ACK_SETTLE_MS = 3_000
ASSIGNMENT_TERMINAL_POLL_MS = 5
ASSIGNMENT_TERMINAL_GUARD_MS = 1
ASSIGNMENT_RESPONSE_SLOT_MS = 2_270

ASSIGNMENT_DEFAULT_BUDGET_MS = 1_800_000
ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS = 1_000


def _bounded(label: str, value: int, minimum: int, maximum: int) -> None:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{label} must be an integer")
    if not minimum <= value <= maximum:
        raise ValueError(f"{label} must be in {minimum}..{maximum}")


def assignment_required_budget_ms(
    response_spread_ms: int,
    expected_anchor_count: int = 0,
    deepest_hop: int = 0,
) -> int:
    _bounded(
        "assignment response spread",
        response_spread_ms,
        ASSIGNMENT_RESPONSE_SPREAD_MIN_MS,
        ASSIGNMENT_RESPONSE_SPREAD_MAX_MS,
    )
    _bounded(
        "expected anchor count",
        expected_anchor_count,
        0,
        EXPECTED_ANCHOR_COUNT_MAX,
    )
    if deepest_hop != 0:
        _bounded("deepest hop", deepest_hop, 1, 8)
    # The assignment request intentionally carries N, not a topology claim.
    # Before CLAIM evidence exists the safe rectangular-chain bound is D=N
    # (capped by the protocol's eight-hop TTL), exactly as in firmware.
    effective_hop_count = min(expected_anchor_count or 8, 8)
    slot_count = expected_anchor_count or EXPECTED_ANCHOR_COUNT_MAX
    prior_hop_count = effective_hop_count - 1
    response_custody_ms = 30_000 + prior_hop_count * 10_000
    claim_ack_settle_ms = 3_000 + prior_hop_count * 1_000
    jitter_cap_ms = min(response_spread_ms, ASSIGNMENT_RESPONSE_SLOT_MS)
    max_initial_delay_ms = (
        ASSIGNMENT_RESPONSE_BASE_MS
        + (effective_hop_count * slot_count - 1)
        * ASSIGNMENT_RESPONSE_SLOT_MS
        + jitter_cap_ms
        - 1
    )
    collection_ms = response_custody_ms + max_initial_delay_ms
    table_collection_ms = (
        collection_ms
        + ASSIGNMENT_ACK_FAST_HANDLE_RETRIES * response_custody_ms
        + ASSIGNMENT_ACK_FAST_RETRY_BACKOFF_MAX_MS
    )
    required_ms = (
        ASSIGNMENT_CONTROL_PHASE_COUNT
        * ASSIGNMENT_CONTROL_FLOOD_DEADLINE_MS
        + collection_ms
        + table_collection_ms
        + ASSIGNMENT_CLAIM_FAST_HANDLE_RETRIES
        * response_custody_ms
        + ASSIGNMENT_CLAIM_FAST_RETRY_BACKOFF_MAX_MS
        + claim_ack_settle_ms
        + ASSIGNMENT_RESPONSE_ACK_SETTLE_MS
        + ASSIGNMENT_CONTROL_PHASE_COUNT * ASSIGNMENT_TERMINAL_POLL_MS
        + ASSIGNMENT_TERMINAL_GUARD_MS
    )
    return min(required_ms, COMMAND_BUDGET_MAX_MS)


@dataclass(frozen=True)
class AssignmentOperationPolicy:
    expected_anchor_count: int = 0
    operation_budget_ms: int = ASSIGNMENT_DEFAULT_BUDGET_MS
    response_spread_ms: int = ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS
    deepest_hop: int = 0
    ram_only_iteration: bool = False

    family: ClassVar[int] = OPERATION_POLICY_FAMILY_ASSIGNMENT

    def __post_init__(self) -> None:
        _bounded(
            "expected anchor count",
            self.expected_anchor_count,
            0,
            EXPECTED_ANCHOR_COUNT_MAX,
        )
        _bounded(
            "assignment budget",
            self.operation_budget_ms,
            COMMAND_BUDGET_MIN_MS,
            COMMAND_BUDGET_MAX_MS,
        )
        _bounded(
            "assignment response spread",
            self.response_spread_ms,
            ASSIGNMENT_RESPONSE_SPREAD_MIN_MS,
            ASSIGNMENT_RESPONSE_SPREAD_MAX_MS,
        )
        if self.deepest_hop != 0:
            _bounded("deepest hop", self.deepest_hop, 1, 8)
        if not isinstance(self.ram_only_iteration, bool):
            raise ValueError("RAM-only iteration must be a boolean")
        required_budget_ms = assignment_required_budget_ms(
            self.response_spread_ms,
            self.expected_anchor_count,
            self.deepest_hop,
        )
        if self.operation_budget_ms < required_budget_ms:
            raise ValueError(
                "assignment budget must cover the selected response spread: "
                f"minimum {required_budget_ms} ms"
            )

    def encode_value(self) -> bytes:
        return struct.pack(
            "<BBBHIH",
            OPERATION_POLICY_VERSION,
            self.family,
            (
                OPERATION_POLICY_ASSIGNMENT_FLAG_RAM_ONLY_ITERATION
                if self.ram_only_iteration
                else OPERATION_POLICY_FLAGS_NONE
            ),
            self.expected_anchor_count,
            self.operation_budget_ms,
            self.response_spread_ms,
        )


@dataclass(frozen=True)
class OperationPolicyProfile:
    assignment: AssignmentOperationPolicy = AssignmentOperationPolicy()

    def encoded_values(self) -> tuple[bytes]:
        return (self.assignment.encode_value(),)


def decode_operation_policy_value(raw: bytes) -> dict[str, Any]:
    if len(raw) < 3:
        raise ValueError("operation policy requires a three-byte prefix")
    version, family, flags = raw[:3]
    if version != OPERATION_POLICY_VERSION:
        raise ValueError(f"unsupported operation policy version {version}")
    if family == OPERATION_POLICY_FAMILY_ASSIGNMENT:
        if len(raw) != 11:
            raise ValueError("assignment operation policy must be 11 bytes")
        if flags & ~OPERATION_POLICY_ASSIGNMENT_FLAGS_MASK:
            raise ValueError(f"unsupported assignment policy flags 0x{flags:02x}")
        expected, budget, spread = struct.unpack_from("<HIH", raw, 3)
        assignment_policy = AssignmentOperationPolicy(
            expected,
            budget,
            spread,
            ram_only_iteration=bool(
                flags & OPERATION_POLICY_ASSIGNMENT_FLAG_RAM_ONLY_ITERATION
            ),
        )
        fields = {
            "expected_anchor_count": assignment_policy.expected_anchor_count,
            "operation_budget_ms": assignment_policy.operation_budget_ms,
            "response_spread_ms": assignment_policy.response_spread_ms,
            "ram_only_iteration": assignment_policy.ram_only_iteration,
        }
        family_name = "assignment"
    else:
        raise ValueError(f"unknown operation policy family {family}")

    return {
        "version": version,
        "family": family_name,
        "flags": flags,
        **fields,
    }
