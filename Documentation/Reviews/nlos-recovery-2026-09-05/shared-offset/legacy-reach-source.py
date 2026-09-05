"""Shared positive-offset hypotheses, evaluated without changing the GUI.

Groups are frozen from measured proximity and an initial solved layout. A group
may use independent NLOS costs or one common positive bias plus individual
deviations. Small errors, negative measurement errors and >8m excess retain
the original penalty. Scene truth is never supplied to the solver.
"""

import argparse
from dataclasses import replace
import hashlib
from itertools import combinations
import json
import math
from multiprocessing import Pool
from pathlib import Path
import random
import time

import numpy as np

from ..anchor_geometry import AnchorPairDistance, _anchor_ids, _preprocess_pairs
from ..anchor_geometry_connectivity import _normalize_constraints, pair_key
from ..anchor_geometry_nlos import _OneSidedProblem, solve_nlos_one_sided_layout
from ..protocol import SurveyAssignmentIdentity, SurveyEvent, SurveyNeighborReport, select_survey_pairs
from . import recovered_nlos_probe as probe
from .verify_nlos_recovery import cases as old_cases, score
from .nlos_metric_probe import metric_bounds
from .nlos_analytic_probe import AnalyticProblem
from unittest.mock import patch


CONFIGS = {"shared_01": (0.1, 0.5), "shared_03": (0.3, 0.5),
           "shared_06": (0.6, 0.5), "shared_03_full": (0.3, 1.0),
           "extra_search": (0.3, 0.0)}


class SharedBiasProblem(_OneSidedProblem):
    def __init__(self, *args, deviation_m=0.3, strength=0.5, **kwargs):
        super().__init__(*args, **kwargs)
        self.deviation_m = deviation_m
        self.strength = strength
        self.groups = []
        self.calls = 0

    def freeze_groups(self, params):
        points = self.positions_array(params)
        distances = np.linalg.norm(points[self.pair_a] - points[self.pair_b], axis=1)
        lookup = {tuple(sorted((int(a), int(b)))): i
                  for i, (a, b) in enumerate(zip(self.pair_a, self.pair_b))}
        candidates = []
        for anchor in range(self.count):
            edges = np.flatnonzero((self.pair_a == anchor) | (self.pair_b == anchor))
            others = np.where(self.pair_a[edges] == anchor, self.pair_b[edges], self.pair_a[edges])
            vectors = points[others] - points[anchor]
            vectors /= np.maximum(np.linalg.norm(vectors, axis=1)[:, None], 1e-9)
            close = {}
            for i, j in combinations(range(len(edges)), 2):
                mutual = lookup.get(tuple(sorted((int(others[i]), int(others[j])))))
                if mutual is None or self.measured[mutual] > 3:
                    continue
                if abs(distances[mutual] - self.measured[mutual]) > 3 * self.sigma[mutual]:
                    continue
                if np.dot(vectors[i], vectors[j]) < math.cos(math.radians(20)):
                    continue
                close[i, j] = self.measured[mutual]
            for size in range(2, min(4, len(edges)) + 1):
                for subset in combinations(range(len(edges)), size):
                    pairs = list(combinations(subset, 2))
                    if all(pair in close for pair in pairs):
                        selected = tuple(int(edges[i]) for i in subset)
                        candidates.append((-size, max(close[p] for p in pairs), selected))
        used = set()
        self.groups = []
        for _, _, selected in sorted(candidates):
            if not used.intersection(selected):
                self.groups.append(np.asarray(selected, dtype=int))
                used.update(selected)

    def residual_terms(self, params):
        terms = super().residual_terms(params)
        if not self.groups or self.strength == 0:
            return terms
        points = self.positions_array(params)
        excess = self.measured - np.linalg.norm(points[self.pair_a] - points[self.pair_b], axis=1)
        for edges in self.groups:
            e, sigma = excess[edges], self.sigma[edges]
            # A shared positive offset cannot excuse even one too-short range.
            gate = float(np.clip((np.min(e / sigma) - 3) / 3, 0, 1))
            if gate == 0:
                continue
            weights = self.sqrt_weights[edges] ** 2
            common = float(np.average(e, weights=weights))
            scatter = float(np.sum(weights * ((e - common) / self.deviation_m) ** 2))
            shared_cost = self.plateau * float(np.mean(weights)) + scatter
            individual_cost = float(np.sum(weights * (-self.plateau * np.expm1(-(e / sigma)**2 / self.plateau))))
            reduction = self.strength * gate * max(0, individual_cost - shared_cost)
            original_cost = float(terms[edges] @ terms[edges])
            if original_cost > 0:
                terms[edges] *= math.sqrt(max(0, 1 - reduction / original_cost))
        return terms

    def local_solve(self, params, *, max_nfev):
        self.calls += 1
        return super().local_solve(params, max_nfev=max_nfev)


