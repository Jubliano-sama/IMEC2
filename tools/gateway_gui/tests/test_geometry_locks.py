"""Hard constraints must survive every solver, seed family, and final frame."""

from dataclasses import replace
from itertools import combinations
import math
import unittest

from tools.gateway_gui.anchor_geometry import AnchorPairDistance, pair_residuals
from tools.gateway_gui.anchor_geometry_connectivity import CONNECTIVITY_INTERVAL_ALGORITHM
from tools.gateway_gui.anchor_geometry_nlos import NLOS_ONE_SIDED_ALGORITHM
from tools.gateway_gui.anchor_geometry_seeds import GEOMETRY_SEEDS, SEED_CURRENT
from tools.gateway_gui.anchor_geometry_visibility import (
    VISIBILITY_BRANCHING_NEIGHBOR_AWARE_ALGORITHM,
    VISIBILITY_BRANCHING_TUNED_ALGORITHM,
)
from tools.gateway_gui.diagnostic_models import refine_geometry, solve_geometry


SOLVERS = (
    "Spring energy",
    NLOS_ONE_SIDED_ALGORITHM,
    CONNECTIVITY_INTERVAL_ALGORITHM,
    VISIBILITY_BRANCHING_TUNED_ALGORITHM,
    VISIBILITY_BRANCHING_NEIGHBOR_AWARE_ALGORITHM,
)
TRUTH = {"A": (12.0, -7.0), "B": (12.0, -4.0), "C": (8.0, -7.0), "D": (9.0, -3.0)}
PAIRS = tuple(
    AnchorPairDistance(a, b, math.dist(TRUTH[a], TRUTH[b]))
    for a, b in combinations(TRUTH, 2)
)


class GeometryLockTests(unittest.TestCase):
    def assert_consistent_diagnostics(self, result):
        residuals = pair_residuals(result.positions_m, result.processed_pairs)
        self.assertEqual(result.residuals_m, residuals)
        self.assertAlmostEqual(result.rmse_m, math.sqrt(sum(r*r for r in residuals.values()) / len(residuals)))

    def test_all_solvers_and_seeds_fit_free_anchor_around_three_off_origin_locks(self):
        fixed = {key: TRUTH[key] for key in ("A", "B", "C")}
        start = {**TRUTH, "D": (10.0, -2.0)}
        for solver, seed in ((s, t) for s in SOLVERS for t in GEOMETRY_SEEDS):
            with self.subTest(solver=solver, seed=seed):
                result = solve_geometry(
                    PAIRS, solver=solver, seed=seed, current_positions_m=start,
                    fixed_positions_m=fixed, distance_weight_power=2,
                )
                for key, position in fixed.items():
                    self.assertEqual(result.positions_m[key], position)
                self.assertLess(math.dist(result.positions_m["D"], TRUTH["D"]), 1e-5)
                self.assert_consistent_diagnostics(result)
        self.assertEqual(start["D"], (10.0, -2.0))

    def test_one_and_two_locks_preserve_exact_coordinates_and_current_seed_frame(self):
        for solver in SOLVERS:
            for keys in (("C",), ("B", "D")):
                with self.subTest(solver=solver, keys=keys):
                    result = solve_geometry(
                        PAIRS, solver=solver, seed=SEED_CURRENT,
                        current_positions_m=TRUTH,
                        fixed_positions_m={key: TRUTH[key] for key in keys},
                    )
                    for key, position in TRUTH.items():
                        self.assertLess(math.dist(result.positions_m[key], position), 1e-6)

    def test_conflicting_ranges_cannot_move_locks_or_hide_their_residuals(self):
        pairs = (replace(PAIRS[0], distance_m=5.0), *PAIRS[1:])
        fixed = {key: TRUTH[key] for key in ("A", "B", "C")}
        for solver in SOLVERS:
            with self.subTest(solver=solver):
                result = solve_geometry(
                    pairs, solver=solver, seed=SEED_CURRENT,
                    current_positions_m={**TRUTH, "D": (10.0, -2.0)},
                    fixed_positions_m=fixed,
                )
                for key, position in fixed.items():
                    self.assertEqual(result.positions_m[key], position)
                self.assertAlmostEqual(result.residuals_m["A-B"], -2.0)
                self.assertGreater(result.rmse_m, 0.8)
                self.assertLess(math.dist(result.positions_m["D"], TRUTH["D"]), 1e-5)
                self.assert_consistent_diagnostics(result)

    def test_every_anchor_can_be_locked_even_when_ranges_and_radio_bounds_disagree(self):
        fixed = {**TRUTH, "A": (1e-13, -7.0)}
        for solver in SOLVERS:
            with self.subTest(solver=solver):
                result = solve_geometry(
                    PAIRS, solver=solver, seed=SEED_CURRENT, current_positions_m=TRUTH,
                    fixed_positions_m=fixed, neighbor_pairs=frozenset({("A", "B")}),
                    nonneighbor_min_m=1, neighbor_max_m=2,
                )
                self.assertEqual(result.positions_m, fixed)
                self.assert_consistent_diagnostics(result)
                self.assertGreater(result.max_residual_m, 1)
                if solver != "Spring energy":
                    self.assertTrue(any("neighbor interval" in warning for warning in result.warnings))

    def test_refinement_aligns_reflected_seed_and_preserves_all_fixed_coordinates(self):
        reflected = {key: (-x + 100, y - 50) for key, (x, y) in TRUTH.items()}
        fixed = {key: TRUTH[key] for key in ("A", "B", "C")}
        result = refine_geometry(PAIRS, reflected, fixed_positions_m=fixed)
        for key, position in TRUTH.items():
            self.assertLess(math.dist(result.positions_m[key], position), 1e-6)
        all_fixed = {**TRUTH, "A": (1e-13, -7.0)}
        result = refine_geometry(PAIRS, reflected, fixed_positions_m=all_fixed)
        self.assertEqual(result.positions_m, all_fixed)
        self.assert_consistent_diagnostics(result)

    def test_unknown_and_nonfinite_locks_are_rejected(self):
        for fixed in ({"missing": (1.0, 2.0)}, {"A": (float("nan"), 2.0)}, {"B": (1.0, float("inf"))}):
            for solver in SOLVERS:
                with self.subTest(solver=solver, fixed=fixed), self.assertRaisesRegex(ValueError, "Locked anchor"):
                    solve_geometry(PAIRS, solver=solver, seed=SEED_CURRENT, current_positions_m=TRUTH, fixed_positions_m=fixed)
            with self.assertRaisesRegex(ValueError, "Locked anchor"):
                refine_geometry(PAIRS, TRUTH, fixed_positions_m=fixed)
