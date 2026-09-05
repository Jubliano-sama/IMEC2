"""Check experimental models independently of synthetic accuracy scores."""

from itertools import combinations
import math
import unittest

import numpy as np

from tools.gateway_gui.anchor_geometry import AnchorPairDistance, _anchor_ids, _preprocess_pairs
from tools.gateway_gui.anchor_geometry_nlos import _OneSidedProblem
from tools.gateway_gui.experiments.nlos_analytic_probe import AnalyticProblem
from tools.gateway_gui.experiments.nlos_metric_probe import metric_bounds
from tools.gateway_gui.experiments.shared_bias_probe import SharedBiasProblem, new_scene, planned_pairs
import random


def problem(cls, pairs, fixed=None, **options):
    processed = _preprocess_pairs(pairs, min_sigma_m=.02, min_distance_m=.05)
    return cls(_anchor_ids(processed), processed, frozenset(), frozenset(),
               neighbor_max_m=15, nonneighbor_min_m=7, interval_sigma_m=.75,
               plateau=16, distance_weight_power=0, bias_cap_m=8,
               fixed_positions_m=fixed, **options)


class NlosResearchTests(unittest.TestCase):
    def test_shared_bias_preserves_differences_and_rejects_disagreeing_offsets(self):
        truth = {"A": (0., 0.), "B": (8., 0.), "C": (8., 1.), "D": (0., 3.)}
        for extra in (2., 4.):
            pairs = [AnchorPairDistance(a, b, math.dist(truth[a], truth[b]) +
                     (2. if (a,b)==("A","B") else extra if (a,b)==("A","C") else 0.))
                     for a,b in combinations(truth,2)]
            p = problem(SharedBiasProblem,pairs)
            params = p.params_from_array(np.array(list(truth.values())))
            p.freeze_groups(params)
            self.assertTrue(any(set(g)=={0,1} for g in p.groups))
            ordinary = _OneSidedProblem.residual_terms(p,params)
            shared = p.residual_terms(params)
            if extra==2.:
                self.assertLess(shared @ shared, ordinary @ ordinary)
            else:
                np.testing.assert_allclose(shared,ordinary)
            # Every shared model must preserve the original small-noise and
            # positive model residual penalty rather than excuse either sign.
            moved=params.copy();moved[0]=12.
            self.assertEqual(p.residual_terms(moved)[0],_OneSidedProblem.residual_terms(p,moved)[0])
            frozen=[g.copy() for g in p.groups]
            p.residual_terms(params+1)
            for a,b in zip(frozen,p.groups):np.testing.assert_array_equal(a,b)

    def test_analytic_derivatives_match_numerical_estimates_with_radio_hinges_and_locks(self):
        pairs=[AnchorPairDistance('A','B',3),AnchorPairDistance('A','C',10),AnchorPairDistance('B','C',2)]
        for fixed in (None, {'A':(10.,-3.)}, {'A':(10.,-3.),'B':(13.,-3.)}):
            p=problem(AnalyticProblem,pairs,fixed)
            p.neighbor_a=np.array([0]);p.neighbor_b=np.array([1]);p.neighbor_max_m=2
            p.nonneighbor_a=np.array([1]);p.nonneighbor_b=np.array([2]);p.nonneighbor_min_m=6
            for c in ((1.,4.),(1.,.2),(2.,1.)):
                coords=np.array([[0.,0.],[3.001,0.],c])
                if fixed:coords+=np.array([10.,-3.])
                params=p.params_from_array(coords)
                numeric=np.column_stack([(p.residual_terms(params+e*1e-6)-p.residual_terms(params-e*1e-6))/2e-6
                                         for e in np.eye(len(params))])
                np.testing.assert_allclose(p.jacobian(params),numeric,rtol=1e-5,atol=1e-5)
                np.testing.assert_allclose(p._residual_jacobian(params),numeric,rtol=1e-5,atol=1e-5)

    def test_triangle_bound_preserves_inputs_and_does_not_complete_missing_edges(self):
        pairs=[AnchorPairDistance('A','B',3),AnchorPairDistance('B','C',4),AnchorPairDistance('A','C',12)]
        corrected,changes=metric_bounds(pairs)
        self.assertEqual(pairs[2].distance_m,12)
        self.assertEqual(len(corrected),3)
        self.assertAlmostEqual(corrected[2].distance_m,7+3*math.sqrt(3*.05**2))
        self.assertEqual(len(changes),1)
        untouched,changes=metric_bounds(pairs[:2])
        self.assertEqual(untouched,pairs[:2]);self.assertEqual(changes,[])

    def test_expanded_reach_has_separate_bitmap_and_degree_capped_plans(self):
        scene=new_scene(151000,'shared','expanded')
        self.assertGreater(len(scene.pairs),2*len(scene.truth))
        self.assertTrue({(p.anchor_a_id,p.anchor_b_id) for p in scene.pairs} <= scene.neighbor_pairs)
        self.assertFalse(scene.neighbor_pairs & scene.nonneighbor_pairs)
        for edge in scene.neighbor_pairs:
            limit=15 if edge in scene.nlos_edges else 20
            self.assertLessEqual(math.dist(*(scene.truth[a] for a in edge)),limit)
        counts={a:0 for a in scene.truth}
        for pair in planned_pairs(scene,random.Random(7)):
            counts[pair.anchor_a_id]+=1;counts[pair.anchor_b_id]+=1
        self.assertLessEqual(max(counts.values()),4)


if __name__ == '__main__':
    unittest.main()
