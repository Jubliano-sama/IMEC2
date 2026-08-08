"""Packet-driven state models for geometry, click, wake, and mesh diagnostics."""

from __future__ import annotations

from collections import Counter, OrderedDict, deque
from dataclasses import dataclass, field
from datetime import datetime, timezone
from itertools import combinations
import json
import math
import os
from pathlib import Path
import random
from statistics import median
from typing import Iterable, Protocol

from .anchor_geometry import AnchorPairDistance, AnchorLayoutResult, solve_anchor_layout
from .localization import LocalizationReading, LocalizationResult, solve_position
from .command_telemetry import (
    GatewayCommandEvent, GATEWAY_COMMAND_REASON_NAMES,
    GATEWAY_COMMAND_STAGE_NAMES,
)
from .protocol import (
    Packet, MSG_CLICK_REPORT, TLV_ANCHOR_ID, TLV_CLICKER_ID, TLV_ATTEMPT_INDEX,
    TLV_DETECTION_SOURCE,
    TLV_DISTANCE_MM, TLV_EVENT_SEQ, TLV_RANGE_STATUS, TLV_SAMPLE_COUNT,
    TLV_SAMPLE_INDEX, TLV_SURVEY_ID, TLV_SURVEY_OPERATION_GENERATION,
    TLV_SURVEY_ROUND_COMMITMENT, TLV_SURVEY_ROUND_ID,
)


TLV_INITIATOR_ID = 0x1F
TLV_RESPONDER_ID = 0x20
MSG_SURVEY_PAIR_RESULT = 0x53
SURVEY_MIN_USABLE_DISTANCE_MM = 0

WAKE_NORMAL = "normal"
WAKE_LATE = "unexplained_late"
WAKE_COLLISION = "collision_explained"
WAKE_UNKNOWN = "unknown"

# Production-candidate timing: 100 ms courtesy + 400 ms wake train + 400 ms
# responder burst. The final 100 ms is conservative host/message-age allowance.
BLE_COURTESY_MS = 100
WAKE_TRAIN_MS = 400
DS_TWR_BURST_MS = 400
HOST_ALLOWANCE_MS = 100
COLLISION_WINDOW_MS = BLE_COURTESY_MS + WAKE_TRAIN_MS + DS_TWR_BURST_MS + HOST_ALLOWANCE_MS


def anchor_label(anchor_id: int) -> str:
    return f"0x{anchor_id:016x}"


@dataclass(frozen=True)
class SurveyPairObservation:
    survey_id: int
    anchor_a: str
    anchor_b: str
    distance_m: float | None
    successful: bool
    sample_index: int | None
    sample_count: int | None


@dataclass(frozen=True)
class _SurveySampleOutcome:
    successful: bool
    distance_mm: int | None
    reporter_priority: int


