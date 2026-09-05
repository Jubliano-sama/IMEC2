import math
import unittest
from unittest.mock import patch

from tools.gateway_gui.anchor_geometry import (
    AnchorPairDistance,
    DISTANCE_ONLY_REFINEMENT_ALGORITHM,
    MANUALLY_EDITED_LAYOUT_ALGORITHM,
    evaluate_anchor_layout,
    pair_residuals,
    refine_anchor_layout_from_seed,
)
from tools.gateway_gui.anchor_geometry_visibility import (
    VISIBILITY_BRANCHING_NEIGHBOR_AWARE_ALGORITHM,
    VISIBILITY_BRANCHING_TUNED_ALGORITHM, VISIBILITY_BRANCHING_TUNED_PARAMETERS,
    solve_visibility_branching_neighbor_aware_tuned,
    solve_visibility_branching_tuned,
)
from tools.gateway_gui.anchor_geometry_connectivity import (
    CONNECTIVITY_INTERVAL_ALGORITHM,
    SEED_AUTO,
    SEED_CURRENT,
    SEED_SPRING,
    solve_connectivity_interval_layout,
)
from tools.gateway_gui.diagnostic_models import (
    select_nearest_anchor_ranges,
    solve_geometry,
)
from tools.gateway_gui.anchor_geometry_weights import (
    distance_weighted_pairs, parse_distance_weight_power,
)


def pair(a, b, distance, sigma=0.03):
    return AnchorPairDistance(a, b, distance, sigma)


