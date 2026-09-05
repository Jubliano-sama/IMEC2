from dataclasses import replace
from itertools import combinations
import math
import unittest
from unittest.mock import patch

import numpy as np

from tools.gateway_gui.anchor_geometry import AnchorPairDistance, _anchor_ids, _preprocess_pairs
from tools.gateway_gui.anchor_geometry_nlos import (
    NLOS_ONE_SIDED_ALGORITHM, _OneSidedProblem, one_sided_residual, solve_nlos_one_sided_layout,
)
from tools.gateway_gui.anchor_geometry_connectivity import solve_connectivity_interval_layout
from tools.gateway_gui.anchor_geometry_seeds import SEED_CURRENT, SEED_SPRING
from tools.gateway_gui.diagnostic_models import solve_geometry
from tools.gateway_gui.experiments.verify_nlos_recovery import drawing
from tools.gateway_gui.experiments.recovered_nlos_probe import drawing_orientation_flip


def problem(pairs, **overrides):
    processed = _preprocess_pairs(pairs, min_sigma_m=0.02, min_distance_m=0.05)
    settings = dict(neighbor_max_m=15, nonneighbor_min_m=7, interval_sigma_m=0.75,
                    plateau=16, distance_weight_power=1, bias_cap_m=8)
    settings.update(overrides)
    return _OneSidedProblem(_anchor_ids(processed), processed, frozenset(), frozenset(), **settings)


