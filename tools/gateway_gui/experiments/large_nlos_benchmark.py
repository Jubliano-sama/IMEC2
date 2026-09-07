"""Deterministic large-office accuracy checks, independent of the sketch family.

Run with ``python -m tools.gateway_gui.experiments.large_nlos_benchmark --out
/tmp/large-nlos.jsonl``. Pilot has 12 scenes and holdout has 24, with disjoint
seed ranges. These are synthetic propagation assumptions, not measured RF
statistics. Office/sparse variants share an environment and are not independent
physical trials. No scene is discarded or repaired for poor connectivity.
"""

from __future__ import annotations

import argparse
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass, replace
import hashlib
import importlib.util
from itertools import combinations
import json
import math
from multiprocessing import Pool
from pathlib import Path
import random
import sys
import time

import numpy as np

from ..anchor_geometry import AnchorLayoutResult, AnchorPairDistance
from ..anchor_geometry_nlos import solve_nlos_one_sided_layout
from ..protocol import (
    SurveyAssignmentIdentity, SurveyEvent, SurveyNeighborReport, select_survey_pairs,
)

Point = tuple[float, float]
Pair = tuple[str, str]
Segment = tuple[Point, Point]
Solver = Callable[..., AnchorLayoutResult]
KINDS = ("clean", "office", "irregular", "office_sparse")


@dataclass(frozen=True)
class LargeScene:
    name: str
    seed: int
    kind: str
    truth: dict[str, Point]
    pairs: tuple[AnchorPairDistance, ...]
    neighbor_pairs: frozenset[Pair]
    nonneighbor_pairs: frozenset[Pair]
    walls: tuple[Segment, ...]
    generated_bias: dict[Pair, float]

    @property
    def neighbor_max_m(self) -> float:
        return max(20.0, max((p.distance_m for p in self.pairs), default=0.0) + 1.0)


def _crosses(a: Point, b: Point, c: Point, d: Point) -> bool:
    def orient(p: Point, q: Point, r: Point) -> float:
        return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0])
    return orient(a, b, c) * orient(a, b, d) < 0 and orient(c, d, a) * orient(c, d, b) < 0


def _building(rng: random.Random, count: int, irregular: bool) -> tuple[dict[str, Point], tuple[Segment, ...]]:
    columns = math.ceil(math.sqrt(count * (1.8 if irregular else 1.4)))
    rows = math.ceil(count / columns / (0.75 if irregular else 1.0))
    width, height = columns * 5.0, rows * 4.5
    cells = [(x, y) for y in range(rows) for x in range(columns)
             if not irregular or x < columns * 0.6 or y < rows * 0.6]
    # Select cells evenly, without rejecting a generated environment.
    selected = [cells[min(len(cells) - 1, int((i + 0.5) * len(cells) / count))] for i in range(count)]
    points = {f"A{i:02d}": ((x + rng.uniform(0.15, 0.85)) * 5.0,
                              (y + rng.uniform(0.15, 0.85)) * 4.5)
              for i, (x, y) in enumerate(selected)}
    corridor = height * 0.5
    walls: list[Segment] = []
    # Two corridor walls with real door openings into rooms.
    for y in (corridor - 1.25, corridor + 1.25):
        for x in np.arange(0.0, width, 7.0):
            walls.extend([((float(x), y), (min(float(x) + 2.2, width), y)),
                          ((min(float(x) + 3.5, width), y), (min(float(x) + 7.0, width), y))])
    for x in np.arange(7.0, width, 7.0):
        walls.extend([((float(x), 0.0), (float(x), corridor - 1.25)),
                      ((float(x), corridor + 1.25), (float(x), height))])
    if irregular:
        walls.append(((width * 0.12, height * 0.15), (width * 0.62, height * 0.4)))
    return points, tuple(w for w in walls if math.dist(*w) > 0.01)


def _planned_pairs(scene: LargeScene) -> tuple[AnchorPairDistance, ...]:
    """Apply the actual GUI planner, preserving its degree-four budget."""
    ids = sorted(scene.truth)
    random.Random(scene.seed + 907).shuffle(ids)
    slots = {anchor: i for i, anchor in enumerate(ids)}
    neighbors = {tuple(sorted((slots[a], slots[b]))) for a, b in scene.neighbor_pairs}
    event = SurveyEvent(
        1, 0, 1, SurveyAssignmentIdentity(1, 1, bytes([1]) * 32, len(ids), 1), 0,
        occupied_slots=frozenset(range(len(ids))),
        neighbor_reports=tuple(SurveyNeighborReport(i, frozenset(b if a == i else a
            for a, b in neighbors if i in (a, b))) for i in range(len(ids))),
    )
    selected = {tuple(sorted((ids[a], ids[b]))) for a, b in select_survey_pairs(event, degree_cap=4)}
    return tuple(p for p in scene.pairs if (p.anchor_a_id, p.anchor_b_id) in selected)