class SurveyGeometryModel:
    """Accept successful pair rows and infer missing pairs only after complete coverage."""

    def __init__(self) -> None:
        self.survey_id: int | None = None
        self.pairs: dict[tuple[str, str], AnchorPairDistance] = {}
        self.failures: set[tuple[str, str]] = set()
        self.observed_opportunities: set[tuple[str, str]] = set()
        self.expected_opportunities: int | None = None
        self.planned_opportunities: set[tuple[str, str]] = set()
        self.successful_opportunities: set[tuple[str, str]] = set()
        self.terminal_seen = False
        self.terminal_complete = False
        self.positions_m: dict[str, tuple[float, float]] = {}
        self.generation = 0
        self._command_identity: tuple[int, int] | None = None
        self._operation_generation: int | None = None
        self._round_commitments: dict[int, bytes] = {}
        self._sample_counts: dict[tuple[tuple[str, str], int], int] = {}
        self._sample_count_priorities: dict[
            tuple[tuple[str, str], int], int
        ] = {}
        self._sample_outcomes: dict[
            tuple[tuple[str, str], int], dict[int, _SurveySampleOutcome]
        ] = {}

    @property
    def missing_pairs(self) -> frozenset[tuple[str, str]]:
        if not self.terminal_complete or self.expected_opportunities is None:
            return frozenset()
        if len(self.observed_opportunities) != self.expected_opportunities:
            return frozenset()
        return frozenset(self.failures)

    def solve_readiness(self) -> tuple[bool, str]:
        successful = len(self.pairs)
        observed = len(self.observed_opportunities)
        expected = self.expected_opportunities
        if expected is not None and observed != expected:
            disposition = "waiting for the remaining results" if observed < expected else "unexpected pair results were received"
            return False, (
                f"Received {observed}/{expected} pair results "
                f"({successful} successful); {disposition}."
            )
        if expected is not None and self._command_identity is not None:
            if not self.terminal_seen:
                return False, (
                    f"Received {successful} successful pair distance(s); waiting "
                    "for the survey terminal result."
                )
            if not self.terminal_complete:
                return False, (
                    f"Retained {successful} successful pair distance(s), but the "
                    "survey did not end successfully."
                )
            if len(self.planned_opportunities) != expected:
                return False, (
                    f"Received {len(self.planned_opportunities)}/{expected} planned "
                    "pair identities from command telemetry."
                )
            if self.observed_opportunities != self.planned_opportunities:
                return False, "Received pair identities do not match the planned survey pairs."
            if set(self.pairs) != self.successful_opportunities:
                return False, "Usable distances do not match the gateway's successful pair set."
        if not self.pairs:
            suffix = f"; {len(self.failures)} pair attempt(s) failed" if self.failures else ""
            return False, f"No successful pair distances received{suffix}."

        anchor_ids = {
            anchor_id
            for pair in self.observed_opportunities
            for anchor_id in pair
        }
        neighbors: dict[str, set[str]] = {
            anchor_id: set() for anchor_id in anchor_ids
        }
        for left, right in self.pairs:
            neighbors[left].add(right)
            neighbors[right].add(left)
        seen = {next(iter(anchor_ids))}
        pending = list(seen)
        while pending:
            current = pending.pop()
            for neighbor in neighbors[current] - seen:
                seen.add(neighbor)
                pending.append(neighbor)
        if seen != anchor_ids:
            return False, (
                f"{successful} successful pair distance(s) received, but the "
                "successful-distance graph is disconnected."
            )

        anchor_count = len(anchor_ids)
        minimum_rigid_pair_count = max(1, 2 * anchor_count - 3)
        if successful < minimum_rigid_pair_count:
            return False, (
                f"{successful} successful pair distance(s) received; at least "
                f"{minimum_rigid_pair_count} are needed for {anchor_count} anchors."
            )
        rigidity_rank = _generic_rigidity_rank(anchor_ids, self.pairs)
        if rigidity_rank < minimum_rigid_pair_count:
            return False, (
                f"{successful} successful pair distance(s) received, but their "
                f"graph has rigidity rank {rigidity_rank}/{minimum_rigid_pair_count}."
            )
        if not _is_generically_globally_rigid(anchor_ids, self.pairs):
            return False, (
                f"{successful} successful pair distance(s) are locally rigid, but "
                "the graph still permits non-equivalent reflected layouts."
            )
        return True, (
            f"{successful} successful pair distance(s) received; ready to solve "
            f"the {anchor_count}-anchor geometry."
        )

    def observe_pair_packet(self, packet: Packet) -> SurveyPairObservation | None:
        if packet.msg_type != MSG_SURVEY_PAIR_RESULT:
            return None
        survey_id = packet.value(TLV_SURVEY_ID)
        initiator = packet.value(TLV_INITIATOR_ID)
        responder = packet.value(TLV_RESPONDER_ID)
        operation_generation = packet.value(TLV_SURVEY_OPERATION_GENERATION)
        round_id = packet.value(TLV_SURVEY_ROUND_ID)
        round_commitment = packet.value(TLV_SURVEY_ROUND_COMMITMENT)
        if not all(
            isinstance(value, int)
            for value in (
                survey_id,
                initiator,
                responder,
                operation_generation,
                round_id,
            )
        ):
            return None
        assert (
            isinstance(survey_id, int)
            and isinstance(initiator, int)
            and isinstance(responder, int)
            and isinstance(operation_generation, int)
            and isinstance(round_id, int)
        )
        if (
            operation_generation == 0
            or operation_generation & 0xFFFFFFFF == 0
            or packet.session_id != (operation_generation & 0xFFFFFFFF)
            or round_id == 0
        ):
            return None
        if round_commitment is not None and (
            not isinstance(round_commitment, bytes)
            or len(round_commitment) != 32
        ):
            return None
        if initiator == 0 or responder == 0 or initiator == responder:
            return None
        if packet.src_id not in (initiator, responder):
            return None
        if self.survey_id is None:
            self.reset(survey_id)
        elif self.survey_id != survey_id:
            return None
        if self._operation_generation is None:
            self._operation_generation = operation_generation
        elif operation_generation < self._operation_generation:
            return None
        elif operation_generation > self._operation_generation:
            self._replace_provisional_operation(operation_generation)
        known_commitment = self._round_commitments.get(round_id)
        if known_commitment is not None and round_commitment != known_commitment:
            return None
        if isinstance(round_commitment, bytes):
            self._round_commitments[round_id] = round_commitment
        left, right = anchor_label(initiator), anchor_label(responder)
        pair = (left, right) if left < right else (right, left)
        before = (self.pairs.get(pair), pair in self.failures,
                  pair in self.observed_opportunities)
        distance_mm = packet.value(TLV_DISTANCE_MM)
        success = (
            packet.value(TLV_RANGE_STATUS) == 0
            and isinstance(distance_mm, int)
            and distance_mm > SURVEY_MIN_USABLE_DISTANCE_MM
        )
        sample_count = packet.value(TLV_SAMPLE_COUNT)
        sample_index = packet.value(TLV_SAMPLE_INDEX)
        if (
            not isinstance(sample_count, int)
            or not 1 <= sample_count <= 1000
            or not isinstance(sample_index, int)
            or not 0 <= sample_index < sample_count
        ):
            return None
        reporter_priority = 2 if packet.src_id == initiator else 1 if packet.src_id == responder else 0
        round_key = (pair, round_id)
        known_sample_count = self._sample_counts.get(round_key)
        known_count_priority = self._sample_count_priorities.get(round_key, -1)
        if known_sample_count is None:
            self._sample_counts[round_key] = sample_count
            self._sample_count_priorities[round_key] = reporter_priority
        elif known_sample_count != sample_count:
            if reporter_priority <= known_count_priority:
                return None
            self._sample_counts[round_key] = sample_count
            self._sample_count_priorities[round_key] = reporter_priority
            self._sample_outcomes.pop(round_key, None)
        elif reporter_priority > known_count_priority:
            self._sample_count_priorities[round_key] = reporter_priority
        candidate = _SurveySampleOutcome(
            success,
            distance_mm if success and isinstance(distance_mm, int) else None,
            reporter_priority,
        )
        samples = self._sample_outcomes.setdefault(round_key, {})
        previous = samples.get(sample_index)
        if (
            previous is None
            or (candidate.successful and not previous.successful)
            or (
                candidate.successful == previous.successful
                and candidate.reporter_priority > previous.reporter_priority
            )
        ):
            samples[sample_index] = candidate

        self._refresh_pair_aggregate(pair, survey_id)
        after = (self.pairs.get(pair), pair in self.failures,
                 pair in self.observed_opportunities)
        if after != before:
            self._invalidate_solution()
        return SurveyPairObservation(
            survey_id, pair[0], pair[1], distance_mm / 1000.0 if success else None,
            success, sample_index, sample_count,
        )

    def _refresh_pair_aggregate(
        self,
        pair: tuple[str, str],
        survey_id: int,
    ) -> None:
        """Publish one complete round without combining samples across rounds."""
        round_states = [
            (
                round_id,
                sample_count,
                self._sample_outcomes.get((candidate_pair, round_id), {}),
            )
            for (candidate_pair, round_id), sample_count
            in self._sample_counts.items()
            if candidate_pair == pair
        ]
        if not round_states:
            self.pairs.pop(pair, None)
            self.observed_opportunities.discard(pair)
            self.failures.discard(pair)
            return

        round_id, sample_count, samples = max(
            round_states, key=lambda state: state[0]
        )
        if not all(index in samples for index in range(sample_count)):
            self.pairs.pop(pair, None)
            self.observed_opportunities.discard(pair)
            if any(
                not outcome.successful for outcome in samples.values()
            ):
                self.failures.add(pair)
            else:
                self.failures.discard(pair)
            return

        self.observed_opportunities.add(pair)
        if not all(
            samples[index].successful for index in range(sample_count)
        ):
            self.pairs.pop(pair, None)
            self.failures.add(pair)
            return

        median_distance_mm = median(
            samples[index].distance_mm or 0 for index in range(sample_count)
        )
        self.pairs[pair] = AnchorPairDistance(
            pair[0],
            pair[1],
            median_distance_mm / 1000.0,
            source=(
                f"survey {survey_id}, generation "
                f"{self._operation_generation}, round {round_id}"
            ),
        )
        self.failures.discard(pair)

    def begin_survey(
        self,
        survey_id: int,
        *,
        host_session_id: int | None = None,
        host_sequence: int | None = None,
    ) -> None:
        if not 1 <= survey_id <= 0xFFFFFFFF:
            raise ValueError("survey ID must be in 1..4294967295")
        if (host_session_id is None) != (host_sequence is None):
            raise ValueError("host session and sequence must be provided together")
        self.reset(survey_id)
        if host_session_id is not None and host_sequence is not None:
            self._command_identity = (host_session_id, host_sequence)

    def observe_command_event(self, event: GatewayCommandEvent) -> bool:
        if event.command_kind != 2:
            return False
        event_identity = (event.host_session_id, event.host_sequence)
        if self._command_identity is not None and event_identity != self._command_identity:
            return False
        event_survey_id = event.gateway_sequence or None
        if self.survey_id is None:
            if event_survey_id is None:
                return False
            self.reset(event.gateway_sequence)
            self._command_identity = event_identity
        elif event_survey_id is not None and event_survey_id != self.survey_id:
            return False
        elif self._command_identity is None:
            self._command_identity = event_identity
        before = (
            self.expected_opportunities, frozenset(self.planned_opportunities),
            frozenset(self.successful_opportunities), self.terminal_seen,
            self.terminal_complete, self.missing_pairs,
        )
        if event.stage == 8 and event.total_count:
            self.expected_opportunities = event.total_count
        if event.stage in (9, 10, 11):
            pair = _anchor_pair_key(
                event.pair_initiator_id, event.pair_responder_id
            )
            if pair is not None:
                self.planned_opportunities.add(pair)
                if event.stage == 10:
                    self.successful_opportunities.add(pair)
                elif event.stage == 11:
                    self.successful_opportunities.discard(pair)
        if event.terminal:
            self.expected_opportunities = event.total_count or self.expected_opportunities
            self.terminal_seen = True
            self.terminal_complete = event.command_status == 0 and event.reason == 0
        after = (
            self.expected_opportunities, frozenset(self.planned_opportunities),
            frozenset(self.successful_opportunities), self.terminal_seen,
            self.terminal_complete, self.missing_pairs,
        )
        if after != before:
            self._invalidate_solution()
        return True

    def reset(self, survey_id: int | None = None) -> None:
        self.survey_id = survey_id
        self.pairs.clear()
        self.failures.clear()
        self.observed_opportunities.clear()
        self.expected_opportunities = None
        self.planned_opportunities.clear()
        self.successful_opportunities.clear()
        self.terminal_seen = False
        self.terminal_complete = False
        self.positions_m.clear()
        self._command_identity = None
        self._operation_generation = None
        self._round_commitments.clear()
        self._sample_counts.clear()
        self._sample_count_priorities.clear()
        self._sample_outcomes.clear()
        self.generation += 1

    def _invalidate_solution(self) -> None:
        self.positions_m.clear()
        self.generation += 1

    def _replace_provisional_operation(self, operation_generation: int) -> None:
        """Replace stale raw results when a newer durable operation arrives."""
        self.pairs.clear()
        self.failures.clear()
        self.observed_opportunities.clear()
        self._sample_counts.clear()
        self._sample_count_priorities.clear()
        self._sample_outcomes.clear()
        self._round_commitments.clear()
        self._operation_generation = operation_generation
        self._invalidate_solution()

    def apply_solution(self, result: AnchorLayoutResult) -> None:
        self.positions_m = dict(result.positions_m)
        self.generation += 1


