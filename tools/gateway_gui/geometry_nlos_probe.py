"""Reproduce a local NLOS reflection with the production GUI solvers.

Run from the repository root: python -m tools.gateway_gui.geometry_nlos_probe
This deliberately constructed counterexample is not a representative benchmark
or a replay of hardware data. Truth is used only to generate and score ranges.
"""

from itertools import combinations
import argparse
import json
import math

import numpy as np

from tools.gateway_gui.anchor_geometry import AnchorPairDistance
from tools.gateway_gui.anchor_geometry_connectivity import CONNECTIVITY_INTERVAL_ALGORITHM
from tools.gateway_gui.anchor_geometry_visibility import VISIBILITY_BRANCHING_NEIGHBOR_AWARE_ALGORITHM
from tools.gateway_gui.diagnostic_models import solve_geometry


def aligned_target_error(positions, truth):
    """Register using the other anchors, allowing a global reflection, no scale."""
    support = sorted(set(truth) - {"target"})
    estimated = np.array([positions[key] for key in support])
    reference = np.array([truth[key] for key in support])
    center_est, center_ref = estimated.mean(axis=0), reference.mean(axis=0)
    u, _, vt = np.linalg.svd((estimated - center_est).T @ (reference - center_ref))
    rotation = u @ vt
    target = (np.array(positions["target"]) - center_est) @ rotation + center_ref
    support_errors = np.linalg.norm((estimated - center_est) @ rotation + center_ref - reference, axis=1)
    return float(np.linalg.norm(target - truth["target"])), float(support_errors.max())


def run(powers, degree_cap):
    rows = []
    for scenario, green_shift, biased in (
        ("LOS control", 0.0, False),
        ("Exact local reflection", 0.0, True),
        ("Nearly collinear LOS supports", 0.15, True),
    ):
        truth = {
            "green1": (0.0, -2.0), "green2": (green_shift, 0.0),
            "green3": (0.0, 2.0), "nlos1": (-8.0, 8.0),
            "nlos2": (-6.0, 8.0), "nlos3": (-4.0, 8.0),
            "nlos4": (-6.0, 5.0), "target": (-3.0, 0.0),
        }
        mirrored = {**truth, "target": (3.0, 0.0)}
        pairs = []
        biases = []
        for a, b in combinations(sorted(truth), 2):
            distance = math.dist(truth[a], truth[b])
            if biased and b == "target" and a.startswith("nlos"):
                inflated = math.dist(mirrored[a], mirrored[b])
                biases.append(inflated - distance)
                distance = inflated
            pairs.append(AnchorPairDistance(a, b, distance, sigma_m=0.05))
        # Give both hypotheses identical, non-discriminating connectivity.
        # A physical maximum of 20 m includes all these true links.
        neighbors = frozenset((pair.anchor_a_id, pair.anchor_b_id) for pair in pairs)
        if degree_cap == 4:
            # A deterministic degree-four survey schedule; the radio bitmap
            # still reports all reachable links, including unmeasured ones.
            names = sorted(truth)
            scheduled = {
                tuple(sorted((names[i], names[(i + step) % len(names)])))
                for i in range(len(names)) for step in (1, 2)
            }
            pairs = [p for p in pairs if (p.anchor_a_id, p.anchor_b_id) in scheduled]
        methods = (
            (solver, power)
            for solver in (CONNECTIVITY_INTERVAL_ALGORITHM, VISIBILITY_BRANCHING_NEIGHBOR_AWARE_ALGORITHM)
            for power in powers
        )
        for solver, power in methods:
            result = solve_geometry(
                tuple(pairs), solver=solver, neighbor_pairs=neighbors,
                nonneighbor_pairs=frozenset(), neighbor_max_m=20.0,
                distance_weight_power=power,
            )
            error, support_error = aligned_target_error(result.positions_m, truth)
            rows.append({
                "scenario": scenario, "solver": solver, "pairs": len(pairs),
                "distance_weight_power": power, "degree_cap": degree_cap,
                "target_error_m": error, "support_max_error_m": support_error,
                "measured_rmse_m": result.rmse_m,
                "bias_range_m": [min(biases), max(biases)] if biases else [0.0, 0.0],
                "warnings": result.warnings,
                "truth_rmse_m": math.sqrt(sum(
                    (math.dist(truth[p.anchor_a_id], truth[p.anchor_b_id]) - p.distance_m) ** 2
                    for p in pairs
                ) / len(pairs)),
                "mirrored_rmse_m": math.sqrt(sum(
                    (math.dist(mirrored[p.anchor_a_id], mirrored[p.anchor_b_id]) - p.distance_m) ** 2
                    for p in pairs
                ) / len(pairs)),
            })
    print(json.dumps(rows, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--powers", nargs="+", type=float, default=[0.0, 1.0, 2.0, 4.0])
    parser.add_argument("--degree-cap", type=int, choices=(0, 4), default=0)
    args = parser.parse_args()
    run(args.powers, args.degree_cap)
