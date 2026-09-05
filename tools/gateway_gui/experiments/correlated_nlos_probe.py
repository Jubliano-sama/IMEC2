"""Research-only ablation of clustered NLOS penalties and RMSE-gated search.

No GUI behavior is changed. All weights/gates are frozen before reflection
trials so moving an anchor cannot make its own discount grow. Truth enters
scene generation and evaluation only. Defaults are hypotheses, not calibration.
"""

import argparse
import hashlib
from itertools import combinations
import json
import math
from multiprocessing import Pool
from pathlib import Path
import random
import time

import numpy as np

from ..anchor_geometry import (
    _anchor_ids, _positions_to_params, _preprocess_pairs, _validate_connected,
    rotate_layout_to_level, solve_anchor_layout,
)
from ..anchor_geometry_connectivity import _normalize_constraints, pair_key
from ..anchor_geometry_nlos import _OneSidedProblem, _reflect_across
from ..anchor_geometry_seeds import graph_mds_seed
from .verify_nlos_recovery import cases, score
from .nlos_analytic_probe import NumericalProblem


class CorrelatedProblem(_OneSidedProblem):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.discount = np.ones(len(self.measured))
        self.has_discount = False
        self.node_rmse = np.zeros(self.count)
        self.local_calls = 0
        self.evaluations = 0

    def local_solve(self, params, *, max_nfev):
        self.local_calls += 1
        return NumericalProblem.local_solve(self, params, max_nfev=max_nfev)

    def residual_terms(self, params):
        self.evaluations += 1
        terms = super().residual_terms(params)
        if not self.has_discount:
            return terms
        points = self.positions_array(params)
        residual = np.linalg.norm(points[self.pair_a] - points[self.pair_b], axis=1) - self.measured
        excess_sigma = np.maximum(0, -residual / self.sigma)
        # Preserve small-noise curvature and the full >8m bias-cap penalty.
        ramp = np.clip((excess_sigma - 3) / 3, 0, 1)
        plateau_cost = -self.plateau * np.expm1(-excess_sigma**2 / self.plateau)
        reduction = (1 - self.discount) * ramp * plateau_cost * self.sqrt_weights**2
        count = len(self.measured)
        terms[:count] = np.sign(terms[:count]) * np.sqrt(np.maximum(0, terms[:count]**2 - reduction))
        return terms

    def freeze_groups(self, params, *, grouped, guarded=False):
        points = self.positions_array(params)
        residual = np.linalg.norm(points[self.pair_a] - points[self.pair_b], axis=1) - self.measured
        lookup = {pair_key(self.anchor_ids[a], self.anchor_ids[b]): edge
                  for edge, (a, b) in enumerate(zip(self.pair_a, self.pair_b))}
        for anchor in range(self.count):
            edges = np.flatnonzero((self.pair_a == anchor) | (self.pair_b == anchor))
            self.node_rmse[anchor] = float(np.sqrt(np.mean(residual[edges]**2)))
            # Begin discount at 15cm RMSE, full strength at 50cm.
            gate = float(np.clip((self.node_rmse[anchor] - 0.15) / 0.35, 0, 1))
            if not grouped or gate == 0:
                continue
            others = np.where(self.pair_a[edges] == anchor, self.pair_b[edges], self.pair_a[edges])
            directions = points[others] - points[anchor]
            directions /= np.maximum(np.linalg.norm(directions, axis=1)[:, None], 1e-9)
            support = np.zeros(len(edges))
            for i, j in combinations(range(len(edges)), 2):
                # A direct short range supports proximity independently of
                # the estimated layout. An unmeasured pair earns no discount.
                mutual = lookup.get(pair_key(self.anchor_ids[others[i]], self.anchor_ids[others[j]]))
                if mutual is None or self.measured[mutual] > 3.0:
                    continue
                if guarded and (abs(residual[mutual]) > 3 * self.sigma[mutual]
                                or residual[edges[i]] >= -3 * self.sigma[edges[i]]
                                or residual[edges[j]] >= -3 * self.sigma[edges[j]]):
                    continue
                if float(np.dot(directions[i], directions[j])) < math.cos(math.radians(20)):
                    continue
                support[i] += 1
                support[j] += 1
            weight = 1 - gate * (1 - np.maximum(0.25, 1 / (1 + support)))
            # Shared endpoint evidence discounts an undirected range once.
            self.discount[edges] = np.minimum(self.discount[edges], weight)
        self.has_discount = bool(np.any(self.discount < 1))

    def search(self, params, value, *, targeted, prioritized=False):
        accepted = 0
        targets = sorted(range(self.count), key=lambda a: (-self.node_rmse[a], self.anchor_ids[a]))
        # Keep the old order in the full search to isolate the score change.
        if not targeted and not prioritized:
            targets = list(range(self.count))
        elif targeted:
            targets = [a for a in targets if self.node_rmse[a] > 0.15]
        for _ in range(3):
            improved = False
            for anchor in targets:
                for a, b in combinations(self.nearest[anchor][:4], 2):
                    points = self.positions_array(params)
                    if np.linalg.norm(points[a] - points[b]) <= 0.1:
                        continue
                    reflected = _reflect_across(points[anchor], points[a], points[b])
                    if np.linalg.norm(reflected - points[anchor]) < 0.2:
                        continue
                    points[anchor] = reflected
                    start = self.params_from_array(points)
                    if start is None:
                        continue
                    candidate, energy = self.local_solve(start, max_nfev=300)
                    if energy < value - 1e-6:
                        params, value = candidate, energy
                        improved = True
                        accepted += 1
            if not improved:
                break
        return params, value, accepted, len(targets)


