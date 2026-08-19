"""Versioned host-configurable operation policy shared with mesh firmware."""

from __future__ import annotations

from dataclasses import dataclass
import struct
from typing import Any, ClassVar


OPERATION_POLICY_VERSION = 1
OPERATION_POLICY_FLAGS_NONE = 0
OPERATION_POLICY_FAMILY_ASSIGNMENT = 1
OPERATION_POLICY_FAMILY_SURVEY_DISCOVERY = 2
OPERATION_POLICY_FAMILY_SURVEY_PAIR = 3

COMMAND_BUDGET_MIN_MS = 1_000
COMMAND_BUDGET_MAX_MS = 1_800_000
EXPECTED_ANCHOR_COUNT_MAX = 50
ASSIGNMENT_RESPONSE_SPREAD_MIN_MS = 20
ASSIGNMENT_RESPONSE_SPREAD_MAX_MS = 10_000
DISCOVERY_START_DELAY_MIN_MS = 20_000
DISCOVERY_START_DELAY_MAX_MS = 20_000
DISCOVERY_SLOT_MIN_MS = 30
DISCOVERY_SLOT_MAX_MS = 1_000
DISCOVERY_SLOT_COUNT_MIN = 1
DISCOVERY_SLOT_COUNT_MAX = 50
DISCOVERY_ROUND_COUNT_MIN = 1
DISCOVERY_ROUND_COUNT_MAX = 4
DISCOVERY_REPORT_GRACE_MIN_MS = 1
DISCOVERY_REPORT_GRACE_MAX_MS = 60_000
PAIR_MAX_RERUNS = 2
PAIR_MAX_PARALLEL_PAIRS = 25

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
DISCOVERY_REPORT_SLOT_MS = 2_270
DISCOVERY_REPORT_CUSTODY_MAX_MS = 42_000
DISCOVERY_REPORT_DELIVERY_TAIL_MS = 63_060
DISCOVERY_TERMINAL_SCHEDULING_GUARD_MS = 102
DISCOVERY_TERMINAL_GUARD_MS = 1

ASSIGNMENT_DEFAULT_BUDGET_MS = 1_591_204
ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS = 1_000
DISCOVERY_DEFAULT_START_DELAY_MS = 20_000
DISCOVERY_DEFAULT_SLOT_MS = 40
DISCOVERY_DEFAULT_SLOT_COUNT = 6
DISCOVERY_DEFAULT_ROUND_COUNT = 4
DISCOVERY_DEFAULT_REPORT_GRACE_MS = 250
DISCOVERY_DEFAULT_BUDGET_MS = 240_000
PAIR_DEFAULT_MAX_RERUNS = 2
# The GUI's "auto" mode exposes every safe pair lane and lets the firmware's
# neighborhood conflict classifier decide how many can actually run together.
PAIR_AUTO_MAX_PARALLEL_PAIRS = PAIR_MAX_PARALLEL_PAIRS


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
    effective_hop_count = min(deepest_hop or expected_anchor_count or 8, 8)
    prior_hop_count = effective_hop_count - 1
    prior_hop_custody_ms = (
        prior_hop_count * 30_000
        + prior_hop_count * (prior_hop_count - 1) // 2 * 10_000
    )
    response_custody_ms = 30_000 + prior_hop_count * 10_000
    claim_ack_settle_ms = 3_000 + prior_hop_count * 1_000
    slot_width_ms = max(1, response_spread_ms // 50)
    max_initial_delay_ms = (
        ASSIGNMENT_RESPONSE_BASE_MS
        + effective_hop_count * 50 * slot_width_ms
        - 1
        + prior_hop_custody_ms
    )
    return (
        ASSIGNMENT_CONTROL_PHASE_COUNT
        * ASSIGNMENT_CONTROL_FLOOD_DEADLINE_MS
        + (
            response_custody_ms
            + max_initial_delay_ms
        )
        + ASSIGNMENT_CLAIM_FAST_HANDLE_RETRIES
        * response_custody_ms
        + ASSIGNMENT_CLAIM_FAST_RETRY_BACKOFF_MAX_MS
        + (1 + ASSIGNMENT_ACK_FAST_HANDLE_RETRIES)
        * response_custody_ms
        + max_initial_delay_ms
        + ASSIGNMENT_ACK_FAST_RETRY_BACKOFF_MAX_MS
        + claim_ack_settle_ms
        + ASSIGNMENT_RESPONSE_ACK_SETTLE_MS
        + ASSIGNMENT_CONTROL_PHASE_COUNT * ASSIGNMENT_TERMINAL_POLL_MS
        + ASSIGNMENT_TERMINAL_GUARD_MS
    )


def discovery_required_budget_ms(
    start_delay_ms: int,
    slot_ms: int,
    slot_count: int,
    round_count: int,
    report_grace_ms: int,
) -> int:
    return (
        start_delay_ms
        + slot_ms * slot_count * round_count
        + DISCOVERY_REPORT_SLOT_MS * slot_count
        + report_grace_ms
        + DISCOVERY_REPORT_CUSTODY_MAX_MS
        + DISCOVERY_REPORT_DELIVERY_TAIL_MS
        + DISCOVERY_TERMINAL_SCHEDULING_GUARD_MS
        + DISCOVERY_TERMINAL_GUARD_MS
    )


@dataclass(frozen=True)
class AssignmentOperationPolicy:
    expected_anchor_count: int = 0
    operation_budget_ms: int = ASSIGNMENT_DEFAULT_BUDGET_MS
    response_spread_ms: int = ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS
    deepest_hop: int = 0

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
            OPERATION_POLICY_FLAGS_NONE,
            self.expected_anchor_count,
            self.operation_budget_ms,
            self.response_spread_ms,
        )


