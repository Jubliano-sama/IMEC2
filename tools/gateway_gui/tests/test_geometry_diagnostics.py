import math
import unittest
from unittest.mock import patch

from tools.gateway_gui.anchor_geometry import AnchorPairDistance
from tools.gateway_gui.anchor_geometry_visibility import (
    VISIBILITY_BRANCHING_TUNED_ALGORITHM, VISIBILITY_BRANCHING_TUNED_PARAMETERS,
    solve_visibility_branching_tuned,
)
from tools.gateway_gui.diagnostic_models import solve_geometry


def pair(a, b, distance, sigma=0.03):
    return AnchorPairDistance(a, b, distance, sigma)


class VisibilityGeometryTests(unittest.TestCase):
    def test_tuned_parameters_and_exact_sparse_geometry(self):
        self.assertEqual(
            VISIBILITY_BRANCHING_TUNED_PARAMETERS,
            {
                "iterations": 45, "beam_width": 32, "optimizer_seeds": 16,
                "radio_radius_m": 8.0, "missing_margin_m": 0.25,
                "missing_weight": 8.0, "missing_sigma_m": 0.75,
                "graph_upper_weight": 0.35, "graph_upper_factor": 1.0,
                "graph_upper_slack_m": 0.75, "graph_upper_sigma_m": 1.0,
                "final_visibility_weight": 1.0, "constrained_polish": True,
                "constrained_iterations": 55, "constrained_known_weight": 1.0,
            },
        )
        pairs = [pair("A", "B", 6), pair("A", "C", 6), pair("B", "C", 6), pair("B", "D", 6), pair("C", "D", 6)]
        result = solve_visibility_branching_tuned(pairs, missing_pairs=[("A", "D")])
        self.assertEqual(result.algorithm, VISIBILITY_BRANCHING_TUNED_ALGORITHM)
        self.assertLess(result.rmse_m, 1e-7)
        self.assertGreater(math.dist(result.positions_m["A"], result.positions_m["D"]), 8.0)

    def test_noisy_determinism_and_input_errors(self):
        pairs = [pair("A", "B", 4.01), pair("A", "C", 3.0), pair("B", "C", 5.01), pair("B", "D", 3.0), pair("C", "D", 4.0)]
        first = solve_visibility_branching_tuned(pairs, missing_pairs=[("A", "D")])
        second = solve_visibility_branching_tuned(pairs, missing_pairs=[("A", "D")])
        self.assertEqual(first.positions_m, second.positions_m)
        self.assertLess(first.rmse_m, 0.04)
        with self.assertRaisesRegex(ValueError, "disconnected"):
            solve_visibility_branching_tuned([pair("A", "B", 2), pair("C", "D", 2)])
        with self.assertRaisesRegex(ValueError, "both known and missing"):
            solve_visibility_branching_tuned([pair("A", "B", 2), pair("B", "C", 2)], missing_pairs=[("A", "B")])

    def test_visibility_failure_never_falls_back_to_spring(self):
        pairs = (pair("A", "B", 2), pair("B", "C", 2))
        with (
            patch("tools.gateway_gui.diagnostic_models.solve_visibility_branching_tuned", side_effect=RuntimeError("visibility failed")),
            patch("tools.gateway_gui.diagnostic_models.solve_anchor_layout") as spring,
        ):
            with self.assertRaisesRegex(RuntimeError, "visibility failed"):
                solve_geometry(pairs, solver="Visibility branching tuned")
        spring.assert_not_called()


if __name__ == "__main__":
    unittest.main()