def solve_shared(pairs, neighbors, nonneighbors, initial, *, maximum, method):
    processed = _preprocess_pairs(pairs, min_sigma_m=.02, min_distance_m=.05)
    ids = _anchor_ids(processed)
    deviation, strength = CONFIGS[method]
    problem = SharedBiasProblem(ids, processed, _normalize_constraints(neighbors, set(ids)),
                                _normalize_constraints(nonneighbors, set(ids)),
                                neighbor_max_m=maximum, nonneighbor_min_m=7, interval_sigma_m=.75,
                                plateau=16, distance_weight_power=1, bias_cap_m=8,
                                deviation_m=deviation, strength=strength)
    start = problem.params_from_array(np.asarray([initial[a] for a in ids]))
    problem.freeze_groups(start)
    original_score = problem.objective(start)
    params, value = start, original_score
    if problem.groups:
        params, value = problem.local_solve(params, max_nfev=300)
        params, value, _ = problem.reflection_search(params, value)
    # The incumbent is a candidate under exactly the same frozen score.
    if value > original_score:
        params, value = start, original_score
    return problem.positions(params), {"groups": [g.tolist() for g in problem.groups],
                                       "energy": value, "initial_energy": original_score,
                                       "additional_local_calls": problem.calls}


def planned_pairs(scene, rng):
    # Use the actual current GUI planner with randomized discovery-slot order.
    ids = list(scene.truth)
    rng.shuffle(ids)
    slots = {a: i for i, a in enumerate(ids)}
    edges = {tuple(sorted((slots[a], slots[b]))) for a, b in scene.neighbor_pairs}
    event = SurveyEvent(1, 0, 1, SurveyAssignmentIdentity(1, 1, bytes([1]) * 32, len(ids), 1), 0,
                        occupied_slots=frozenset(range(len(ids))),
                        neighbor_reports=tuple(SurveyNeighborReport(i, frozenset(b if a == i else a
                             for a, b in edges if i in (a, b))) for i in range(len(ids))))
    selected = {pair_key(ids[a], ids[b]) for a, b in select_survey_pairs(event, degree_cap=4)}
    return [p for p in scene.pairs if pair_key(p.anchor_a_id, p.anchor_b_id) in selected]


def new_scene(seed, category):
    rng = random.Random(seed)
    truth = probe.drawing_truth()
    # Vary the actual shape, not only a rigid transformation of the sketch.
    truth = {a: (x + rng.uniform(-.65, .65), y + rng.uniform(-.65, .65)) for a, (x, y) in truth.items()}
    if category.startswith("random"):
        truth = probe.random_truth(rng, n_above=4, n_below=7, min_spacing=.8)
        assert truth is not None
    gap = rng.uniform(1, 4) if category == "wall_edge" else 0
    walls = probe.random_walls(rng, gap)
    shadow = {a: rng.uniform(.1, .35) for a in truth}
    if "T" in truth:
        shadow["T"] = 1.
    common = {a: rng.uniform(1.2, 4.5) * shadow[a] for a in truth}
    reach = {a: rng.uniform(10, 15) for a in truth}
    pairs, neighbors, nonneighbors, nlos, biases = [], set(), set(), set(), {}
    for a, b in combinations(sorted(truth), 2):
        distance = math.dist(truth[a], truth[b])
        obstructed = category != "clean" and any(probe.segments_cross(truth[a], truth[b], *wall) for wall in walls)
        radius = min(reach[a], reach[b]) - (1.0 if obstructed else 0)
        if distance > radius:
            nonneighbors.add((a, b))
            continue
        neighbors.add((a, b))
        bias = 0.
        if obstructed:
            owner = max((a, b), key=lambda node: shadow[node])
            bias = max(0., common[owner] + rng.gauss(0, .08))
            if category in ("unequal", "random_unequal"):
                bias = rng.uniform(.8, 4.5) * shadow[owner]
            nlos.add((a, b))
            biases[a, b] = bias
        measured = max(.06, distance + bias + rng.gauss(0, .03))
        pairs.append(AnchorPairDistance(a, b, measured))
    return probe.Scene(f"{category}/{seed}", truth, walls, pairs, neighbors, nonneighbors,
                       nlos, biases, {"group": category, "seed": seed}, tuple(a for a, p in truth.items() if p[1] < 6))


def suite(split):
    scenes = []
    if split == "development":
        counts = {}
        for scene in old_cases():
            group = scene.meta["group"]
            if counts.get(group, 0) < 4:
                scenes.append(scene)
                counts[group] = counts.get(group, 0) + 1
        count, base = 6, 31000
    else:
        count, base = 16, 81000
    for category in ("shared", "unequal", "wall_edge", "clean", "random_shared", "random_unequal"):
        for i in range(count):
            scene = new_scene(base + i, category)
            scenes.append(scene)
            if category in ("shared", "clean", "random_shared"):
                scenes.append(replace(scene, name="planned/" + scene.name,
                                      pairs=planned_pairs(scene, random.Random(base + 100 + i)),
                                      meta={**scene.meta, "group": category + "_planned"}))
    return scenes


