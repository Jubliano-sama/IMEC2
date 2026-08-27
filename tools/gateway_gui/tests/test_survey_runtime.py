from __future__ import annotations

from dataclasses import replace
import unittest

from tools.gateway_gui.command_telemetry import GatewayCommandEvent
from tools.gateway_gui.diagnostic_models import solve_geometry
from tools.gateway_gui.protocol import (
    CMD_ASSIGN_DISCOVERY_SLOTS,
    CMD_FORCE_REDISCOVERY,
    CMD_SURVEY_PLAN,
    CMD_SURVEY_START,
    SURVEY_EVENT_NEIGHBOR_GRAPH,
    SURVEY_EVENT_PLAN_ACCEPTED,
    SURVEY_EVENT_RANGE_PROGRESS,
    SURVEY_EVENT_TERMINAL,
    SURVEY_TERMINAL_COMPLETE,
    SurveyAssignmentIdentity,
    SurveyEvent,
    SurveyNeighborReport,
    SurveyPlanPair,
    SurveyRangeResult,
)
from tools.gateway_gui.survey_runtime import (
    StaleSurveyEvent,
    SurveyCommandOwner,
    SurveyEventNotReady,
    SurveyOperationModel,
    SurveyStateError,
)


ANCHORS = (0xA1, 0xB2, 0xC3)


def command_event(
    *,
    kind: int,
    stage: int,
    command_id: int,
    anchor_id: int = 0,
    slot: int = 0xFF,
    hop: int = 0,
    progress: int = 0,
    total: int = 0,
    terminal: bool = False,
    status: int = 0,
    reason: int = 0,
    success: int | None = None,
    failure: int = 0,
) -> GatewayCommandEvent:
    return GatewayCommandEvent(
        command_kind=kind,
        stage=stage,
        flags=1 if terminal else 0,
        attempt=1,
        command_status=status,
        reason=reason,
        command_id=command_id,
        route_epoch=7,
        correlation_id=100 + kind,
        gateway_sequence=1,
        host_session_id=100 + kind,
        host_sequence=kind,
        event_sequence=stage,
        anchor_id=anchor_id,
        pair_initiator_id=0,
        pair_responder_id=0,
        previous_hop_id=0,
        progress_count=progress,
        total_count=total,
        success_count=(
            success
            if success is not None
            else total if terminal and status == 0 else progress
        ),
        failure_count=failure,
        duplicate_count=0,
        lost_event_count=0,
        hop_count=hop,
        discovery_slot=slot,
    )


def assignment() -> SurveyAssignmentIdentity:
    return SurveyAssignmentIdentity(71, 81, bytes((0x5A,)) * 32, 3, 3)


def neighbor_event(generation: int = 9) -> SurveyEvent:
    return SurveyEvent(
        kind=SURVEY_EVENT_NEIGHBOR_GRAPH,
        status=SURVEY_TERMINAL_COMPLETE,
        generation=generation,
        assignment=assignment(),
        partial_reasons=0,
        occupied_slots=frozenset((0, 1, 2)),
        neighbor_reports=(
            SurveyNeighborReport(0, frozenset((1, 2))),
            SurveyNeighborReport(1, frozenset((0, 2))),
            SurveyNeighborReport(2, frozenset((0, 1))),
        ),
    )


def plan_event(generation: int = 9) -> SurveyEvent:
    return SurveyEvent(
        kind=SURVEY_EVENT_PLAN_ACCEPTED,
        status=SURVEY_TERMINAL_COMPLETE,
        generation=generation,
        assignment=assignment(),
        partial_reasons=0,
        plan_pairs=(
            SurveyPlanPair(0, 1, 0),
            SurveyPlanPair(0, 2, 0),
            SurveyPlanPair(1, 2, 1),
        ),
        wave_count=2,
    )


def result_event(*, terminal: bool, generation: int = 9) -> SurveyEvent:
    return SurveyEvent(
        kind=SURVEY_EVENT_TERMINAL if terminal else SURVEY_EVENT_RANGE_PROGRESS,
        status=SURVEY_TERMINAL_COMPLETE,
        generation=generation,
        assignment=assignment(),
        partial_reasons=0,
        range_results=(
            SurveyRangeResult(0, 5, 1, 3000),
            SurveyRangeResult(1, 5, 2, 4000),
            SurveyRangeResult(2, 5, 2, 5000),
        ),
    )


def enumerate_three(model: SurveyOperationModel) -> None:
    model.observe_command_event(
        command_event(
            kind=3,
            stage=12,
            command_id=CMD_FORCE_REDISCOVERY,
            terminal=True,
        )
    )
    for slot, anchor_id in enumerate(ANCHORS):
        model.observe_command_event(
            command_event(
                kind=1,
                stage=6,
                command_id=CMD_ASSIGN_DISCOVERY_SLOTS,
                anchor_id=anchor_id,
                slot=slot,
                hop=slot + 1,
                progress=slot + 1,
                total=3,
            )
        )
    model.observe_command_event(
        command_event(
            kind=1,
            stage=12,
            command_id=CMD_ASSIGN_DISCOVERY_SLOTS,
            progress=3,
            total=3,
            terminal=True,
        )
    )


