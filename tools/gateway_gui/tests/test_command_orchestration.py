from __future__ import annotations

import ast
from dataclasses import replace
from pathlib import Path
import unittest

from tools.gateway_gui.command_orchestration import (
    CMD_SURVEY_ABORT,
    GatewayCommandDispatch,
    GatewayCommandOrchestrator,
    GatewayCommandPlan,
    gateway_command_requires_preflight,
)
from tools.gateway_gui.command_telemetry import (
    GatewayCommandEvent,
    GatewayCommandRequestTracker,
)
from tools.gateway_gui.protocol import (
    CMD_ASSIGN_DISCOVERY_SLOTS,
    CMD_FORCE_REDISCOVERY,
    CMD_SURVEY_REACHABILITY,
)


def dispatch(
    command_id: int,
    command_kind: int,
    session_id: int,
    sequence: int,
) -> GatewayCommandDispatch:
    return GatewayCommandDispatch(
        command_kind=command_kind,
        command_id=command_id,
        session_id=session_id,
        sequence=sequence,
        frame=f"frame-{command_id}-{session_id}".encode(),
        label=f"command-{command_id}",
        timeout_s=10.0,
        status_text="sending",
    )


def terminal(
    item: GatewayCommandDispatch,
    *,
    status: int = 0,
    reason: int = 0,
) -> GatewayCommandEvent:
    return GatewayCommandEvent(
        command_kind=item.command_kind,
        stage=12,
        flags=1,
        attempt=1,
        command_status=status,
        reason=reason,
        command_id=item.command_id,
        route_epoch=0,
        correlation_id=item.session_id,
        gateway_sequence=1,
        host_session_id=item.session_id,
        host_sequence=item.sequence,
        event_sequence=1,
        anchor_id=0,
        pair_initiator_id=0,
        pair_responder_id=0,
        previous_hop_id=0,
        progress_count=0,
        total_count=0,
        success_count=0,
        failure_count=0,
        duplicate_count=0,
        lost_event_count=0,
        hop_count=0,
        discovery_slot=0,
    )


class GatewayCommandPlanTests(unittest.TestCase):
    def test_preflight_is_default_and_immediate_controls_are_explicit(self) -> None:
        assignment = dispatch(CMD_ASSIGN_DISCOVERY_SLOTS, 1, 10, 1)
        here_i_am = dispatch(CMD_FORCE_REDISCOVERY, 3, 11, 2)

        self.assertTrue(gateway_command_requires_preflight(CMD_ASSIGN_DISCOVERY_SLOTS))
        self.assertFalse(gateway_command_requires_preflight(CMD_FORCE_REDISCOVERY))
        self.assertFalse(gateway_command_requires_preflight(CMD_SURVEY_ABORT))
        with self.assertRaisesRegex(ValueError, "requires a Here-I-Am"):
            GatewayCommandPlan.user_triggered(assignment)
        self.assertEqual(
            GatewayCommandPlan.user_triggered(here_i_am).target, here_i_am
        )
        abort = dispatch(CMD_SURVEY_ABORT, 2, 12, 3)
        with self.assertRaisesRegex(ValueError, "must not be preflighted"):
            GatewayCommandPlan.user_triggered(abort, preflight=here_i_am)

    def test_preflight_and_target_must_have_distinct_identities(self) -> None:
        assignment = dispatch(CMD_ASSIGN_DISCOVERY_SLOTS, 1, 10, 1)
        same_identity = dispatch(CMD_FORCE_REDISCOVERY, 3, 10, 1)

        with self.assertRaisesRegex(ValueError, "identities must differ"):
            GatewayCommandPlan.user_triggered(
                assignment, preflight=same_identity
            )


class GatewayCommandSourceInvariantTests(unittest.TestCase):
    def test_user_handlers_cannot_bypass_the_single_transport_dispatch_seam(self) -> None:
        source = (
            Path(__file__).resolve().parents[1] / "app.py"
        ).read_text(encoding="utf-8")
        tree = ast.parse(source)
        direct_senders: list[str] = []
        orchestrated_handlers: set[str] = set()

        for node in ast.walk(tree):
            if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                continue
            for child in ast.walk(node):
                if not isinstance(child, ast.Call) or not isinstance(
                    child.func, ast.Attribute
                ):
                    continue
                if child.func.attr == "send_frame":
                    direct_senders.append(node.name)
                if child.func.attr == "_submit_gateway_command":
                    orchestrated_handlers.add(node.name)

        self.assertEqual(direct_senders, ["_dispatch_gateway_command"])
        self.assertTrue(
            {"_send_discovery", "_send_here_i_am", "_send_assign_discovery_slots"}
            <= orchestrated_handlers
        )


class GatewayCommandOrchestratorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tracker = GatewayCommandRequestTracker()
        self.orchestrator = GatewayCommandOrchestrator(self.tracker)
        self.preflight = dispatch(CMD_FORCE_REDISCOVERY, 3, 101, 1)
        self.target = dispatch(CMD_SURVEY_REACHABILITY, 2, 102, 2)
        self.plan = GatewayCommandPlan.user_triggered(
            self.target, preflight=self.preflight
        )

    def test_matching_preflight_terminal_atomically_arms_target(self) -> None:
        self.assertEqual(self.orchestrator.begin(self.plan, now=0.0), self.preflight)
        transition = self.orchestrator.observe_event(
            terminal(self.preflight), now=1.0
        )

        self.assertTrue(transition.matched)
        self.assertEqual(transition.dispatch, self.target)
        self.assertEqual(transition.phase, "preflight")
        self.assertTrue(self.orchestrator.active)
        self.assertIsNotNone(self.tracker.pending)
        assert self.tracker.pending is not None
        self.assertEqual(self.tracker.pending.host_session_id, self.target.session_id)

        completed = self.orchestrator.observe_event(terminal(self.target), now=2.0)
        self.assertTrue(completed.completed)
        self.assertEqual(completed.outcome, "complete")
        self.assertEqual(completed.phase, "target")
        self.assertFalse(self.orchestrator.active)

    def test_intermediate_stale_and_duplicate_events_never_dispatch_target_twice(self) -> None:
        self.orchestrator.begin(self.plan, now=0.0)
        intermediate = replace(terminal(self.preflight), stage=4, flags=0)
        stale = replace(terminal(self.preflight), correlation_id=999)

        self.assertFalse(self.orchestrator.observe_event(intermediate).matched)
        self.assertFalse(self.orchestrator.observe_event(stale).matched)
        first = self.orchestrator.observe_event(terminal(self.preflight), now=1.0)
        self.assertEqual(first.dispatch, self.target)
        self.assertFalse(
            self.orchestrator.observe_event(terminal(self.preflight), now=1.1).matched
        )

    def test_failed_preflight_never_arms_target(self) -> None:
        self.orchestrator.begin(self.plan, now=0.0)
        failed = self.orchestrator.observe_event(
            terminal(self.preflight, status=5, reason=6), now=1.0
        )

        self.assertTrue(failed.completed)
        self.assertEqual(failed.outcome, "failed")
        self.assertEqual(failed.phase, "preflight")
        self.assertFalse(self.orchestrator.active)
        self.assertIsNone(self.tracker.pending)

    def test_positive_result_waits_for_terminal_but_negative_result_stops(self) -> None:
        self.orchestrator.begin(self.plan, now=0.0)
        positive = self.orchestrator.observe_command_result(
            command_id=self.preflight.command_id,
            host_session_id=self.preflight.session_id,
            host_sequence=self.preflight.sequence,
            command_status=0,
        )
        self.assertFalse(positive.matched)
        self.assertEqual(self.orchestrator.current, self.preflight)

        negative = self.orchestrator.observe_command_result(
            command_id=self.preflight.command_id,
            host_session_id=self.preflight.session_id,
            host_sequence=self.preflight.sequence,
            command_status=5,
        )
        self.assertTrue(negative.completed)
        self.assertEqual(negative.phase, "preflight")
        self.assertFalse(self.orchestrator.active)

    def test_timeout_and_disconnect_clear_the_whole_operation(self) -> None:
        self.orchestrator.begin(self.plan, now=0.0)
        timeout = self.orchestrator.expire(now=10.0)
        self.assertEqual((timeout.outcome, timeout.phase), ("timeout", "preflight"))
        self.assertFalse(self.orchestrator.active)

        self.orchestrator.begin(self.plan, now=20.0)
        disconnected = self.orchestrator.disconnect()
        self.assertEqual(
            (disconnected.outcome, disconnected.phase),
            ("disconnected", "preflight"),
        )
        self.assertFalse(self.orchestrator.active)


if __name__ == "__main__":
    unittest.main()
