"""Protect benchmark evidence: inputs, solver boundary, metrics and failures."""

from dataclasses import replace
from itertools import combinations
import math
from types import SimpleNamespace
import unittest

import numpy as np

from tools.gateway_gui.anchor_geometry import AnchorPairDistance
from tools.gateway_gui.experiments.large_nlos_benchmark import (
    make_scene, run_scene, scene_record, score_positions, suite, summarize,
)


class LargeNlosBenchmarkTests(unittest.TestCase):
    def test_fixed_splits_cover_large_and_sparse_scenes_without_seed_overlap(self):
        pilot, holdout = suite(), suite("holdout")
        self.assertEqual(len(pilot), 12)
        self.assertEqual(len(holdout), 24)
        self.assertEqual({len(s.truth) for s in pilot}, {20, 35, 50})
        self.assertFalse({s.seed for s in pilot} & {s.seed for s in holdout})
        self.assertEqual(scene_record(pilot[0]), scene_record(suite()[0]))
        for scene in pilot + holdout:
            measured = {(p.anchor_a_id, p.anchor_b_id) for p in scene.pairs}
            self.assertTrue(measured <= scene.neighbor_pairs)
            self.assertFalse(scene.neighbor_pairs & scene.nonneighbor_pairs)
            self.assertEqual(len(scene.neighbor_pairs | scene.nonneighbor_pairs),
                             len(scene.truth) * (len(scene.truth) - 1) // 2)
            if scene.kind == "office_sparse":
                self.assertLessEqual(max(scene_record(scene)["degrees"].values()), 4)
            if scene.kind == "clean":
                self.assertFalse(scene.generated_bias)
            else:
                self.assertTrue(scene.generated_bias)
                self.assertTrue(all(0.3 <= b <= 5 for b in scene.generated_bias.values()))

    def test_missing_ranges_do_not_create_negative_contact_evidence_or_repair_edges(self):
        complete = make_scene(123, 20, omission_rate=0)
        omitted = make_scene(123, 20, omission_rate=1)
        self.assertTrue(complete.pairs)
        self.assertFalse(omitted.pairs)
        self.assertEqual(complete.neighbor_pairs, omitted.neighbor_pairs)
        self.assertEqual(complete.nonneighbor_pairs, omitted.nonneighbor_pairs)
        self.assertEqual(complete.truth, omitted.truth)

    def test_rigid_scoring_accepts_global_reflection_but_never_rescales(self):
        scene = make_scene(3, 20, "clean")
        transformed = {a: (7 - p[1], -4 - p[0]) for a, p in scene.truth.items()}
        metrics = score_positions(scene, transformed)
        self.assertLess(metrics["anchor_max_m"], 1e-10)
        self.assertEqual(metrics["orientation_reversed_triangles"], 0)
        self.assertEqual(metrics["orientation_collapsed_triangles"], 0)
        self.assertGreater(metrics["orientation_eligible_triangles"], 0)
        scaled = {a: (p[0] * 1.5, p[1] * 1.5) for a, p in scene.truth.items()}
        self.assertGreater(score_positions(scene, scaled)["scene_rms_m"], 1)
        with self.assertRaises(ValueError):
            score_positions(scene, dict(list(scene.truth.items())[1:]))
        with self.assertRaises(ValueError):
            score_positions(scene, {**scene.truth, "A00": (float("nan"), 0)})

    def test_local_reversals_require_complete_measured_triangle_and_clear_altitudes(self):
        truth = {"A": (-4., 0.), "B": (4., 0.), "C": (-4., -5.),
                 "D": (4., -5.), "E": (0., -8.), "F": (0., 3.)}
        pairs = tuple(AnchorPairDistance(a, b, math.dist(truth[a], truth[b]))
                      for a, b in combinations(truth, 2))
        scene = replace(make_scene(3, 20), truth=truth, pairs=pairs)
        flipped = {**truth, "F": (0., -3.)}
        metrics = score_positions(scene, flipped)
        self.assertGreater(metrics["orientation_reversed_triangles"], 0)
        self.assertIn("F", metrics["orientation_reversal_anchors"])
        triangle_pairs = tuple(p for p in pairs if {p.anchor_a_id, p.anchor_b_id} <= {"A", "B", "F"})
        triangle_scene = replace(scene, pairs=triangle_pairs)
        metrics = score_positions(triangle_scene, flipped)
        self.assertEqual(metrics["orientation_reversed_triangles"], 1)
        self.assertEqual(metrics["orientation_eligible_triangles"], 1)
        # Radio contact without the third measurement cannot supply a triangle.
        incomplete = replace(triangle_scene, pairs=triangle_pairs[1:])
        metrics = score_positions(incomplete, flipped)
        self.assertEqual(metrics["orientation_eligible_triangles"], 0)
        self.assertEqual(metrics["orientation_reversed_triangles"], 0)
        # A predicted collapse cannot remove a truth-eligible triangle or hide
        # a shallow wrong-side placement; zero area has no orientation sign.
        nearly_collinear = {**flipped, "F": (0., -0.1)}
        metrics = score_positions(triangle_scene, nearly_collinear)
        self.assertEqual(metrics["orientation_eligible_triangles"], 1)
        self.assertEqual(metrics["orientation_collapsed_triangles"], 1)
        self.assertEqual(metrics["orientation_reversed_triangles"], 1)
        collapsed = {**flipped, "F": (0., 0.)}
        metrics = score_positions(triangle_scene, collapsed)
        self.assertEqual(metrics["orientation_eligible_triangles"], 1)
        self.assertEqual(metrics["orientation_collapsed_triangles"], 1)
        self.assertEqual(metrics["orientation_reversed_triangles"], 0)
        # Near-collinearity in the truth remains excluded, regardless of estimate.
        metrics = score_positions(replace(triangle_scene, truth=nearly_collinear), flipped)
        self.assertEqual(metrics["orientation_eligible_triangles"], 0)

    def test_solver_only_sees_measured_inputs_and_failed_cases_stay_counted(self):
        scene = make_scene(17, 20)
        received = {}
        def solver(pairs, **kwargs):
            received.update(kwargs)
            self.assertEqual(pairs, scene.pairs)
            return SimpleNamespace(positions_m=scene.truth, energy=0.0, rmse_m=0.0, warnings=())
        good = run_scene(scene, solver)
        self.assertEqual(set(received), {"neighbor_pairs", "nonneighbor_pairs", "neighbor_max_m"})
        self.assertEqual(good["status"], "ok")
        def broken(*args, **kwargs):
            raise ValueError("disconnected")
        bad = run_scene(replace(scene, pairs=()), broken)
        summary = summarize([good, bad])
        self.assertEqual((summary["scenes"], summary["solved"], summary["failed"]), (2, 1, 1))
        self.assertEqual(summary["total_anchors"], 40)
        self.assertEqual(summary["scored_anchors"], 20)

    def test_pooled_percentiles_count_anchors_instead_of_averaging_case_percentiles(self):
        rows = []
        for errors in ([0.0] * 20, [3.0] * 50):
            rows.append({"status": "ok", "anchor_count": len(errors),
                         "anchor_errors_m": dict(enumerate(errors)), "anchor_max_m": max(errors),
                         "scene_rms_m": float(np.sqrt(np.mean(np.square(errors)))), "runtime_s": 1.0})
        summary = summarize(rows)
        self.assertEqual(summary["anchor_median_m"], 3)
        self.assertEqual(summary["anchors_over_2m"], 50)
        self.assertEqual(summary["scenes_over_2m"], 1)


if __name__ == "__main__":
    unittest.main()