def solve(pairs, neighbors, nonneighbors, *, method, maximum):
    processed = _preprocess_pairs(pairs, min_sigma_m=0.02, min_distance_m=0.05)
    ids = _anchor_ids(processed)
    _validate_connected(ids, processed)
    problem = CorrelatedProblem(
        ids, processed, _normalize_constraints(neighbors, set(ids)),
        _normalize_constraints(nonneighbors, set(ids)),
        neighbor_max_m=maximum, nonneighbor_min_m=7, interval_sigma_m=0.75,
        plateau=16, distance_weight_power=1, bias_cap_m=8,
    )
    seeds = [solve_anchor_layout(pairs, seed_count=8, basin_hops=3).positions_m,
             graph_mds_seed(pairs, ids)]
    rng = random.Random(7)
    scale = float(np.median(problem.measured))
    candidates = []
    for positions in seeds:
        first = np.asarray(_positions_to_params(problem.parameterization, rotate_layout_to_level(positions, ids[0], ids[1])))
        starts = [first] + [first + np.array([rng.gauss(0, 0.08 * scale) for _ in first]) for _ in range(3)]
        for start in starts:
            params, value = problem.local_solve(start, max_nfev=300)
            candidates.append((value, params))
    value, params = min(candidates, key=lambda item: item[0])
    grouped = "grouped" in method
    problem.freeze_groups(params, grouped=grouped, guarded="guarded" in method)
    if problem.has_discount:
        # Rank every original solution under the same frozen objective.
        reranked = [problem.local_solve(start, max_nfev=300) for _, start in candidates]
        params, value = min(reranked, key=lambda item: item[1])
    params, value, accepted, targets = problem.search(
        params, value, targeted="targeted" in method, prioritized="prioritized" in method,
    )
    return problem.positions(params), {
        "energy": value, "discounted_edges": int(np.sum(problem.discount < 1)),
        "node_rmse_before_search": dict(zip(ids, problem.node_rmse.tolist())),
        "discounts": problem.discount.tolist(), "local_calls": problem.local_calls,
        "evaluations": problem.evaluations, "reflections_accepted": accepted, "targets": targets,
    }


def run(job):
    scene, method = job
    started = time.perf_counter()
    row = {"scene": scene.name, "group": scene.meta["group"], "method": method}
    try:
        positions, diagnostics = solve(scene.pairs, scene.neighbor_pairs, scene.nonneighbor_pairs,
                                      method=method, maximum=20 if scene.name == "indistinguishable" else 15)
        row.update(status="ok", positions=positions, **diagnostics, **score(scene, positions))
    except Exception as exc:
        row.update(status="error", error=f"{type(exc).__name__}: {exc}")
    row["runtime_s"] = time.perf_counter() - started
    return row


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--workers", type=int, default=6)
    parser.add_argument("--methods", default="baseline,grouped_guarded,prioritized,grouped_guarded_prioritized")
    parser.add_argument("--limit-per-group", type=int, default=0)
    args = parser.parse_args()
    counts = {}
    selected = []
    for scene in cases():
        group = scene.meta["group"]
        if args.limit_per_group and counts.get(group, 0) >= args.limit_per_group:
            continue
        selected.append(scene)
        counts[group] = counts.get(group, 0) + 1
    args.out.parent.mkdir(parents=True, exist_ok=True)
    sources = [Path(__file__), Path(__file__).parents[1] / "anchor_geometry_nlos.py",
               Path(__file__).with_name("verify_nlos_recovery.py")]
    args.out.with_suffix(".meta.json").write_text(json.dumps({
        "source_sha256": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
        "cases": [s.name for s in selected], "methods": args.methods,
        "proximity_m": 3, "angle_degrees": 20, "rmse_gate_m": [0.15, 0.5], "discount_floor": 0.25,
    }, indent=2) + "\n")
    jobs = [(scene, method) for scene in selected for method in args.methods.split(",")]
    with args.out.open("w") as stream, Pool(args.workers) as pool:
        for i, row in enumerate(pool.imap_unordered(run, jobs), 1):
            stream.write(json.dumps(row, allow_nan=False) + "\n")
            stream.flush()
            if i % 20 == 0 or i == len(jobs):
                print(f"{i}/{len(jobs)} completed", flush=True)


if __name__ == "__main__":
    main()