class VisibilityGeometryTests(unittest.TestCase):
    def test_distance_weighting_preserves_ranges_and_scales_inverse_variance(self):
        pairs = (pair("A", "B", 2, 0.05), pair("A", "C", 4, 0.10))
        self.assertIs(distance_weighted_pairs(pairs, 0), pairs)
        for power, relative_weight in ((1, 0.5), (2, 0.25), (4, 0.0625)):
            weighted = distance_weighted_pairs(pairs, power)
            self.assertEqual(weighted[0], pairs[0])
            self.assertAlmostEqual((pairs[1].sigma_m / weighted[1].sigma_m) ** 2, relative_weight)
            self.assertEqual(weighted[1].distance_m, pairs[1].distance_m)
            self.assertEqual(weighted[1].source, pairs[1].source)
        self.assertEqual(pairs[1].sigma_m, 0.10)
        disabled = AnchorPairDistance("X", "Y", 0.1, enabled=False)
        self.assertEqual(distance_weighted_pairs((disabled, *pairs), 2)[1:], distance_weighted_pairs(pairs, 2))
        for invalid in ("", "bad", "nan", "inf", "-1", "4.1"):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                parse_distance_weight_power(invalid)

    def test_distance_weights_reach_solver_without_changing_neighbor_evidence(self):
        pairs = (pair("A", "B", 2), pair("A", "C", 4))
        neighbors = frozenset({("A", "B"), ("B", "C")})
        for solver, function in (
            (CONNECTIVITY_INTERVAL_ALGORITHM, "solve_connectivity_interval_layout"),
            (VISIBILITY_BRANCHING_TUNED_ALGORITHM, "solve_visibility_branching_tuned"),
            (VISIBILITY_BRANCHING_NEIGHBOR_AWARE_ALGORITHM, "solve_visibility_branching_neighbor_aware_tuned"),
        ):
            with self.subTest(solver=solver), patch(f"tools.gateway_gui.diagnostic_models.{function}") as implementation:
                solve_geometry(pairs, solver=solver, neighbor_pairs=neighbors, distance_weight_power=2)
                self.assertEqual(implementation.call_args.args[0], distance_weighted_pairs(pairs, 2))
                self.assertEqual(implementation.call_args.kwargs["neighbor_pairs"], neighbors)

    def test_manual_layout_evaluation_keeps_coordinates_and_recomputes_fit(self):
        pairs = (
            pair("A", "B", 3.0),
            pair("A", "C", 4.0),
            pair("B", "C", 5.0),
        )
        positions = {"A": (0.0, 0.0), "B": (3.0, 0.0), "C": (0.5, 4.0)}

        layout = evaluate_anchor_layout(pairs, positions)

        self.assertEqual(layout.algorithm, MANUALLY_EDITED_LAYOUT_ALGORITHM)
        self.assertEqual(layout.positions_m, positions)
        self.assertGreater(layout.rmse_m, 0.0)
        self.assertEqual(set(layout.residuals_m), {"A-B", "A-C", "B-C"})

    def test_nearest_range_filter_keeps_union_of_each_anchor_choice(self):
        pairs = tuple(
            pair(first, second, distance)
            for first, second, distance in (
                ("A", "B", 1.0),
                ("A", "C", 2.0),
                ("A", "D", 3.0),
                ("B", "C", 4.0),
                ("B", "D", 5.0),
                ("C", "D", 6.0),
            )
        )

        selected = select_nearest_anchor_ranges(pairs, 1)

        self.assertEqual(
            tuple((item.anchor_a_id, item.anchor_b_id) for item in selected),
            (("A", "B"), ("A", "C"), ("A", "D")),
        )
        self.assertIs(select_nearest_anchor_ranges(pairs, 0), pairs)
        with self.assertRaisesRegex(ValueError, "zero or greater"):
            select_nearest_anchor_ranges(pairs, -1)

    def test_selected_radio_interval_reaches_connectivity_solver(self):
        expected = object()
        with patch(
            "tools.gateway_gui.diagnostic_models.solve_connectivity_interval_layout",
            return_value=expected,
        ) as connectivity:
            result = solve_geometry(
                (),
                solver=CONNECTIVITY_INTERVAL_ALGORITHM,
                nonneighbor_min_m=5.5,
                neighbor_max_m=22.5,
            )

        self.assertIs(result, expected)
        self.assertEqual(connectivity.call_args.kwargs["nonneighbor_min_m"], 5.5)
        self.assertEqual(connectivity.call_args.kwargs["neighbor_max_m"], 22.5)

    def test_seed_and_radio_interval_reach_visibility_solver(self):
        expected = object()
        current = {"A": (0.0, 0.0), "B": (1.0, 0.0)}
        with patch(
            "tools.gateway_gui.diagnostic_models.solve_visibility_branching_tuned",
            return_value=expected,
        ) as visibility:
            result = solve_geometry(
                (),
                solver=VISIBILITY_BRANCHING_TUNED_ALGORITHM,
                seed=SEED_CURRENT,
                neighbor_pairs=frozenset({("A", "B")}),
                nonneighbor_pairs=frozenset({("A", "C")}),
                current_positions_m=current,
                nonneighbor_min_m=5.5,
                neighbor_max_m=22.5,
            )

        self.assertIs(result, expected)
        self.assertEqual(visibility.call_args.kwargs["seed"], SEED_CURRENT)
        self.assertEqual(
            visibility.call_args.kwargs["current_positions_m"], current
        )
        self.assertEqual(visibility.call_args.kwargs["nonneighbor_min_m"], 5.5)
        self.assertEqual(visibility.call_args.kwargs["neighbor_max_m"], 22.5)
        self.assertEqual(
            visibility.call_args.kwargs["neighbor_pairs"],
            frozenset({("A", "B")}),
        )

    def test_neighbor_interval_default_accepts_observed_fourteen_meter_link(self):
        positions = {
            "A": (0.0, 3.0),
            "B": (0.0, 0.0),
            "C": (14.0, 3.0),
            "D": (14.0, 0.0),
        }
        pairs = [
            pair(first, second, math.dist(positions[first], positions[second]))
            for first, second in (
                ("A", "B"),
                ("B", "C"),
                ("A", "D"),
                ("B", "D"),
                ("C", "D"),
            )
        ]

        result = solve_connectivity_interval_layout(
            pairs,
            neighbor_pairs={("A", "C")},
            nonneighbor_pairs=set(),
            seed=SEED_SPRING,
        )

        self.assertAlmostEqual(
            math.dist(result.positions_m["A"], result.positions_m["C"]),
            14.0,
            places=5,
        )
        self.assertFalse(
            any("neighbor interval" in warning for warning in result.warnings)
        )
        self.assertIn("(7-15 m)", result.algorithm)

    def test_neighbor_interval_solver_uses_graph_to_reject_folded_layout(self):
        root_two = math.sqrt(2.0)
        pairs = [
            pair("A", "B", 5.0),
            pair("A", "C", 5.0),
            pair("B", "C", 5.0 * root_two),
            pair("B", "D", 5.0),
            pair("C", "D", 5.0),
            pair("C", "E", 5.0 * root_two),
            pair("D", "E", 5.0),
        ]
        neighbors = {
            (item.anchor_a_id, item.anchor_b_id)
            for item in pairs
        }

        result = solve_connectivity_interval_layout(
            pairs,
            neighbor_pairs=neighbors,
            nonneighbor_pairs={("A", "E")},
            seed=SEED_AUTO,
        )

        self.assertTrue(result.algorithm.startswith(CONNECTIVITY_INTERVAL_ALGORITHM))
        self.assertLess(result.rmse_m, 1e-5)
        self.assertGreaterEqual(
            math.dist(result.positions_m["A"], result.positions_m["E"]),
            7.0 - 1e-4,
        )

    def test_distance_only_refinement_uses_current_layout_as_its_seed(self):
        pairs = [
            pair("A", "B", 2.0),
            pair("A", "C", 2.0),
            pair("B", "C", math.sqrt(8.0)),
            pair("B", "D", 2.0),
            pair("C", "D", 2.0),
        ]
        seed = {
            "A": (0.0, 0.0),
            "B": (2.2, 0.0),
            "C": (0.2, 2.3),
            "D": (2.5, 2.5),
        }
        initial_rmse = math.sqrt(
            sum(value * value for value in pair_residuals(seed, pairs).values())
            / len(pairs)
        )

        result = refine_anchor_layout_from_seed(pairs, seed)

        self.assertEqual(result.algorithm, DISTANCE_ONLY_REFINEMENT_ALGORITHM)
        self.assertLess(result.rmse_m, initial_rmse)
        self.assertLess(result.rmse_m, 1e-6)
        self.assertLess(math.dist(result.positions_m["A"], result.positions_m["D"]), 8.0)

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
        self.assertTrue(
            result.algorithm.startswith(VISIBILITY_BRANCHING_TUNED_ALGORITHM)
        )
        self.assertIn("(7-15 m)", result.algorithm)
        self.assertLess(result.rmse_m, 1e-7)
        self.assertGreaterEqual(
            math.dist(result.positions_m["A"], result.positions_m["D"]),
            7.0 - 1e-6,
        )

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

    def test_neighbor_aware_visibility_protects_unranged_positive_edge(self):
        root_eight = math.sqrt(8.0)
        pairs = [
            pair("A", "B", 2.0),
            pair("A", "C", 2.0),
            pair("B", "C", root_eight),
            pair("B", "D", 2.0),
            pair("C", "D", 2.0),
        ]

        result = solve_visibility_branching_neighbor_aware_tuned(
            pairs,
            missing_pairs={("A", "D")},
            neighbor_pairs={("A", "D")},
            seed=SEED_SPRING,
            nonneighbor_min_m=7.0,
            neighbor_max_m=15.0,
        )

        self.assertTrue(
            result.algorithm.startswith(
                VISIBILITY_BRANCHING_NEIGHBOR_AWARE_ALGORITHM
            )
        )
        self.assertLess(
            math.dist(result.positions_m["A"], result.positions_m["D"]),
            7.0,
        )
        self.assertLess(result.rmse_m, 1e-6)

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
