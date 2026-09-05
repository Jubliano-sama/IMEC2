#!/usr/bin/env python3
"""THROWAWAY probe: NLOS-induced mirror flips in the anchor geometry solvers.

Recovered from Claude's /tmp/nlos_probe on 2026-09-05; original is archived.
Research harness only; not imported by the GUI. Builds synthetic scenes with a wall (optionally with a
gap), applies one-sided NLOS range bias to wall-crossing edges, and compares
the GUI solvers against candidate objective functions.
"""
from __future__ import annotations

import argparse
import csv
import itertools
import json
import math
import random
import sys
import time
from dataclasses import dataclass, field
from multiprocessing import Pool

import numpy as np
from scipy.optimize import least_squares  # type: ignore[import-untyped]

from pathlib import Path
REPO = str(Path(__file__).resolve().parents[3])
sys.path.insert(0, REPO)

from tools.gateway_gui.anchor_geometry import (  # noqa: E402
    AnchorPairDistance,
    rotate_layout_to_level,
    solve_anchor_layout,
)
from tools.gateway_gui.anchor_geometry_connectivity import (  # noqa: E402
    solve_connectivity_interval_layout,
)
from tools.gateway_gui.anchor_geometry_visibility import (  # noqa: E402
    solve_visibility_branching_neighbor_aware_tuned,
)
from tools.gateway_gui.anchor_geometry_seeds import graph_mds_seed  # noqa: E402

Point = tuple[float, float]
Segment = tuple[Point, Point]
PairKey = tuple[str, str]


def key(a: str, b: str) -> PairKey:
    return (a, b) if a < b else (b, a)


def _orient(p, q, r):
    return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0])


def segments_cross(a1, a2, b1, b2) -> bool:
    d1 = _orient(b1, b2, a1)
    d2 = _orient(b1, b2, a2)
    d3 = _orient(a1, a2, b1)
    d4 = _orient(a1, a2, b2)
    return (d1 * d2 < 0) and (d3 * d4 < 0)


# --------------------------------------------------------------------------- scenes


@dataclass
class Scene:
    name: str
    truth: dict[str, Point]
    walls: list[Segment]
    pairs: list[AnchorPairDistance]
    neighbor_pairs: set[PairKey]
    nonneighbor_pairs: set[PairKey]
    nlos_edges: set[PairKey]
    bias: dict[PairKey, float]
    meta: dict = field(default_factory=dict)
    los_group: tuple[str, ...] = ()


def build_scene(name, truth, walls, rng, *, los_radius, nlos_radius, sigma,
                bias_lo, bias_hi, shadow=None, degree_cap=None, meta=None) -> Scene:
    """Wall-crossing edges get bias U(lo,hi) * max(shadow[a], shadow[b]); shadow defaults to 1."""
    ids = sorted(truth)
    shadow = dict(shadow or {})
    pairs = []
    nb: set[PairKey] = set()
    nnb: set[PairKey] = set()
    nlos: set[PairKey] = set()
    bias: dict[PairKey, float] = {}
    for a, b in itertools.combinations(ids, 2):
        d = math.dist(truth[a], truth[b])
        crosses = any(segments_cross(truth[a], truth[b], w[0], w[1]) for w in walls)
        radius = nlos_radius if crosses else los_radius
        k = key(a, b)
        if d > radius:
            nnb.add(k)
            continue
        nb.add(k)
        measured = d + rng.gauss(0.0, sigma)
        if crosses:
            bb = rng.uniform(bias_lo, bias_hi) * max(shadow.get(a, 1.0), shadow.get(b, 1.0))
            bias[k] = bb
            nlos.add(k)
            measured += bb
        pairs.append(AnchorPairDistance(a, b, max(measured, 0.06), 0.05, True, "synthetic"))
    if degree_cap:
        pairs = cap_degree(pairs, ids, rng, degree_cap)
    wall_y = walls[0][0][1]
    los_group = tuple(sorted(a for a, p in truth.items() if p[1] < wall_y))
    return Scene(name, dict(truth), list(walls), pairs, nb, nnb, nlos, bias, dict(meta or {}), los_group)


