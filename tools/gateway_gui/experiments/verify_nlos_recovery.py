"""Paired, reproducible checks of the recovered solver on unseen inputs.

Truth and obstacle labels are used only by scene generation and scoring.
Run with: python -m tools.gateway_gui.experiments.verify_nlos_recovery --out PATH
Every generated case is retained, including disconnected sparse schedules.
"""

import argparse
import hashlib
from dataclasses import replace
from itertools import combinations
import json
import math
from multiprocessing import Pool
from pathlib import Path
import random
import sys
import time

import numpy as np

from tools.gateway_gui.anchor_geometry import AnchorPairDistance
from tools.gateway_gui.anchor_geometry_connectivity import solve_connectivity_interval_layout
from tools.gateway_gui.anchor_geometry_nlos import solve_nlos_one_sided_layout
from . import recovered_nlos_probe as probe


def strict_cap(pairs, cap, rng):
    edges = list(pairs)
    rng.shuffle(edges)
    degrees = {}
    kept = []
    for pair in edges:
        a, b = pair.anchor_a_id, pair.anchor_b_id
        if degrees.get(a, 0) < cap and degrees.get(b, 0) < cap:
            kept.append(pair)
            degrees[a] = degrees.get(a, 0) + 1
            degrees[b] = degrees.get(b, 0) + 1
    return kept


def drawing(seed, bias_lo, bias_hi, group):
    scene = probe.build_scene(
        f"{group}/{seed}/{bias_lo}", probe.drawing_truth(), probe.drawing_walls(0),
        random.Random(seed), los_radius=12, nlos_radius=12, sigma=0.03,
        bias_lo=bias_lo, bias_hi=bias_hi, shadow=probe.drawing_shadow("hetero"),
        meta={"group": group, "seed": seed, "bias_lo": bias_lo},
    )
    return scene


def relabel(scene, rng):
    names = list(scene.truth)
    rng.shuffle(names)
    aliases = {a: f"anchor{i:02d}" for i, a in enumerate(names)}
    return replace(
        scene, name="relabelled/" + scene.name,
        truth={aliases[a]: point for a, point in scene.truth.items()},
        pairs=[replace(p, anchor_a_id=aliases[p.anchor_a_id], anchor_b_id=aliases[p.anchor_b_id]) for p in scene.pairs],
        neighbor_pairs={probe.key(aliases[a], aliases[b]) for a, b in scene.neighbor_pairs},
        nonneighbor_pairs={probe.key(aliases[a], aliases[b]) for a, b in scene.nonneighbor_pairs},
        nlos_edges={probe.key(aliases[a], aliases[b]) for a, b in scene.nlos_edges},
        bias={probe.key(aliases[a], aliases[b]): value for (a, b), value in scene.bias.items()},
        los_group=tuple(aliases[a] for a in scene.los_group),
        meta={**scene.meta, "group": "drawing_relabelled", "aliases": aliases},
    )


def cases():
    scenes = []
    for lo, hi in ((1.6, 3.0), (3.0, 5.0)):
        for rep in range(10):
            scenes.append(drawing(1000 + rep, lo, hi, "drawing_replay"))
        for rep in range(12):
            scene = drawing(7000 + rep, lo, hi, "drawing_unseen")
            scenes.append(scene)
            if rep < 6:
                scenes.append(relabel(scene, random.Random(9100 + rep)))

    for rep in range(24):
        rng = random.Random(10000 + rep)
        truth = probe.random_truth(rng, n_above=rng.randint(3, 5), n_below=rng.randint(6, 9))
        assert truth is not None
        walls = probe.random_walls(rng, (0, 1.5, 3)[rep % 3])
        lo, hi = ((0.5, 1.5), (1.2, 2.5), (2.5, 4.5))[rep % 3]
        scene = probe.build_scene(
            f"wall_unseen/{rep}", truth, walls, rng,
            los_radius=rng.uniform(10, 14), nlos_radius=rng.uniform(7, 11), sigma=0.03,
            bias_lo=lo, bias_hi=hi, shadow={a: rng.uniform(0.1, 1) for a in truth},
            meta={"group": "wall_unseen", "seed": 10000 + rep, "bias_lo": lo},
        )
        scenes.append(scene)
        scenes.append(replace(
            scene, name=f"wall_cap4/{rep}", pairs=strict_cap(scene.pairs, 4, random.Random(12000 + rep)),
            meta={**scene.meta, "group": "wall_cap4"},
        ))

    for rep in range(12):
        rng = random.Random(20000 + rep)
        truth = {f"A{i}": (rng.uniform(0, 9), rng.uniform(0, 9)) for i in range(8)}
        pairs = [AnchorPairDistance(a, b, max(0.06, math.dist(truth[a], truth[b]) + rng.gauss(0, 0.03)))
                 for a, b in combinations(truth, 2)]
        neighbors = {(p.anchor_a_id, p.anchor_b_id) for p in pairs}
        clean = probe.Scene(f"clean/{rep}", truth, [], pairs, neighbors, set(), set(), {},
                            {"group": "clean", "seed": 20000 + rep}, tuple(truth))
        scenes.append(clean)
        scenes.append(replace(clean, name=f"clean_cap4/{rep}",
                              pairs=strict_cap(pairs, 4, random.Random(22000 + rep)),
                              meta={**clean.meta, "group": "clean_cap4"}))
        shortest = sorted(pairs, key=lambda p: p.distance_m)[:4]
        bad_keys = {(p.anchor_a_id, p.anchor_b_id) for p in shortest}
        offsets = {key: rng.uniform(0.7, 2.0) for key in sorted(bad_keys)}
        biased = [replace(p, distance_m=p.distance_m + offsets.get((p.anchor_a_id, p.anchor_b_id), 0)) for p in pairs]
        scenes.append(replace(clean, name=f"short_nlos/{rep}", pairs=biased, nlos_edges=bad_keys,
                              bias=offsets, meta={**clean.meta, "group": "short_nlos"}))

    truth = {"green1": (0., -2.), "green2": (0., 0.), "green3": (0., 2.),
             "nlos1": (-8., 8.), "nlos2": (-6., 8.), "nlos3": (-4., 8.),
             "nlos4": (-6., 5.), "target": (-3., 0.)}
    mirror = {**truth, "target": (3., 0.)}
    pairs = [AnchorPairDistance(a, b, math.dist(mirror[a], mirror[b])) for a, b in combinations(truth, 2)]
    scenes.append(probe.Scene("indistinguishable", truth, [], pairs,
                             {(p.anchor_a_id, p.anchor_b_id) for p in pairs}, set(), set(), {},
                             {"group": "indistinguishable"}, tuple(truth)))
    return scenes