@dataclass(frozen=True)
class DiscoveryOperationPolicy:
    start_delay_ms: int = DISCOVERY_DEFAULT_START_DELAY_MS
    slot_ms: int = DISCOVERY_DEFAULT_SLOT_MS
    slot_count: int = DISCOVERY_DEFAULT_SLOT_COUNT
    round_count: int = DISCOVERY_DEFAULT_ROUND_COUNT
    report_grace_ms: int = DISCOVERY_DEFAULT_REPORT_GRACE_MS
    operation_budget_ms: int = DISCOVERY_DEFAULT_BUDGET_MS

    family: ClassVar[int] = OPERATION_POLICY_FAMILY_SURVEY_DISCOVERY

    def __post_init__(self) -> None:
        _bounded(
            "discovery start delay",
            self.start_delay_ms,
            DISCOVERY_START_DELAY_MIN_MS,
            DISCOVERY_START_DELAY_MAX_MS,
        )
        _bounded(
            "discovery slot duration",
            self.slot_ms,
            DISCOVERY_SLOT_MIN_MS,
            DISCOVERY_SLOT_MAX_MS,
        )
        _bounded(
            "discovery slot count",
            self.slot_count,
            DISCOVERY_SLOT_COUNT_MIN,
            DISCOVERY_SLOT_COUNT_MAX,
        )
        _bounded(
            "discovery round count",
            self.round_count,
            DISCOVERY_ROUND_COUNT_MIN,
            DISCOVERY_ROUND_COUNT_MAX,
        )
        _bounded(
            "discovery report grace",
            self.report_grace_ms,
            DISCOVERY_REPORT_GRACE_MIN_MS,
            DISCOVERY_REPORT_GRACE_MAX_MS,
        )
        _bounded(
            "discovery budget",
            self.operation_budget_ms,
            COMMAND_BUDGET_MIN_MS,
            COMMAND_BUDGET_MAX_MS,
        )
        required_budget_ms = discovery_required_budget_ms(
            self.start_delay_ms,
            self.slot_ms,
            self.slot_count,
            self.round_count,
            self.report_grace_ms,
        )
        if self.operation_budget_ms < required_budget_ms:
            raise ValueError(
                "discovery budget must cover start, rounds, report delivery, "
                f"and custody: minimum {required_budget_ms} ms"
            )

    def encode_value(self) -> bytes:
        return struct.pack(
            "<BBBIHBBII",
            OPERATION_POLICY_VERSION,
            self.family,
            OPERATION_POLICY_FLAGS_NONE,
            self.start_delay_ms,
            self.slot_ms,
            self.slot_count,
            self.round_count,
            self.report_grace_ms,
            self.operation_budget_ms,
        )


