"""Packet-driven state models for geometry, click, wake, and mesh diagnostics."""

from __future__ import annotations

from collections import Counter, OrderedDict, deque
from dataclasses import dataclass, field, replace
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
from typing import Protocol

from .anchor_geometry import (
    AnchorLayoutResult,
    AnchorPairDistance,
    refine_anchor_layout_from_seed,
    solve_anchor_layout,
)
from .anchor_geometry_connectivity import (
    CONNECTIVITY_INTERVAL_ALGORITHM,
    DEFAULT_NEIGHBOR_MAX_M,
    DEFAULT_NONNEIGHBOR_MIN_M,
    solve_connectivity_interval_layout,
)
from .anchor_geometry_visibility import (
    VISIBILITY_BRANCHING_NEIGHBOR_AWARE_ALGORITHM,
    VISIBILITY_BRANCHING_TUNED_ALGORITHM,
    solve_visibility_branching_neighbor_aware_tuned,
    solve_visibility_branching_tuned,
)
from .anchor_geometry_seeds import (
    SEED_AUTO,
    SEED_CURRENT,
    SEED_GRAPH_MDS,
    SEED_SPRING,
    SEED_VISIBILITY,
    graph_mds_seed,
)
from .localization import LocalizationReading, LocalizationResult, solve_position
from .command_telemetry import (
    GatewayCommandEvent, GATEWAY_COMMAND_REASON_NAMES,
    GATEWAY_COMMAND_STAGE_NAMES, is_enumeration_count_mismatch,
)
from .protocol import (
    Packet, MSG_CLICK_REPORT, TLV_ANCHOR_ID, TLV_CLICKER_ID, TLV_ATTEMPT_INDEX,
    TLV_DETECTION_SOURCE,
    TLV_DISTANCE_MM, TLV_EVENT_SEQ, TLV_RANGE_STATUS, TLV_SAMPLE_COUNT,
    TLV_SAMPLE_INDEX,
)