def cap_degree(pairs, ids, rng, cap):
    """Random degree-capped ranging plan: keep connectivity and min degree 2 where possible."""
    edges = list(pairs)
    rng.shuffle(edges)
    deg = {i: 0 for i in ids}
    kept = []
    rest = []
    for p in edges:
        if deg[p.anchor_a_id] < cap and deg[p.anchor_b_id] < cap:
            kept.append(p)
            deg[p.anchor_a_id] += 1
            deg[p.anchor_b_id] += 1
        else:
            rest.append(p)
    # connectivity repair
    def components(es):
        parent = {i: i for i in ids}
        def find(x):
            while parent[x] != x:
                parent[x] = parent[parent[x]]
                x = parent[x]
            return x
        for p in es:
            ra, rb = find(p.anchor_a_id), find(p.anchor_b_id)
            if ra != rb:
                parent[ra] = rb
        return {i: find(i) for i in ids}
    comp = components(kept)
    for p in sorted(rest, key=lambda p: p.distance_m):
        if comp[p.anchor_a_id] != comp[p.anchor_b_id]:
            kept.append(p)
            deg[p.anchor_a_id] += 1
            deg[p.anchor_b_id] += 1
            comp = components(kept)
    rest = [p for p in rest if p not in kept]
    for p in sorted(rest, key=lambda p: p.distance_m):
        if deg[p.anchor_a_id] < 2 or deg[p.anchor_b_id] < 2:
            kept.append(p)
            deg[p.anchor_a_id] += 1
            deg[p.anchor_b_id] += 1
    return kept


def scene_is_fair(scene: Scene, min_degree: int = 3) -> bool:
    ids = sorted(scene.truth)
    deg = {i: 0 for i in ids}
    adj: dict[str, set[str]] = {i: set() for i in ids}
    for p in scene.pairs:
        deg[p.anchor_a_id] += 1
        deg[p.anchor_b_id] += 1
        adj[p.anchor_a_id].add(p.anchor_b_id)
        adj[p.anchor_b_id].add(p.anchor_a_id)
    if min(deg.values()) < min_degree:
        return False
    seen = {ids[0]}
    stack = [ids[0]]
    while stack:
        cur = stack.pop()
        for n in adj[cur]:
            if n not in seen:
                seen.add(n)
                stack.append(n)
    return len(seen) == len(ids)


def drawing_truth() -> dict[str, Point]:
    # Read off the sketch at ~100 px per metre, y up.
    return {
        "N1": (2.7, 8.3), "N2": (4.0, 8.3), "N3": (5.6, 8.3), "N4": (3.2, 7.0),
        "G1": (10.3, 3.1), "G2": (10.3, 1.1), "G3": (10.5, 0.0),
        "T": (8.2, 1.6),
    }


def drawing_shadow(mode: str) -> dict[str, float]:
    if mode == "uniform":
        return {}
    # T is deep in the wall shadow; the greens only graze the wall edge.
    return {"T": 1.0, "G1": 0.15, "G2": 0.15, "G3": 0.15, "N1": 0.0, "N2": 0.0, "N3": 0.0, "N4": 0.0}


def drawing_walls(gap: float) -> list[Segment]:
    if gap <= 0:
        return [((0.0, 6.0), (14.0, 6.0))]
    return [((0.0, 6.0), (6.0 - gap / 2, 6.0)), ((6.0 + gap / 2, 6.0), (14.0, 6.0))]


def random_truth(rng, *, n_above, n_below, width=16.0, wall_y=6.0,
                 height_above=5.0, height_below=7.0, min_spacing=2.0):
    pts: dict[str, Point] = {}

    def place(prefix, count, y0, y1):
        placed = 0
        tries = 0
        while placed < count and tries < 5000:
            tries += 1
            p = (rng.uniform(0.0, width), rng.uniform(y0, y1))
            if all(math.dist(p, q) >= min_spacing for q in pts.values()):
                pts[f"{prefix}{placed:02d}"] = p
                placed += 1
        return placed == count

    ok = place("U", n_above, wall_y + 1.0, wall_y + height_above)
    ok = ok and place("L", n_below, wall_y - height_below, wall_y - 1.0)
    return pts if ok else None


def random_walls(rng, gap, width=16.0, wall_y=6.0) -> list[Segment]:
    if gap <= 0:
        return [((-1.0, wall_y), (width + 1.0, wall_y))]
    cx = rng.uniform(3.0, width - 3.0)
    return [((-1.0, wall_y), (cx - gap / 2, wall_y)), ((cx + gap / 2, wall_y), (width + 1.0, wall_y))]