@dataclass(frozen=True)
class PairOperationPolicy:
    max_reruns: int = PAIR_DEFAULT_MAX_RERUNS
    max_parallel_pairs: int = PAIR_AUTO_MAX_PARALLEL_PAIRS

    family: ClassVar[int] = OPERATION_POLICY_FAMILY_SURVEY_PAIR

    def __post_init__(self) -> None:
        _bounded("pair reruns", self.max_reruns, 0, PAIR_MAX_RERUNS)
        _bounded(
            "parallel pairs",
            self.max_parallel_pairs,
            1,
            PAIR_MAX_PARALLEL_PAIRS,
        )

    def encode_value(self) -> bytes:
        return bytes(
            (
                OPERATION_POLICY_VERSION,
                self.family,
                OPERATION_POLICY_FLAGS_NONE,
                self.max_reruns,
                self.max_parallel_pairs,
            )
        )


@dataclass(frozen=True)
class OperationPolicyProfile:
    assignment: AssignmentOperationPolicy = AssignmentOperationPolicy()
    discovery: DiscoveryOperationPolicy = DiscoveryOperationPolicy()
    pair: PairOperationPolicy = PairOperationPolicy()

    def encoded_values(self) -> tuple[bytes, bytes, bytes]:
        return (
            self.assignment.encode_value(),
            self.discovery.encode_value(),
            self.pair.encode_value(),
        )


def decode_operation_policy_value(raw: bytes) -> dict[str, Any]:
    if len(raw) < 3:
        raise ValueError("operation policy requires a three-byte prefix")
    version, family, flags = raw[:3]
    if version != OPERATION_POLICY_VERSION:
        raise ValueError(f"unsupported operation policy version {version}")
    if flags != OPERATION_POLICY_FLAGS_NONE:
        raise ValueError(f"unsupported operation policy flags 0x{flags:02x}")

    if family == OPERATION_POLICY_FAMILY_ASSIGNMENT:
        if len(raw) != 11:
            raise ValueError("assignment operation policy must be 11 bytes")
        expected, budget, spread = struct.unpack_from("<HIH", raw, 3)
        assignment_policy = AssignmentOperationPolicy(expected, budget, spread)
        fields = {
            "expected_anchor_count": assignment_policy.expected_anchor_count,
            "operation_budget_ms": assignment_policy.operation_budget_ms,
            "response_spread_ms": assignment_policy.response_spread_ms,
        }
        family_name = "assignment"
    elif family == OPERATION_POLICY_FAMILY_SURVEY_DISCOVERY:
        if len(raw) != 19:
            raise ValueError("discovery operation policy must be 19 bytes")
        values = struct.unpack_from("<IHBBII", raw, 3)
        discovery_policy = DiscoveryOperationPolicy(*values)
        fields = {
            "start_delay_ms": discovery_policy.start_delay_ms,
            "slot_ms": discovery_policy.slot_ms,
            "slot_count": discovery_policy.slot_count,
            "round_count": discovery_policy.round_count,
            "report_grace_ms": discovery_policy.report_grace_ms,
            "operation_budget_ms": discovery_policy.operation_budget_ms,
        }
        family_name = "survey_discovery"
    elif family == OPERATION_POLICY_FAMILY_SURVEY_PAIR:
        if len(raw) != 5:
            raise ValueError("pair operation policy must be 5 bytes")
        pair_policy = PairOperationPolicy(raw[3], raw[4])
        fields = {
            "max_reruns": pair_policy.max_reruns,
            "max_parallel_pairs": pair_policy.max_parallel_pairs,
        }
        family_name = "survey_pair"
    else:
        raise ValueError(f"unknown operation policy family {family}")

    return {
        "version": version,
        "family": family_name,
        "flags": flags,
        **fields,
    }
