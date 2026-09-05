"""Boundary checks for the research-only correlated-path experiment."""

from dataclasses import replace
from itertools import combinations
import math
import unittest

import numpy as np

from tools.gateway_gui.anchor_geometry import AnchorPairDistance, _anchor_ids, _preprocess_pairs
from tools.gateway_gui.anchor_geometry_nlos import _OneSidedProblem
from tools.gateway_gui.experiments.correlated_nlos_probe import CorrelatedProblem


def fixture(*, second_bias=2.0, mutual=True):
    points = {"A": (0., 0.), "B": (8., 0.), "C": (8., 1.), "D": (0., 3.), "E": (-3., 0.)}
    pairs = [AnchorPairDistance(a, b, math.dist(points[a], points[b])) for a, b in combinations(points, 2)]
    biases = {("A", "B"): 2.0, ("A", "C"): second_bias}
    pairs = [replace(p, distance_m=p.distance_m + biases.get((p.anchor_a_id, p.anchor_b_id), 0))
             for p in pairs if mutual or (p.anchor_a_id, p.anchor_b_id) != ("B", "C")]
    processed = _preprocess_pairs(pairs, min_sigma_m=0.02, min_distance_m=0.05)
    problem = CorrelatedProblem(_anchor_ids(processed), processed, frozenset(), frozenset(),
                                neighbor_max_m=15, nonneighbor_min_m=7, interval_sigma_m=.75,
                                plateau=16, distance_weight_power=1, bias_cap_m=8)
    params = problem.params_from_array(np.array(list(points.values())))
    problem.freeze_groups(params, grouped=True, guarded=True)
    return problem, params


class CorrelatedPathProbeTests(unittest.TestCase):
    def test_only_coherent_excess_with_observed_proximity_gets_discount(self):
        problem, _ = fixture()
        np.testing.assert_allclose(problem.discount[:2], [.5, .5])
        np.testing.assert_array_equal(problem.discount[2:], np.ones(8))
        for options in ({"second_bias": 0}, {"mutual": False}):
            with self.subTest(options=options):
                problem, _ = fixture(**options)
                self.assertFalse(problem.has_discount)

    def test_frozen_groups_do_not_change_when_trial_layout_moves(self):
        problem, params = fixture()
        discounts = problem.discount.copy()
        initial = problem.residual_terms(params)
        baseline = _OneSidedProblem.residual_terms(problem, params)
        self.assertLess(float(initial @ initial), float(baseline @ baseline))
        problem.residual_terms(params + 5)
        np.testing.assert_array_equal(problem.discount, discounts)
        np.testing.assert_array_equal(problem.residual_terms(params), initial)

    def test_discount_preserves_positive_residuals_small_noise_and_bias_cap_growth(self):
        problem, params = fixture()
        for excess in (-2., .01, .1, 8., 9.):
            trial = params.copy()
            trial[0] = problem.measured[0] - excess  # B lies on the x axis.
            changed = problem.residual_terms(trial)[0]
            base = _OneSidedProblem.residual_terms(problem, trial)[0]
            if excess <= .1:
                self.assertEqual(changed, base)
            else:
                reduction = (base**2 - changed**2) / problem.sqrt_weights[0]**2
                self.assertAlmostEqual(reduction, 8., places=6)


if __name__ == "__main__":
    unittest.main()