def make_family_scene(case_seed: int, *, bias_lo, bias_hi, gap, shadow_mode="hetero",
                      degree_cap=None, los_radius=12.0, nlos_radius=9.0, sigma=0.03) -> Scene:
    rng = random.Random(case_seed)
    for _ in range(300):
        n_above = rng.randint(3, 5)
        n_below = rng.randint(6, 9)
        truth = random_truth(rng, n_above=n_above, n_below=n_below)
        if truth is None:
            continue
        walls = random_walls(rng, gap)
        shadow = {} if shadow_mode == "uniform" else {a: rng.uniform(0.1, 1.0) for a in truth}
        scene = build_scene(
            f"fam{case_seed}_{shadow_mode}_cap{degree_cap or 0}", truth, walls, rng,
            los_radius=los_radius + rng.uniform(-1.0, 1.0),
            nlos_radius=nlos_radius + rng.uniform(-1.0, 1.0),
            sigma=sigma, bias_lo=bias_lo, bias_hi=bias_hi, shadow=shadow, degree_cap=degree_cap,
            meta={"bias_lo": bias_lo, "bias_hi": bias_hi, "gap": gap, "seed": case_seed,
                  "shadow": shadow_mode, "cap": degree_cap or 0},
        )
        if scene_is_fair(scene):
            return scene
    raise RuntimeError("could not build a fair scene")


# --------------------------------------------------------------------------- metrics


def align_offsets(truth: dict[str, Point], est: dict[str, Point], subset=None) -> dict[str, float]:
    """Fit a rigid transform (reflection allowed) on ``subset`` and report offsets for all."""
    ids = sorted(set(truth) & set(est))
    fit = sorted(set(subset) & set(ids)) if subset else ids
    T = np.array([truth[i] for i in ids], dtype=float)
    S = np.array([est[i] for i in ids], dtype=float)
    fi = np.array([ids.index(i) for i in fit])
    tmean = T[fi].mean(axis=0)
    smean = S[fi].mean(axis=0)
    Tc = T - tmean
    best = None
    best_rms = math.inf
    for refl in (1.0, -1.0):
        Sr = (S - smean).copy()
        Sr[:, 1] *= refl
        u, _s, vt = np.linalg.svd(Sr[fi].T @ Tc[fi])
        d = np.sign(np.linalg.det(u @ vt))
        rot = u @ np.diag([1.0, d]) @ vt
        aligned = Sr @ rot
        off = np.linalg.norm(aligned - Tc, axis=1)
        rms = float(np.sqrt(np.mean(off[fi] ** 2)))
        if rms < best_rms:
            best_rms = rms
            best = off
    if best is None:
        raise ValueError("No finite rigid alignment could be found")
    return {i: float(o) for i, o in zip(ids, best)}


def loo_offsets(truth: dict[str, Point], est: dict[str, Point]) -> dict[str, float]:
    """Per-anchor offset when the rigid fit is computed on every other anchor."""
    out = {}
    for a in truth:
        others = [b for b in truth if b != a]
        out[a] = align_offsets(truth, est, others)[a]
    return out


def side_sign(p, q, r) -> float:
    return 1.0 if _orient(p, q, r) > 0 else -1.0


def drawing_orientation_flip(truth, est) -> int:
    """1 if T sits on the wrong side of the G1-G3 line relative to the N cluster."""
    if not all(k in est for k in ("G1", "G3", "T", "N1", "N2", "N3", "N4")):
        return -1
    def rel(pos):
        c = np.mean([pos[n] for n in ("N1", "N2", "N3", "N4")], axis=0)
        return side_sign(pos["G1"], pos["G3"], pos["T"]) * side_sign(pos["G1"], pos["G3"], tuple(c))
    return int(rel(truth) != rel(est))


# --------------------------------------------------------------------------- probe solver


def params_to_xy(x: np.ndarray, n: int) -> np.ndarray:
    P = np.zeros((n, 2))
    if n > 1:
        P[1, 0] = x[0]
    for k in range(2, n):
        P[k, 0] = x[2 * k - 3]
        P[k, 1] = x[2 * k - 2]
    return P


def xy_to_params(P: np.ndarray) -> np.ndarray:
    n = P.shape[0]
    x = np.zeros(max(2 * n - 3, 0))
    if n > 1:
        x[0] = P[1, 0]
    for k in range(2, n):
        x[2 * k - 3] = P[k, 0]
        x[2 * k - 2] = P[k, 1]
    return x


