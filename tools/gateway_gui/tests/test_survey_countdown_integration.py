from __future__ import annotations

from dataclasses import replace
import unittest
from unittest.mock import Mock, patch

from tools.gateway_gui.app import GatewayGui
from tools.gateway_gui.command_telemetry import GatewayCommandRequestTracker
from tools.gateway_gui.protocol import (
    CMD_FORCE_REDISCOVERY, CMD_SURVEY_START, CMD_SURVEY_PLAN,
    SURVEY_TERMINAL_ABORTED,
)
from tools.gateway_gui.tests.test_survey_app_integration import gui_model, neighbor_packet
from tools.gateway_gui.tests.test_survey_runtime import (
    command_event, enumerate_three, neighbor_event, plan_event, result_event,
)


class SurveyCountdownIntegrationTests(unittest.TestCase):
    def ranging_gui(self) -> GatewayGui:
        gui = gui_model()
        gui.root = Mock()
        gui._append_log = Mock()
        gui._schedule_survey_geometry_solve = Mock()
        gui._survey_auto_all = False
        enumerate_three(gui.survey_model)
        gui.survey_model.note_command_accepted(CMD_SURVEY_START)
        gui._set_scheduled_phase_estimate("neighbors", "Neighbors", 30_000, now=100)
        self.deliver(gui, neighbor_event(), 120)
        self.assertIsNone(gui._scheduled_phase_estimate)
        self.assertEqual(gui._phase_calibration().offsets_ms["neighbors"], -10_000)
        gui.survey_model.set_requested_pairs(((0, 1), (0, 2), (1, 2)))
        gui.survey_model.note_command_accepted(CMD_SURVEY_PLAN)
        self.deliver(gui, plan_event(), 125)
        return gui

    @staticmethod
    def deliver(gui, event, now):
        with patch("tools.gateway_gui.app.decode_survey_event", return_value=event):
            return gui._observe_survey_event_packet(neighbor_packet(), received_at=now)

    def test_all_results_stop_countdown_before_terminal_and_calibrate_only_once(self):
        gui = self.ranging_gui()
        estimate = gui._scheduled_phase_estimate
        self.deliver(gui, replace(result_event(terminal=False), range_results=(
            result_event(terminal=False).range_results[0],
        )), 130)
        self.assertIs(gui._scheduled_phase_estimate, estimate)
        self.deliver(gui, result_event(terminal=False), 140)
        self.assertIsNone(gui._scheduled_phase_estimate)
        self.assertTrue(gui.survey_model.active)
        learned = dict(gui._phase_calibration().offsets_ms)
        self.deliver(gui, result_event(terminal=True), 150)
        self.assertFalse(gui.survey_model.active)
        self.assertEqual(gui._phase_calibration().offsets_ms, learned)
        gui._set_scheduled_phase_estimate(
            "ranging", "Ranges", estimate.nominal_duration_ms, now=200,
        )
        self.assertEqual(gui._scheduled_phase_estimate.duration_ms, 15_000)

    def test_stale_generation_cannot_stop_or_calibrate_current_countdown(self):
        gui = self.ranging_gui()
        estimate = gui._scheduled_phase_estimate
        self.deliver(gui, result_event(terminal=True, generation=8), 140)
        self.assertIs(gui._scheduled_phase_estimate, estimate)
        self.assertNotIn("ranging", gui._phase_calibration().offsets_ms)

    def test_prior_batch_results_do_not_complete_next_batch_countdown(self):
        gui = self.ranging_gui()
        self.deliver(gui, result_event(terminal=False), 140)
        gui.survey_model.batch_plan_offset = 3
        gui._set_scheduled_phase_estimate("ranging", "Ranges", 30_000, now=150)
        current = gui._scheduled_phase_estimate
        learned = dict(gui._phase_calibration().offsets_ms)
        gui._finish_received_range_countdown(now=155)
        self.assertIs(gui._scheduled_phase_estimate, current)
        self.assertEqual(gui._phase_calibration().offsets_ms, learned)

    def test_abort_and_failed_results_stop_without_learning_success_duration(self):
        for abort in (True, False):
            with self.subTest(abort=abort):
                gui = self.ranging_gui()
                event = result_event(terminal=abort)
                if abort:
                    event = replace(event, status=SURVEY_TERMINAL_ABORTED)
                else:
                    event = replace(event, range_results=tuple(
                        replace(result, success_count=0, median_mm=None)
                        for result in event.range_results
                    ))
                self.deliver(gui, event, 140)
                self.assertIsNone(gui._scheduled_phase_estimate)
                self.assertNotIn("ranging", gui._phase_calibration().offsets_ms)

    def test_route_terminal_must_match_command_before_stopping_or_learning(self):
        gui = GatewayGui.__new__(GatewayGui)
        gui.command_request_tracker = GatewayCommandRequestTracker()
        gui.command_request_tracker.begin(3, 103, 3, now=100)
        gui._survey_chain_pending = False
        gui._survey_phase = "idle"
        gui._update_command_state = Mock()
        gui._set_scheduled_phase_estimate("routes", "Routes", 23_000, now=100)
        event = command_event(kind=3, stage=12, command_id=CMD_FORCE_REDISCOVERY, terminal=True)
        with patch("tools.gateway_gui.app.time.monotonic", return_value=120):
            gui._observe_operation_progress(replace(event, host_sequence=4))
            self.assertIsNotNone(gui._scheduled_phase_estimate)
            gui._observe_operation_progress(event)
        self.assertIsNone(gui._scheduled_phase_estimate)
        self.assertEqual(gui._phase_calibration().duration_ms("routes", 23_000), 20_000)


if __name__ == "__main__":
    unittest.main()