def make_scene(seed: int, anchor_count: int, kind: str = "office", *, omission_rate: float = 0.08) -> LargeScene:
    """Create ranges and complete radio reports; omission is never a nonneighbor.

    Bias combines wall-dependent, endpoint-dependent and independent components.
    Irregular scenes also bias some short links, so proximity is not a LOS oracle.
    A sparse scene is the same office realization passed through the GUI planner.
    """
    if kind not in KINDS or not 3 <= anchor_count <= 50 or not 0 <= omission_rate <= 1:
        raise ValueError("Invalid scene kind, anchor count, or omission rate")
    rng = random.Random(seed)
    truth, walls = _building(rng, anchor_count, kind == "irregular")
    active_walls = () if kind == "clean" else walls
    wall_bias = [rng.uniform(0.15, 0.75) for _ in walls]
    shadow = {a: rng.uniform(1.5, 3.2) if i % 9 == 0 else rng.uniform(0.0, 0.8)
              for i, a in enumerate(truth)}
    los_reach = {a: rng.uniform(17.0, 20.0) for a in truth}
    nlos_reach = {a: rng.uniform(12.0, 15.0) for a in truth}
    neighbors: set[Pair] = set()
    nonneighbors: set[Pair] = set()
    biases: dict[Pair, float] = {}
    pairs = []
    for a, b in combinations(sorted(truth), 2):
        distance = math.dist(truth[a], truth[b])
        crossings = [i for i, wall in enumerate(active_walls) if _crosses(truth[a], truth[b], *wall)]
        short_nlos = kind == "irregular" and distance < 6.0 and rng.random() < 0.25
        obstructed = bool(crossings) or short_nlos
        reach = nlos_reach if obstructed else los_reach
        if distance > min(reach[a], reach[b]):
            nonneighbors.add((a, b))
            continue
        neighbors.add((a, b))
        bias = 0.0
        if obstructed:
            bias = min(5.0, max(0.3, sum(wall_bias[i] for i in crossings)
                                + max(shadow[a], shadow[b]) + rng.uniform(0.0, 1.0)))
            biases[a, b] = bias
        measured = max(0.06, distance + bias + rng.gauss(0.0, 0.03))
        if rng.random() >= omission_rate:
            pairs.append(AnchorPairDistance(a, b, measured, 0.05, source="synthetic"))
    scene = LargeScene(f"{kind}/n{anchor_count}/{seed}", seed, kind, truth, tuple(pairs),
                       frozenset(neighbors), frozenset(nonneighbors), active_walls, biases)
    if kind == "office_sparse":
        scene = replace(scene, pairs=_planned_pairs(scene))
    return scene


def suite(split: str = "pilot", *, seeds: Sequence[int] | None = None,
          sizes: Sequence[int] = (20, 35, 50)) -> list[LargeScene]:
    if split not in ("pilot", "holdout"):
        raise ValueError("Unknown benchmark split")
    selected_seeds = seeds if seeds is not None else ((451000,) if split == "pilot" else (551000, 551001))
    return [make_scene(seed + count * 100, count, kind)
            for seed in selected_seeds for count in sizes for kind in KINDS]