class NlosSolverTests(unittest.TestCase):
    def test_exact_derivatives_accelerate_only_graphs_beyond_degree_four_capacity(self):
        truth = {f'A{i}': (i % 4 * 2., i // 4 * 3.) for i in range(8)}
        pairs = [AnchorPairDistance(a, b, math.dist(truth[a], truth[b])) for a, b in combinations(truth, 2)]
        self.assertFalse(problem(pairs[:16])._use_analytic_jacobian)
        self.assertTrue(problem(pairs[:17])._use_analytic_jacobian)

    def test_dense_exact_derivatives_preserve_hard_locks(self):
        truth = {f'A{i}': (10. + i % 4 * 2., -3. + i // 4 * 3.) for i in range(8)}
        pairs = [AnchorPairDistance(a, b, math.dist(truth[a], truth[b])) for a, b in combinations(truth, 2)]
        fixed = {a: truth[a] for a in ('A0', 'A1', 'A4')}
        result = solve_nlos_one_sided_layout(pairs, neighbor_pairs=(), nonneighbor_pairs=(), fixed_positions_m=fixed)
        self.assertEqual({a: result.positions_m[a] for a in fixed}, fixed)
        self.assertLess(result.max_residual_m, 1e-5)

    def test_loss_preserves_small_noise_on_both_sides_and_saturates_only_positive_bias(self):
        residuals = np.array([-9, -5, -1, -1e-10, 0, 1e-10, 1, 5], dtype=float)
        terms = one_sided_residual(residuals, np.full(residuals.size, 0.05), plateau=16, bias_cap_m=8)
        np.testing.assert_allclose(terms, [-math.sqrt(416), -4, -4, -2e-9, 0, 2e-9, 20, 100], rtol=1e-8, atol=1e-12)

    def test_weight_power_changes_influence_without_widening_noise_tolerance(self):
        pairs = [AnchorPairDistance("A", "B", 1), AnchorPairDistance("A", "C", 4), AnchorPairDistance("B", "C", 4)]
        plain, weighted = problem(pairs, distance_weight_power=0), problem(pairs, distance_weight_power=2)
        np.testing.assert_array_equal(plain.sigma, weighted.sigma)
        np.testing.assert_allclose(weighted.sqrt_weights, [4, 1, 1])
        with patch("tools.gateway_gui.diagnostic_models.solve_nlos_one_sided_layout") as implementation:
            solve_geometry(tuple(pairs), solver=NLOS_ONE_SIDED_ALGORITHM, distance_weight_power=2)
            self.assertEqual(implementation.call_args.args[0], tuple(pairs))
            self.assertEqual(implementation.call_args.kwargs["distance_weight_power"], 2)

    def test_reflection_search_escapes_a_flat_wrong_basin(self):
        truth = {"A": (0., 0.), "B": (4., 0.), "C": (2., -3.), "D": (1., 2.)}
        pairs = [AnchorPairDistance(a, b, math.dist(truth[a], truth[b])) for a, b in combinations(truth, 2)]
        p = problem(pairs, distance_weight_power=0)
        start = p.params_from_array(np.array([truth["A"], truth["B"], truth["C"], (1., -2.)]))
        params, value = p.local_solve(start, max_nfev=300)
        self.assertGreater(value, 10)
        params, value, accepted = p.reflection_search(params, value)
        self.assertLess(value, 1e-6)
        self.assertGreater(accepted, 0)
        self.assertAlmostEqual(math.dist(p.positions(params)["C"], p.positions(params)["D"]), math.sqrt(26), places=5)

    def test_recovered_search_resolves_sketch_replay_and_unseen_noise(self):
        # Includes both seeds for which the saved plan's changed search failed.
        for seed in (1001, 1009, 7003):
            with self.subTest(seed=seed):
                scene = drawing(seed, 1.6, 3.0, "test")
                baseline = solve_connectivity_interval_layout(scene.pairs, neighbor_pairs=scene.neighbor_pairs, nonneighbor_pairs=scene.nonneighbor_pairs)
                solved = solve_nlos_one_sided_layout(scene.pairs, neighbor_pairs=scene.neighbor_pairs, nonneighbor_pairs=scene.nonneighbor_pairs)
                self.assertEqual(drawing_orientation_flip(scene.truth, baseline.positions_m), 1)
                self.assertEqual(drawing_orientation_flip(scene.truth, solved.positions_m), 0)
                self.assertTrue(any("possible NLOS" in warning for warning in solved.warnings))

    def test_exact_data_is_reproduced_and_deterministic(self):
        truth = {"A": (0., 0.), "B": (6., 0.), "C": (6., 5.), "D": (0., 5.), "E": (3., 2.)}
        pairs = [AnchorPairDistance(a, b, math.dist(truth[a], truth[b])) for a, b in combinations(truth, 2)]
        first = solve_nlos_one_sided_layout(pairs, neighbor_pairs=(), nonneighbor_pairs=())
        second = solve_nlos_one_sided_layout(pairs, neighbor_pairs=(), nonneighbor_pairs=())
        self.assertEqual(first.positions_m, second.positions_m)
        self.assertLess(first.max_residual_m, 1e-5)
        self.assertFalse(any("NLOS" in warning or "shape dimensions" in warning for warning in first.warnings))

    def test_relaxing_edges_reports_lost_geometric_support(self):
        truth = {"A": (0., 0.), "B": (3., 0.), "C": (0., 4.)}
        pairs = [AnchorPairDistance("A", "B", 3), AnchorPairDistance("A", "C", 4), AnchorPairDistance("B", "C", 7)]
        p = problem(pairs)
        start = p.params_from_array(np.array(list(truth.values())))
        self.assertEqual(p.near_fit_rank(start), (2, 3))
        result = solve_nlos_one_sided_layout(pairs, neighbor_pairs=(), nonneighbor_pairs=(), seed=SEED_CURRENT, current_positions_m=truth)
        # Even if the solve moves to fit every range, its diagnostics must
        # agree with the returned geometry rather than with the original seed.
        for pair in pairs:
            key = f"{pair.anchor_a_id}-{pair.anchor_b_id}"
            self.assertAlmostEqual(result.residuals_m[key], math.dist(result.positions_m[pair.anchor_a_id], result.positions_m[pair.anchor_b_id]) - pair.distance_m)

    def test_validation_rejects_nonfinite_controls_and_contradictory_radio_evidence(self):
        pairs = [AnchorPairDistance("A", "B", 3), AnchorPairDistance("A", "C", 4), AnchorPairDistance("B", "C", 5)]
        for name in ("plateau", "bias_cap_m", "interval_sigma_m", "distance_weight_power", "neighbor_max_m", "nonneighbor_min_m"):
            for value in (float("nan"), float("inf"), -1):
                with self.subTest(name=name, value=value), self.assertRaises(ValueError):
                    solve_nlos_one_sided_layout(pairs, neighbor_pairs=(), nonneighbor_pairs=(), **{name: value})
        for neighbors, nonneighbors in (({("A", "B")}, {("A", "B")}), (set(), {("A", "C")})):
            with self.assertRaises(ValueError):
                solve_nlos_one_sided_layout(pairs, neighbor_pairs=neighbors, nonneighbor_pairs=nonneighbors)

    def test_dispatch_preserves_locks_and_observed_unmeasured_neighbors(self):
        pairs = (AnchorPairDistance("A", "B", 3), AnchorPairDistance("A", "C", 4))
        neighbors = frozenset({("A", "B"), ("B", "C")})
        fixed = {"A": (10., -3.)}
        with patch("tools.gateway_gui.diagnostic_models.solve_nlos_one_sided_layout") as implementation:
            solve_geometry(pairs, solver=NLOS_ONE_SIDED_ALGORITHM, neighbor_pairs=neighbors,
                           fixed_positions_m=fixed, seed=SEED_SPRING, nonneighbor_min_m=5, neighbor_max_m=19)
            arguments = implementation.call_args.kwargs
            self.assertEqual(arguments["fixed_positions_m"], fixed)
            self.assertEqual(arguments["neighbor_pairs"], neighbors)
            self.assertEqual(arguments["distance_weight_power"], 1)
            self.assertEqual((arguments["nonneighbor_min_m"], arguments["neighbor_max_m"]), (5, 19))
