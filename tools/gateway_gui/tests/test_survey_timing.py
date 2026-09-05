from __future__ import annotations

import unittest
import json
from pathlib import Path
import tempfile

from tools.gateway_gui.survey_timing import (
    ROUTE_FULL_WAVE_MS,
    ROUTE_REFRESH_NOMINAL_MS,
    ROUTE_REFRESH_SCHEDULED_MS,
    ROUTE_REFRESH_TERMINAL_MARGIN_MS,
    ScheduledPhaseEstimate,
    PhaseTimingCalibration,
    survey_control_delivery_ms,
    survey_enumeration_phase_ms,
    survey_neighbor_phase_ms,
    survey_ranging_phase_ms,
    survey_response_lane_ms,
    survey_wave_stride_ms,
)


class SurveyTimingTests(unittest.TestCase):
    def test_success_offsets_survive_restart_and_keep_topology_baseline(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "survey-timing.json"
            calibration = PhaseTimingCalibration(path)
            calibration.observe("neighbors", 30_000, 20_000)
            restored = PhaseTimingCalibration(path)
            self.assertEqual(restored.duration_ms("neighbors", 30_000), 20_000)
            self.assertEqual(restored.duration_ms("neighbors", 40_000), 30_000)
            self.assertEqual(restored.duration_ms("routes", 23_000), 23_000)
            restored.observe("neighbors", 30_000, 24_000)
            self.assertEqual(restored.duration_ms("neighbors", 30_000), 21_400)

    def test_large_errors_and_different_topologies_keep_estimates_bounded(self) -> None:
        calibration = PhaseTimingCalibration()
        calibration.observe("ranging", 500_000, 1000)
        self.assertEqual(calibration.offsets_ms["ranging"], -300_000)
        self.assertEqual(calibration.duration_ms("ranging", 10_000), 1000)
        calibration.observe("routes", 23_000, 10_000_000)
        self.assertEqual(calibration.duration_ms("routes", 23_000), 46_000)
        self.assertFalse(calibration.observe("neighbors", 30_000, -1000))
        self.assertNotIn("neighbors", calibration.offsets_ms)

    def test_bad_persisted_entries_are_ignored_without_losing_valid_phases(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "timing.json"
            path.write_text(json.dumps({"version": 1, "offsets_ms": {
                "routes": -2000, "neighbors": True, "ranging": float("nan"),
                "enumeration": 400_000, "unknown": 3000,
            }}))
            self.assertEqual(PhaseTimingCalibration(path).offsets_ms, {"routes": -2000})
            for invalid in ("{", "[]", '{"version":1,"offsets_ms":null}'):
                path.write_text(invalid)
                self.assertEqual(PhaseTimingCalibration(path).offsets_ms, {})

    def test_unwritable_storage_keeps_session_calibration_and_existing_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            occupied = Path(directory) / "not-a-directory"
            occupied.write_text("keep this")
            calibration = PhaseTimingCalibration(occupied / "timing.json")
            self.assertTrue(calibration.observe("routes", 23_000, 20_000))
            self.assertEqual(calibration.duration_ms("routes", 23_000), 20_000)
            self.assertEqual(occupied.read_text(), "keep this")

    def test_route_refresh_countdown_includes_terminal_margin(self) -> None:
        self.assertEqual(ROUTE_REFRESH_NOMINAL_MS, 20_000)
        self.assertEqual(ROUTE_REFRESH_TERMINAL_MARGIN_MS, 3_000)
        self.assertEqual(ROUTE_REFRESH_SCHEDULED_MS, 23_000)
        self.assertEqual(ROUTE_FULL_WAVE_MS, 45_150)

    def test_prearmed_enumeration_counts_only_overlap_tail_and_table(self) -> None:
        self.assertEqual(survey_enumeration_phase_ms(1), 37_290)
        self.assertEqual(survey_enumeration_phase_ms(8), 41_070)

    def test_neighbor_estimate_uses_depth_and_sparse_slot_span(self) -> None:
        self.assertEqual(survey_control_delivery_ms(3), 3_830)
        self.assertEqual(survey_response_lane_ms(3), 5_250)
        self.assertEqual(survey_neighbor_phase_ms(3, 3), 22_120)

    def test_ranging_estimate_uses_accepted_wave_count_and_drain(self) -> None:
        self.assertEqual(survey_wave_stride_ms(3), 5_950)
        self.assertEqual(survey_ranging_phase_ms(3, 2), 27_630)

    def test_phase_snapshot_saturates_without_becoming_a_deadline(self) -> None:
        estimate = ScheduledPhaseEstimate("routes", "Refresh routes", 10.0, 1_000)

        halfway = estimate.snapshot(10.5)
        self.assertEqual(halfway.remaining_ms, 500)
        self.assertEqual(halfway.fraction, 0.5)
        self.assertFalse(halfway.elapsed)

        late = estimate.snapshot(12.0)
        self.assertEqual(late.remaining_ms, 0)
        self.assertEqual(late.fraction, 1.0)
        self.assertTrue(late.elapsed)

    def test_invalid_topology_inputs_fail_closed(self) -> None:
        for depth in (0, 9):
            with self.subTest(depth=depth), self.assertRaises(ValueError):
                survey_response_lane_ms(depth)
        for slot_span in (0, 51):
            with self.subTest(slot_span=slot_span), self.assertRaises(ValueError):
                survey_neighbor_phase_ms(1, slot_span)


if __name__ == "__main__":
    unittest.main()