def score_positions(scene: LargeScene, positions: Mapping[str, Point]) -> dict:
    """Rigid errors and measured-triangle diagnostics, never individual flip labels.

    Alignment permits reflection but no scaling. Triangle eligibility depends
    only on measured edges and truth, so paired solvers share the denominator.
    A shallow estimate can count as both collapsed and orientation-reversed.
    """
    if set(positions) != set(scene.truth):
        raise ValueError("Solver omitted or invented anchors")
    ids = sorted(scene.truth)
    truth = np.asarray([scene.truth[a] for a in ids], dtype=float)
    estimated = np.asarray([positions[a] for a in ids], dtype=float)
    if estimated.shape != truth.shape or not np.all(np.isfinite(estimated)):
        raise ValueError("Solver returned invalid coordinates")
    centered = estimated - estimated.mean(axis=0)
    target = truth - truth.mean(axis=0)
    u, _, vt = np.linalg.svd(centered.T @ target)
    rotation = u @ vt
    aligned = centered @ rotation
    rotation_determinant = float(np.linalg.det(rotation))
    errors = np.linalg.norm(aligned - target, axis=1)
    measured = {tuple(sorted((p.anchor_a_id, p.anchor_b_id))) for p in scene.pairs if p.enabled}
    eligible_triangles = reversed_triangles = collapsed_triangles = 0
    reversal_anchors: set[str] = set()
    for i, j, k in combinations(range(len(ids)), 3):
        triangle = (ids[i], ids[j], ids[k])
        if not all(edge in measured for edge in combinations(triangle, 2)):
            continue
        reference_ab, reference_ac = target[j] - target[i], target[k] - target[i]
        estimate_ab, estimate_ac = estimated[j] - estimated[i], estimated[k] - estimated[i]
        reference_area = reference_ab[0] * reference_ac[1] - reference_ab[1] * reference_ac[0]
        # An orthogonal transform changes signed area only by its determinant.
        # Computing it before rotation preserves an exactly zero input area.
        estimate_area = ((estimate_ab[0] * estimate_ac[1] - estimate_ab[1] * estimate_ac[0])
                         * rotation_determinant)
        # Any vertex can be the target, but count the triangle only once. Its
        # orientation sign changes identically for every cyclic vertex choice.
        # Truth alone supplies the stable-support eligibility gate.
        truth_supported = estimate_supported = False
        for a, b in ((i, j), (j, k), (k, i)):
            reference_base = float(np.linalg.norm(target[b] - target[a]))
            estimate_base = float(np.linalg.norm(estimated[b] - estimated[a]))
            truth_supported |= reference_base >= 2.0 and abs(reference_area) / reference_base >= 1.0
            estimate_supported |= estimate_base >= 2.0 and abs(estimate_area) / estimate_base >= 1.0
        if not truth_supported:
            continue
        eligible_triangles += 1
        collapsed_triangles += not estimate_supported
        if reference_area * estimate_area < 0:
            reversed_triangles += 1
            reversal_anchors.update(triangle)
    return {
        "anchor_errors_m": dict(zip(ids, errors.tolist())),
        "anchor_median_m": float(np.median(errors)), "anchor_p95_m": float(np.quantile(errors, 0.95)),
        "anchor_max_m": float(errors.max()), "anchors_over_1m": int(np.sum(errors > 1.0)),
        "anchors_over_2m": int(np.sum(errors > 2.0)), "scene_rms_m": float(np.sqrt(np.mean(errors ** 2))),
        "orientation_reversed_triangles": reversed_triangles,
        "orientation_eligible_triangles": eligible_triangles,
        "orientation_collapsed_triangles": collapsed_triangles,
        # These anchors participate in a reversal; this does not identify which
        # endpoint moved or prove an isolated anchor reflection.
        "orientation_reversal_anchors": sorted(reversal_anchors),
    }


def scene_record(scene: LargeScene) -> dict:
    degrees = {a: 0 for a in scene.truth}
    for pair in scene.pairs:
        degrees[pair.anchor_a_id] += 1
        degrees[pair.anchor_b_id] += 1
    return {"scene": scene.name, "seed": scene.seed, "kind": scene.kind,
            "anchor_count": len(scene.truth), "edge_count": len(scene.pairs), "degrees": degrees,
            "truth": scene.truth, "walls": scene.walls,
            "pairs": [[p.anchor_a_id, p.anchor_b_id, p.distance_m, p.sigma_m] for p in scene.pairs],
            "neighbors": sorted(scene.neighbor_pairs), "nonneighbors": sorted(scene.nonneighbor_pairs),
            "neighbor_max_m": scene.neighbor_max_m,
            "generated_bias": [[a, b, value] for (a, b), value in sorted(scene.generated_bias.items())]}


def run_scene(scene: LargeScene, solver: Solver = solve_nlos_one_sided_layout, *, method: str = "current") -> dict:
    """The solver boundary receives measured inputs only, never truth or walls."""
    started = time.perf_counter()
    row = {"scene": scene.name, "kind": scene.kind, "seed": scene.seed, "method": method,
           "anchor_count": len(scene.truth), "edge_count": len(scene.pairs)}
    try:
        result = solver(scene.pairs, neighbor_pairs=scene.neighbor_pairs,
                        nonneighbor_pairs=scene.nonneighbor_pairs, neighbor_max_m=scene.neighbor_max_m)
        metrics = score_positions(scene, result.positions_m)
        row.update(status="ok", positions=result.positions_m, energy=result.energy,
                   range_rmse_m=result.rmse_m, warnings=result.warnings, **metrics)
    except Exception as exc:
        row.update(status="error", error=f"{type(exc).__name__}: {exc}")
    row["runtime_s"] = time.perf_counter() - started
    return row


