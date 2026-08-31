"""Generation-bound host model for live enumeration and survey progress."""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass
import time

from .anchor_geometry import AnchorLayoutResult, AnchorPairDistance
from .command_telemetry import (
    GatewayCommandEvent,
    is_enumeration_count_mismatch,
)
from .diagnostic_models import anchor_label
from .protocol import (
    CMD_SURVEY_CANCEL,
    CMD_SURVEY_PLAN,
    CMD_SURVEY_START,
    SURVEY_EVENT_BATCH_COMPLETE,
    SURVEY_EVENT_NEIGHBOR_GRAPH,
    SURVEY_EVENT_PLAN_ACCEPTED,
    SURVEY_EVENT_RANGE_PROGRESS,
    SURVEY_EVENT_SIGNALS,
    SURVEY_EVENT_TERMINAL,
    SURVEY_TERMINAL_ABORTED,
    SURVEY_TERMINAL_COMPLETE,
    SURVEY_TERMINAL_PARTIAL,
    SurveyAssignmentIdentity,
    SurveyEvent,
    SurveyNeighborReport,
    SurveyPlanPair,
    SurveyRangeResult,
    SurveySignalMeasurement,
    select_closest_survey_pairs,
    select_survey_pairs,
)


SURVEY_PASS_FRESH = "fresh"
SURVEY_PASS_ADDITIONAL_MERGE = "additional-merge"
SURVEY_PASS_ADDITIONAL_ONLY = "additional-only"
SURVEY_PASS_CLOSEST_ONLY = "closest-only"
SURVEY_PASS_MODES = frozenset((
    SURVEY_PASS_FRESH,
    SURVEY_PASS_ADDITIONAL_MERGE,
    SURVEY_PASS_ADDITIONAL_ONLY,
    SURVEY_PASS_CLOSEST_ONLY,
))


class SurveyStateError(ValueError):
    """A current-operation event contradicts retained host state."""


class StaleSurveyEvent(SurveyStateError):
    """A survey event belongs to an older or unrelated operation."""


class SurveyEventNotReady(SurveyStateError):
    """A reliable event arrived before its current START result."""


@dataclass(frozen=True)
class SurveyCommandRequest:
    command_id: int
    session_id: int
    sequence: int
    label: str
    started_at: float
    timeout_s: float


@dataclass(frozen=True)
class SurveyCommandTransition:
    matched: bool = False
    outcome: str | None = None
    request: SurveyCommandRequest | None = None
    status: int | None = None


class SurveyCommandOwner:
    """Correlate one survey control until its reliable command result."""

    def __init__(self) -> None:
        self.pending: SurveyCommandRequest | None = None

    def begin(
        self,
        command_id: int,
        session_id: int,
        sequence: int,
        label: str,
        *,
        timeout_s: float,
        now: float | None = None,
    ) -> bool:
        if self.pending is not None:
            return False
        if command_id not in (CMD_SURVEY_START, CMD_SURVEY_PLAN, CMD_SURVEY_CANCEL):
            raise ValueError("survey command owner received an unrelated command")
        if session_id == 0 or sequence == 0 or timeout_s <= 0.0:
            raise ValueError("survey command identity and timeout must be nonzero")
        self.pending = SurveyCommandRequest(
            command_id,
            session_id,
            sequence,
            label,
            time.monotonic() if now is None else now,
            timeout_s,
        )
        return True

    def observe_result(
        self,
        command_id: int,
        session_id: int,
        sequence: int,
        status: int,
    ) -> SurveyCommandTransition:
        pending = self.pending
        if pending is None or (
            command_id,
            session_id,
            sequence,
        ) != (
            pending.command_id,
            pending.session_id,
            pending.sequence,
        ):
            return SurveyCommandTransition()
        self.pending = None
        return SurveyCommandTransition(
            matched=True,
            outcome="accepted" if status == 0 else "rejected",
            request=pending,
            status=status,
        )

    def expire(self, *, now: float | None = None) -> SurveyCommandTransition:
        pending = self.pending
        if pending is None:
            return SurveyCommandTransition()
        current = time.monotonic() if now is None else now
        if current < pending.started_at + pending.timeout_s:
            return SurveyCommandTransition()
        self.pending = None
        return SurveyCommandTransition(
            matched=True,
            outcome="timeout",
            request=pending,
        )

    def reset(self) -> None:
        self.pending = None


@dataclass
class SurveyProgressStep:
    key: str
    title: str
    state: str = "pending"
    detail: str = "Waiting"
    current: int = 0
    total: int = 0

    @property
    def fraction(self) -> float:
        if self.state in ("done", "warning", "failed"):
            return 1.0
        if self.state != "running":
            return 0.0
        if self.total > 0:
            return min(max(self.current / self.total, 0.05), 0.95)
        return 0.15


