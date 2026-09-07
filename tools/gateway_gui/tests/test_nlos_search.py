"""Numerical and ownership invariants for temporary NLOS search losses."""

from itertools import combinations
import math
import unittest
from unittest.mock import patch

import numpy as np

from tools.gateway_gui.anchor_geometry import AnchorPairDistance, _anchor_ids, _preprocess_pairs
from tools.gateway_gui.anchor_geometry_nlos import (
    _OneSidedProblem, one_sided_residual, solve_nlos_one_sided_layout,
)
from tools.gateway_gui.anchor_geometry_seeds import SEED_AUTO, SEED_GRAPH_MDS
from tools.gateway_gui.experiments.shared_bias_probe import new_scene
from tools.gateway_gui.experiments.verify_nlos_recovery import drawing, score


def make_problem(*, fixed=None):
    points = {"A": (0., 0.), "B": (4., 0.), "C": (0., 3.),
              "D": (4., 3.), "E": (2., 1.), "F": (7., 2.)}
    offsets = (.35, .06, -.4, 9.2, -.08)
    pairs = [AnchorPairDistance(a, b, math.dist(points[a], points[b]) + offsets[i % len(offsets)], .05)
             for i, (a, b) in enumerate(combinations(points, 2)) if (a, b) != ("A", "E")]
    processed = _preprocess_pairs(pairs, min_sigma_m=.02, min_distance_m=.05)
    problem = _OneSidedProblem(
        _anchor_ids(processed), processed, frozenset({("A", "F")}), frozenset({("A", "E")}),
        neighbor_max_m=5., nonneighbor_min_m=3., interval_sigma_m=.75,
        plateau=16., distance_weight_power=1., bias_cap_m=8., fixed_positions_m=fixed,
    )
    params = problem.params_from_array(np.asarray([points[a] for a in problem.anchor_ids]))
    return problem, params