def _generic_rigidity_rank(
    anchor_ids: set[str],
    pairs: dict[tuple[str, str], AnchorPairDistance],
) -> int:
    """Return the generic 2D rigidity-matrix rank for the successful graph."""
    ordered = sorted(anchor_ids)
    best_rank = 0
    for seed in (0x1A2B3C4D, 0xC001D00D):
        best_rank = max(
            best_rank,
            _matrix_rank(_rigidity_rows(ordered, pairs, seed)),
        )
    return best_rank


def _anchor_pair_key(left_id: int, right_id: int) -> tuple[str, str] | None:
    if left_id == 0 or right_id == 0 or left_id == right_id:
        return None
    left, right = anchor_label(left_id), anchor_label(right_id)
    return (left, right) if left < right else (right, left)


def _rigidity_rows(
    ordered: list[str],
    pairs: dict[tuple[str, str], AnchorPairDistance],
    seed: int,
) -> list[list[float]]:
    columns = {anchor_id: 2 * index for index, anchor_id in enumerate(ordered)}
    rng = random.Random(seed)
    coordinates = {
        anchor_id: (rng.uniform(-10.0, 10.0), rng.uniform(-10.0, 10.0))
        for anchor_id in ordered
    }
    rows = []
    for left, right in pairs:
        left_x, left_y = coordinates[left]
        right_x, right_y = coordinates[right]
        dx, dy = left_x - right_x, left_y - right_y
        row = [0.0] * (2 * len(ordered))
        left_column, right_column = columns[left], columns[right]
        row[left_column:left_column + 2] = (dx, dy)
        row[right_column:right_column + 2] = (-dx, -dy)
        rows.append(row)
    return rows


