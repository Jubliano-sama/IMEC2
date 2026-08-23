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
OPERATION_POLICY_FAMILY_SURVEY_DISCOVERY = 2
OPERATION_POLICY_FAMILY_SURVEY_PAIR = 3

COMMAND_BUDGET_MIN_MS = 1_000
COMMAND_BUDGET_MAX_MS = 1_800_000
EXPECTED_ANCHOR_COUNT_MAX = 50
ASSIGNMENT_RESPONSE_SPREAD_MIN_MS = 20
ASSIGNMENT_RESPONSE_SPREAD_MAX_MS = 10_000
DISCOVERY_START_DELAY_MIN_MS = 20_000
DISCOVERY_START_DELAY_MAX_MS = 25_104
DISCOVERY_SLOT_MIN_MS = 200
DISCOVERY_SLOT_MAX_MS = 200
DISCOVERY_SLOT_COUNT_MIN = 1
DISCOVERY_SLOT_COUNT_MAX = 50
DISCOVERY_ROUND_COUNT_MIN = 4
DISCOVERY_ROUND_COUNT_MAX = 4
DISCOVERY_REPORT_GRACE_MIN_MS = 1
DISCOVERY_REPORT_GRACE_MAX_MS = 60_000
PAIR_MAX_RERUNS = 2
PAIR_MAX_PARALLEL_PAIRS = 1

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
DISCOVERY_REPORT_CUSTODY_BASE_MS = 30_000
DISCOVERY_REPORT_CUSTODY_PER_ADDITIONAL_HOP_MS = 4_000
DISCOVERY_REPORT_MAX_HOPS = 8
DISCOVERY_REPORT_CUSTODY_MAX_MS = (
    DISCOVERY_REPORT_CUSTODY_BASE_MS
    + (DISCOVERY_REPORT_MAX_HOPS - 1)
    * DISCOVERY_REPORT_CUSTODY_PER_ADDITIONAL_HOP_MS
)
DISCOVERY_REPORT_DELIVERY_TAIL_MS = 63_060
DISCOVERY_TERMINAL_SCHEDULING_GUARD_MS = 102
DISCOVERY_TERMINAL_GUARD_MS = 1

ASSIGNMENT_DEFAULT_BUDGET_MS = 1_800_000
ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS = 1_000
DISCOVERY_CONTROL_HOP_BUDGET_MS = 2_000
DISCOVERY_ORIGIN_REDRIVE_COUNT = 4
DISCOVERY_PHY_PREP_BUDGET_MS = 1_103
DISCOVERY_DEFAULT_START_DELAY_MS = DISCOVERY_START_DELAY_MAX_MS
DISCOVERY_DEFAULT_SLOT_MS = 200
DISCOVERY_DEFAULT_SLOT_COUNT = 6
DISCOVERY_DEFAULT_ROUND_COUNT = 4
DISCOVERY_DEFAULT_REPORT_GRACE_MS = 250
DISCOVERY_DEFAULT_BUDGET_MS = 260_277
PAIR_DEFAULT_MAX_RERUNS = 2
PAIR_DEFAULT_MAX_PARALLEL_PAIRS = 1
# Backward-compatible import name for callers that do not expose the setting.
PAIR_AUTO_MAX_PARALLEL_PAIRS = PAIR_DEFAULT_MAX_PARALLEL_PAIRS