def level(P: np.ndarray) -> np.ndarray:
    """Rigidly move so anchor 0 is at origin and anchor 1 on +x axis."""
    Q = P - P[0]
    ang = -math.atan2(Q[1, 1], Q[1, 0])
    c, s = math.cos(ang), math.sin(ang)
    R = np.array([[c, -s], [s, c]])
    return Q @ R.T


@dataclass
class Objective:
    kind: str                     # l2 | cauchy | softl1 | asym_flat | asym_cauchy
    dist_power: float = 0.0       # weight = (d_ref/d)^p on measured edges
    sigma: float = 0.05
    robust_scale_m: float = 0.2   # symmetric robust scale
    plateau: float = 9.0          # asym: plateau in sigma^2 units on the "model shorter" side
    neg_sigma_m: float = 0.05     # asym: tolerance scale on the "model shorter" side
    bias_cap_m: float = 8.0       # asym: NLOS bias beyond this is penalised again
    neighbor_max_m: float = 15.0
    nonneighbor_min_m: float = 7.0
    hinge_sigma_m: float = 0.75
    use_hinges: bool = True

    def label(self) -> str:
        parts = [self.kind]
        if self.dist_power:
            parts.append(f"d{self.dist_power:g}")
        if not self.use_hinges:
            parts.append("nohinge")
        return "_".join(parts)