def _is_generically_globally_rigid(
    anchor_ids: set[str],
    pairs: dict[tuple[str, str], AnchorPairDistance],
) -> bool:
    anchor_count = len(anchor_ids)
    if anchor_count == 2:
        return len(pairs) == 1
    if anchor_count == 3:
        return len(pairs) == 3
    if not _is_three_vertex_connected(anchor_ids, pairs):
        return False
    target_rank = 2 * anchor_count - 3
    ordered = sorted(anchor_ids)
    for seed in (0x1A2B3C4D, 0xC001D00D):
        rows = _rigidity_rows(ordered, pairs, seed)
        if _matrix_rank(rows) != target_rank:
            continue
        supported = _row_dependency_support(rows)
        if len(supported) == len(rows):
            return True
    return False


def _is_three_vertex_connected(
    anchor_ids: set[str],
    pairs: dict[tuple[str, str], AnchorPairDistance],
) -> bool:
    neighbors: dict[str, set[str]] = {
        anchor_id: set() for anchor_id in anchor_ids
    }
    for left, right in pairs:
        neighbors[left].add(right)
        neighbors[right].add(left)
    ordered = sorted(anchor_ids)
    for removed_count in (0, 1, 2):
        for removed_tuple in combinations(ordered, removed_count):
            removed = set(removed_tuple)
            remaining = anchor_ids - removed
            start = next(iter(remaining))
            seen = {start}
            pending = [start]
            while pending:
                current = pending.pop()
                for neighbor in neighbors[current] - removed - seen:
                    seen.add(neighbor)
                    pending.append(neighbor)
            if seen != remaining:
                return False
    return True


def _row_dependency_support(
    rows: list[list[float]], *, epsilon: float = 1e-9
) -> set[int]:
    """Return row indices participating in at least one linear dependency."""
    if not rows:
        return set()
    matrix = [list(column) for column in zip(*rows)]
    variable_count = len(rows)
    pivot_columns: list[int] = []
    rank = 0
    for column in range(variable_count):
        pivot = max(
            range(rank, len(matrix)),
            key=lambda row_index: abs(matrix[row_index][column]),
            default=rank,
        )
        if pivot >= len(matrix) or abs(matrix[pivot][column]) <= epsilon:
            continue
        matrix[rank], matrix[pivot] = matrix[pivot], matrix[rank]
        divisor = matrix[rank][column]
        matrix[rank] = [value / divisor for value in matrix[rank]]
        for row_index in range(len(matrix)):
            if row_index == rank:
                continue
            factor = matrix[row_index][column]
            if abs(factor) <= epsilon:
                continue
            matrix[row_index] = [
                value - factor * pivot_value
                for value, pivot_value in zip(matrix[row_index], matrix[rank])
            ]
        pivot_columns.append(column)
        rank += 1
        if rank == len(matrix):
            break
    free_columns = set(range(variable_count)) - set(pivot_columns)
    supported = set(free_columns)
    for row_index, pivot_column in enumerate(pivot_columns):
        if any(abs(matrix[row_index][free]) > epsilon for free in free_columns):
            supported.add(pivot_column)
    return supported


def _matrix_rank(rows: list[list[float]], *, epsilon: float = 1e-9) -> int:
    if not rows:
        return 0
    matrix = [row[:] for row in rows]
    rank = 0
    for column in range(len(matrix[0])):
        pivot = max(
            range(rank, len(matrix)),
            key=lambda row_index: abs(matrix[row_index][column]),
            default=rank,
        )
        if pivot >= len(matrix) or abs(matrix[pivot][column]) <= epsilon:
            continue
        matrix[rank], matrix[pivot] = matrix[pivot], matrix[rank]
        divisor = matrix[rank][column]
        matrix[rank] = [value / divisor for value in matrix[rank]]
        for row_index in range(len(matrix)):
            if row_index == rank:
                continue
            factor = matrix[row_index][column]
            if abs(factor) <= epsilon:
                continue
            matrix[row_index] = [
                value - factor * pivot_value
                for value, pivot_value in zip(matrix[row_index], matrix[rank])
            ]
        rank += 1
        if rank == len(matrix):
            break
    return rank


@dataclass(frozen=True)
class WakeEvidence:
    key: tuple[object, ...]
    session_id: int | None
    click_key: tuple[int, int, int] | None
    click_id: str
    attempt: int | None
    event_time_ms: float | None


@dataclass(frozen=True)
class WakeDiagnostic:
    classification: str
    attempt: int | None
    nearby_click_ids: tuple[str, ...]
    event_time_ms: float | None
    reason: str

    @property
    def marker(self) -> str:
        return {WAKE_NORMAL: "OK", WAKE_LATE: "!", WAKE_COLLISION: "C", WAKE_UNKNOWN: "?"}[self.classification]


class WakeAttemptAdapter(Protocol):
    def attempt(self, packet: Packet) -> int | None: ...


class PendingWakeAttemptAdapter:
    """Read additive wake-attempt evidence without guessing from unrelated TLVs."""

    def attempt(self, packet: Packet) -> int | None:
        attempt = packet.value(TLV_ATTEMPT_INDEX)
        source = packet.value(TLV_DETECTION_SOURCE)
        if source != 1 or not isinstance(attempt, int) or attempt <= 0:
            return None
        return attempt