class SurveyCommandOwnerTests(unittest.TestCase):
    def test_exact_result_identity_and_timeout_release_one_owner(self) -> None:
        owner = SurveyCommandOwner()
        self.assertTrue(owner.begin(CMD_SURVEY_START, 10, 2, "start", timeout_s=5, now=1))
        self.assertFalse(owner.begin(CMD_SURVEY_PLAN, 11, 3, "plan", timeout_s=5, now=1))
        self.assertFalse(owner.observe_result(CMD_SURVEY_START, 10, 3, 0).matched)
        accepted = owner.observe_result(CMD_SURVEY_START, 10, 2, 0)
        self.assertEqual(accepted.outcome, "accepted")
        self.assertIsNone(owner.pending)

        self.assertTrue(owner.begin(CMD_SURVEY_PLAN, 11, 3, "plan", timeout_s=5, now=10))
        self.assertFalse(owner.expire(now=14.9).matched)
        self.assertEqual(owner.expire(now=15).outcome, "timeout")

    def test_delivered_busy_is_a_terminal_rejection(self) -> None:
        owner = SurveyCommandOwner()
        owner.begin(CMD_SURVEY_START, 10, 2, "start", timeout_s=5)
        transition = owner.observe_result(CMD_SURVEY_START, 10, 2, 3)
        self.assertEqual((transition.matched, transition.outcome, transition.status), (True, "rejected", 3))