class ProbeProblem:
    def __init__(self, scene: Scene, obj: Objective):
        self.scene = scene
        self.obj = obj
        self.ids = sorted(scene.truth)
        self.index = {a: i for i, a in enumerate(self.ids)}
        self.n = len(self.ids)
        self.ia = np.array([self.index[p.anchor_a_id] for p in scene.pairs])
        self.ib = np.array([self.index[p.anchor_b_id] for p in scene.pairs])
        self.meas = np.array([p.distance_m for p in scene.pairs])
        d_ref = float(np.median(self.meas))
        self.w = (d_ref / np.maximum(self.meas, 0.3)) ** obj.dist_power if obj.dist_power else np.ones_like(self.meas)
        self.sw = np.sqrt(self.w)
        nb = sorted(scene.neighbor_pairs)
        self.nba = np.array([self.index[a] for a, _ in nb], dtype=int)
        self.nbb = np.array([self.index[b] for _, b in nb], dtype=int)
        self.nna = np.array([self.index[a] for a, _ in scene.nonneighbor_pairs], dtype=int)
        self.nnb = np.array([self.index[b] for _, b in scene.nonneighbor_pairs], dtype=int)
        # neighbours sorted by measured distance, for the flip search
        self.nbrs: dict[int, list[int]] = {i: [] for i in range(self.n)}
        order = np.argsort(self.meas)
        for e in order:
            self.nbrs[int(self.ia[e])].append(int(self.ib[e]))
            self.nbrs[int(self.ib[e])].append(int(self.ia[e]))

    # residual vector f such that objective = sum rho(f^2)
    def residuals(self, x: np.ndarray) -> np.ndarray:
        P = params_to_xy(x, self.n)
        d = np.linalg.norm(P[self.ia] - P[self.ib], axis=1)
        r = d - self.meas
        z = r / self.obj.sigma
        kind = self.obj.kind
        if kind in ("l2", "cauchy", "softl1"):
            f = self.sw * z
        elif kind in ("asym_flat", "asym_cauchy"):
            C = self.obj.plateau
            pos = z >= 0
            f = np.empty_like(z)
            f[pos] = z[pos]
            zn = r[~pos] / self.obj.neg_sigma_m
            if kind == "asym_flat":
                cost = C * (1.0 - np.exp(-(zn * zn) / C))
            else:
                cost = C * np.log1p((zn * zn) / C)
            over = np.maximum(0.0, (-r[~pos]) - self.obj.bias_cap_m) / self.obj.sigma
            cost = cost + over * over
            f[~pos] = -np.sqrt(cost)
            f = self.sw * f
        else:
            raise ValueError(kind)
        parts = [f]
        if self.obj.use_hinges:
            if self.nba.size:
                dn = np.linalg.norm(P[self.nba] - P[self.nbb], axis=1)
                parts.append(np.maximum(0.0, dn - self.obj.neighbor_max_m) / self.obj.hinge_sigma_m)
            if self.nna.size:
                dm = np.linalg.norm(P[self.nna] - P[self.nnb], axis=1)
                parts.append(np.maximum(0.0, self.obj.nonneighbor_min_m - dm) / self.obj.hinge_sigma_m)
        return np.concatenate(parts)

    def scipy_loss(self):
        k = self.obj.kind
        if k == "cauchy":
            return "cauchy", self.obj.robust_scale_m / self.obj.sigma
        if k == "softl1":
            return "soft_l1", self.obj.robust_scale_m / self.obj.sigma
        return "linear", 1.0

    def value(self, x: np.ndarray) -> float:
        f = self.residuals(x)
        loss, s = self.scipy_loss()
        z = f * f
        if loss == "linear":
            return float(z.sum())
        if loss == "cauchy":
            return float((s * s * np.log1p(z / (s * s))).sum())
        return float((s * s * 2.0 * (np.sqrt(1.0 + z / (s * s)) - 1.0)).sum())

    def local(self, x0: np.ndarray, max_nfev: int = 300) -> tuple[np.ndarray, float]:
        loss, s = self.scipy_loss()
        res = least_squares(self.residuals, x0, method="trf", loss=loss, f_scale=s,
                            max_nfev=max_nfev, x_scale="jac")
        return res.x, self.value(res.x)

    def positions(self, x: np.ndarray) -> dict[str, Point]:
        P = params_to_xy(x, self.n)
        return {a: (float(P[i, 0]), float(P[i, 1])) for a, i in self.index.items()}

    def params_from_positions(self, pos: dict[str, Point]) -> np.ndarray:
        P = np.array([pos[a] for a in self.ids], dtype=float)
        return xy_to_params(level(P))

    def flip_search(self, x: np.ndarray, val: float, *, k_nearest: int = 4, max_sweeps: int = 3):
        best_x, best_val = x, val
        margins: dict[str, float] = {}
        for sweep in range(max_sweeps):
            improved = False
            for a in range(self.n):
                P = params_to_xy(best_x, self.n)
                nn = self.nbrs[a][:k_nearest]
                best_alt = math.inf
                for b, c in itertools.combinations(nn, 2):
                    u = P[c] - P[b]
                    L = np.linalg.norm(u)
                    if L < 0.1:
                        continue
                    u = u / L
                    v = P[a] - P[b]
                    refl = P[b] + 2.0 * np.dot(v, u) * u - v
                    if np.linalg.norm(refl - P[a]) < 0.2:
                        continue
                    Q = P.copy()
                    Q[a] = refl
                    x0 = xy_to_params(level(Q))
                    xa, va = self.local(x0, max_nfev=300)
                    moved = loo_offsets(self.positions(best_x), self.positions(xa))[self.ids[a]]
                    if moved > 0.5:
                        best_alt = min(best_alt, va)
                    if va < best_val - 1e-6:
                        best_x, best_val = xa, va
                        improved = True
                        P = params_to_xy(best_x, self.n)
                if sweep == max_sweeps - 1 or not improved:
                    margins[self.ids[a]] = best_alt - best_val
            if not improved:
                break
        return best_x, best_val, margins


def probe_solve(scene: Scene, obj: Objective, *, flip: bool = True, seed_spring: bool = True, search: str = "flip"):
    from dataclasses import replace as _replace
    prob = ProbeProblem(scene, obj)
    rng = random.Random(7)
    seeds: list[dict[str, Point]] = []
    if seed_spring:
        try:
            seeds.append(solve_anchor_layout(scene.pairs, seed_count=8, basin_hops=3).positions_m)
        except Exception:
            pass
    try:
        seeds.append(graph_mds_seed(scene.pairs, prob.ids))
    except Exception:
        pass
    scale = float(np.median(prob.meas))
    starts = []
    for s in seeds:
        x0 = prob.params_from_positions(s)
        starts.append(x0)
        for _ in range(3):
            starts.append(x0 + np.array([rng.gauss(0.0, 0.08 * scale) for _ in x0]))
    best_x, best_val = None, math.inf
    for x0 in starts:
        x, v = prob.local(x0)
        if v < best_val:
            best_x, best_val = x, v
    margins = {}
    if best_x is None:
        raise ValueError("No finite seed could be solved")
    if search == "gnc" and obj.kind == "asym_flat":
        x = best_x
        for ns in (1.0, 0.5, 0.25, 0.1):
            p2 = ProbeProblem(scene, _replace(obj, neg_sigma_m=ns))
            x, v = p2.local(x)
            x, v, _ = p2.flip_search(x, v, max_sweeps=2)
        best_x, best_val = prob.local(x)
    if flip:
        best_x, best_val, margins = prob.flip_search(best_x, best_val)
    return prob.positions(best_x), best_val, margins, prob