class WakeTrainMonitor:
    def __init__(self, *, max_recent: int = 256, window_ms: int = COLLISION_WINDOW_MS) -> None:
        self.max_recent = max(8, int(max_recent))
        self.window_ms = int(window_ms)
        self._order: deque[tuple[object, ...]] = deque()
        self._evidence: dict[tuple[object, ...], WakeEvidence] = {}
        self._diagnostics: dict[tuple[object, ...], WakeDiagnostic] = {}

    @property
    def counters(self) -> dict[str, int]:
        counts = Counter(value.classification for value in self._diagnostics.values())
        return {name: counts[name] for name in (WAKE_NORMAL, WAKE_LATE, WAKE_COLLISION, WAKE_UNKNOWN)}

    def observe(self, evidence: WakeEvidence) -> tuple[tuple[tuple[object, ...], WakeDiagnostic], ...]:
        if evidence.key not in self._evidence:
            self._order.append(evidence.key)
        self._evidence[evidence.key] = evidence
        while len(self._order) > self.max_recent:
            old = self._order.popleft()
            self._evidence.pop(old, None)
            self._diagnostics.pop(old, None)
        affected = [evidence.key]
        if evidence.session_id is not None and evidence.event_time_ms is not None:
            event_time_ms = evidence.event_time_ms
            affected.extend(
                key for key in self._order
                if key != evidence.key and self._evidence[key].session_id == evidence.session_id
                and self._evidence[key].event_time_ms is not None
                and abs(self._evidence[key].event_time_ms - event_time_ms) <= self.window_ms  # type: ignore[operator]
            )
        updates = []
        for key in dict.fromkeys(affected):
            diagnostic = self._classify(self._evidence[key])
            if self._diagnostics.get(key) != diagnostic:
                self._diagnostics[key] = diagnostic
                updates.append((key, diagnostic))
        return tuple(updates)

    def _classify(self, evidence: WakeEvidence) -> WakeDiagnostic:
        nearby = self._nearby(evidence)
        if evidence.attempt == 1:
            return WakeDiagnostic(WAKE_NORMAL, 1, nearby, evidence.event_time_ms, "First-attempt detection is expected.")
        if evidence.attempt is None:
            return WakeDiagnostic(WAKE_UNKNOWN, None, nearby, evidence.event_time_ms, "Detection-attempt evidence is absent.")
        if not 2 <= evidence.attempt <= 5:
            return WakeDiagnostic(WAKE_UNKNOWN, evidence.attempt, nearby, evidence.event_time_ms, "Attempt is outside invariant range 1-5.")
        if evidence.click_key is None or evidence.event_time_ms is None:
            return WakeDiagnostic(WAKE_UNKNOWN, evidence.attempt, nearby, evidence.event_time_ms, "Comparable click identity or timing is absent.")
        if nearby:
            return WakeDiagnostic(WAKE_COLLISION, evidence.attempt, nearby, evidence.event_time_ms, "Nearby click can plausibly explain collision.")
        return WakeDiagnostic(WAKE_LATE, evidence.attempt, (), evidence.event_time_ms, "No nearby click explains late detection.")

    def _nearby(self, evidence: WakeEvidence) -> tuple[str, ...]:
        if evidence.session_id is None or evidence.click_key is None or evidence.event_time_ms is None:
            return ()
        values = {
            other.click_id for other in self._evidence.values()
            if other.session_id == evidence.session_id and other.click_key != evidence.click_key
            and other.event_time_ms is not None
            and abs(other.event_time_ms - evidence.event_time_ms) <= self.window_ms
        }
        return tuple(sorted(values))


@dataclass(frozen=True)
class ClickDiagnosticState:
    status: str
    message: str
    identity: tuple[int, int, int] | None
    geometry_generation: int
    ranges_m: dict[str, float] = field(default_factory=dict)
    result: LocalizationResult | None = None
    wake: WakeDiagnostic | None = None