def summarize(rows: Sequence[dict]) -> dict:
    """Pool anchor errors, keeping solve failures and scene-tail counts visible."""
    successful = [row for row in rows if row["status"] == "ok"]
    errors = np.asarray([error for row in successful for error in row["anchor_errors_m"].values()])
    result = {"scenes": len(rows), "solved": len(successful), "failed": len(rows) - len(successful),
              "scored_anchors": int(errors.size), "total_anchors": sum(row["anchor_count"] for row in rows)}
    if errors.size:
        result.update(anchor_median_m=float(np.median(errors)), anchor_p95_m=float(np.quantile(errors, 0.95)),
                      anchor_max_m=float(errors.max()), anchors_over_1m=int(np.sum(errors > 1.0)),
                      anchors_over_2m=int(np.sum(errors > 2.0)),
                      scenes_over_1m=sum(row["anchor_max_m"] > 1 for row in successful),
                      scenes_over_2m=sum(row["anchor_max_m"] > 2 for row in successful),
                      scene_rms_median_m=float(np.median([row["scene_rms_m"] for row in successful])),
                      scene_rms_max_m=max(row["scene_rms_m"] for row in successful),
                      runtime_median_s=float(np.median([row["runtime_s"] for row in successful])))
    return result


def _load_solver(source: str | None) -> Solver:
    if source is None:
        return solve_nlos_one_sided_layout
    path = Path(source).resolve()
    module_name = "tools.gateway_gui._large_nlos_baseline_" + hashlib.sha256(path.read_bytes()).hexdigest()[:12]
    if module_name not in sys.modules:
        spec = importlib.util.spec_from_file_location(module_name, path)
        if spec is None or spec.loader is None:
            raise ValueError("Cannot import baseline source")
        module = importlib.util.module_from_spec(spec)
        sys.modules[module_name] = module
        spec.loader.exec_module(module)
    return sys.modules[module_name].solve_nlos_one_sided_layout


def _run_job(job: tuple[LargeScene, str, str | None]) -> dict:
    scene, method, source = job
    return run_scene(scene, _load_solver(source), method=method)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--split", choices=("pilot", "holdout"), default="pilot")
    parser.add_argument("--seeds", help="Comma-separated seed bases, overriding the selected split")
    parser.add_argument("--sizes", default="20,35,50")
    parser.add_argument("--kinds", default=",".join(KINDS))
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument("--baseline-source", type=Path, help="Frozen anchor_geometry_nlos.py; shared imports use this checkout")
    parser.add_argument("--inputs-only", action="store_true")
    args = parser.parse_args()
    kinds = set(args.kinds.split(","))
    if not kinds <= set(KINDS) or args.workers < 1:
        parser.error("Unknown scene kind or invalid worker count")
    scenes = [scene for scene in suite(args.split,
              seeds=None if args.seeds is None else tuple(map(int, args.seeds.split(","))),
              sizes=tuple(map(int, args.sizes.split(",")))) if scene.kind in kinds]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.with_suffix(".inputs.jsonl").write_text("".join(json.dumps(scene_record(s), allow_nan=False) + "\n" for s in scenes))
    sources = [Path(__file__), *Path(__file__).parents[1].glob("anchor_geometry*.py"),
               Path(__file__).parents[1] / "protocol.py"]
    if args.baseline_source:
        sources.append(args.baseline_source)
    args.out.with_suffix(".meta.json").write_text(json.dumps({
        "split": args.split, "scene_count": len(scenes), "python": sys.version, "numpy": np.__version__,
        "alignment": "whole-map translation and orthogonal transform; reflection allowed, no scale",
        "baseline_shared_imports": "current checkout", "arguments": {k: str(v) for k, v in vars(args).items()},
        "source_sha256": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
    }, indent=2) + "\n")
    if args.inputs_only:
        print(f"Saved {len(scenes)} input scenes", flush=True)
        return
    methods: list[tuple[str, str | None]] = [("current", None)]
    if args.baseline_source:
        methods.insert(0, ("baseline", str(args.baseline_source.resolve())))
    jobs = [(s, method, source) for s in scenes for method, source in methods]
    rows = []
    with args.out.open("w") as stream, Pool(args.workers) as pool:
        for i, row in enumerate(pool.imap_unordered(_run_job, jobs), 1):
            rows.append(row)
            stream.write(json.dumps(row, allow_nan=False) + "\n")
            stream.flush()
            print(f"{i}/{len(jobs)} {row['method']} {row['scene']}: {row['status']} ({row['runtime_s']:.1f}s)", flush=True)
    summary = {method: {scope: summarize([r for r in rows if r["method"] == method and
                 (scope == "all" or r["kind"] == scope)]) for scope in ("all", *KINDS)} for method, _ in methods}
    args.out.with_suffix(".summary.json").write_text(json.dumps(summary, indent=2, allow_nan=False) + "\n")


if __name__ == "__main__":
    main()