# --------------------------------------------------------------------------- methods


def gui_spring(scene):
    return solve_anchor_layout(scene.pairs).positions_m, math.nan, {}, None


def gui_intervals(scene):
    r = solve_connectivity_interval_layout(
        scene.pairs, neighbor_pairs=scene.neighbor_pairs, nonneighbor_pairs=scene.nonneighbor_pairs)
    return r.positions_m, r.energy, {}, None


def gui_vis_nbaware(scene):
    r = solve_visibility_branching_neighbor_aware_tuned(
        scene.pairs, missing_pairs=scene.nonneighbor_pairs, neighbor_pairs=scene.neighbor_pairs)
    return r.positions_m, r.energy, {}, None


OBJECTIVES = {
    "p_l2": Objective("l2"),
    "p_l2_d1": Objective("l2", dist_power=1.0),
    "p_l2_d2": Objective("l2", dist_power=2.0),
    "p_cauchy": Objective("cauchy", robust_scale_m=0.2),
    "p_softl1": Objective("softl1", robust_scale_m=0.3),
    "p_asym_flat": Objective("asym_flat"),
    "p_asym_cauchy": Objective("asym_cauchy"),
    "p_asym_flat_d1": Objective("asym_flat", dist_power=1.0),
    "p_l2_nmax10": Objective("l2", neighbor_max_m=10.0),
    "p_asym_flat_nmax10": Objective("asym_flat", neighbor_max_m=10.0),
    "p_asym_flat_d1_nmax10": Objective("asym_flat", dist_power=1.0, neighbor_max_m=10.0),
    "p_asym2": Objective("asym_flat", neg_sigma_m=0.5),
    "p_asym2_d1": Objective("asym_flat", neg_sigma_m=0.5, dist_power=1.0),
    "p_asym2_nmax10": Objective("asym_flat", neg_sigma_m=0.5, neighbor_max_m=10.0),
    "p_asym2_d1_nmax10": Objective("asym_flat", neg_sigma_m=0.5, dist_power=1.0, neighbor_max_m=10.0),
    "p_asym2_ns03_d1": Objective("asym_flat", neg_sigma_m=0.3, dist_power=1.0),
    "p_asym2_ns08_d1": Objective("asym_flat", neg_sigma_m=0.8, dist_power=1.0),
    "p_af_c9_d1": Objective("asym_flat", plateau=9.0, dist_power=1.0),
    "p_af_c9_d2": Objective("asym_flat", plateau=9.0, dist_power=2.0),
    "p_af_c16_d1": Objective("asym_flat", plateau=16.0, dist_power=1.0),
    "p_af_c16_d2": Objective("asym_flat", plateau=16.0, dist_power=2.0),
    "p_af_c25_d1": Objective("asym_flat", plateau=25.0, dist_power=1.0),
    "p_af_c25_d2": Objective("asym_flat", plateau=25.0, dist_power=2.0),
    "p_af_c9_ns1_d1": Objective("asym_flat", plateau=9.0, neg_sigma_m=0.1, dist_power=1.0),
    "p_af_c16_ns1_d1": Objective("asym_flat", plateau=16.0, neg_sigma_m=0.1, dist_power=1.0),
    "p_af_c16_ns1_d2": Objective("asym_flat", plateau=16.0, neg_sigma_m=0.1, dist_power=2.0),
    "p_af_c16_d1_nmax10": Objective("asym_flat", plateau=16.0, dist_power=1.0, neighbor_max_m=10.0),
}

METHODS = {
    "gui_spring": gui_spring,
    "gui_intervals": gui_intervals,
    "gui_vis_nbaware": gui_vis_nbaware,
}
for _name, _obj in OBJECTIVES.items():
    METHODS[_name] = (lambda o: (lambda scene: probe_solve(scene, o)))(_obj)