# The full-survey estimate is deliberately softer than the command deadline.
# It models the ordinary rectangular/chain topology promised by the product
# contract; RF retries may take longer without making an otherwise valid
# survey fail.  The 67 s pair allowance is the measured/proven serialized
# PREPARE/PREPARE/START/START plus shared execution-and-ranging horizon.
SURVEY_FIRST_CONTACT_ALLOWANCE_MS = 2_270
SURVEY_PAIR_ESTIMATE_MS = 67_000
SURVEY_GATEWAY_MAX_PAIRS = 150
SURVEY_OPERATION_SAFETY_LIMIT_MS = 1_800_000


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
    jitter_cap_ms = min(response_spread_ms, DISCOVERY_REPORT_SLOT_MS)
    max_initial_delay_ms = (
        ASSIGNMENT_RESPONSE_BASE_MS
        + (effective_hop_count * slot_count - 1)
        * DISCOVERY_REPORT_SLOT_MS
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


def discovery_required_budget_ms(
    start_delay_ms: int,
    slot_ms: int,
    slot_count: int,
    round_count: int,
    report_grace_ms: int,
    deepest_hop: int = 0,
) -> int:
    if deepest_hop != 0:
        _bounded("deepest hop", deepest_hop, 1, 8)
    effective_hop_count = deepest_hop or DISCOVERY_REPORT_MAX_HOPS
    required_start_delay_ms = discovery_required_start_delay_ms(deepest_hop)
    if start_delay_ms < required_start_delay_ms:
        raise ValueError(
            "discovery start delay must cover the selected topology: "
            f"minimum {required_start_delay_ms} ms"
        )
    custody_ms = (
        DISCOVERY_REPORT_CUSTODY_BASE_MS
        + (effective_hop_count - 1)
        * DISCOVERY_REPORT_CUSTODY_PER_ADDITIONAL_HOP_MS
    )
    return (
        start_delay_ms
        + slot_ms * slot_count * round_count
        + DISCOVERY_REPORT_SLOT_MS * slot_count * effective_hop_count
        + report_grace_ms
        + custody_ms
        + DISCOVERY_REPORT_DELIVERY_TAIL_MS
        + DISCOVERY_TERMINAL_SCHEDULING_GUARD_MS
        + DISCOVERY_TERMINAL_GUARD_MS
    )


def discovery_required_start_delay_ms(deepest_hop: int = 0) -> int:
    if deepest_hop != 0:
        _bounded("deepest hop", deepest_hop, 1, DISCOVERY_REPORT_MAX_HOPS)
    effective_hop_count = deepest_hop or DISCOVERY_REPORT_MAX_HOPS
    control_delivery_ms = (
        (effective_hop_count + DISCOVERY_ORIGIN_REDRIVE_COUNT)
        * DISCOVERY_CONTROL_HOP_BUDGET_MS
        + DISCOVERY_PHY_PREP_BUDGET_MS
        + 1
    )
    return max(DISCOVERY_START_DELAY_MIN_MS, control_delivery_ms)


def survey_estimated_duration_ms(
    anchor_count: int,
    deepest_hop: int,
    slot_span: int,
    pair_count: int | None = None,
) -> int:
    """Return the topology service target, never the terminal deadline.

    Before the gateway publishes its real pair plan, every possible pair is
    assumed (bounded by the gateway's 150-pair storage contract).  A live pair
    count can then tighten the same estimate without changing RF behavior.
    """
    _bounded("anchor count", anchor_count, 1, EXPECTED_ANCHOR_COUNT_MAX)
    _bounded("deepest hop", deepest_hop, 1, 8)
    _bounded("occupied slot span", slot_span, 1, DISCOVERY_SLOT_COUNT_MAX)
    if slot_span < anchor_count:
        raise ValueError("occupied slot span must cover every anchor")
    conservative_pair_count = min(
        anchor_count * (anchor_count - 1) // 2,
        SURVEY_GATEWAY_MAX_PAIRS,
    )
    if pair_count is None:
        pair_count = conservative_pair_count
    _bounded("survey pair count", pair_count, 0, SURVEY_GATEWAY_MAX_PAIRS)
    if pair_count > conservative_pair_count:
        raise ValueError("survey pair count exceeds the topology maximum")

    discovery_ms = (
        discovery_required_start_delay_ms(deepest_hop)
        + DISCOVERY_DEFAULT_ROUND_COUNT * slot_span * DISCOVERY_DEFAULT_SLOT_MS
        + (
            ((deepest_hop + 1) * slot_span)
            + anchor_count
            + deepest_hop
        )
        * SURVEY_FIRST_CONTACT_ALLOWANCE_MS
    )
    return discovery_ms + pair_count * SURVEY_PAIR_ESTIMATE_MS

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
class DiscoveryOperationPolicy:
    start_delay_ms: int = DISCOVERY_DEFAULT_START_DELAY_MS
    slot_ms: int = DISCOVERY_DEFAULT_SLOT_MS
    slot_count: int = DISCOVERY_DEFAULT_SLOT_COUNT
    round_count: int = DISCOVERY_DEFAULT_ROUND_COUNT
    report_grace_ms: int = DISCOVERY_DEFAULT_REPORT_GRACE_MS
    operation_budget_ms: int = DISCOVERY_DEFAULT_BUDGET_MS
    # Host-only topology estimate. It sizes the budget but is not encoded;
    # the gateway verifies the live route depth against the same budget.
    deepest_hop: int = 0

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
        if self.deepest_hop != 0:
            _bounded("deepest hop", self.deepest_hop, 1, 8)
        required_budget_ms = discovery_required_budget_ms(
            self.start_delay_ms,
            self.slot_ms,
            self.slot_count,
            self.round_count,
            self.report_grace_ms,
            self.deepest_hop,
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
    elif family == OPERATION_POLICY_FAMILY_SURVEY_DISCOVERY:
        if flags != OPERATION_POLICY_FLAGS_NONE:
            raise ValueError(f"unsupported discovery policy flags 0x{flags:02x}")
        if len(raw) != 19:
            raise ValueError("discovery operation policy must be 19 bytes")
        values = struct.unpack_from("<IHBBII", raw, 3)
        # The wire value deliberately carries no topology estimate. Validate
        # its topology-independent floor here; the gateway binds the same
        # value to the durable enumeration depth before admitting a survey.
        discovery_policy = DiscoveryOperationPolicy(*values, deepest_hop=1)
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
        if flags != OPERATION_POLICY_FLAGS_NONE:
            raise ValueError(f"unsupported pair policy flags 0x{flags:02x}")
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