class SurveyOperationModel:
    """Own one exact enumeration -> survey -> relative-geometry run."""

    _STEP_DEFINITIONS = (
        ("routes", "Refresh routes"),
        ("enumeration", "Enumerate anchors"),
        ("neighbors", "Collect neighbors"),
        ("plan", "Build ranging plan"),
        ("ranging", "Range anchor pairs"),
        ("geometry", "Solve 2D geometry"),
    )

    def __init__(self) -> None:
        self.run_serial = 0
        self._seen_identities: deque[
            tuple[int, SurveyAssignmentIdentity]
        ] = deque(maxlen=256)
        self.clear()

    def clear(self) -> None:
        self.run_serial += 1
        self._seen_identities.clear()
        self._retained_geometry_pairs: dict[
            tuple[str, str], AnchorPairDistance
        ] = {}
        self._retained_neighbor_pairs: set[tuple[str, str]] = set()
        self._surveyed_anchor_pairs: set[tuple[int, int]] = set()
        self._solve_base_pairs: dict[tuple[str, str], AnchorPairDistance] = {}
        self._solve_base_neighbor_pairs: set[tuple[str, str]] = set()
        self._followup_roster: frozenset[int] | None = None
        self.pass_mode = SURVEY_PASS_FRESH
        self.pass_index = 0
        self._clear_run()

    def _clear_run(self) -> None:
        self.active = False
        self.phase = "idle"
        self.expected_anchor_count = 0
        self.generation: int | None = None
        self.assignment: SurveyAssignmentIdentity | None = None
        self.slot_to_anchor: dict[int, int] = {}
        self.slot_hops: dict[int, int] = {}
        self.neighbor_reports: tuple[SurveyNeighborReport, ...] = ()
        self.signal_measurements: dict[
            tuple[int, int], SurveySignalMeasurement
        ] = {}
        self.requested_pairs: tuple[tuple[int, int], ...] = ()
        self.plan_pairs: tuple[SurveyPlanPair, ...] = ()
        self.results: dict[int, SurveyRangeResult] = {}
        self.batch_plan_pairs: tuple[SurveyPlanPair, ...] = ()
        self.batch_plan_offset = 0
        self.next_batch_index = 0
        self.partial_reasons = 0
        self.terminal_status: int | None = None
        self.error: str | None = None
        self.start_dispatched_at: float | None = None
        self.start_accepted = False
        self.plan_accepted = False
        self.layout: AnchorLayoutResult | None = None
        self.geometry_revision = 0
        self.layout_revision = -1
        self.steps = {
            key: SurveyProgressStep(key, title)
            for key, title in self._STEP_DEFINITIONS
        }

    def begin(
        self,
        *,
        expected_anchor_count: int = 0,
        pass_mode: str = SURVEY_PASS_FRESH,
    ) -> None:
        if pass_mode not in SURVEY_PASS_MODES:
            raise SurveyStateError(f"unknown survey pass mode {pass_mode}")
        seed_layout: AnchorLayoutResult | None = None
        followup_roster: frozenset[int] | None = None
        next_pass_index = 1
        if pass_mode == SURVEY_PASS_FRESH:
            self._retained_geometry_pairs.clear()
            self._retained_neighbor_pairs.clear()
            self._surveyed_anchor_pairs.clear()
            self._solve_base_pairs.clear()
            self._solve_base_neighbor_pairs.clear()
        else:
            self._validate_followup(pass_mode)
            self._record_current_pass()
            followup_roster = frozenset(self.slot_to_anchor.values())
            seed_layout = self.layout
            next_pass_index = self.pass_index + 1
            self._solve_base_pairs = (
                dict(self._retained_geometry_pairs)
                if pass_mode == SURVEY_PASS_ADDITIONAL_MERGE
                else {}
            )
            self._solve_base_neighbor_pairs = (
                set(self._retained_neighbor_pairs)
                if pass_mode == SURVEY_PASS_ADDITIONAL_MERGE
                else set()
            )
        self.run_serial += 1
        self._clear_run()
        self.active = True
        self.phase = "routes"
        self.pass_mode = pass_mode
        self.pass_index = next_pass_index
        self._followup_roster = followup_roster
        self.layout = seed_layout
        self.expected_anchor_count = max(0, int(expected_anchor_count))
        self._set_step(
            "routes",
            "running",
            "Waiting for the gateway route-refresh terminal",
        )

    def _validate_followup(self, pass_mode: str) -> None:
        if self.active or self.phase != "terminal":
            raise SurveyStateError(
                "A follow-up pass requires one terminal survey first."
            )
        if not self.slot_to_anchor:
            raise SurveyStateError(
                "The previous survey has no anchor roster to follow."
            )
        if pass_mode == SURVEY_PASS_CLOSEST_ONLY:
            expected_labels = {
                anchor_label(anchor_id) for anchor_id in self.slot_to_anchor.values()
            }
            if (
                self.layout is None
                or set(self.layout.positions_m) != expected_labels
            ):
                raise SurveyStateError(
                    "Closest-4 iteration requires a solved layout covering every anchor."
                )

    @staticmethod
    def _anchor_key(first: str, second: str) -> tuple[str, str]:
        return (first, second) if first < second else (second, first)

    @classmethod
    def _distance_key(cls, pair: AnchorPairDistance) -> tuple[str, str]:
        return cls._anchor_key(pair.anchor_a_id, pair.anchor_b_id)

    def _record_current_pass(self) -> None:
        for pair in self.current_geometry_pairs:
            self._retained_geometry_pairs[self._distance_key(pair)] = pair
        self._retained_neighbor_pairs.update(self.current_neighbor_pairs)
        for plan in self.plan_pairs:
            first = self.slot_to_anchor.get(plan.initiator_slot)
            second = self.slot_to_anchor.get(plan.responder_slot)
            if first is not None and second is not None and first != second:
                self._surveyed_anchor_pairs.add(
                    (min(first, second), max(first, second))
                )

    def observe_command_event(self, event: GatewayCommandEvent) -> bool:
        if not self.active or event.command_kind not in (1, 3):
            return False
        if event.command_kind == 3:
            self.phase = "routes"
            detail = {
                1: "Route refresh accepted",
                2: "Route refresh queued",
                3: "Waiting for a safe radio boundary",
                4: f"Broadcast attempt {max(event.attempt, 1)}",
                5: f"Retry {max(event.attempt, 1)} after radio contention",
            }.get(event.stage, "Refreshing gateway routes")
            self._set_step("routes", "running", detail)
            if event.terminal:
                if event.command_status == 0 and event.reason == 0:
                    self._set_step("routes", "done", "Route refresh completed")
                    self._set_step(
                        "enumeration", "running", "Starting anchor enumeration"
                    )
                    self.phase = "enumeration"
                else:
                    self.fail(
                        "routes",
                        f"Route refresh failed with status {event.command_status}, "
                        f"reason {event.reason}",
                    )
            return True

        self.phase = "enumeration"
        self._set_step("routes", "done", "Route refresh completed")
        expected = self.expected_anchor_count or event.total_count
        if event.stage == 6 and event.anchor_id:
            if event.discovery_slot != 0xFF:
                if event.hop_count == 0:
                    raise SurveyStateError(
                        "enumeration assigned a slot without a gateway hop"
                    )
                old_anchor = self.slot_to_anchor.get(event.discovery_slot)
                old_slot = next(
                    (
                        slot
                        for slot, anchor_id in self.slot_to_anchor.items()
                        if anchor_id == event.anchor_id
                    ),
                    None,
                )
                if old_anchor not in (None, event.anchor_id) or old_slot not in (
                    None,
                    event.discovery_slot,
                ):
                    raise SurveyStateError(
                        "enumeration published a conflicting anchor/slot mapping"
                    )
                self.slot_to_anchor[event.discovery_slot] = event.anchor_id
                self.slot_hops[event.discovery_slot] = event.hop_count
                detail = (
                    f"Mapped {len(self.slot_to_anchor)}/{expected or '?'} anchors; "
                    f"slot {event.discovery_slot} is {anchor_label(event.anchor_id)}"
                )
            else:
                detail = (
                    f"Collected {max(event.progress_count, len(self.slot_to_anchor))}"
                    f"/{expected or '?'} anchor replies"
                )
            self._set_step(
                "enumeration",
                "running",
                detail,
                current=max(event.progress_count, len(self.slot_to_anchor)),
                total=expected,
            )
        elif event.stage == 7:
            self._set_step(
                "enumeration",
                "running",
                f"Response collection complete; {event.progress_count} unique anchors",
                current=event.progress_count,
                total=event.total_count or expected,
            )
        elif event.stage == 8:
            self._set_step(
                "enumeration",
                "running",
                f"Publishing confirmations {event.progress_count}/{event.total_count}",
                current=event.progress_count,
                total=event.total_count,
            )
        else:
            self._set_step(
                "enumeration", "running", "Gateway enumeration is active"
            )
        if not event.terminal:
            return True

        configured_hint = self.expected_anchor_count
        mapped_count = len(self.slot_to_anchor)
        exact_mapping = (
            mapped_count > 0
            and len(set(self.slot_to_anchor.values())) == mapped_count
        )
        count_mismatch = (
            is_enumeration_count_mismatch(event)
            and exact_mapping
            and event.success_count == mapped_count
        )
        complete = (
            event.command_status == 0
            and event.reason == 0
            and event.failure_count == 0
            and event.total_count > 0
            and mapped_count == event.total_count
            and exact_mapping
        )
        roster = frozenset(self.slot_to_anchor.values())
        if self._followup_roster is not None and roster != self._followup_roster:
            self.fail(
                "enumeration",
                "Follow-up survey roster changed; refusing to mix distances "
                "from different anchor sets",
            )
            return True
        if not complete and not count_mismatch:
            self.fail(
                "enumeration",
                "Enumeration ended without one exact slot and hop for every anchor",
            )
            return True
        hint_mismatch = configured_hint not in (0, mapped_count)
        self.expected_anchor_count = mapped_count
        self._set_step(
            "enumeration",
            "warning" if count_mismatch or hint_mismatch else "done",
            (
                f"{mapped_count} anchors mapped to exact discovery slots; "
                f"optional count hint was {configured_hint}"
                if hint_mismatch
                else f"{mapped_count} anchors mapped to exact discovery slots; "
                f"firmware expected count was {event.total_count}"
                if count_mismatch
                else f"{mapped_count} anchors mapped to exact discovery slots"
            ),
            current=mapped_count,
            total=mapped_count,
        )
        self.phase = "waiting-start"
        self._set_step(
            "neighbors", "running", "Preparing the survey START command"
        )
        return True

    def note_command_dispatched(
        self, command_id: int, *, now: float | None = None
    ) -> None:
        if command_id == CMD_SURVEY_START:
            self.start_dispatched_at = time.monotonic() if now is None else now
            self.start_accepted = False
            self.phase = "neighbors"
            self._set_step(
                "neighbors", "running", "Survey START sent; waiting for acceptance"
            )
        elif command_id == CMD_SURVEY_PLAN:
            self.plan_accepted = False
            self.phase = "plan"
            self._set_step(
                "plan", "running", f"Submitting {len(self.requested_pairs)} mutual pairs"
            )
        elif command_id == CMD_SURVEY_CANCEL:
            self.phase = "aborting"

    def note_command_accepted(self, command_id: int) -> None:
        if command_id == CMD_SURVEY_START:
            self.start_accepted = True
            self.phase = "neighbors"
            self._set_step(
                "neighbors",
                "running",
                "START accepted; anchors are exchanging scheduled neighbor beacons",
            )
        elif command_id == CMD_SURVEY_PLAN:
            self.plan_accepted = True
            self._set_step(
                "plan", "running", "PLAN accepted; waiting for the exact radio schedule"
            )
        elif command_id == CMD_SURVEY_CANCEL:
            self._set_step("ranging", "warning", "Abort accepted; waiting for terminal state")

    def note_command_rejected(self, command_id: int, status_name: str) -> None:
        command_name = {
            CMD_SURVEY_START: "Survey START",
            CMD_SURVEY_PLAN: "Survey PLAN",
            CMD_SURVEY_CANCEL: "Survey abort",
        }.get(command_id, "Survey command")
        if command_id == CMD_SURVEY_CANCEL:
            self.error = f"{command_name} was rejected with {status_name}"
            self._set_step("ranging", "warning", self.error)
            self.phase = "ranging"
            return
        step = "neighbors" if command_id == CMD_SURVEY_START else "plan"
        self.fail(step, f"{command_name} was rejected with {status_name}")

    def note_command_timeout(self, command_id: int) -> None:
        if command_id == CMD_SURVEY_CANCEL:
            self.error = "Survey abort result timed out; the remote operation may still be active"
            self._set_step("ranging", "warning", self.error)
            self.phase = "ranging"
            return
        step = "neighbors" if command_id == CMD_SURVEY_START else "plan"
        self.fail(step, "Gateway command result timed out")

    def set_requested_pairs(self, pairs: tuple[tuple[int, int], ...]) -> None:
        normalized = tuple((min(a, b), max(a, b)) for a, b in pairs)
        if len(normalized) != len(set(normalized)):
            raise SurveyStateError("survey request contains duplicate pairs")
        self.requested_pairs = normalized
        self._set_step(
            "plan", "running", f"Submitting {len(normalized)} mutual pairs"
        )

    def select_plan_pairs(
        self, event: SurveyEvent, *, degree_cap: int = 4
    ) -> tuple[tuple[int, int], ...]:
        """Select the current pass plan from retained pass intent."""

        if self.pass_mode in (
            SURVEY_PASS_ADDITIONAL_MERGE,
            SURVEY_PASS_ADDITIONAL_ONLY,
        ):
            slot_by_anchor = {
                anchor_id: slot for slot, anchor_id in self.slot_to_anchor.items()
            }
            excluded = tuple(
                (slot_by_anchor[first], slot_by_anchor[second])
                for first, second in sorted(self._surveyed_anchor_pairs)
                if first in slot_by_anchor and second in slot_by_anchor
            )
            return select_survey_pairs(
                event, degree_cap=degree_cap, excluded_pairs=excluded
            )
        if self.pass_mode == SURVEY_PASS_CLOSEST_ONLY:
            if self.layout is None:
                raise SurveyStateError(
                    "Closest-4 iteration lost its solved-layout seed."
                )
            positions = {
                slot: self.layout.positions_m[anchor_label(anchor_id)]
                for slot, anchor_id in self.slot_to_anchor.items()
            }
            return select_closest_survey_pairs(
                event, positions, degree_cap=degree_cap
            )
        return select_survey_pairs(event, degree_cap=degree_cap)

    def observe_survey_event(
        self,
        event: SurveyEvent,
        *,
        created_at: float | None = None,
    ) -> bool:
        if not self.active:
            raise StaleSurveyEvent("survey event arrived without an active GUI run")
        if self.generation is None:
            if not self.start_accepted:
                raise SurveyEventNotReady(
                    "survey event is waiting for the exact START command result"
                )
            if event.kind != SURVEY_EVENT_NEIGHBOR_GRAPH:
                raise StaleSurveyEvent(
                    "a new survey must begin with its generation-bound neighbor graph"
                )
            identity = (event.generation, event.assignment)
            if identity in self._seen_identities:
                raise StaleSurveyEvent(
                    "survey generation and assignment identity were already "
                    "observed by this GUI"
                )
            if (
                created_at is not None
                and self.start_dispatched_at is not None
                and created_at + 1.0 < self.start_dispatched_at
            ):
                raise StaleSurveyEvent(
                    "survey event predates the current START dispatch"
                )
            self._validate_first_event(event)
            self.generation = event.generation
            self.assignment = event.assignment
            self._seen_identities.append(identity)
        elif (
            event.generation != self.generation
            or event.assignment != self.assignment
        ):
            raise StaleSurveyEvent(
                "survey event does not match the active generation and assignment"
            )

        self.partial_reasons |= event.partial_reasons
        if event.kind == SURVEY_EVENT_NEIGHBOR_GRAPH:
            self._observe_neighbor_graph(event)
        elif event.kind == SURVEY_EVENT_SIGNALS:
            self._observe_signals(event)
        elif event.kind == SURVEY_EVENT_PLAN_ACCEPTED:
            self._observe_plan(event)
        elif event.kind in (
            SURVEY_EVENT_RANGE_PROGRESS,
            SURVEY_EVENT_BATCH_COMPLETE,
            SURVEY_EVENT_TERMINAL,
        ):
            self._observe_ranges(event)
        else:  # The decoder rejects this, but the model remains fail closed.
            raise SurveyStateError(f"unsupported survey event kind {event.kind}")
        return True

    def _observe_signals(self, event: SurveyEvent) -> None:
        if not self.neighbor_reports:
            raise SurveyStateError("survey signals arrived before the neighbor graph")
        for measurement in event.signal_measurements:
            if (
                measurement.observer_slot not in self.slot_to_anchor
                or measurement.target_slot not in self.slot_to_anchor
                or measurement.target_slot >= measurement.observer_slot
            ):
                raise SurveyStateError(
                    "survey signal measurement references an invalid anchor pair"
                )
            key = (measurement.observer_slot, measurement.target_slot)
            previous = self.signal_measurements.get(key)
            if previous is not None and previous != measurement:
                raise SurveyStateError(
                    "survey signal measurement changed after host acceptance"
                )
            self.signal_measurements[key] = measurement

    def _validate_first_event(self, event: SurveyEvent) -> None:
        occupied = set(event.occupied_slots)
        if occupied != set(self.slot_to_anchor):
            raise StaleSurveyEvent(
                "survey occupied slots do not match the current roster enumeration"
            )
        if not occupied:
            raise SurveyStateError("survey neighbor graph has no enumerated anchors")
        assert self.slot_hops
        if event.assignment.slot_span <= max(occupied):
            raise SurveyStateError("survey assignment slot span excludes an anchor")
        if event.assignment.max_hop_count < max(self.slot_hops.values()):
            raise SurveyStateError("survey assignment understates the enumerated route depth")

    def _observe_neighbor_graph(self, event: SurveyEvent) -> None:
        if self.plan_pairs or self.results:
            raise SurveyStateError("neighbor graph arrived after the ranging plan")
        if set(event.occupied_slots) != set(self.slot_to_anchor):
            raise SurveyStateError("neighbor graph changed the enumerated occupied slots")
        report_slots = {report.own_slot for report in event.neighbor_reports}
        if not report_slots <= set(event.occupied_slots):
            raise SurveyStateError("neighbor graph has a report from an unknown slot")
        self.neighbor_reports = event.neighbor_reports
        report_count = len(event.neighbor_reports)
        expected = len(self.slot_to_anchor)
        state = (
            "done"
            if report_slots == set(event.occupied_slots)
            and event.partial_reasons == 0
            else "warning"
        )
        self._set_step(
            "neighbors",
            state,
            f"Received {report_count}/{expected} neighbor reports",
            current=report_count,
            total=expected,
        )
        self.phase = "planning"
        detail = {
            SURVEY_PASS_FRESH: "Selecting rigidity-aware mutual pairs",
            SURVEY_PASS_ADDITIONAL_MERGE: (
                "Selecting mutual pairs not attempted in earlier passes"
            ),
            SURVEY_PASS_ADDITIONAL_ONLY: (
                "Selecting mutual pairs not attempted in earlier passes"
            ),
            SURVEY_PASS_CLOSEST_ONLY: (
                "Selecting closest degree-4 pairs from the solved layout"
            ),
        }[self.pass_mode]
        self._set_step("plan", "running", detail)

    def _observe_plan(self, event: SurveyEvent) -> None:
        if not self.neighbor_reports:
            raise SurveyStateError("survey plan arrived before the neighbor graph")
        if not self.requested_pairs and event.plan_pairs:
            raise SurveyStateError("gateway added pairs to an empty host request")
        if not self.plan_accepted:
            raise SurveyEventNotReady(
                "survey plan event is waiting for the exact PLAN command result"
            )
        if event.batch_index != self.next_batch_index:
            raise SurveyStateError("survey plan batch arrived out of order")
        accepted: set[tuple[int, int]] = set()
        requested = set(self.requested_pairs)
        for pair in event.plan_pairs:
            key = (
                min(pair.initiator_slot, pair.responder_slot),
                max(pair.initiator_slot, pair.responder_slot),
            )
            if key in accepted or (requested and key not in requested):
                raise SurveyStateError(
                    "gateway plan contains a duplicate or unrequested pair"
                )
            if (
                pair.initiator_slot not in self.slot_to_anchor
                or pair.responder_slot not in self.slot_to_anchor
            ):
                raise SurveyStateError("gateway plan references an unknown slot")
            accepted.add(key)
        self.batch_plan_offset = len(self.plan_pairs)
        self.batch_plan_pairs = event.plan_pairs
        self.plan_pairs = self.plan_pairs + event.plan_pairs
        self._geometry_changed()
        skipped = len(event.skipped_pairs)
        state = "warning" if skipped or not event.plan_pairs else "done"
        self._set_step(
            "plan",
            state,
            f"Accepted {len(event.plan_pairs)} pairs in {event.wave_count} waves"
            + (f"; skipped {skipped}" if skipped else ""),
            current=len(event.plan_pairs),
            total=len(self.requested_pairs),
        )
        self.phase = "ranging"
        self._set_step(
            "ranging",
            "running",
            f"Waiting for 0/{len(event.plan_pairs)} pair results",
            current=0,
            total=len(event.plan_pairs),
        )

    def _observe_ranges(self, event: SurveyEvent) -> None:
        if not self.plan_accepted and (event.range_results or self.requested_pairs):
            raise SurveyEventNotReady(
                "survey range event is waiting for the exact PLAN command result"
            )
        if event.range_results and not self.plan_pairs:
            raise SurveyStateError("range results arrived before an accepted plan")
        changed = False
        for result in event.range_results:
            if result.pair_index >= len(self.batch_plan_pairs):
                raise SurveyStateError("range result has no accepted plan pair")
            pair = self.batch_plan_pairs[result.pair_index]
            if result.responder_slot != pair.responder_slot:
                raise SurveyStateError(
                    "range result responder contradicts its accepted plan pair"
                )
            global_index = self.batch_plan_offset + result.pair_index
            previous = self.results.get(global_index)
            if previous is not None and previous != result:
                raise SurveyStateError(
                    "range result changed after its first accepted value"
                )
            if previous is None:
                self.results[global_index] = result
                changed = True
        if changed:
            self._geometry_changed()
        total = len(self.batch_plan_pairs)
        received = sum(
            index in self.results
            for index in range(
                self.batch_plan_offset,
                self.batch_plan_offset + total,
            )
        )
        usable = sum(
            self.results[index].usable
            for index in range(
                self.batch_plan_offset,
                self.batch_plan_offset + total,
            )
            if index in self.results
        )
        self._set_step(
            "ranging",
            "running",
            f"Received {received}/{total} batch results; {usable} usable medians",
            current=received,
            total=total,
        )
        if self.geometry_solve_pending:
            self._set_step(
                "geometry",
                "running",
                f"Solving from {len(self.geometry_pairs)} usable distances",
            )
        if event.kind == SURVEY_EVENT_BATCH_COMPLETE:
            if event.final_batch:
                raise SurveyStateError("a final batch cannot request another PLAN")
            self.next_batch_index += 1
            self.plan_accepted = False
            self.phase = "planning"
            self._set_step(
                "plan", "running", "Firmware RAM is free; submitting the next batch"
            )
            return
        if event.kind != SURVEY_EVENT_TERMINAL:
            return
        total = len(self.plan_pairs)
        usable = sum(result.usable for result in self.results.values())
        self.terminal_status = event.status
        self.active = False
        self.phase = "terminal"
        if event.status == SURVEY_TERMINAL_COMPLETE and usable == total:
            state = "done"
            detail = f"All {total} pair medians are usable"
        elif event.status == SURVEY_TERMINAL_ABORTED:
            state = "warning"
            detail = f"Survey aborted with {usable}/{total} usable medians"
        elif event.status == SURVEY_TERMINAL_PARTIAL:
            state = "warning"
            detail = f"Partial survey: {usable}/{total} usable medians"
        else:
            state = "failed"
            detail = f"Survey terminal status {event.status}"
        self._set_step(
            "ranging", state, detail, current=len(self.results), total=total
        )
        if not self.geometry_solve_ready:
            self._set_step("geometry", "warning", self.geometry_requirement)

    def fail(self, step: str, detail: str) -> None:
        self.active = False
        self.phase = "failed"
        self.error = detail
        self._set_step(step, "failed", detail)

    def _geometry_changed(self) -> None:
        self.geometry_revision += 1
        if self.geometry_solve_pending:
            self._set_step(
                "geometry",
                "running",
                f"Solving from {len(self.geometry_pairs)} usable distances",
            )

    @property
    def current_geometry_pairs(self) -> tuple[AnchorPairDistance, ...]:
        pairs: list[AnchorPairDistance] = []
        for pair_index, result in sorted(self.results.items()):
            if not result.usable or result.median_mm is None:
                continue
            plan = self.plan_pairs[pair_index]
            anchor_a = self.slot_to_anchor.get(plan.initiator_slot)
            anchor_b = self.slot_to_anchor.get(plan.responder_slot)
            if anchor_a is None or anchor_b is None:
                continue
            pairs.append(
                AnchorPairDistance(
                    anchor_label(anchor_a),
                    anchor_label(anchor_b),
                    result.median_mm / 1000.0,
                    sigma_m=0.05,
                    source=f"survey generation {self.generation}, pair {pair_index}",
                )
            )
        return tuple(pairs)

    @property
    def geometry_pairs(self) -> tuple[AnchorPairDistance, ...]:
        combined = dict(self._solve_base_pairs)
        for pair in self.current_geometry_pairs:
            combined[self._distance_key(pair)] = pair
        if not combined and self.geometry_uses_retained_fallback:
            combined.update(self._retained_geometry_pairs)
        return tuple(combined[key] for key in sorted(combined))

    @property
    def geometry_uses_retained_fallback(self) -> bool:
        """Keep an exhausted follow-up from erasing a solvable dataset."""

        return bool(
            self.pass_mode != SURVEY_PASS_FRESH
            and self._retained_geometry_pairs
            and not self.current_geometry_pairs
            and (
                self.phase == "terminal"
                or (self.plan_accepted and not self.plan_pairs)
            )
        )

    @property
    def current_neighbor_pairs(self) -> frozenset[tuple[str, str]]:
        """Return radio-neighbor edges observed in either direction."""

        heard = {
            report.own_slot: report.heard_slots
            for report in self.neighbor_reports
        }
        slots = sorted(self.slot_to_anchor)
        pairs: set[tuple[str, str]] = set()
        for index, first in enumerate(slots):
            for second in slots[index + 1:]:
                if (
                    second in heard.get(first, frozenset())
                    or first in heard.get(second, frozenset())
                ):
                    pairs.add(self._anchor_key(
                        anchor_label(self.slot_to_anchor[first]),
                        anchor_label(self.slot_to_anchor[second]),
                    ))
        return frozenset(pairs)

    @property
    def neighbor_pairs(self) -> frozenset[tuple[str, str]]:
        retained = (
            self._retained_neighbor_pairs
            if self.geometry_uses_retained_fallback
            else set()
        )
        return frozenset(
            retained
            | self._solve_base_neighbor_pairs
            | set(self.current_neighbor_pairs)
        )

    @property
    def nonneighbor_pairs(self) -> frozenset[tuple[str, str]]:
        """Return confirmed negative edges only when both reports exist."""

        heard = {
            report.own_slot: report.heard_slots
            for report in self.neighbor_reports
        }
        reported = set(heard)
        slots = sorted(self.slot_to_anchor)
        pairs: set[tuple[str, str]] = set()
        for index, first in enumerate(slots):
            for second in slots[index + 1:]:
                if first not in reported or second not in reported:
                    continue
                if (
                    second not in heard[first]
                    and first not in heard[second]
                ):
                    pairs.add(self._anchor_key(
                        anchor_label(self.slot_to_anchor[first]),
                        anchor_label(self.slot_to_anchor[second]),
                    ))
        measured = {
            self._distance_key(pair)
            for pair in self.geometry_pairs
        }
        positive = set(self.neighbor_pairs)
        return frozenset(
            pair
            for pair in pairs
            if pair not in measured and pair not in positive
        )

    @property
    def geometry_solve_ready(self) -> bool:
        pairs = self.geometry_pairs
        if (
            self.pass_mode != SURVEY_PASS_FRESH
            and not self.current_geometry_pairs
            and not self.geometry_uses_retained_fallback
        ):
            return False
        expected_anchors = {
            anchor_label(anchor_id) for anchor_id in self.slot_to_anchor.values()
        }
        anchors = {
            anchor
            for pair in pairs
            for anchor in (pair.anchor_a_id, pair.anchor_b_id)
        }
        minimum_pairs = (
            2 * len(expected_anchors) - 3
            if self.pass_mode == SURVEY_PASS_FRESH or self.layout is None
            else len(expected_anchors) - 1
        )
        if (
            len(expected_anchors) < 3
            or anchors != expected_anchors
            or len(pairs) < minimum_pairs
        ):
            return False
        adjacency: dict[str, set[str]] = {anchor: set() for anchor in anchors}
        for pair in pairs:
            adjacency[pair.anchor_a_id].add(pair.anchor_b_id)
            adjacency[pair.anchor_b_id].add(pair.anchor_a_id)
        seen: set[str] = set()
        frontier = [next(iter(anchors))]
        while frontier:
            anchor = frontier.pop()
            if anchor in seen:
                continue
            seen.add(anchor)
            frontier.extend(adjacency[anchor] - seen)
        return seen == anchors

    @property
    def geometry_solve_pending(self) -> bool:
        """Return whether the current geometry revision still needs solving."""

        return (
            self.geometry_solve_ready
            and self.layout_revision != self.geometry_revision
        )

    @property
    def geometry_requirement(self) -> str:
        pairs = self.geometry_pairs
        expected_anchors = {
            anchor_label(anchor_id) for anchor_id in self.slot_to_anchor.values()
        }
        anchors = {
            anchor
            for pair in pairs
            for anchor in (pair.anchor_a_id, pair.anchor_b_id)
        }
        if len(expected_anchors) < 3:
            return "Need usable ranges connecting at least three anchors"
        missing = expected_anchors - anchors
        if missing:
            return (
                f"Need usable ranges covering all anchors; {len(missing)} "
                f"of {len(expected_anchors)} are still unconnected"
            )
        required = (
            2 * len(expected_anchors) - 3
            if self.pass_mode == SURVEY_PASS_FRESH or self.layout is None
            else len(expected_anchors) - 1
        )
        if len(pairs) < required:
            return (
                f"Need at least {required} usable constraints for "
                f"{len(expected_anchors)} anchors"
                + (
                    " in this layout-seeded follow-up"
                    if self.pass_mode != SURVEY_PASS_FRESH
                    and self.layout is not None
                    else ""
                )
            )
        return "Usable ranges do not form one connected geometry graph"

    def apply_layout(
        self,
        revision: int,
        layout: AnchorLayoutResult,
    ) -> bool:
        if revision != self.geometry_revision:
            return False
        expected = {
            anchor
            for pair in self.geometry_pairs
            for anchor in (pair.anchor_a_id, pair.anchor_b_id)
        }
        if set(layout.positions_m) != expected:
            raise SurveyStateError("geometry solver returned the wrong anchor set")
        self.layout = layout
        self.layout_revision = revision
        state = "warning" if layout.warnings else "done"
        self._set_step(
            "geometry",
            state,
            f"Relative 2D fit RMSE {layout.rmse_m:.3f} m; "
            f"max residual {layout.max_residual_m:.3f} m",
        )
        return True

    def geometry_failed(self, revision: int, message: str) -> bool:
        if revision != self.geometry_revision:
            return False
        self._set_step("geometry", "failed", message)
        self.error = message
        return True

    @property
    def progress_percent(self) -> float:
        if not self.steps:
            return 0.0
        return 100.0 * sum(step.fraction for step in self.steps.values()) / len(
            self.steps
        )

    @property
    def headline(self) -> str:
        if self.phase == "idle":
            return "Run a survey to enumerate anchors, range pairs, and solve geometry."
        if self.phase == "terminal":
            usable = sum(result.usable for result in self.results.values())
            if self.geometry_uses_retained_fallback:
                return (
                    f"Survey pass {self.pass_index}, generation "
                    f"{self.generation} found no new pair ranges; retained "
                    f"{len(self.geometry_pairs)} earlier ranges for re-solving."
                )
            return (
                f"Survey pass {self.pass_index}, generation {self.generation} "
                "finished with "
                f"{usable}/{len(self.plan_pairs)} usable pair ranges."
            )
        if self.phase == "failed":
            return self.error or "Survey failed."
        current = next(
            (step for step in self.steps.values() if step.state == "running"),
            None,
        )
        return current.detail if current is not None else "Survey operation is active."

    def _set_step(
        self,
        key: str,
        state: str,
        detail: str,
        *,
        current: int = 0,
        total: int = 0,
    ) -> None:
        step = self.steps[key]
        step.state = state
        step.detail = detail
        step.current = max(0, current)
        step.total = max(0, total)