class ClickLocationModel:
    MAX_TRACKED_EVENTS = 32

    def __init__(self) -> None:
        self.geometry_generation = 0
        self.positions_m: dict[str, tuple[float, float]] = {}
        self.current_key: tuple[int, int, int] | None = None
        self.ranges_m: dict[str, float] = {}
        self._ranges_by_key: OrderedDict[
            tuple[int, int, int], dict[str, float]
        ] = OrderedDict()
        self.state = ClickDiagnosticState("no_geometry", "No solved anchor geometry.", None, 0)

    def set_geometry(self, positions: dict[str, tuple[float, float]], generation: int) -> ClickDiagnosticState:
        self.positions_m = dict(positions)
        self.geometry_generation = generation
        self.current_key = None
        self.ranges_m.clear()
        self._ranges_by_key.clear()
        status = "stale" if positions else "no_geometry"
        self.state = ClickDiagnosticState(status, "Waiting for a new click event." if positions else "No solved anchor geometry.", None, generation)
        return self.state

    def observe(self, packet: Packet, wake: WakeDiagnostic | None = None) -> ClickDiagnosticState | None:
        if packet.msg_type != MSG_CLICK_REPORT:
            return None
        event_seq = packet.value(TLV_EVENT_SEQ)
        clicker = packet.value(TLV_CLICKER_ID)
        anchor = packet.value(TLV_ANCHOR_ID)
        if not all(isinstance(value, int) for value in (event_seq, clicker, anchor)):
            self.state = ClickDiagnosticState("invalid", "Click report lacks event/clicker/anchor identity.", None, self.geometry_generation, wake=wake)
            return self.state
        assert isinstance(event_seq, int) and isinstance(clicker, int) and isinstance(anchor, int)
        key = packet.session_id, event_seq, clicker
        ranges_m = self._ranges_by_key.get(key)
        if ranges_m is None:
            if len(self._ranges_by_key) >= self.MAX_TRACKED_EVENTS:
                self._ranges_by_key.popitem(last=False)
            ranges_m = {}
            self._ranges_by_key[key] = ranges_m
        else:
            self._ranges_by_key.move_to_end(key)
        self.current_key = key
        self.ranges_m = ranges_m
        if not self.positions_m:
            self.state = ClickDiagnosticState("invalid", "No solved anchor geometry.", key, self.geometry_generation, wake=wake)
            return self.state
        anchor_id = anchor_label(anchor)
        if anchor_id not in self.positions_m:
            self.state = ClickDiagnosticState("invalid", f"Anchor {anchor_id} is absent from current geometry.", key, self.geometry_generation, dict(self.ranges_m), wake=wake)
            return self.state
        if anchor_id in self.ranges_m:
            self.state = ClickDiagnosticState("invalid", f"Duplicate range from {anchor_id}.", key, self.geometry_generation, dict(self.ranges_m), wake=wake)
            return self.state
        distance_mm = packet.value(TLV_DISTANCE_MM)
        if (
            packet.value(TLV_RANGE_STATUS) != 0
            or not isinstance(distance_mm, int)
            or distance_mm <= SURVEY_MIN_USABLE_DISTANCE_MM
        ):
            self.state = ClickDiagnosticState("invalid", f"Invalid range from {anchor_id}.", key, self.geometry_generation, dict(self.ranges_m), wake=wake)
            return self.state
        self.ranges_m[anchor_id] = distance_mm / 1000.0
        if len(self.ranges_m) < 3:
            self.state = ClickDiagnosticState("pending", f"Waiting for ranges ({len(self.ranges_m)}/3).", key, self.geometry_generation, dict(self.ranges_m), wake=wake)
            return self.state
        points = [self.positions_m[name] for name in self.ranges_m]
        if not _noncollinear(points):
            self.state = ClickDiagnosticState("invalid", "Click anchors are collinear.", key, self.geometry_generation, dict(self.ranges_m), wake=wake)
            return self.state
        readings = [LocalizationReading(name, *self.positions_m[name], distance) for name, distance in self.ranges_m.items()]
        try:
            result = solve_position(readings)
        except ValueError as exc:
            self.state = ClickDiagnosticState("invalid", str(exc), key, self.geometry_generation, dict(self.ranges_m), wake=wake)
            return self.state
        self.state = ClickDiagnosticState("solved", f"Solved from {len(readings)} anchors.", key, self.geometry_generation, dict(self.ranges_m), result, wake)
        return self.state


def _noncollinear(points: list[tuple[float, float]]) -> bool:
    return any(abs((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])) > 1e-8 for a in points for b in points for c in points)


@dataclass(frozen=True)
class AnchorBaseline:
    anchor_ids: tuple[int, ...]
    accepted_at: str
    source: str


@dataclass(frozen=True)
class TopologyComparison:
    status: str
    expected: tuple[int, ...]
    actual: tuple[int, ...]
    missing: tuple[int, ...]
    added: tuple[int, ...]
    complete: bool
    eligibility_reason: str = ""


def _u32_serial_newer(candidate: int, reference: int) -> bool:
    """Compare wrapping event sequences using RFC 1982 half-range ordering."""
    difference = (candidate - reference) & 0xFFFFFFFF
    return 0 < difference < 0x80000000