def rigidity_rank(scene):
    ids = list(scene.truth)
    index = {a: i for i, a in enumerate(ids)}
    matrix = np.zeros((len(scene.pairs), 2 * len(ids)))
    for row, pair in enumerate(scene.pairs):
        a, b = pair.anchor_a_id, pair.anchor_b_id
        delta = np.asarray(scene.truth[a]) - scene.truth[b]
        norm = np.linalg.norm(delta)
        if norm:
            delta /= norm
        ia, ib = 2 * index[a], 2 * index[b]
        matrix[row, ia:ia+2] = delta
        matrix[row, ib:ib+2] = -delta
    return int(np.linalg.matrix_rank(matrix))


def score(scene, positions):
    if set(positions) != set(scene.truth):
        raise ValueError("Solver omitted anchors from the supplied graph")
    full = probe.align_offsets(scene.truth, positions)
    group = scene.los_group
    aligned = probe.align_offsets(scene.truth, positions, group)
    loo = {a: probe.align_offsets(scene.truth, positions, [b for b in group if b != a])[a] for a in group}
    aliases = scene.meta.get("aliases", {})
    remapped = {old: positions[new] for old, new in aliases.items()} if aliases else positions
    original_truth = {old: scene.truth[new] for old, new in aliases.items()} if aliases else scene.truth
    return {
        "full_rms_m": float(np.sqrt(np.mean([v*v for v in full.values()]))),
        "full_max_m": max(full.values()),
        "group_rms_m": float(np.sqrt(np.mean([aligned[a]**2 for a in group]))),
        "group_loo_over_1m": max(loo.values()) > 1,
        "target_flip": probe.drawing_orientation_flip(original_truth, remapped),
        "target_error_m": loo.get(aliases.get("T", "T")),
    }


def run(job):
    scene, method = job
    started = time.perf_counter()
    row = {"scene": scene.name, "method": method, **scene.meta,
           "anchor_count": len(scene.truth), "edge_count": len(scene.pairs),
           "rigidity_rank": rigidity_rank(scene), "required_rank": 2 * len(scene.truth) - 3,
           "truth": scene.truth,
           "pairs": [[p.anchor_a_id, p.anchor_b_id, p.distance_m, p.sigma_m] for p in scene.pairs],
           "neighbors": sorted(scene.neighbor_pairs), "nonneighbors": sorted(scene.nonneighbor_pairs)}
    try:
        fn = solve_connectivity_interval_layout if method == "baseline" else solve_nlos_one_sided_layout
        # Only measured ranges and the observed radio graph cross this boundary.
        result = fn(scene.pairs, neighbor_pairs=scene.neighbor_pairs,
                    nonneighbor_pairs=scene.nonneighbor_pairs,
                    neighbor_max_m=20 if scene.name == "indistinguishable" else 15)
        row.update(status="ok", positions=result.positions_m, energy=result.energy,
                   range_rmse_m=result.rmse_m, warnings=result.warnings, **score(scene, result.positions_m))
    except Exception as exc:
        row.update(status="error", error=f"{type(exc).__name__}: {exc}")
    row["runtime_s"] = time.perf_counter() - started
    return row


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=6)
    parser.add_argument("--groups", help="Optional comma-separated groups")
    args = parser.parse_args()
    selected = cases()
    if args.groups:
        selected = [scene for scene in selected if scene.meta["group"] in args.groups.split(",")]
    jobs = [(scene, method) for scene in selected for method in ("baseline", "candidate")]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    source_root = Path(__file__).resolve().parents[1]
    sources = [Path(__file__), Path(probe.__file__)] + [source_root / name for name in (
        "anchor_geometry.py", "anchor_geometry_connectivity.py", "anchor_geometry_nlos.py",
        "anchor_geometry_seeds.py", "anchor_geometry_visibility.py",
    )]
    args.out.with_suffix(".meta.json").write_text(json.dumps({
        "python": sys.version, "numpy": np.__version__, "scene_count": len(selected),
        "groups": args.groups, "source_sha256": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
    }, indent=2) + "\n")
    started = time.perf_counter()
    print(f"{len(selected)} scenes, {len(jobs)} paired solves", flush=True)
    with args.out.open("w") as stream, Pool(args.workers) as pool:
        for i, row in enumerate(pool.imap_unordered(run, jobs), 1):
            stream.write(json.dumps(row, allow_nan=False) + "\n")
            stream.flush()
            if i % 20 == 0 or i == len(jobs):
                print(f"{i}/{len(jobs)} completed in {time.perf_counter() - started:.0f}s", flush=True)


if __name__ == "__main__":
    main()
