"""Generation-bound host model for live enumeration and survey progress."""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass
import time

from .anchor_geometry import AnchorLayoutResult, AnchorPairDistance
from .command_telemetry import GatewayCommandEvent
from .diagnostic_models import anchor_label
from .protocol import (
    CMD_SURVEY_CANCEL,
    CMD_SURVEY_PLAN,
    CMD_SURVEY_START,
    SURVEY_EVENT_NEIGHBOR_GRAPH,
    SURVEY_EVENT_PLAN_ACCEPTED,
    SURVEY_EVENT_RANGE_PROGRESS,
    SURVEY_EVENT_TERMINAL,
    SURVEY_TERMINAL_ABORTED,
    SURVEY_TERMINAL_COMPLETE,
    SURVEY_TERMINAL_PARTIAL,
    SurveyAssignmentIdentity,
    SurveyEvent,
    SurveyNeighborReport,
    SurveyPlanPair,
    SurveyRangeResult,
)


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
        self.requested_pairs: tuple[tuple[int, int], ...] = ()
        self.plan_pairs: tuple[SurveyPlanPair, ...] = ()
        self.results: dict[int, SurveyRangeResult] = {}
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

    def begin(self, *, expected_anchor_count: int = 0) -> None:
        self.run_serial += 1
        self._clear_run()
        self.active = True
        self.phase = "routes"
        self.expected_anchor_count = max(0, int(expected_anchor_count))
        self._set_step(
            "routes",
            "running",
            "Waiting for the gateway route-refresh terminal",
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

        complete = (
            event.command_status == 0
            and event.reason == 0
            and event.failure_count == 0
            and event.total_count > 0
            and len(self.slot_to_anchor) == event.total_count
            and len(set(self.slot_to_anchor.values())) == event.total_count
        )
        if not complete:
            self.fail(
                "enumeration",
                "Enumeration ended without one exact slot and hop for every anchor",
            )
            return True
        self.expected_anchor_count = event.total_count
        self._set_step(
            "enumeration",
            "done",
            f"{event.total_count} anchors mapped to exact discovery slots",
            current=event.total_count,
            total=event.total_count,
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
        elif event.kind == SURVEY_EVENT_PLAN_ACCEPTED:
            self._observe_plan(event)
        elif event.kind in (SURVEY_EVENT_RANGE_PROGRESS, SURVEY_EVENT_TERMINAL):
            self._observe_ranges(event)
        else:  # The decoder rejects this, but the model remains fail closed.
            raise SurveyStateError(f"unsupported survey event kind {event.kind}")
        return True

    def _validate_first_event(self, event: SurveyEvent) -> None:
        occupied = set(event.occupied_slots)
        if occupied != set(self.slot_to_anchor):
            raise StaleSurveyEvent(
                "survey occupied slots do not match the fresh enumeration"
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
        self._set_step("plan", "running", "Selecting mutual degree-balanced pairs")

    def _observe_plan(self, event: SurveyEvent) -> None:
        if not self.neighbor_reports:
            raise SurveyStateError("survey plan arrived before the neighbor graph")
        if not self.requested_pairs:
            raise SurveyStateError("survey plan arrived without a host pair request")
        if not self.plan_accepted:
            raise SurveyEventNotReady(
                "survey plan event is waiting for the exact PLAN command result"
            )
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
        self.plan_pairs = event.plan_pairs
        self.results.clear()
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
            if result.pair_index >= len(self.plan_pairs):
                raise SurveyStateError("range result has no accepted plan pair")
            pair = self.plan_pairs[result.pair_index]
            if result.responder_slot != pair.responder_slot:
                raise SurveyStateError(
                    "range result responder contradicts its accepted plan pair"
                )
            previous = self.results.get(result.pair_index)
            if previous is not None and previous != result:
                raise SurveyStateError(
                    "range result changed after its first accepted value"
                )
            if previous is None:
                self.results[result.pair_index] = result
                changed = True
        if changed:
            self._geometry_changed()
        usable = sum(result.usable for result in self.results.values())
        total = len(self.plan_pairs)
        self._set_step(
            "ranging",
            "running",
            f"Received {len(self.results)}/{total} results; {usable} usable medians",
            current=len(self.results),
            total=total,
        )
        if self.geometry_solve_ready:
            self._set_step(
                "geometry",
                "running",
                f"Solving from {len(self.geometry_pairs)} usable distances",
            )
        if event.kind != SURVEY_EVENT_TERMINAL:
            return
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
        if self.geometry_solve_ready:
            self._set_step(
                "geometry",
                "running",
                f"Solving from {len(self.geometry_pairs)} usable distances",
            )

    @property
    def geometry_pairs(self) -> tuple[AnchorPairDistance, ...]:
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
    def geometry_solve_ready(self) -> bool:
        pairs = self.geometry_pairs
        expected_anchors = {
            anchor_label(anchor_id) for anchor_id in self.slot_to_anchor.values()
        }
        anchors = {
            anchor
            for pair in pairs
            for anchor in (pair.anchor_a_id, pair.anchor_b_id)
        }
        if (
            len(expected_anchors) < 3
            or anchors != expected_anchors
            or len(pairs) < 2 * len(expected_anchors) - 3
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
        required = 2 * len(expected_anchors) - 3
        if len(pairs) < required:
            return (
                f"Need at least {required} usable constraints for "
                f"{len(expected_anchors)} anchors"
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
            return (
                f"Survey generation {self.generation} finished with "
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