class SurveyOperationModelTests(unittest.TestCase):
    def model_after_enumeration(self) -> SurveyOperationModel:
        model = SurveyOperationModel()
        model.begin(expected_anchor_count=3)
        enumerate_three(model)
        self.assertEqual(model.slot_to_anchor, dict(enumerate(ANCHORS)))
        return model

    def test_full_operation_binds_generation_and_solves_geometry(self) -> None:
        model = self.model_after_enumeration()
        model.note_command_dispatched(CMD_SURVEY_START, now=10.0)
        with self.assertRaises(SurveyEventNotReady):
            model.observe_survey_event(neighbor_event(), created_at=12.0)
        model.note_command_accepted(CMD_SURVEY_START)
        with self.assertRaises(StaleSurveyEvent):
            model.observe_survey_event(neighbor_event(), created_at=8.0)

        model.observe_survey_event(neighbor_event(), created_at=12.0)
        model.set_requested_pairs(((0, 1), (0, 2), (1, 2)))
        model.note_command_dispatched(CMD_SURVEY_PLAN)
        with self.assertRaises(SurveyEventNotReady):
            model.observe_survey_event(plan_event())
        model.note_command_accepted(CMD_SURVEY_PLAN)
        model.observe_survey_event(plan_event())
        model.observe_survey_event(result_event(terminal=False))
        self.assertTrue(model.geometry_solve_ready)
        self.assertEqual(
            sorted(round(pair.distance_m, 3) for pair in model.geometry_pairs),
            [3.0, 4.0, 5.0],
        )
        revision = model.geometry_revision
        layout = solve_geometry(model.geometry_pairs, solver="Visibility branching tuned")
        self.assertTrue(model.apply_layout(revision, layout))
        self.assertEqual(set(layout.positions_m), {anchor_label for pair in model.geometry_pairs for anchor_label in (pair.anchor_a_id, pair.anchor_b_id)})
        self.assertEqual(model.steps["geometry"].state, "done")
        self.assertFalse(model.geometry_solve_pending)

        # A terminal event may repeat results after the asynchronous solver
        # has already completed. It must not put the solved revision back into
        # a permanent "running" state when no new solve will be scheduled.
        model.observe_survey_event(result_event(terminal=True))
        self.assertEqual(model.steps["geometry"].state, "done")
        self.assertFalse(model.geometry_solve_pending)
        self.assertEqual(model.progress_percent, 100.0)

    def test_unexpected_anchor_count_warns_and_continues_with_actual_table(self) -> None:
        for configured, terminal_total, reason, failure in (
            (4, 4, 6, 1),
            (2, 2, 0, 0),
        ):
            with self.subTest(configured=configured):
                model = SurveyOperationModel()
                model.begin(expected_anchor_count=configured)
                model.observe_command_event(
                    command_event(
                        kind=3,
                        stage=12,
                        command_id=CMD_FORCE_REDISCOVERY,
                        terminal=True,
                    )
                )
                for slot, anchor_id in enumerate(ANCHORS):
                    model.observe_command_event(
                        command_event(
                            kind=1,
                            stage=6,
                            command_id=CMD_ASSIGN_DISCOVERY_SLOTS,
                            anchor_id=anchor_id,
                            slot=slot,
                            hop=slot + 1,
                            progress=slot + 1,
                            total=terminal_total,
                        )
                    )
                model.observe_command_event(
                    command_event(
                        kind=1,
                        stage=12,
                        command_id=CMD_ASSIGN_DISCOVERY_SLOTS,
                        progress=3,
                        total=terminal_total,
                        terminal=True,
                        reason=reason,
                        success=3,
                        failure=failure,
                    )
                )

                self.assertTrue(model.active)
                self.assertEqual(model.phase, "waiting-start")
                self.assertEqual(model.expected_anchor_count, 3)
                self.assertEqual(model.steps["enumeration"].state, "warning")
                self.assertIn("configured count", model.steps["enumeration"].detail)
                self.assertNotIn("failed", model.headline.lower())

    def test_new_run_rejects_an_already_seen_identity(self) -> None:
        model = self.model_after_enumeration()
        model.note_command_dispatched(CMD_SURVEY_START, now=1.0)
        model.note_command_accepted(CMD_SURVEY_START)
        model.observe_survey_event(neighbor_event(generation=9), created_at=2.0)

        model.begin(expected_anchor_count=3)
        enumerate_three(model)
        model.note_command_dispatched(CMD_SURVEY_START, now=3.0)
        model.note_command_accepted(CMD_SURVEY_START)
        with self.assertRaises(StaleSurveyEvent):
            model.observe_survey_event(neighbor_event(generation=9), created_at=4.0)

    def test_conflicting_result_cannot_replace_first_accepted_value(self) -> None:
        model = self.model_after_enumeration()
        model.note_command_dispatched(CMD_SURVEY_START, now=1.0)
        model.note_command_accepted(CMD_SURVEY_START)
        model.observe_survey_event(neighbor_event(), created_at=2.0)
        model.set_requested_pairs(((0, 1), (0, 2), (1, 2)))
        model.note_command_accepted(CMD_SURVEY_PLAN)
        model.observe_survey_event(plan_event())
        progress = result_event(terminal=False)
        model.observe_survey_event(progress)

        changed = replace(
            progress,
            range_results=(SurveyRangeResult(0, 5, 1, 3333),),
        )
        with self.assertRaisesRegex(SurveyStateError, "changed"):
            model.observe_survey_event(changed)

    def test_underconstrained_ranges_remain_visible_without_fake_coordinates(self) -> None:
        model = self.model_after_enumeration()
        model.note_command_dispatched(CMD_SURVEY_START, now=1.0)
        model.note_command_accepted(CMD_SURVEY_START)
        model.observe_survey_event(neighbor_event(), created_at=2.0)
        model.set_requested_pairs(((0, 1), (1, 2)))
        model.note_command_accepted(CMD_SURVEY_PLAN)
        sparse_plan = replace(
            plan_event(),
            plan_pairs=(SurveyPlanPair(0, 1, 0), SurveyPlanPair(1, 2, 1)),
        )
        model.observe_survey_event(sparse_plan)
        model.observe_survey_event(
            replace(
                result_event(terminal=True),
                range_results=(
                    SurveyRangeResult(0, 5, 1, 3000),
                    SurveyRangeResult(1, 5, 2, 4000),
                ),
            )
        )

        self.assertFalse(model.geometry_solve_ready)
        self.assertIn("at least 3 usable constraints", model.geometry_requirement)
        self.assertIsNone(model.layout)

    def test_partial_layout_cannot_silently_omit_an_enumerated_anchor(self) -> None:
        model = self.model_after_enumeration()
        model.slot_to_anchor[3] = 0xD4
        model.slot_hops[3] = 2
        model.plan_pairs = (
            SurveyPlanPair(0, 1, 0),
            SurveyPlanPair(0, 2, 0),
            SurveyPlanPair(1, 2, 1),
        )
        model.results = {
            0: SurveyRangeResult(0, 5, 1, 3000),
            1: SurveyRangeResult(1, 5, 2, 4000),
            2: SurveyRangeResult(2, 5, 2, 5000),
        }

        self.assertFalse(model.geometry_solve_ready)
        self.assertIn("all anchors", model.geometry_requirement)

    def test_fresh_assignment_can_restart_at_the_same_generation_after_reboot(self) -> None:
        model = self.model_after_enumeration()
        model.note_command_dispatched(CMD_SURVEY_START, now=1.0)
        model.note_command_accepted(CMD_SURVEY_START)
        model.observe_survey_event(neighbor_event(generation=9), created_at=2.0)

        model.begin(expected_anchor_count=3)
        enumerate_three(model)
        model.note_command_dispatched(CMD_SURVEY_START, now=3.0)
        model.note_command_accepted(CMD_SURVEY_START)
        fresh_assignment = SurveyAssignmentIdentity(
            72, 82, bytes((0x6B,)) * 32, 3, 3
        )

        self.assertTrue(
            model.observe_survey_event(
                replace(
                    neighbor_event(generation=9),
                    assignment=fresh_assignment,
                ),
                created_at=4.0,
            )
        )


if __name__ == "__main__":
    unittest.main()
