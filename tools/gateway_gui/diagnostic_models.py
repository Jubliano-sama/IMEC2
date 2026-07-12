"""Packet-driven state models for geometry, click, wake, and mesh diagnostics."""

from __future__ import annotations

from collections import Counter, deque
from dataclasses import dataclass, field
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
from typing import Protocol

from .anchor_geometry import AnchorPairDistance, AnchorLayoutResult, solve_anchor_layout
from .anchor_geometry_visibility import solve_visibility_branching_tuned
from .localization import LocalizationReading, LocalizationResult, solve_position
from .command_telemetry import GatewayCommandEvent
from .protocol import (
    Packet, MSG_CLICK_REPORT, TLV_ANCHOR_ID, TLV_CLICKER_ID,
    TLV_DISTANCE_MM, TLV_EVENT_SEQ, TLV_RANGE_STATUS, TLV_SAMPLE_COUNT,
    TLV_SAMPLE_INDEX, TLV_SURVEY_ID,
)


TLV_INITIATOR_ID = 0x1F
TLV_RESPONDER_ID = 0x20
MSG_SURVEY_PAIR_RESULT = 0x53

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


class SurveyGeometryModel:
    """Accept successful pair rows and infer missing pairs only after complete coverage."""

    def __init__(self) -> None:
        self.survey_id: int | None = None
        self.pairs: dict[tuple[str, str], AnchorPairDistance] = {}
        self.failures: set[tuple[str, str]] = set()
        self.observed_opportunities: set[tuple[str, str]] = set()
        self.expected_opportunities: int | None = None
        self.terminal_complete = False
        self.positions_m: dict[str, tuple[float, float]] = {}
        self.generation = 0

    @property
    def missing_pairs(self) -> frozenset[tuple[str, str]]:
        if not self.terminal_complete or self.expected_opportunities is None:
            return frozenset()
        if len(self.observed_opportunities) != self.expected_opportunities:
            return frozenset()
        return frozenset(self.failures)

    def observe_pair_packet(self, packet: Packet) -> SurveyPairObservation | None:
        if packet.msg_type != MSG_SURVEY_PAIR_RESULT:
            return None
        survey_id = packet.value(TLV_SURVEY_ID)
        initiator = packet.value(TLV_INITIATOR_ID)
        responder = packet.value(TLV_RESPONDER_ID)
        if not all(isinstance(value, int) for value in (survey_id, initiator, responder)):
            return None
        assert isinstance(survey_id, int) and isinstance(initiator, int) and isinstance(responder, int)
        if initiator == 0 or responder == 0 or initiator == responder:
            return None
        if self.survey_id != survey_id:
            self.reset(survey_id)
        left, right = anchor_label(initiator), anchor_label(responder)
        pair = (left, right) if left < right else (right, left)
        self.observed_opportunities.add(pair)
        distance_mm = packet.value(TLV_DISTANCE_MM)
        success = packet.value(TLV_RANGE_STATUS) == 0 and isinstance(distance_mm, int) and distance_mm > 50
        if success:
            assert isinstance(distance_mm, int)
            self.pairs[pair] = AnchorPairDistance(pair[0], pair[1], distance_mm / 1000.0, source=f"survey {survey_id}")
            self.failures.discard(pair)
        else:
            self.pairs.pop(pair, None)
            self.failures.add(pair)
        return SurveyPairObservation(
            survey_id, pair[0], pair[1], distance_mm / 1000.0 if success else None,
            success, packet.value(TLV_SAMPLE_INDEX), packet.value(TLV_SAMPLE_COUNT),
        )

    def observe_command_event(self, event: GatewayCommandEvent) -> None:
        if event.command_kind != 2:
            return
        if self.survey_id != event.gateway_sequence:
            self.reset(event.gateway_sequence)
        if event.stage == 8 and event.total_count:
            self.expected_opportunities = event.total_count
        if event.terminal:
            self.expected_opportunities = event.total_count or self.expected_opportunities
            self.terminal_complete = event.command_status == 0 and event.reason == 0

    def reset(self, survey_id: int | None = None) -> None:
        self.survey_id = survey_id
        self.pairs.clear()
        self.failures.clear()
        self.observed_opportunities.clear()
        self.expected_opportunities = None
        self.terminal_complete = False
        self.positions_m.clear()
        self.generation += 1

    def apply_solution(self, result: AnchorLayoutResult) -> None:
        self.positions_m = dict(result.positions_m)
        self.generation += 1


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
    """Current click schema has no detection-attempt field; never guess from unrelated TLVs."""

    def attempt(self, packet: Packet) -> int | None:
        del packet
        return None


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
    def __init__(self) -> None:
        self.geometry_generation = 0
        self.positions_m: dict[str, tuple[float, float]] = {}
        self.current_key: tuple[int, int, int] | None = None
        self.ranges_m: dict[str, float] = {}
        self.state = ClickDiagnosticState("no_geometry", "No solved anchor geometry.", None, 0)

    def set_geometry(self, positions: dict[str, tuple[float, float]], generation: int) -> ClickDiagnosticState:
        self.positions_m = dict(positions)
        self.geometry_generation = generation
        self.current_key = None
        self.ranges_m.clear()
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
        if key != self.current_key:
            self.current_key = key
            self.ranges_m.clear()
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
        if packet.value(TLV_RANGE_STATUS) != 0 or not isinstance(distance_mm, int) or distance_mm <= 50:
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

    def observe(self, event: GatewayCommandEvent) -> TopologyComparison | None:
        if event.command_kind != 1:
            return None
        if event.correlation_key != self.current_key:
            self.current_key = event.correlation_key
            self.current_ids.clear()
        if event.stage == 6 and event.anchor_id:
            self.current_ids.add(event.anchor_id)
        if not event.terminal:
            return None
        actual = tuple(sorted(self.current_ids))
        complete = (
            event.command_status == 0 and event.reason == 0 and
            event.lost_event_count == 0 and event.total_count > 0 and
            event.failure_count == 0 and event.success_count == event.total_count and
            len(actual) == event.total_count
        )
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
        self.latest = TopologyComparison(status, expected, actual, missing, added, complete)
        return self.latest

    def accept_latest(self) -> AnchorBaseline:
        if self.latest is None or not self.latest.complete:
            raise ValueError("Only a complete terminal enumeration can become the baseline.")
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
        self.latest = TopologyComparison("exact", baseline.anchor_ids, baseline.anchor_ids, (), (), True)
        return baseline

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


def solve_visibility(model: SurveyGeometryModel) -> AnchorLayoutResult:
    return solve_visibility_branching_tuned(model.pairs.values(), missing_pairs=model.missing_pairs)


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
