from __future__ import annotations

import unittest

from tools.gateway_gui.survey_timing import (
    ROUTE_FULL_WAVE_MS,
    ROUTE_REFRESH_SCHEDULED_MS,
    ScheduledPhaseEstimate,
    survey_control_delivery_ms,
    survey_enumeration_phase_ms,
    survey_neighbor_phase_ms,
    survey_ranging_phase_ms,
    survey_response_lane_ms,
    survey_wave_stride_ms,
)


class SurveyTimingTests(unittest.TestCase):
    def test_route_refresh_releases_when_wave_reaches_depth_four(self) -> None:
        self.assertEqual(ROUTE_REFRESH_SCHEDULED_MS, 20_000)
        self.assertEqual(ROUTE_FULL_WAVE_MS, 45_150)

    def test_prearmed_enumeration_counts_only_overlap_tail_and_table(self) -> None:
        self.assertEqual(survey_enumeration_phase_ms(1), 40_290)
        self.assertEqual(survey_enumeration_phase_ms(8), 44_070)

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