class TopologyBaselineModel:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.load_error: str | None = None
        try:
            self.baseline = self._load()
        except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
            self.baseline = None
            self.load_error = str(exc)
        self.current_key: tuple[int, int, int, int] | None = None
        self.current_ids: set[int] = set()
        self.latest: TopologyComparison | None = None
        self._latest_key: tuple[int, int, int, int] | None = None
        self._newest_event_sequence: int | None = None
        self._anchors_by_key: dict[tuple[int, int, int, int], set[int]] = {}
        self._terminals: dict[tuple[int, int, int, int], GatewayCommandEvent] = {}
        self._live_keys: set[tuple[int, int, int, int]] = set()
        self._first_sequence_by_key: dict[
            tuple[int, int, int, int], int
        ] = {}
        self._first_loss_by_key: dict[tuple[int, int, int, int], int] = {}

    def observe(self, event: GatewayCommandEvent) -> TopologyComparison | None:
        if event.command_kind != 1:
            return None
        key = event.correlation_key
        anchor_ids = self._anchors_by_key.setdefault(key, set())
        if not event.flags & 0x04:
            self._live_keys.add(key)
        first_sequence = self._first_sequence_by_key.get(key)
        if (
            first_sequence is None
            or _u32_serial_newer(first_sequence, event.event_sequence)
        ):
            self._first_sequence_by_key[key] = event.event_sequence
            self._first_loss_by_key[key] = event.lost_event_count
        elif event.event_sequence == first_sequence:
            self._first_loss_by_key[key] = min(
                self._first_loss_by_key[key], event.lost_event_count
            )
        if event.stage == 6 and event.anchor_id:
            anchor_ids.add(event.anchor_id)
        if event.terminal:
            previous_terminal = self._terminals.get(key)
            if (
                previous_terminal is None
                or _u32_serial_newer(
                    event.event_sequence,
                    previous_terminal.event_sequence,
                )
            ):
                self._terminals[key] = event

        if self.current_key is None:
            self._select_current(key, event.event_sequence)
        elif key == self.current_key:
            if (
                self._newest_event_sequence is None
                or _u32_serial_newer(
                    event.event_sequence, self._newest_event_sequence
                )
            ):
                self._newest_event_sequence = event.event_sequence
        elif (
            self._newest_event_sequence is not None
            and _u32_serial_newer(
                event.event_sequence, self._newest_event_sequence
            )
        ):
            self._select_current(key, event.event_sequence)

        if key != self.current_key:
            return None
        self.current_ids = anchor_ids
        terminal = self._terminals.get(key)
        if terminal is None:
            return None
        actual = tuple(sorted(self.current_ids))
        telemetry_lost = terminal.lost_event_count > self._first_loss_by_key[key]
        if key not in self._live_keys:
            reason = (
                "Incomplete: this enumeration is available only as replayed "
                "history; run a new enumeration before accepting a baseline."
            )
        elif terminal.command_status != 0 or terminal.reason != 0:
            reason = f"Gateway ended the enumeration with status {terminal.command_status}, reason {terminal.reason}."
        elif terminal.total_count == 0:
            reason = "Completed, but no anchors replied."
        elif telemetry_lost:
            reason = f"Incomplete: {terminal.lost_event_count - self._first_loss_by_key[key]} telemetry event(s) were lost during this run."
        elif terminal.failure_count:
            reason = f"Incomplete: {terminal.failure_count} anchor assignment(s) failed."
        elif terminal.success_count != terminal.total_count:
            reason = f"Incomplete: gateway reported {terminal.success_count} of {terminal.total_count} successful anchors."
        elif len(actual) != terminal.total_count:
            reason = f"Waiting for anchor details: received {len(actual)} of {terminal.total_count}."
        else:
            reason = f"Complete: {terminal.total_count} of {terminal.total_count} anchors reported and were assigned."
        complete = reason.startswith("Complete:")
        expected = self.baseline.anchor_ids if self.baseline else ()
        missing = tuple(sorted(set(expected) - set(actual)))
        added = tuple(sorted(set(actual) - set(expected)))
        if not complete:
            status = "incomplete"
        elif self.baseline is None:
            status = "no_baseline"
        elif not missing and not added:
            status = "exact"
        elif missing and added and len(actual) == len(expected):
            status = "replacement"
        elif missing and not added:
            status = "missing"
        elif added and not missing:
            status = "added"
        else:
            status = "changed"
        self.latest = TopologyComparison(status, expected, actual, missing, added, complete, reason)
        self._latest_key = key
        return self.latest

    def accept_latest(self) -> AnchorBaseline:
        if (
            self.latest is None
            or not self.latest.complete
            or self._latest_key != self.current_key
        ):
            reason = self.latest.eligibility_reason if self.latest else "Run anchor enumeration and wait for its terminal result."
            raise ValueError(f"Baseline unavailable: {reason}")
        baseline = AnchorBaseline(self.latest.actual, datetime.now(timezone.utc).isoformat(timespec="seconds"), "user accepted Here-I-Am")
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.path.with_name(f".{self.path.name}.tmp")
        with temporary.open("w", encoding="utf-8") as handle:
            json.dump({"version": 1, "anchor_ids": list(baseline.anchor_ids), "accepted_at": baseline.accepted_at, "source": baseline.source}, handle, indent=2)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, self.path)
        directory_fd = os.open(self.path.parent, os.O_RDONLY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
        self.baseline = baseline
        self.latest = TopologyComparison("exact", baseline.anchor_ids, baseline.anchor_ids, (), (), True,
                                         f"Complete: {len(baseline.anchor_ids)} anchors accepted as the baseline.")
        self._latest_key = self.current_key
        return baseline

    def _select_current(
        self,
        key: tuple[int, int, int, int],
        event_sequence: int,
    ) -> None:
        self.current_key = key
        self.current_ids = self._anchors_by_key[key]
        self._newest_event_sequence = event_sequence
        self.latest = None
        self._latest_key = None

    def _load(self) -> AnchorBaseline | None:
        if not self.path.exists():
            return None
        data = json.loads(self.path.read_text(encoding="utf-8"))
        return AnchorBaseline(tuple(sorted(set(int(value) for value in data["anchor_ids"]))), str(data["accepted_at"]), str(data["source"]))


class CommandTimelineModel:
    def __init__(self, *, max_events: int = 1000) -> None:
        self.max_events = max_events
        self.events: dict[tuple[tuple[int, int, int, int], int], GatewayCommandEvent] = {}
        self.terminals: dict[tuple[int, int, int, int], GatewayCommandEvent] = {}
        self.enumerated_anchors: dict[tuple[int, int, int, int], dict[int, GatewayCommandEvent]] = {}

    def observe(self, event: GatewayCommandEvent) -> None:
        key = event.correlation_key
        self.events[(key, event.event_sequence)] = event
        if event.command_kind == 1 and event.stage == 6 and event.anchor_id:
            anchors = self.enumerated_anchors.setdefault(key, {})
            previous = anchors.get(event.anchor_id)
            if previous is None or event.discovery_slot != 255 or previous.discovery_slot == 255:
                anchors[event.anchor_id] = event
        if event.terminal:
            self.terminals[key] = event
        while len(self.events) > self.max_events:
            oldest = min(self.events, key=lambda item: item[1])
            self.events.pop(oldest)

    def ordered(self) -> tuple[GatewayCommandEvent, ...]:
        return tuple(sorted(self.events.values(), key=lambda event: event.event_sequence))

    def terminal_for(self, key: tuple[int, int, int, int]) -> GatewayCommandEvent | None:
        return self.terminals.get(key)

    def runs(self) -> tuple[tuple[tuple[int, int, int, int], tuple[GatewayCommandEvent, ...]], ...]:
        grouped: dict[tuple[int, int, int, int], list[GatewayCommandEvent]] = {}
        for event in self.events.values():
            grouped.setdefault(event.correlation_key, []).append(event)
        return tuple(sorted(
            ((key, tuple(sorted(events, key=lambda item: item.event_sequence)))
             for key, events in grouped.items()),
            key=lambda item: item[1][0].event_sequence,
        ))


def command_run_status(events: tuple[GatewayCommandEvent, ...]) -> tuple[str, str]:
    """Return an operational status and result sentence for one correlated run."""
    terminal = next((event for event in reversed(events) if event.terminal), None)
    if terminal is None:
        latest = events[-1]
        if latest.stage == 5:
            return "Running", "Waiting to retry after the gateway reported a busy radio path."
        return "Running", command_step_sentence(latest)
    loss_delta = terminal.lost_event_count - min(event.lost_event_count for event in events)
    if terminal.command_status == 0 and terminal.reason == 0:
        if terminal.total_count == 0 and terminal.command_kind == 1:
            return "Incomplete", "Completed, but no anchors replied."
        noun = "anchor" if terminal.command_kind == 1 else "pair"
        result = f"Completed: {terminal.success_count} {noun}{'' if terminal.success_count == 1 else 's'} succeeded"
        if terminal.failure_count:
            return "Incomplete", f"{result}; {terminal.failure_count} failed."
        if loss_delta > 0:
            return "Succeeded with warnings", f"{result}; {loss_delta} telemetry event(s) were lost."
        return "Succeeded", result + "."
    reason = GATEWAY_COMMAND_REASON_NAMES[terminal.reason]
    if terminal.command_kind == 2 and terminal.reason == 3:
        return "Failed", "No survey reports were received before the collection deadline."
    if terminal.reason == 1:
        return "Failed", "Invalid request: check the gateway identity and survey parameters."
    if terminal.reason == 14:
        return "Failed", "Survey radio preparation failed before broadcast; retry after the gateway radio is idle."
    if terminal.reason == 2 or terminal.command_status == 3:
        return "Rejected", f"Rejected: {reason.lower()}."
    if terminal.reason in (6, 9):
        pair_failure = next(
            (event for event in reversed(events)
             if event.command_kind == 2 and event.stage == 11),
            None,
        )
        if pair_failure is not None:
            return "Timed out", (
                f"Survey ended with {terminal.success_count} pair(s) succeeded "
                f"and {terminal.failure_count} failed. Last failure: "
                f"{command_step_sentence(pair_failure)}"
            )
        return "Timed out", f"Timed out: {reason.lower()}."
    return "Failed", f"Failed: {reason.lower()}."


def command_step_sentence(event: GatewayCommandEvent) -> str:
    if event.stage == 1:
        return "Command accepted by gateway."
    if event.stage == 2:
        return "Command queued as priority work."
    if event.stage == 3:
        return "Gateway is preparing the radio operation."
    if event.stage == 4:
        return f"Broadcast attempt {max(1, event.attempt)} sent."
    if event.stage == 5:
        reason = GATEWAY_COMMAND_REASON_NAMES[event.reason]
        return f"Retrying after {reason.lower()}."
    if event.stage == 6:
        anchor = anchor_label(event.anchor_id)
        hop = f" on hop {event.hop_count}" if event.hop_count else ""
        if event.discovery_slot != 255:
            return f"Anchor {anchor} assigned discovery slot {event.discovery_slot}{hop}."
        return f"Anchor {anchor} replied{hop}."
    if event.stage == 7:
        noun = "reply" if event.progress_count == 1 else "replies"
        return f"Anchor collection finished with {event.progress_count} unique {noun}."
    if event.stage == 8:
        return f"Pair schedule prepared for {event.total_count} pair(s)."
    if event.stage == 9:
        return "Anchor pair ranging started."
    if event.stage == 10:
        return "Anchor pair ranging succeeded."
    if event.stage == 11:
        reason = GATEWAY_COMMAND_REASON_NAMES[event.reason].lower()
        if event.pair_initiator_id and event.pair_responder_id:
            initiator = anchor_label(event.pair_initiator_id)
            responder = anchor_label(event.pair_responder_id)
            phase = {
                0x0101: "PREPARE",
                0x0102: "START",
            }.get(event.command_id, f"command 0x{event.command_id:04x}")
            if event.anchor_id == event.pair_initiator_id:
                target = f"initiator {initiator}"
            elif event.anchor_id == event.pair_responder_id:
                target = f"responder {responder}"
            elif event.anchor_id:
                target = f"anchor {anchor_label(event.anchor_id)}"
            else:
                target = "unknown anchor"
            attempts = (
                f" after {event.attempt} gateway control attempt"
                f"{'' if event.attempt == 1 else 's'}"
                if event.attempt else ""
            )
            return (
                f"Pair {initiator} -> {responder} failed during {phase} to "
                f"{target}{attempts}: {reason}."
            )
        return f"Anchor pair ranging failed: {reason}."
    if event.terminal:
        reason = GATEWAY_COMMAND_REASON_NAMES[event.reason]
        if event.command_status == 0 and event.reason == 0:
            return f"Completed: {event.success_count} succeeded, {event.failure_count} failed."
        if event.command_kind == 2 and event.reason == 3:
            return "Command ended: no survey reports were received."
        return f"Command ended: {reason.lower()}."
    return GATEWAY_COMMAND_STAGE_NAMES[event.stage]


def solve_visibility(model: SurveyGeometryModel) -> AnchorLayoutResult:
    return solve_visibility_branching_tuned(model.pairs.values(), missing_pairs=model.missing_pairs)


def solve_visibility_branching_tuned(
    pairs: Iterable[AnchorPairDistance],
    *,
    missing_pairs: Iterable[tuple[str, str]] = (),
) -> AnchorLayoutResult:
    from .anchor_geometry_visibility import (
        solve_visibility_branching_tuned as implementation,
    )

    return implementation(pairs, missing_pairs=missing_pairs)


def solve_geometry(
    pairs: tuple[AnchorPairDistance, ...],
    *,
    solver: str,
    missing_pairs: frozenset[tuple[str, str]] = frozenset(),
) -> AnchorLayoutResult:
    if solver == "Visibility branching tuned":
        return solve_visibility_branching_tuned(pairs, missing_pairs=missing_pairs)
    if solver == "Spring energy":
        return solve_anchor_layout(pairs)
    raise ValueError(f"Unknown geometry solver: {solver}")