def run(job):
    scene, methods = job
    maximum = 20 if scene.name == "indistinguishable" else 15
    rows = []
    t0 = time.perf_counter()
    try:
        baseline = solve_nlos_one_sided_layout(scene.pairs, neighbor_pairs=scene.neighbor_pairs,
                                             nonneighbor_pairs=scene.nonneighbor_pairs, neighbor_max_m=maximum)
        baseline_time = time.perf_counter() - t0
        for method in methods:
            started = time.perf_counter()
            try:
                positions, diagnostics = baseline.positions_m, {"energy": baseline.energy, "additional_local_calls": 0}
                if method in ("metric", "analytic"):
                    if method == "metric":
                        pairs, changes = metric_bounds(scene.pairs)
                        result = solve_nlos_one_sided_layout(pairs, neighbor_pairs=scene.neighbor_pairs,
                            nonneighbor_pairs=scene.nonneighbor_pairs, neighbor_max_m=maximum)
                        diagnostics = {"energy": result.energy, "adjusted_pairs": changes}
                    else:
                        with patch("tools.gateway_gui.anchor_geometry_nlos._OneSidedProblem", AnalyticProblem):
                            result = solve_nlos_one_sided_layout(scene.pairs, neighbor_pairs=scene.neighbor_pairs,
                                nonneighbor_pairs=scene.nonneighbor_pairs, neighbor_max_m=maximum)
                        diagnostics = {"energy": result.energy}
                    positions = result.positions_m
                elif method != "baseline":
                    positions, diagnostics = solve_shared(scene.pairs, scene.neighbor_pairs, scene.nonneighbor_pairs,
                                                          baseline.positions_m, maximum=maximum, method=method)
                rows.append({"scene": scene.name, "group": scene.meta["group"], "method": method,
                             "status": "ok", "positions": positions, **diagnostics, **score(scene, positions),
                             "runtime_s": (0 if method in ("metric", "analytic") else baseline_time) + time.perf_counter() - started})
            except Exception as exc:
                rows.append({"scene": scene.name, "group": scene.meta["group"], "method": method,
                             "status": "error", "error": f"{type(exc).__name__}: {exc}"})
    except Exception as exc:
        for method in methods:
            rows.append({"scene": scene.name, "group": scene.meta["group"], "method": method,
                         "status": "error", "error": f"{type(exc).__name__}: {exc}"})
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--split", choices=("development", "holdout"), default="development")
    parser.add_argument("--methods", default="baseline,shared_01,shared_03,shared_06,shared_03_full")
    parser.add_argument("--workers", type=int, default=8)
    args = parser.parse_args()
    methods = args.methods.split(",")
    if set(methods) - {"baseline", "analytic", "metric", *CONFIGS}:
        parser.error("Unknown method")
    scenes = suite(args.split)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    sources = [Path(__file__), Path(probe.__file__), Path(__file__).with_name("verify_nlos_recovery.py")]
    sources += list(Path(__file__).parents[1].glob("anchor_geometry*.py"))
    sources += [Path(__file__).parents[1] / "protocol.py"]
    sources += [Path(__file__).with_name("nlos_metric_probe.py"), Path(__file__).with_name("nlos_analytic_probe.py")]
    args.out.with_suffix(".meta.json").write_text(json.dumps({"split": args.split, "methods": methods,
        "configs": CONFIGS, "source_sha256": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}}, indent=2) + "\n")
    with args.out.with_suffix(".inputs.jsonl").open("w") as stream:
        for scene in scenes:
            stream.write(json.dumps({"scene": scene.name, "meta": scene.meta, "truth": scene.truth,
                "pairs": [[p.anchor_a_id, p.anchor_b_id, p.distance_m, p.sigma_m] for p in scene.pairs],
                "neighbors": sorted(scene.neighbor_pairs), "nonneighbors": sorted(scene.nonneighbor_pairs),
                "generated_bias": [[a, b, v] for (a, b), v in sorted(scene.bias.items())]}) + "\n")
    with args.out.open("w") as stream, Pool(args.workers) as pool:
        for i, rows in enumerate(pool.imap_unordered(run, [(s, methods) for s in scenes]), 1):
            for row in rows:
                stream.write(json.dumps(row, allow_nan=False) + "\n")
            stream.flush()
            if i % 10 == 0 or i == len(scenes):
                print(f"{i}/{len(scenes)} scenes completed ({len(methods)} methods)", flush=True)


if __name__ == "__main__":
    main()