MIN_USABLE_DISTANCE_MM = 0

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
    def reset(self) -> None:
        self._order.clear()
        self._evidence.clear()
        self._diagnostics.clear()

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
        self.range_scale = 1.0
        self.current_key: tuple[int, int, int] | None = None
        self.ranges_m: dict[str, float] = {}
        self._ranges_by_key: OrderedDict[
            tuple[int, int, int], dict[str, float]
        ] = OrderedDict()
        self._states_by_key: OrderedDict[
            tuple[int, int, int], ClickDiagnosticState
        ] = OrderedDict()
        self.state = ClickDiagnosticState("no_geometry", "No solved anchor geometry.", None, 0)
    def reset(self) -> None:
        self.geometry_generation = 0
        self.positions_m.clear()
        self.range_scale = 1.0
        self.current_key = None
        self.ranges_m.clear()
        self._ranges_by_key.clear()
        self._states_by_key.clear()
        self.state = ClickDiagnosticState("no_geometry", "No solved anchor geometry.", None, 0)

    def set_geometry(
        self,
        positions: dict[str, tuple[float, float]],
        generation: int,
        *,
        range_scale: float = 1.0,
    ) -> ClickDiagnosticState:
        if not math.isfinite(range_scale) or range_scale <= 0.0:
            raise ValueError("range scale must be finite and greater than zero")
        preserve_click = bool(
            self.positions_m
            and positions
            and generation == self.geometry_generation
            and set(positions) == set(self.positions_m)
        )
        previous_scale = self.range_scale
        self.positions_m = dict(positions)
        self.range_scale = range_scale
        self.geometry_generation = generation
        if preserve_click:
            scale_ratio = range_scale / previous_scale
            if scale_ratio != 1.0:
                for ranges_m in self._ranges_by_key.values():
                    for anchor_id in ranges_m:
                        ranges_m[anchor_id] *= scale_ratio
            if self.current_key is not None:
                selected_key = self.current_key
                selected_wake = self.state.wake
                for key in tuple(self._ranges_by_key):
                    self._solve_ranges(key, self._states_by_key.get(key, self.state).wake)
                return self.select(selected_key) or self._solve_ranges(selected_key, selected_wake)
        self.current_key = None
        self.ranges_m.clear()
        self._ranges_by_key.clear()
        self._states_by_key.clear()
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
                expired_key, _ranges = self._ranges_by_key.popitem(last=False)
                self._states_by_key.pop(expired_key, None)
            ranges_m = {}
            self._ranges_by_key[key] = ranges_m
        else:
            self._ranges_by_key.move_to_end(key)
        self.current_key = key
        self.ranges_m = ranges_m
        if not self.positions_m:
            self.state = ClickDiagnosticState("invalid", "No solved anchor geometry.", key, self.geometry_generation, wake=wake)
            self._states_by_key[key] = self.state
            return self.state
        anchor_id = anchor_label(anchor)
        if anchor_id not in self.positions_m:
            self.state = ClickDiagnosticState("invalid", f"Anchor {anchor_id} is absent from current geometry.", key, self.geometry_generation, dict(self.ranges_m), wake=wake)
            self._states_by_key[key] = self.state
            return self.state
        if anchor_id in self.ranges_m:
            self.state = ClickDiagnosticState("invalid", f"Duplicate range from {anchor_id}.", key, self.geometry_generation, dict(self.ranges_m), wake=wake)
            self._states_by_key[key] = self.state
            return self.state
        distance_mm = packet.value(TLV_DISTANCE_MM)
        if (
            packet.value(TLV_RANGE_STATUS) != 0
            or not isinstance(distance_mm, int)
            or distance_mm <= MIN_USABLE_DISTANCE_MM
        ):
            self.state = ClickDiagnosticState("invalid", f"Invalid range from {anchor_id}.", key, self.geometry_generation, dict(self.ranges_m), wake=wake)
            self._states_by_key[key] = self.state
            return self.state
        self.ranges_m[anchor_id] = distance_mm / 1000.0 * self.range_scale
        return self._solve_ranges(key, wake)

    @property
    def event_states(self) -> tuple[ClickDiagnosticState, ...]:
        """Every retained click, in first-seen order, with identity-bound data."""

        return tuple(self._states_by_key.values())

    def select(self, key: tuple[int, int, int]) -> ClickDiagnosticState | None:
        state = self._states_by_key.get(key)
        ranges = self._ranges_by_key.get(key)
        if state is None or ranges is None:
            return None
        self.current_key = key
        self.ranges_m = ranges
        self.state = state
        return state

    def delete(self, key: tuple[int, int, int]) -> ClickDiagnosticState:
        """Delete exactly one retained click and select the newest survivor."""

        self._ranges_by_key.pop(key, None)
        self._states_by_key.pop(key, None)
        if self._states_by_key:
            newest_key = next(reversed(self._states_by_key))
            selected = self.select(newest_key)
            assert selected is not None
            return selected
        self.current_key = None
        self.ranges_m = {}
        self.state = ClickDiagnosticState(
            "stale",
            "Waiting for a new click event.",
            None,
            self.geometry_generation,
        )
        return self.state

    def _solve_ranges(
        self,
        key: tuple[int, int, int],
        wake: WakeDiagnostic | None,
    ) -> ClickDiagnosticState:
        ranges_m = self._ranges_by_key[key]
        is_selected = key == self.current_key
        self.ranges_m = ranges_m if is_selected else self.ranges_m
        def publish(state: ClickDiagnosticState) -> ClickDiagnosticState:
            self._states_by_key[key] = state
            if is_selected:
                self.state = state
            return state
        if not self.positions_m:
            return publish(ClickDiagnosticState(
                "invalid",
                "No solved anchor geometry.",
                key,
                self.geometry_generation,
                dict(ranges_m),
                wake=wake,
            ))
        absent = sorted(set(ranges_m) - set(self.positions_m))
        if absent:
            return publish(ClickDiagnosticState(
                "invalid",
                f"Anchor {absent[0]} is absent from current geometry.",
                key,
                self.geometry_generation,
                dict(ranges_m),
                wake=wake,
            ))
        if len(ranges_m) < 3:
            return publish(ClickDiagnosticState("pending", f"Waiting for ranges ({len(ranges_m)}/3).", key, self.geometry_generation, dict(ranges_m), wake=wake))
        points = [self.positions_m[name] for name in ranges_m]
        if not _noncollinear(points):
            return publish(ClickDiagnosticState("invalid", "Click anchors are collinear.", key, self.geometry_generation, dict(ranges_m), wake=wake))
        readings = [LocalizationReading(name, *self.positions_m[name], distance) for name, distance in ranges_m.items()]
        try:
            result = solve_position(readings)
        except ValueError as exc:
            return publish(ClickDiagnosticState("invalid", str(exc), key, self.geometry_generation, dict(ranges_m), wake=wake))
        algorithm = (
            "height-agnostic least squares"
            if result.algorithm == "height_agnostic_range_ls"
            else "the radical-axis theorem"
        )
        return publish(ClickDiagnosticState("solved", f"Solved from {len(readings)} anchors with {algorithm}.", key, self.geometry_generation, dict(ranges_m), result, wake))


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
    def reset(self) -> None:
        self.current_key = None
        self.current_ids.clear()
        self.latest = None
        self._latest_key = None
        self._newest_event_sequence = None
        self._anchors_by_key.clear()
        self._terminals.clear()
        self._live_keys.clear()
        self._first_sequence_by_key.clear()
        self._first_loss_by_key.clear()


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
        count_mismatch = is_enumeration_count_mismatch(terminal)
        if key not in self._live_keys:
            reason = (
                "Incomplete: this enumeration is available only as replayed "
                "history; run a new enumeration before accepting a baseline."
            )
        elif (
            terminal.command_status != 0
            or (terminal.reason != 0 and not count_mismatch)
        ):
            reason = f"Gateway ended the enumeration with status {terminal.command_status}, reason {terminal.reason}."
        elif terminal.total_count == 0:
            reason = "Completed, but no anchors replied."
        elif telemetry_lost:
            reason = f"Incomplete: {terminal.lost_event_count - self._first_loss_by_key[key]} telemetry event(s) were lost during this run."
        elif terminal.failure_count and not count_mismatch:
            reason = f"Incomplete: {terminal.failure_count} anchor assignment(s) failed."
        elif not count_mismatch and terminal.success_count != terminal.total_count:
            reason = f"Incomplete: gateway reported {terminal.success_count} of {terminal.total_count} successful anchors."
        elif len(actual) != terminal.success_count:
            reason = f"Waiting for anchor details: received {len(actual)} of {terminal.success_count}."
        elif count_mismatch:
            reason = (
                f"Complete: found and assigned {terminal.success_count} anchors; "
                f"the configured count was {terminal.total_count}."
            )
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
    def reset(self) -> None:
        self.events.clear()
        self.terminals.clear()
        self.enumerated_anchors.clear()

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
    if is_enumeration_count_mismatch(terminal):
        return (
            "Succeeded with warnings",
            f"Completed: {terminal.success_count} anchors found; configured "
            f"count was {terminal.total_count}.",
        )
    if terminal.command_status == 0 and terminal.reason == 0:
        if terminal.total_count == 0 and terminal.command_kind == 1:
            return "Incomplete", "Completed, but no anchors replied."
        noun = "anchor" if terminal.command_kind == 1 else "operation"
        result = f"Completed: {terminal.success_count} {noun}{'' if terminal.success_count == 1 else 's'} succeeded"
        if terminal.failure_count:
            return "Incomplete", f"{result}; {terminal.failure_count} failed."
        if loss_delta > 0:
            return "Succeeded with warnings", f"{result}; {loss_delta} telemetry event(s) were lost."
        return "Succeeded", result + "."
    reason = GATEWAY_COMMAND_REASON_NAMES[terminal.reason]
    if terminal.reason == 1:
        return (
            "Failed",
            "Gateway rejected this command payload. Restart the GUI from the "
            "same firmware revision, then verify the gateway identity and "
            "operation policy if it persists.",
        )
    if terminal.reason == 2 or terminal.command_status == 3:
        return "Rejected", f"Rejected: {reason.lower()}."
    if terminal.reason in (6, 9):
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
        return f"Assignment table prepared for {event.total_count} anchor(s)."
    if event.terminal:
        if is_enumeration_count_mismatch(event):
            return (
                f"Enumeration completed with {event.success_count} anchors; "
                f"configured count was {event.total_count}."
            )
        reason = GATEWAY_COMMAND_REASON_NAMES[event.reason]
        if event.command_status == 0 and event.reason == 0:
            return f"Completed: {event.success_count} succeeded, {event.failure_count} failed."
        return f"Command ended: {reason.lower()}."
    return GATEWAY_COMMAND_STAGE_NAMES[event.stage]