METHODS["p_l2_noflip"] = lambda scene: probe_solve(scene, OBJECTIVES["p_l2"], flip=False)
METHODS["p_af_c16_d1_gnc"] = lambda scene: probe_solve(scene, OBJECTIVES["p_af_c16_d1"], search="gnc")
METHODS["p_af_c9_d1_gnc"] = lambda scene: probe_solve(scene, OBJECTIVES["p_af_c9_d1"], search="gnc")
METHODS["p_asym_flat_noflip"] = lambda scene: probe_solve(scene, OBJECTIVES["p_asym_flat"], flip=False)


def run_one(args):
    scene, method_name = args
    fn = METHODS[method_name]
    t0 = time.perf_counter()
    oracle_val = math.nan
    oracle_flips = -1
    failure = ""
    try:
        pos, val, margins, prob = fn(scene)
        offs = align_offsets(scene.truth, pos, scene.los_group)
        loo = loo_offsets(scene.truth, pos)
        orient = drawing_orientation_flip(scene.truth, pos)
        status = "ok"
        err = ""
        if prob is not None:
            x_or, oracle_val = prob.local(prob.params_from_positions(scene.truth))
            o_loo = loo_offsets(scene.truth, prob.positions(x_or))
            oracle_flips = sum(1 for a in scene.los_group if o_loo[a] > 1.0)
    except Exception as exc:  # noqa: BLE001
        pos = {}
        offs = {a: math.inf for a in scene.truth}
        loo = dict(offs)
        orient = -1
        val, margins, status, err = math.nan, {}, "error", f"{type(exc).__name__}: {exc}"
    dt = time.perf_counter() - t0
    flipped = sorted(a for a in scene.los_group if loo[a] > 1.0)
    los_offs = [offs[a] for a in scene.los_group]
    nlos_offs = [offs[a] for a in scene.truth if a not in scene.los_group]
    if status == "ok":
        if not flipped:
            failure = "none"
        elif math.isnan(oracle_val):
            failure = "unknown"
        elif val < oracle_val - 1e-3:
            failure = "objective"
        else:
            failure = "search"
    return {
        "scene": scene.name, "method": method_name, "status": status, "error": err,
        "anchors": len(scene.truth), "edges": len(scene.pairs), "nlos_edges": len(scene.nlos_edges),
        "los_max_offset_m": max(los_offs), "los_median_offset_m": float(np.median(los_offs)),
        "nlos_median_offset_m": float(np.median(nlos_offs)) if nlos_offs else math.nan,
        "flipped_count": len(flipped), "flipped": " ".join(flipped),
        "objective": val, "oracle_objective": oracle_val, "oracle_flips": oracle_flips,
        "failure": failure, "runtime_s": dt, "orient_flip": orient,
        "loo_max_los_m": max(loo[a] for a in scene.los_group),
        "margins": json.dumps({k: round(v, 2) for k, v in margins.items()}),
        "positions": json.dumps({k: [round(v[0], 3), round(v[1], 3)] for k, v in pos.items()}),
        "truth": json.dumps({k: [round(v[0], 3), round(v[1], 3)] for k, v in scene.truth.items()}),
        **{f"meta_{k}": v for k, v in scene.meta.items()},
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["drawing", "family"], default="drawing")
    ap.add_argument("--methods", default=",".join(METHODS))
    ap.add_argument("--cases", type=int, default=10)
    ap.add_argument("--workers", type=int, default=1)
    ap.add_argument("--bias", default="0.3:0.8,0.8:1.6,1.6:3.0,3.0:5.0")
    ap.add_argument("--gaps", default="0,1.5")
    ap.add_argument("--out", default="/tmp/nlos_probe/detail.csv")
    ap.add_argument("--shadow", default="hetero", help="uniform|hetero (comma list ok)")
    ap.add_argument("--caps", default="0", help="degree caps, 0 = range every neighbour")
    a = ap.parse_args()
    methods = [m.strip() for m in a.methods.split(",") if m.strip()]
    scenes: list[Scene] = []
    if a.mode == "drawing":
        for bias in a.bias.split(","):
            lo, hi = (float(v) for v in bias.split(":"))
            for gap in (float(g) for g in a.gaps.split(",")):
              for shadow_mode in a.shadow.split(","):
                for cap in (int(c) for c in a.caps.split(",")):
                  for rep in range(a.cases):
                    rng = random.Random(1000 + rep)
                    scenes.append(build_scene(
                        f"drawing_b{lo:g}-{hi:g}_g{gap:g}_{shadow_mode}_cap{cap}_r{rep}", drawing_truth(),
                        drawing_walls(gap), rng, los_radius=12.0, nlos_radius=12.0, sigma=0.03,
                        bias_lo=lo, bias_hi=hi, shadow=drawing_shadow(shadow_mode), degree_cap=cap or None,
                        meta={"bias_lo": lo, "bias_hi": hi, "gap": gap, "seed": rep,
                              "shadow": shadow_mode, "cap": cap}))
    else:
        for bias in a.bias.split(","):
            lo, hi = (float(v) for v in bias.split(":"))
            for gap in (float(g) for g in a.gaps.split(",")):
              for shadow_mode in a.shadow.split(","):
                for cap in (int(c) for c in a.caps.split(",")):
                  for rep in range(a.cases):
                    scenes.append(make_family_scene(rep, bias_lo=lo, bias_hi=hi, gap=gap,
                                                    shadow_mode=shadow_mode, degree_cap=cap or None))
    jobs = [(s, m) for s in scenes for m in methods]
    print(f"scenes={len(scenes)} methods={len(methods)} jobs={len(jobs)} workers={a.workers}", flush=True)
    t0 = time.perf_counter()
    if a.workers > 1:
        rows = []
        with Pool(a.workers) as pool:
            for i, row in enumerate(pool.imap_unordered(run_one, jobs), 1):
                rows.append(row)
                if i % 20 == 0 or i == len(jobs):
                    print(f"  {i}/{len(jobs)} elapsed {time.perf_counter() - t0:.0f}s", flush=True)
    else:
        rows = [run_one(j) for j in jobs]
    with open(a.out, "w", newline="") as fh:
        wr = csv.DictWriter(fh, fieldnames=sorted({k for r in rows for k in r}))
        wr.writeheader()
        wr.writerows(rows)
    summarize(rows)