class NlosSearchTests(unittest.TestCase):
    def test_sparse_plans_and_explicit_seeds_skip_the_additional_search(self):
        scene = drawing(7000, 1.6, 3., "preserved_search")
        for pairs, seed in ((scene.pairs[:2 * len(scene.truth)], SEED_AUTO),
                            (scene.pairs, SEED_GRAPH_MDS)):
            with self.subTest(seed=seed), patch.object(
                _OneSidedProblem, "bias_tail_search",
                side_effect=AssertionError("Unexpected additional search"),
            ) as search:
                result = solve_nlos_one_sided_layout(
                    pairs, neighbor_pairs=scene.neighbor_pairs,
                    nonneighbor_pairs=scene.nonneighbor_pairs, seed=seed,
                )
                self.assertEqual(set(result.positions_m), set(scene.truth))
                search.assert_not_called()

    def test_dense_nlos_search_reduces_worst_anchor_deviation(self):
        scene = new_scene(651000, "unequal", "expanded")
        measured = {f"{pair.anchor_a_id}-{pair.anchor_b_id}": pair.distance_m for pair in scene.pairs}
        result = solve_nlos_one_sided_layout(
            scene.pairs, neighbor_pairs=scene.neighbor_pairs,
            nonneighbor_pairs=scene.nonneighbor_pairs,
            neighbor_max_m=max(20., max(measured.values()) + 1.),
        )
        # Truth is used only after solving, for a rigid fit without scale. The
        # previous search put an anchor 3.34 m away; this case retains a separate
        # reflection ambiguity, so it does not assert every anchor is correct.
        self.assertLess(score(scene, result.positions_m)["full_max_m"], 2.6)
        self.assertEqual(
            {f"{pair.anchor_a_id}-{pair.anchor_b_id}": pair.distance_m for pair in scene.pairs},
            measured,
        )
        processed = {f"{pair.anchor_a_id}-{pair.anchor_b_id}": pair.distance_m for pair in result.processed_pairs}
        self.assertEqual(set(processed), set(measured))
        for key, distance in measured.items():
            self.assertAlmostEqual(processed[key], distance, places=12)
        self.assertEqual(set(result.residuals_m), set(measured))
        for pair in scene.pairs:
            key = f"{pair.anchor_a_id}-{pair.anchor_b_id}"
            expected = math.dist(result.positions_m[pair.anchor_a_id], result.positions_m[pair.anchor_b_id])
            self.assertAlmostEqual(result.residuals_m[key], expected - measured[key], places=10)

    def test_optional_numerical_failure_retains_the_public_incumbent(self):
        scene = drawing(7000, 1.6, 3., "search_failure")

        def no_improvement(problem, starts, *, max_nfev):
            name, start = starts[0]
            return start, math.inf, name, 0

        options = dict(neighbor_pairs=scene.neighbor_pairs, nonneighbor_pairs=scene.nonneighbor_pairs)
        with patch.object(_OneSidedProblem, "bias_tail_search", no_improvement):
            baseline = solve_nlos_one_sided_layout(scene.pairs, **options)
        local = _OneSidedProblem.local_solve
        for error_type in (ValueError, FloatingPointError, np.linalg.LinAlgError):
            with self.subTest(error_type=error_type):
                failed_problems = []

                def fail_only_temporary_search(problem, start, *, max_nfev):
                    if problem._search_bias_tail_per_m:
                        failed_problems.append(problem)
                        raise error_type("synthetic temporary-search numerical failure")
                    return local(problem, start, max_nfev=max_nfev)

                with patch.object(_OneSidedProblem, "local_solve", fail_only_temporary_search):
                    result = solve_nlos_one_sided_layout(scene.pairs, **options)
                self.assertEqual(len(failed_problems), 1)
                problem = failed_problems[0]
                self.assertEqual(problem._search_bias_tail_per_m, 0.)
                self.assertEqual(result.positions_m, baseline.positions_m)
                self.assertEqual(result.energy, baseline.energy)
                params = problem.params_from_array(np.asarray([result.positions_m[a] for a in problem.anchor_ids]))
                self.assertAlmostEqual(result.energy, problem.objective(params), places=8)
                self.assertTrue(set(result.warnings) - set(baseline.warnings))

    def test_temporary_tail_gradient_matches_central_differences(self):
        # Cover both measurement signs, the three-sigma tail threshold, bias-cap
        # overflow, radio hinges, and fixed coordinates in the same objective.
        for fixed in (None, {"A": (0., 0.), "B": (4., 0.)}):
            with self.subTest(fixed=fixed):
                problem, params = make_problem(fixed=fixed)
                problem._search_bias_tail_per_m = .25
                analytic = problem._residual_jacobian(params)
                numerical = np.empty_like(analytic)
                for column in range(params.size):
                    offset = np.zeros_like(params)
                    offset[column] = 1e-6
                    numerical[:, column] = (problem.residual_terms(params + offset) -
                                            problem.residual_terms(params - offset)) / 2e-6
                np.testing.assert_allclose(analytic, numerical, rtol=2e-5, atol=2e-5)

    def test_search_restores_original_loss_after_local_or_reflection_failure(self):
        for failure in ("local", "reflection"):
            with self.subTest(failure=failure):
                problem, params = make_problem()
                terms = problem.residual_terms(params).copy()
                energy = problem.objective(params)

                def local(start, *, max_nfev):
                    self.assertEqual(problem._search_bias_tail_per_m, .25)
                    if failure == "local":
                        raise RuntimeError("synthetic optimizer failure")
                    return start, problem.objective(start)

                with patch.object(problem, "local_solve", side_effect=local), \
                        patch.object(problem, "reflection_search", side_effect=RuntimeError("reflection failure")):
                    with self.assertRaises(RuntimeError):
                        problem.bias_tail_search([("supplied", params)], max_nfev=10)
                self.assertEqual(problem._search_bias_tail_per_m, 0.)
                np.testing.assert_array_equal(problem.residual_terms(params), terms)
                self.assertEqual(problem.objective(params), energy)

    def test_successful_search_restores_original_loss_before_final_polish(self):
        problem, params = make_problem()
        original_terms = problem.residual_terms(params).copy()
        original_energy = problem.objective(params)
        seen_tails = []

        def local(start, *, max_nfev):
            seen_tails.append(problem._search_bias_tail_per_m)
            return start, problem.objective(start)

        def reflect(start, value):
            self.assertEqual(problem._search_bias_tail_per_m, .25)
            return start, value, 0

        with patch.object(problem, "local_solve", side_effect=local), \
                patch.object(problem, "reflection_search", side_effect=reflect):
            solved, energy, seed, hops = problem.bias_tail_search([("supplied", params)], max_nfev=10)
        self.assertEqual(seen_tails, [.25, 0.])
        self.assertEqual((seed, hops), ("supplied", 0))
        self.assertEqual(energy, original_energy)
        np.testing.assert_array_equal(problem.residual_terms(solved), original_terms)

    def test_actual_temporary_search_preserves_hard_locks_and_final_objective(self):
        fixed = {"A": (0., 0.), "B": (4., 0.), "C": (0., 3.)}
        problem, params = make_problem(fixed=fixed)
        reflect = problem.reflection_search

        def bounded_reflection(start, value):
            return reflect(start, value, max_sweeps=1, nearest_count=3, max_nfev=30)

        with patch.object(problem, "reflection_search", side_effect=bounded_reflection):
            solved, energy, _seed, _hops = problem.bias_tail_search([("supplied", params)], max_nfev=30)
        self.assertEqual(problem._search_bias_tail_per_m, 0.)
        self.assertEqual({a: problem.positions(solved)[a] for a in fixed}, fixed)
        points = problem.positions_array(solved)
        measured = problem.sqrt_weights * one_sided_residual(
            np.linalg.norm(points[problem.pair_a] - points[problem.pair_b], axis=1) - problem.measured,
            problem.sigma, plateau=16., bias_cap_m=8.,
        )
        neighbor = np.maximum(0., np.linalg.norm(points[problem.neighbor_a] -
                                                points[problem.neighbor_b], axis=1) - 5.) / .75
        nonneighbor = np.maximum(0., 3. - np.linalg.norm(points[problem.nonneighbor_a] -
                                                       points[problem.nonneighbor_b], axis=1)) / .75
        expected = float(measured @ measured + neighbor @ neighbor + nonneighbor @ nonneighbor)
        self.assertAlmostEqual(energy, expected, places=10)


if __name__ == "__main__":
    unittest.main()