def select_nearest_anchor_ranges(
    pairs: tuple[AnchorPairDistance, ...],
    nearest_per_anchor: int,
) -> tuple[AnchorPairDistance, ...]:
    """Keep the union of every anchor's N shortest measured links.

    The union keeps every anchor represented even when another endpoint has
    already filled its own nearest-link quota. Zero intentionally means that
    no distance filtering is requested.
    """

    if isinstance(nearest_per_anchor, bool) or nearest_per_anchor < 0:
        raise ValueError("Closest ranges per anchor must be zero or greater.")
    if nearest_per_anchor == 0:
        return pairs
    by_anchor: dict[str, list[tuple[float, str, int]]] = {}
    for index, pair in enumerate(pairs):
        by_anchor.setdefault(pair.anchor_a_id, []).append(
            (pair.distance_m, pair.anchor_b_id, index)
        )
        by_anchor.setdefault(pair.anchor_b_id, []).append(
            (pair.distance_m, pair.anchor_a_id, index)
        )
    selected: set[int] = set()
    for candidates in by_anchor.values():
        candidates.sort()
        selected.update(
            index for _distance, _other, index in candidates[:nearest_per_anchor]
        )
    return tuple(pair for index, pair in enumerate(pairs) if index in selected)


def solve_geometry(
    pairs: tuple[AnchorPairDistance, ...],
    *,
    solver: str,
    seed: str = "Auto (best of all)",
    neighbor_pairs: frozenset[tuple[str, str]] = frozenset(),
    nonneighbor_pairs: frozenset[tuple[str, str]] = frozenset(),
    current_positions_m: dict[str, tuple[float, float]] | None = None,
    nonneighbor_min_m: float = DEFAULT_NONNEIGHBOR_MIN_M,
    neighbor_max_m: float = DEFAULT_NEIGHBOR_MAX_M,
) -> AnchorLayoutResult:
    """Run the retained standalone geometry solver selected by the caller."""
    if solver == CONNECTIVITY_INTERVAL_ALGORITHM:
        return solve_connectivity_interval_layout(
            pairs,
            neighbor_pairs=neighbor_pairs,
            nonneighbor_pairs=nonneighbor_pairs,
            seed=seed,
            current_positions_m=current_positions_m,
            nonneighbor_min_m=nonneighbor_min_m,
            neighbor_max_m=neighbor_max_m,
        )
    if solver == VISIBILITY_BRANCHING_TUNED_ALGORITHM:
        return solve_visibility_branching_tuned(
            pairs,
            missing_pairs=nonneighbor_pairs,
            neighbor_pairs=neighbor_pairs,
            seed=seed,
            current_positions_m=current_positions_m,
            nonneighbor_min_m=nonneighbor_min_m,
            neighbor_max_m=neighbor_max_m,
        )
    if solver == VISIBILITY_BRANCHING_NEIGHBOR_AWARE_ALGORITHM:
        return solve_visibility_branching_neighbor_aware_tuned(
            pairs,
            missing_pairs=nonneighbor_pairs,
            neighbor_pairs=neighbor_pairs,
            seed=seed,
            current_positions_m=current_positions_m,
            nonneighbor_min_m=nonneighbor_min_m,
            neighbor_max_m=neighbor_max_m,
        )
    if solver == "Spring energy":
        if seed in (SEED_AUTO, SEED_SPRING):
            return solve_anchor_layout(pairs)
        anchor_ids = sorted({
            anchor_id
            for pair in pairs
            for anchor_id in (pair.anchor_a_id, pair.anchor_b_id)
        })
        if seed == SEED_CURRENT:
            if current_positions_m is None:
                raise ValueError("No current solved layout is available as a seed.")
            if set(current_positions_m) != set(anchor_ids):
                raise ValueError("Current layout seed does not match the anchor graph.")
            seed_positions = current_positions_m
        elif seed == SEED_GRAPH_MDS:
            seed_positions = graph_mds_seed(list(pairs), anchor_ids)
        elif seed == SEED_VISIBILITY:
            seed_positions = solve_visibility_branching_tuned(
                pairs,
                missing_pairs=nonneighbor_pairs,
                neighbor_pairs=neighbor_pairs,
                seed=SEED_VISIBILITY,
                current_positions_m=current_positions_m,
                nonneighbor_min_m=nonneighbor_min_m,
                neighbor_max_m=neighbor_max_m,
            ).positions_m
        else:
            raise ValueError(f"Unknown geometry seed: {seed}")
        result = refine_anchor_layout_from_seed(pairs, seed_positions)
        return replace(result, algorithm=f"Spring energy; seed {seed}")
    raise ValueError(f"Unknown geometry solver: {solver}")


def refine_geometry(
    pairs: tuple[AnchorPairDistance, ...],
    positions_m: dict[str, tuple[float, float]],
) -> AnchorLayoutResult:
    """Run one measured-distance-only pass from the selected solved layout."""

    return refine_anchor_layout_from_seed(pairs, positions_m)