def summarize(rows):
    groups = {}
    for r in rows:
        g = (r.get("meta_bias_lo"), r.get("meta_bias_hi"), r.get("meta_gap"), r["method"],
             r.get("meta_shadow"), r.get("meta_cap"))
        groups.setdefault(g, []).append(r)
    print(f"{'bias':>9} {'gap':>4} {'method':<20} {'n':>3} {'err':>3} {'flip%':>6} {'Tflip%':>6} {'obj%':>5} {'srch%':>5} "
          f"{'orclFlip%':>9} {'losMed':>6} {'looMax':>6} {'nlosMed':>7} {'medT':>5}")
    last = None
    for g in sorted(groups, key=lambda k: (str(k[4]), float(k[5] or 0), float(k[0] or 0), float(k[2] or 0), k[3])):
        if (g[4], g[5]) != last:
            last = (g[4], g[5])
            print(f"--- shadow={g[4]} cap={g[5]}")
        rs = groups[g]
        ok = [r for r in rs if r["status"] == "ok"]
        if not ok:
            print(f"{g[0]}:{g[1]:<5} {g[2]:>4} {g[3]:<20} {len(rs):>3} {len(rs):>3}  all errors")
            continue
        fc = np.array([r["flipped_count"] for r in ok])
        obj = np.mean([r["failure"] == "objective" for r in ok])
        srch = np.mean([r["failure"] == "search" for r in ok])
        orc = [r["oracle_flips"] for r in ok if r["oracle_flips"] >= 0]
        orc_s = f"{100*np.mean(np.array(orc)>0):>8.0f}%" if orc else f"{'n/a':>9}"
        orient = [r["orient_flip"] for r in ok if r["orient_flip"] >= 0]
        orient_s = f"{100*np.mean(orient):>5.0f}%" if orient else f"{'n/a':>6}"
        print(f"{g[0]}:{g[1]:<5} {g[2]:>4} {g[3]:<20} {len(rs):>3} {len(rs)-len(ok):>3} "
              f"{100*np.mean(fc>0):>5.0f}% {orient_s} {100*obj:>4.0f}% {100*srch:>4.0f}% {orc_s} "
              f"{np.median([r['los_median_offset_m'] for r in ok]):>6.2f} "
              f"{np.median([r['loo_max_los_m'] for r in ok]):>6.2f} "
              f"{np.nanmedian([r['nlos_median_offset_m'] for r in ok]):>7.2f} "
              f"{np.median([r['runtime_s'] for r in rs]):>5.1f}")


if __name__ == "__main__":
    main()
