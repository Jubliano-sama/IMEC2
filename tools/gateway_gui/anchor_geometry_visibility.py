"""Visibility-aware anchor geometry solver.

Adapted from ``AnchorGeometrySolver`` commit
``01c3edb470bcd868403e04a6cded754360decdf0`` by Jubliano-sama. The repository
owner authorized this local port. Only explicitly supplied missing pairs are
treated as visibility evidence; absent known-distance edges are otherwise
unconstrained.
"""

from __future__ import annotations

from dataclasses import dataclass, replace
import itertools
import math
from typing import Any, Iterable

import numpy as np

from .anchor_geometry import (
    AnchorLayoutResult,
    AnchorPairDistance,
    _Parameterization,
    _anchor_ids,
    _clean_positions,
    _layout_warnings,
    _local_minimize,
    _positions_to_params,
    _preprocess_pairs,
    _rmse,
    _spring_energy,
    _validate_connected,
    pair_residuals,
    rotate_layout_to_level,
    solve_anchor_layout,
)
from .anchor_geometry_seeds import (
    GEOMETRY_SEEDS,
    SEED_AUTO,
    SEED_CURRENT,
    SEED_GRAPH_MDS,
    SEED_SPRING,
    SEED_VISIBILITY,
    graph_mds_seed,
)


INF = 1e12
PairKey = tuple[str, str]

VISIBILITY_BRANCHING_TUNED_ALGORITHM = "Visibility branching tuned"
VISIBILITY_BRANCHING_NEIGHBOR_AWARE_ALGORITHM = (
    "Visibility branching neighbor-aware tuned"
)
VISIBILITY_BRANCHING_TUNED_PARAMETERS: dict[str, Any] = {
    "iterations": 45,
    "beam_width": 32,
    "optimizer_seeds": 16,
    "radio_radius_m": 8.0,
    "missing_margin_m": 0.25,
    "missing_weight": 8.0,
    "missing_sigma_m": 0.75,
    "graph_upper_weight": 0.35,
    "graph_upper_factor": 1.0,
    "graph_upper_slack_m": 0.75,
    "graph_upper_sigma_m": 1.0,
    "final_visibility_weight": 1.0,
    "constrained_polish": True,
    "constrained_iterations": 55,
    "constrained_known_weight": 1.0,
}


@dataclass(frozen=True)
class RangeGraph:
    anchor_ids: tuple[str, ...]
    distances: dict[tuple[str, str], float]
    sigmas: dict[tuple[str, str], float]
    degree: dict[str, int]
    shortest_m: np.ndarray
    hops: np.ndarray
    index: dict[str, int]


@dataclass(frozen=True)
class PartialLayout:
    positions: dict[str, tuple[float, float]]
    score: float


def pair_key(anchor_a: str, anchor_b: str) -> tuple[str, str]:
    return (anchor_a, anchor_b) if anchor_a < anchor_b else (anchor_b, anchor_a)


def _normalized_missing_pairs(
    missing_pairs: Iterable[tuple[str, str]],
    anchor_ids: Iterable[str],
    known_keys: Iterable[PairKey],
) -> frozenset[PairKey]:
    valid_ids = set(anchor_ids)
    known = set(known_keys)
    normalized: set[PairKey] = set()
    for raw_pair in missing_pairs:
        if len(raw_pair) != 2:
            raise ValueError("Each missing-edge constraint must contain two anchor IDs.")
        anchor_a = str(raw_pair[0]).strip()
        anchor_b = str(raw_pair[1]).strip()
        if not anchor_a or not anchor_b or anchor_a == anchor_b:
            raise ValueError("Missing-edge constraints require two different anchor IDs.")
        key = pair_key(anchor_a, anchor_b)
        if anchor_a not in valid_ids or anchor_b not in valid_ids:
            raise ValueError(f"Missing-edge constraint {anchor_a}-{anchor_b} references an unknown anchor.")
        if key in known:
            raise ValueError(f"Pair {anchor_a}-{anchor_b} cannot be both known and missing.")
        normalized.add(key)
    return frozenset(normalized)


def _normalized_neighbor_pairs(
    neighbor_pairs: Iterable[tuple[str, str]],
    anchor_ids: Iterable[str],
) -> frozenset[PairKey]:
    valid_ids = set(anchor_ids)
    normalized: set[PairKey] = set()
    for raw_pair in neighbor_pairs:
        if len(raw_pair) != 2:
            raise ValueError("Each neighbor constraint must contain two anchor IDs.")
        anchor_a = str(raw_pair[0]).strip()
        anchor_b = str(raw_pair[1]).strip()
        if not anchor_a or not anchor_b or anchor_a == anchor_b:
            raise ValueError("Neighbor constraints require two different anchor IDs.")
        if anchor_a not in valid_ids or anchor_b not in valid_ids:
            raise ValueError(
                f"Neighbor constraint {anchor_a}-{anchor_b} references an unknown anchor."
            )
        normalized.add(pair_key(anchor_a, anchor_b))
    return frozenset(normalized)


def _classical_mds_seed(pairs: list[AnchorPairDistance]) -> dict[str, tuple[float, float]]:
    processed = _preprocess_pairs(pairs, min_sigma_m=0.02, min_distance_m=0.05)
    anchor_ids = _anchor_ids(processed)
    index = {anchor_id: i for i, anchor_id in enumerate(anchor_ids)}
    count = len(anchor_ids)
    matrix = np.full((count, count), np.inf, dtype=float)
    np.fill_diagonal(matrix, 0.0)
    for pair in processed:
        left = index[pair.anchor_a_id]
        right = index[pair.anchor_b_id]
        if pair.distance_m < matrix[left, right]:
            matrix[left, right] = matrix[right, left] = pair.distance_m
    for intermediate in range(count):
        matrix = np.minimum(
            matrix,
            matrix[:, intermediate : intermediate + 1] + matrix[intermediate : intermediate + 1, :],
        )
    squared = matrix * matrix
    centering = np.eye(count) - np.full((count, count), 1.0 / count)
    gram = -0.5 * centering @ squared @ centering
    values, vectors = np.linalg.eigh(gram)
    order = np.argsort(values)[::-1][:2]
    coordinates = vectors[:, order] * np.sqrt(np.maximum(values[order], 0.0))
    positions = {
        anchor_id: (float(coordinates[row, 0]), float(coordinates[row, 1]))
        for anchor_id, row in index.items()
    }
    return rotate_layout_to_level(positions, anchor_ids[0], anchor_ids[1])


def _solve_from_seed(
    seed_positions: dict[str, tuple[float, float]],
    pairs: list[AnchorPairDistance],
    *,
    max_iterations: int,
) -> dict[str, tuple[float, float]]:
    processed = _preprocess_pairs(pairs, min_sigma_m=0.02, min_distance_m=0.05)
    anchor_ids = _anchor_ids(processed)
    parameterization = _Parameterization(anchor_ids)
    seed_positions = rotate_layout_to_level(seed_positions, anchor_ids[0], anchor_ids[1])
    parameters = _positions_to_params(parameterization, seed_positions)
    parameters, _energy = _local_minimize(
        parameters,
        parameterization,
        processed,
        max_iterations=max_iterations,
    )
    return rotate_layout_to_level(
        parameterization.to_positions(parameters),
        anchor_ids[0],
        anchor_ids[1],
    )


def _pair_metrics(
    positions: dict[str, tuple[float, float]],
    pairs: list[AnchorPairDistance],
) -> tuple[float, float]:
    values = list(pair_residuals(positions, pairs).values())
    if not values:
        return math.inf, math.inf
    return _rmse(values), max(abs(value) for value in values)


def range_graph(known_pairs: list[AnchorPairDistance]) -> RangeGraph:
    processed = _preprocess_pairs(known_pairs, min_sigma_m=0.02, min_distance_m=0.05)
    anchor_ids = tuple(_anchor_ids(processed))
    index = {anchor_id: i for i, anchor_id in enumerate(anchor_ids)}
    n = len(anchor_ids)
    shortest = np.full((n, n), INF, dtype=float)
    hops = np.full((n, n), 10**9, dtype=float)
    np.fill_diagonal(shortest, 0.0)
    np.fill_diagonal(hops, 0.0)
    distances: dict[tuple[str, str], float] = {}
    sigmas: dict[tuple[str, str], float] = {}
    degree = {anchor_id: 0 for anchor_id in anchor_ids}
    for pair in processed:
        key = pair_key(pair.anchor_a_id, pair.anchor_b_id)
        distances[key] = float(pair.distance_m)
        sigmas[key] = float(pair.sigma_m)
        degree[pair.anchor_a_id] += 1
        degree[pair.anchor_b_id] += 1
        i = index[pair.anchor_a_id]
        j = index[pair.anchor_b_id]
        if pair.distance_m < shortest[i, j]:
            shortest[i, j] = shortest[j, i] = float(pair.distance_m)
            hops[i, j] = hops[j, i] = 1.0
    for k in range(n):
        for i in range(n):
            via = shortest[i, k]
            if via >= INF:
                continue
            for j in range(n):
                candidate = via + shortest[k, j]
                candidate_hops = hops[i, k] + hops[k, j]
                if candidate < shortest[i, j] - 1e-9 or (
                    abs(candidate - shortest[i, j]) <= 1e-9 and candidate_hops < hops[i, j]
                ):
                    shortest[i, j] = candidate
                    hops[i, j] = candidate_hops
    return RangeGraph(anchor_ids, distances, sigmas, degree, shortest, hops, index)


def circle_intersections(
    center_a: tuple[float, float],
    radius_a: float,
    center_b: tuple[float, float],
    radius_b: float,
) -> list[tuple[float, float]]:
    ax, ay = center_a
    bx, by = center_b
    dx = bx - ax
    dy = by - ay
    d = math.hypot(dx, dy)
    if d < 1e-9:
        return []
    along = (radius_a * radius_a - radius_b * radius_b + d * d) / (2.0 * d)
    h2 = radius_a * radius_a - along * along
    ux = dx / d
    uy = dy / d
    px = ax + along * ux
    py = ay + along * uy
    if h2 <= 1e-10:
        return [(px, py)]
    h = math.sqrt(h2)
    ox = -uy * h
    oy = ux * h
    return [(px + ox, py + oy), (px - ox, py - oy)]


def _triangle_area(side_ab: float, side_ac: float, side_bc: float) -> float:
    semiperimeter = 0.5 * (side_ab + side_ac + side_bc)
    return math.sqrt(max(semiperimeter * (semiperimeter - side_ab) * (semiperimeter - side_ac) * (semiperimeter - side_bc), 0.0))


def _starting_triangle(graph: RangeGraph) -> tuple[str, str, str] | None:
    if len(graph.anchor_ids) < 3:
        return None
    first = min(
        graph.anchor_ids,
        key=lambda anchor_id: (
            -graph.degree[anchor_id],
            -sum(graph.degree[other] for other in graph.anchor_ids if pair_key(anchor_id, other) in graph.distances),
            anchor_id,
        ),
    )
    candidates: list[tuple[float, tuple[str, str, str]]] = []
    for a, b, c in itertools.combinations(graph.anchor_ids, 3):
        keys = (pair_key(a, b), pair_key(a, c), pair_key(b, c))
        if any(key not in graph.distances for key in keys):
            continue
        if first not in {a, b, c}:
            continue
        side_ab = graph.distances[keys[0]]
        side_ac = graph.distances[keys[1]]
        side_bc = graph.distances[keys[2]]
        sides = (side_ab, side_ac, side_bc)
        area = _triangle_area(side_ab, side_ac, side_bc)
        if area <= 1e-8:
            continue
        skinny = min(sides) / max(sides)
        degree_bonus = 1.0 + 0.03 * (graph.degree[a] + graph.degree[b] + graph.degree[c])
        candidates.append((area * skinny * degree_bonus, (a, b, c)))
    if not candidates:
        return None
    _score, triangle = max(candidates, key=lambda item: item[0])
    if triangle[0] == first:
        return triangle
    ordered = [first, *[anchor_id for anchor_id in triangle if anchor_id != first]]
    return ordered[0], ordered[1], ordered[2]


def _triangle_positions(triangle: tuple[str, str, str], graph: RangeGraph) -> dict[str, tuple[float, float]]:
    anchor_a, anchor_b, anchor_c = triangle
    side_ab = graph.distances[pair_key(anchor_a, anchor_b)]
    side_ac = graph.distances[pair_key(anchor_a, anchor_c)]
    side_bc = graph.distances[pair_key(anchor_b, anchor_c)]
    x_c = (side_ac * side_ac + side_ab * side_ab - side_bc * side_bc) / max(2.0 * side_ab, 1e-9)
    y2 = max(side_ac * side_ac - x_c * x_c, 0.0)
    return {
        anchor_a: (0.0, 0.0),
        anchor_b: (side_ab, 0.0),
        anchor_c: (x_c, math.sqrt(y2)),
    }


def _known_refs(anchor_id: str, positions: dict[str, tuple[float, float]], graph: RangeGraph) -> list[tuple[str, tuple[float, float], float, float]]:
    refs: list[tuple[str, tuple[float, float], float, float]] = []
    for other, point in positions.items():
        key = pair_key(anchor_id, other)
        if key in graph.distances:
            refs.append((other, point, graph.distances[key], graph.sigmas.get(key, 0.05)))
    return refs


def _refine_point(
    start: tuple[float, float],
    refs: list[tuple[str, tuple[float, float], float, float]],
    *,
    iterations: int,
) -> tuple[float, float]:
    point = np.array(start, dtype=float)
    for _ in range(max(iterations, 0)):
        jacobian_rows: list[np.ndarray] = []
        residuals: list[float] = []
        for _anchor_id, ref_point, target, sigma in refs:
            ref = np.array(ref_point, dtype=float)
            diff = point - ref
            distance = max(float(np.linalg.norm(diff)), 1e-9)
            sigma = max(float(sigma), 0.02)
            residuals.append((distance - target) / sigma)
            jacobian_rows.append(diff / distance / sigma)
        if len(residuals) < 2:
            break
        jacobian = np.vstack(jacobian_rows)
        residual = np.array(residuals, dtype=float)
        lhs = jacobian.T @ jacobian + np.eye(2) * 1e-4
        rhs = -(jacobian.T @ residual)
        try:
            step = np.linalg.solve(lhs, rhs)
        except np.linalg.LinAlgError:
            break
        if float(np.linalg.norm(step)) > 2.0:
            step = step / max(float(np.linalg.norm(step)), 1e-9) * 2.0
        point = point + step
        if float(np.linalg.norm(step)) < 1e-5:
            break
    return float(point[0]), float(point[1])


def _unique_points(points: list[tuple[float, float]]) -> list[tuple[float, float]]:
    seen: set[tuple[int, int]] = set()
    out: list[tuple[float, float]] = []
    for x, y in points:
        key = (round(x * 10000), round(y * 10000))
        if key in seen:
            continue
        seen.add(key)
        out.append((float(x), float(y)))
    return out


def _position_candidates(
    anchor_id: str,
    positions: dict[str, tuple[float, float]],
    graph: RangeGraph,
    *,
    one_link_angles: int,
    point_refine_iterations: int,
) -> list[tuple[float, float]]:
    refs = _known_refs(anchor_id, positions, graph)
    if len(refs) >= 2:
        seeds: list[tuple[float, float]] = []
        for left, right in itertools.combinations(refs, 2):
            _left_id, left_point, left_distance, _left_sigma = left
            _right_id, right_point, right_distance, _right_sigma = right
            seeds.extend(circle_intersections(left_point, left_distance, right_point, right_distance))
        if len(refs) >= 3:
            centroid = np.mean(np.array([ref[1] for ref in refs], dtype=float), axis=0)
            seeds.append((float(centroid[0]), float(centroid[1])))
            seeds = [_refine_point(seed, refs, iterations=point_refine_iterations) for seed in seeds]
        return _unique_points(seeds)
    if len(refs) == 1:
        _other_id, center, radius, _sigma = refs[0]
        count = max(one_link_angles, 4)
        return [
            (center[0] + math.cos(2.0 * math.pi * step / count) * radius, center[1] + math.sin(2.0 * math.pi * step / count) * radius)
            for step in range(count)
        ]
    return []


def _visibility_score(
    anchor_a: str,
    point_a: tuple[float, float],
    anchor_b: str,
    point_b: tuple[float, float],
    graph: RangeGraph,
    *,
    missing_pairs: frozenset[PairKey],
    neighbor_pairs: frozenset[PairKey],
    radio_radius_m: float,
    neighbor_max_m: float,
    missing_margin_m: float,
    missing_sigma_m: float,
    missing_weight: float,
    graph_upper_factor: float,
    graph_upper_slack_m: float,
    graph_upper_sigma_m: float,
    graph_upper_weight: float,
) -> float:
    distance = math.dist(point_a, point_b)
    key = pair_key(anchor_a, anchor_b)
    score = 0.0
    if key in graph.distances:
        sigma = max(graph.sigmas.get(key, 0.05), 0.02)
        residual = (distance - graph.distances[key]) / sigma
        score += residual * residual
    elif key in missing_pairs:
        lower_bound = radio_radius_m + missing_margin_m
        if distance < lower_bound:
            residual = (lower_bound - distance) / max(missing_sigma_m, 1e-6)
            score += missing_weight * residual * residual
    if key in neighbor_pairs and distance > neighbor_max_m:
        residual = (distance - neighbor_max_m) / max(graph_upper_sigma_m, 1e-6)
        score += missing_weight * residual * residual
    i = graph.index[anchor_a]
    j = graph.index[anchor_b]
    shortest = graph.shortest_m[i, j]
    if shortest < INF * 0.5 and graph.hops[i, j] >= 2:
        upper_bound = shortest * graph_upper_factor + graph_upper_slack_m
        if distance > upper_bound:
            residual = (distance - upper_bound) / max(graph_upper_sigma_m, 1e-6)
            score += graph_upper_weight * residual * residual
    return score


def _candidate_score(
    anchor_id: str,
    point: tuple[float, float],
    positions: dict[str, tuple[float, float]],
    graph: RangeGraph,
    params: dict[str, Any],
) -> float:
    return sum(
        _visibility_score(anchor_id, point, other_id, other_point, graph, **params)
        for other_id, other_point in positions.items()
    )


def _next_anchor(unplaced: set[str], placed: set[str], graph: RangeGraph) -> str:
    def key(anchor_id: str) -> tuple[int, int, str]:
        links_to_placed = sum(1 for other in placed if pair_key(anchor_id, other) in graph.distances)
        tier = links_to_placed if links_to_placed >= 2 else links_to_placed - 10
        return -tier, -graph.degree[anchor_id], anchor_id

    return min(unplaced, key=key)


def visibility_branching_seed_layouts(
    known_pairs: list[AnchorPairDistance],
    *,
    missing_pairs: Iterable[tuple[str, str]] = (),
    neighbor_pairs: Iterable[tuple[str, str]] = (),
    beam_width: int = 32,
    radio_radius_m: float = 8.0,
    neighbor_max_m: float = math.inf,
    missing_margin_m: float = 0.0,
    missing_sigma_m: float = 0.75,
    missing_weight: float = 1.0,
    graph_upper_factor: float = 1.0,
    graph_upper_slack_m: float = 0.75,
    graph_upper_sigma_m: float = 1.0,
    graph_upper_weight: float = 0.35,
    one_link_angles: int = 16,
    point_refine_iterations: int = 12,
) -> list[dict[str, tuple[float, float]]]:
    graph = range_graph(known_pairs)
    normalized_missing = _normalized_missing_pairs(
        missing_pairs,
        graph.anchor_ids,
        graph.distances,
    )
    normalized_neighbors = _normalized_neighbor_pairs(
        neighbor_pairs,
        graph.anchor_ids,
    )
    if normalized_missing & normalized_neighbors:
        raise ValueError("A pair cannot be both a neighbor and a non-neighbor.")
    triangle = _starting_triangle(graph)
    if triangle is None:
        return [_classical_mds_seed(known_pairs)]
    partials = [PartialLayout(_triangle_positions(triangle, graph), 0.0)]
    score_params: dict[str, Any] = {
        "missing_pairs": normalized_missing,
        "neighbor_pairs": normalized_neighbors,
        "radio_radius_m": radio_radius_m,
        "neighbor_max_m": neighbor_max_m,
        "missing_margin_m": missing_margin_m,
        "missing_sigma_m": missing_sigma_m,
        "missing_weight": missing_weight,
        "graph_upper_factor": graph_upper_factor,
        "graph_upper_slack_m": graph_upper_slack_m,
        "graph_upper_sigma_m": graph_upper_sigma_m,
        "graph_upper_weight": graph_upper_weight,
    }
    while len(partials[0].positions) < len(graph.anchor_ids):
        placed = set(partials[0].positions)
        unplaced = set(graph.anchor_ids) - placed
        anchor_id = _next_anchor(unplaced, placed, graph)
        expanded: list[PartialLayout] = []
        for partial in partials:
            candidates = _position_candidates(
                anchor_id,
                partial.positions,
                graph,
                one_link_angles=one_link_angles,
                point_refine_iterations=point_refine_iterations,
            )
            for point in candidates:
                score = partial.score + _candidate_score(anchor_id, point, partial.positions, graph, score_params)
                positions = dict(partial.positions)
                positions[anchor_id] = point
                expanded.append(PartialLayout(positions, score))
        if not expanded:
            return [_classical_mds_seed(known_pairs)]
        expanded.sort(key=lambda partial: partial.score)
        partials = expanded[: max(1, beam_width)]
    return [partial.positions for partial in partials]


def visibility_score_all(
    positions: dict[str, tuple[float, float]],
    known_pairs: list[AnchorPairDistance],
    *,
    missing_pairs: Iterable[tuple[str, str]] = (),
    neighbor_pairs: Iterable[tuple[str, str]] = (),
    radio_radius_m: float = 8.0,
    neighbor_max_m: float = math.inf,
    missing_margin_m: float = 0.0,
    missing_sigma_m: float = 0.75,
    missing_weight: float = 1.0,
    graph_upper_factor: float = 1.0,
    graph_upper_slack_m: float = 0.75,
    graph_upper_sigma_m: float = 1.0,
    graph_upper_weight: float = 0.35,
) -> float:
    graph = range_graph(known_pairs)
    normalized_missing = _normalized_missing_pairs(
        missing_pairs,
        graph.anchor_ids,
        graph.distances,
    )
    normalized_neighbors = _normalized_neighbor_pairs(
        neighbor_pairs,
        graph.anchor_ids,
    )
    if normalized_missing & normalized_neighbors:
        raise ValueError("A pair cannot be both a neighbor and a non-neighbor.")
    score_params = {
        "missing_pairs": normalized_missing,
        "neighbor_pairs": normalized_neighbors,
        "radio_radius_m": radio_radius_m,
        "neighbor_max_m": neighbor_max_m,
        "missing_margin_m": missing_margin_m,
        "missing_sigma_m": missing_sigma_m,
        "missing_weight": missing_weight,
        "graph_upper_factor": graph_upper_factor,
        "graph_upper_slack_m": graph_upper_slack_m,
        "graph_upper_sigma_m": graph_upper_sigma_m,
        "graph_upper_weight": graph_upper_weight,
    }
    score = 0.0
    count = 0
    for index, anchor_a in enumerate(graph.anchor_ids):
        if anchor_a not in positions:
            continue
        for anchor_b in graph.anchor_ids[index + 1 :]:
            if anchor_b not in positions:
                continue
            score += _visibility_score(
                anchor_a, positions[anchor_a], anchor_b, positions[anchor_b], graph,
                missing_pairs=normalized_missing, radio_radius_m=radio_radius_m,
                neighbor_pairs=normalized_neighbors,
                neighbor_max_m=neighbor_max_m,
                missing_margin_m=missing_margin_m, missing_sigma_m=missing_sigma_m,
                missing_weight=missing_weight, graph_upper_factor=graph_upper_factor,
                graph_upper_slack_m=graph_upper_slack_m,
                graph_upper_sigma_m=graph_upper_sigma_m,
                graph_upper_weight=graph_upper_weight,
            )
            count += 1
    return score / max(count, 1)


def visibility_branching_solve(
    known_pairs: list[AnchorPairDistance],
    *,
    missing_pairs: Iterable[tuple[str, str]] = (),
    neighbor_pairs: Iterable[tuple[str, str]] = (),
    beam_width: int = 32,
    optimizer_seeds: int = 32,
    iterations: int = 45,
    radio_radius_m: float = 8.0,
    neighbor_max_m: float = math.inf,
    final_visibility_weight: float = 1.0,
    constrained_polish: bool = False,
    constrained_iterations: int | None = None,
    constrained_known_weight: float = 1.0,
    **visibility_params: Any,
) -> dict[str, tuple[float, float]]:
    seed_params = {
        "radio_radius_m": radio_radius_m,
        "neighbor_max_m": neighbor_max_m,
        "missing_pairs": missing_pairs,
        "neighbor_pairs": neighbor_pairs,
        **visibility_params,
    }
    seeds = visibility_branching_seed_layouts(known_pairs, beam_width=beam_width, **seed_params)
    score_keys = {
        "missing_margin_m",
        "missing_sigma_m",
        "missing_weight",
        "graph_upper_factor",
        "graph_upper_slack_m",
        "graph_upper_sigma_m",
        "graph_upper_weight",
    }
    score_params = {key: value for key, value in visibility_params.items() if key in score_keys}
    best_positions: dict[str, tuple[float, float]] | None = None
    best_score = math.inf
    optimizer_errors: list[Exception] = []
    for seed in seeds[: max(1, optimizer_seeds)]:
        try:
            if constrained_polish:
                positions = visibility_constrained_solve_from_seed(
                    seed,
                    known_pairs,
                    max_iterations=constrained_iterations or iterations,
                    radio_radius_m=radio_radius_m,
                    neighbor_max_m=neighbor_max_m,
                    known_weight=constrained_known_weight,
                    missing_pairs=missing_pairs,
                    neighbor_pairs=neighbor_pairs,
                    **score_params,
                )
            else:
                positions = _solve_from_seed(seed, known_pairs, max_iterations=iterations)
        except Exception as exc:
            optimizer_errors.append(exc)
            continue
        known_rmse, _known_max = _pair_metrics(positions, known_pairs)
        visibility = visibility_score_all(
            positions,
            known_pairs,
            radio_radius_m=radio_radius_m,
            neighbor_max_m=neighbor_max_m,
            missing_pairs=missing_pairs,
            neighbor_pairs=neighbor_pairs,
            **score_params,
        )
        score = known_rmse * known_rmse + final_visibility_weight * visibility
        if score < best_score:
            best_score = score
            best_positions = positions
    if best_positions is None:
        detail = str(optimizer_errors[-1]) if optimizer_errors else "no candidate layout was produced"
        raise RuntimeError(f"Visibility branching tuned failed: {detail}")
    return best_positions



def _canonical_seed_positions(
    seed_positions: dict[str, tuple[float, float]],
    anchor_ids: tuple[str, ...],
    known_pairs: list[AnchorPairDistance],
) -> dict[str, tuple[float, float]]:
    if any(anchor_id not in seed_positions for anchor_id in anchor_ids):
        seed_positions = _classical_mds_seed(known_pairs)
    try:
        return rotate_layout_to_level(seed_positions, anchor_ids[0], anchor_ids[1])
    except Exception:
        ax, ay = seed_positions.get(anchor_ids[0], (0.0, 0.0))
        return {anchor_id: (seed_positions[anchor_id][0] - ax, seed_positions[anchor_id][1] - ay) for anchor_id in anchor_ids}


def visibility_constrained_solve_from_seed(
    seed_positions: dict[str, tuple[float, float]],
    known_pairs: list[AnchorPairDistance],
    *,
    max_iterations: int,
    missing_pairs: Iterable[tuple[str, str]] = (),
    neighbor_pairs: Iterable[tuple[str, str]] = (),
    radio_radius_m: float = 8.0,
    neighbor_max_m: float = math.inf,
    missing_margin_m: float = 0.0,
    missing_sigma_m: float = 0.75,
    missing_weight: float = 1.0,
    graph_upper_factor: float = 1.0,
    graph_upper_slack_m: float = 0.75,
    graph_upper_sigma_m: float = 1.0,
    graph_upper_weight: float = 0.35,
    known_weight: float = 1.0,
) -> dict[str, tuple[float, float]]:
    try:
        from scipy.optimize import least_squares  # type: ignore[import-untyped]
    except Exception as exc:  # pragma: no cover - scipy is in requirements.
        raise RuntimeError("visibility-constrained polish requires scipy") from exc

    graph = range_graph(known_pairs)
    normalized_missing = _normalized_missing_pairs(
        missing_pairs,
        graph.anchor_ids,
        graph.distances,
    )
    normalized_neighbors = _normalized_neighbor_pairs(
        neighbor_pairs,
        graph.anchor_ids,
    )
    if normalized_missing & normalized_neighbors:
        raise ValueError("A pair cannot be both a neighbor and a non-neighbor.")
    processed = _preprocess_pairs(known_pairs, min_sigma_m=0.02, min_distance_m=0.05)
    anchor_ids = graph.anchor_ids
    parameterization = _Parameterization(list(anchor_ids))
    seed_positions = _canonical_seed_positions(seed_positions, anchor_ids, known_pairs)
    x0 = np.asarray(_positions_to_params(parameterization, seed_positions), dtype=float)
    if x0.size == 0:
        return seed_positions
    sqrt_known = math.sqrt(max(known_weight, 0.0))
    sqrt_missing = math.sqrt(max(missing_weight, 0.0))
    sqrt_graph = math.sqrt(max(graph_upper_weight, 0.0))
    lower_bound = radio_radius_m + missing_margin_m

    def residuals(params: np.ndarray) -> np.ndarray:
        positions = parameterization.to_positions(params.tolist())
        values: list[float] = []
        if sqrt_known > 0.0:
            for pair in processed:
                ax, ay = positions[pair.anchor_a_id]
                bx, by = positions[pair.anchor_b_id]
                distance = math.hypot(ax - bx, ay - by)
                sigma = max(pair.sigma_m, 0.02)
                values.append(sqrt_known * (distance - pair.distance_m) / sigma)
        for i, anchor_a in enumerate(anchor_ids):
            ax, ay = positions[anchor_a]
            for j in range(i + 1, len(anchor_ids)):
                anchor_b = anchor_ids[j]
                bx, by = positions[anchor_b]
                distance = math.hypot(ax - bx, ay - by)
                key = pair_key(anchor_a, anchor_b)
                if key in normalized_missing and sqrt_missing > 0.0:
                    violation = lower_bound - distance
                    if violation > 0.0:
                        values.append(sqrt_missing * violation / max(missing_sigma_m, 1e-6))
                    else:
                        values.append(0.0)
                if key in normalized_neighbors and sqrt_missing > 0.0:
                    violation = distance - neighbor_max_m
                    if violation > 0.0:
                        values.append(
                            sqrt_missing
                            * violation
                            / max(missing_sigma_m, 1e-6)
                        )
                    else:
                        values.append(0.0)
                shortest = graph.shortest_m[i, j]
                if shortest < INF * 0.5 and graph.hops[i, j] >= 2 and sqrt_graph > 0.0:
                    upper_bound = shortest * graph_upper_factor + graph_upper_slack_m
                    violation = distance - upper_bound
                    if violation > 0.0:
                        values.append(sqrt_graph * violation / max(graph_upper_sigma_m, 1e-6))
                    else:
                        values.append(0.0)
        return np.asarray(values, dtype=float)

    result = least_squares(
        residuals,
        x0,
        max_nfev=max(1, int(max_iterations)),
        method="trf",
        x_scale="jac",
    )
    params = result.x if result.x is not None else x0
    positions = parameterization.to_positions(params.tolist())
    try:
        return rotate_layout_to_level(positions, anchor_ids[0], anchor_ids[1])
    except Exception:
        return positions


def solve_visibility_branching_tuned(
    pairs: Iterable[AnchorPairDistance],
    *,
    missing_pairs: Iterable[tuple[str, str]] = (),
    neighbor_pairs: Iterable[tuple[str, str]] = (),
    seed: str = SEED_AUTO,
    current_positions_m: dict[str, tuple[float, float]] | None = None,
    nonneighbor_min_m: float = 7.0,
    neighbor_max_m: float = 15.0,
    random_seed: int = 1337,
    neighbor_evidence_precedence: bool = False,
) -> AnchorLayoutResult:
    """Solve with tuned visibility branching from the selected seed family.

    ``random_seed`` is part of the stable solver interface. The upstream tuned
    branching path is deterministic and does not consume randomness.
    """

    del random_seed
    if seed not in GEOMETRY_SEEDS:
        raise ValueError(f"Unknown geometry seed: {seed}")
    if not 0.0 < nonneighbor_min_m <= neighbor_max_m:
        raise ValueError("Radio interval must satisfy 0 < minimum <= maximum.")
    known_pairs = list(pairs)
    processed = _preprocess_pairs(known_pairs, min_sigma_m=0.02, min_distance_m=0.05)
    anchor_ids = _anchor_ids(processed)
    _validate_connected(anchor_ids, processed)
    graph = range_graph(known_pairs)
    normalized_missing = _normalized_missing_pairs(
        missing_pairs,
        graph.anchor_ids,
        graph.distances,
    )
    normalized_neighbors = _normalized_neighbor_pairs(
        neighbor_pairs,
        graph.anchor_ids,
    )
    overlap = normalized_missing & normalized_neighbors
    if overlap:
        if neighbor_evidence_precedence:
            normalized_missing = frozenset(normalized_missing - overlap)
        else:
            raise ValueError("A pair cannot be both a neighbor and a non-neighbor.")

    parameters = dict(VISIBILITY_BRANCHING_TUNED_PARAMETERS)
    parameters["radio_radius_m"] = nonneighbor_min_m
    parameters["missing_margin_m"] = 0.0
    base_seeds: list[tuple[str, dict[str, tuple[float, float]]]] = []
    if seed in (SEED_AUTO, SEED_CURRENT):
        if current_positions_m is not None:
            if set(current_positions_m) != set(anchor_ids):
                raise ValueError("Current layout seed does not match the anchor graph.")
            base_seeds.append((SEED_CURRENT, dict(current_positions_m)))
        elif seed == SEED_CURRENT:
            raise ValueError("No current solved layout is available as a seed.")

    requested = (
        (SEED_VISIBILITY, SEED_SPRING, SEED_GRAPH_MDS)
        if seed == SEED_AUTO
        else (seed,)
    )
    for seed_name in requested:
        try:
            if seed_name == SEED_VISIBILITY:
                native = visibility_branching_seed_layouts(
                    known_pairs,
                    missing_pairs=normalized_missing,
                    neighbor_pairs=normalized_neighbors,
                    beam_width=int(parameters["beam_width"]),
                    radio_radius_m=nonneighbor_min_m,
                    neighbor_max_m=neighbor_max_m,
                    missing_margin_m=0.0,
                    missing_sigma_m=float(parameters["missing_sigma_m"]),
                    missing_weight=float(parameters["missing_weight"]),
                    graph_upper_factor=float(parameters["graph_upper_factor"]),
                    graph_upper_slack_m=float(parameters["graph_upper_slack_m"]),
                    graph_upper_sigma_m=float(parameters["graph_upper_sigma_m"]),
                    graph_upper_weight=float(parameters["graph_upper_weight"]),
                )
                base_seeds.extend(
                    (SEED_VISIBILITY, positions)
                    for positions in native[
                        : max(1, int(parameters["optimizer_seeds"]))
                    ]
                )
            elif seed_name == SEED_SPRING:
                base_seeds.append(
                    (SEED_SPRING, solve_anchor_layout(known_pairs).positions_m)
                )
            elif seed_name == SEED_GRAPH_MDS:
                base_seeds.append(
                    (SEED_GRAPH_MDS, graph_mds_seed(known_pairs, anchor_ids))
                )
            elif seed_name != SEED_CURRENT:
                raise ValueError(f"Unknown geometry seed: {seed_name}")
        except Exception:
            if seed != SEED_AUTO:
                raise
    if not base_seeds:
        raise ValueError("No geometry seed could be produced.")

    candidates: list[
        tuple[float, str, dict[str, tuple[float, float]]]
    ] = []
    optimizer_errors: list[Exception] = []
    for seed_name, base_positions in base_seeds:
        try:
            if bool(parameters["constrained_polish"]):
                candidate = visibility_constrained_solve_from_seed(
                    base_positions,
                    known_pairs,
                    max_iterations=int(parameters["constrained_iterations"]),
                    missing_pairs=normalized_missing,
                    neighbor_pairs=normalized_neighbors,
                    radio_radius_m=nonneighbor_min_m,
                    neighbor_max_m=neighbor_max_m,
                    missing_margin_m=0.0,
                    missing_sigma_m=float(parameters["missing_sigma_m"]),
                    missing_weight=float(parameters["missing_weight"]),
                    graph_upper_factor=float(parameters["graph_upper_factor"]),
                    graph_upper_slack_m=float(parameters["graph_upper_slack_m"]),
                    graph_upper_sigma_m=float(parameters["graph_upper_sigma_m"]),
                    graph_upper_weight=float(parameters["graph_upper_weight"]),
                    known_weight=float(parameters["constrained_known_weight"]),
                )
            else:
                candidate = _solve_from_seed(
                    base_positions,
                    known_pairs,
                    max_iterations=int(parameters["iterations"]),
                )
            known_rmse, _known_max = _pair_metrics(candidate, known_pairs)
            visibility = visibility_score_all(
                candidate,
                known_pairs,
                missing_pairs=normalized_missing,
                neighbor_pairs=normalized_neighbors,
                radio_radius_m=nonneighbor_min_m,
                neighbor_max_m=neighbor_max_m,
                missing_margin_m=0.0,
                missing_sigma_m=float(parameters["missing_sigma_m"]),
                missing_weight=float(parameters["missing_weight"]),
                graph_upper_factor=float(parameters["graph_upper_factor"]),
                graph_upper_slack_m=float(parameters["graph_upper_slack_m"]),
                graph_upper_sigma_m=float(parameters["graph_upper_sigma_m"]),
                graph_upper_weight=float(parameters["graph_upper_weight"]),
            )
            score = (
                known_rmse * known_rmse
                + float(parameters["final_visibility_weight"]) * visibility
            )
            candidates.append((score, seed_name, candidate))
        except Exception as exc:
            optimizer_errors.append(exc)
    if not candidates:
        detail = (
            str(optimizer_errors[-1])
            if optimizer_errors
            else "no candidate layout was produced"
        )
        raise RuntimeError(f"Visibility branching tuned failed: {detail}")

    _score, selected_seed, positions = min(candidates, key=lambda item: item[0])
    positions = rotate_layout_to_level(positions, anchor_ids[0], anchor_ids[1])
    positions = _clean_positions(positions)
    residuals = pair_residuals(positions, processed)
    rmse = _rmse(residuals.values())
    max_residual = max((abs(value) for value in residuals.values()), default=0.0)
    parameterization = _Parameterization(anchor_ids)
    energy = _spring_energy(
        _positions_to_params(parameterization, positions),
        parameterization,
        processed,
    )
    warnings = list(_layout_warnings(anchor_ids, processed, rmse, max_residual))
    neighbor_violations = sum(
        math.dist(positions[first], positions[second]) > neighbor_max_m + 1e-6
        for first, second in normalized_neighbors
    )
    nonneighbor_violations = sum(
        math.dist(positions[first], positions[second]) < nonneighbor_min_m - 1e-6
        for first, second in normalized_missing
    )
    if neighbor_violations:
        warnings.append(
            f"{neighbor_violations} neighbor interval(s) exceed {neighbor_max_m:.1f} m"
        )
    if nonneighbor_violations:
        warnings.append(
            f"{nonneighbor_violations} non-neighbor interval(s) are below "
            f"{nonneighbor_min_m:.1f} m"
        )
    return AnchorLayoutResult(
        algorithm=(
            f"{VISIBILITY_BRANCHING_TUNED_ALGORITHM} "
            f"({nonneighbor_min_m:g}-{neighbor_max_m:g} m); "
            f"seed {selected_seed}"
        ),
        energy=energy,
        rmse_m=rmse,
        max_residual_m=max_residual,
        positions_m=positions,
        processed_pairs=tuple(processed),
        residuals_m=residuals,
        warnings=tuple(warnings),
        seed_count=len(candidates),
        basin_hop_count=0,
    )


def solve_visibility_branching_neighbor_aware_tuned(
    pairs: Iterable[AnchorPairDistance],
    *,
    missing_pairs: Iterable[tuple[str, str]] = (),
    neighbor_pairs: Iterable[tuple[str, str]] = (),
    seed: str = SEED_AUTO,
    current_positions_m: dict[str, tuple[float, float]] | None = None,
    nonneighbor_min_m: float = 7.0,
    neighbor_max_m: float = 15.0,
    random_seed: int = 1337,
) -> AnchorLayoutResult:
    """Visibility tuning where surveyed-neighbor evidence protects an edge.

    A pair present in the measured neighbor graph is never given a lower-bound
    missing-edge penalty merely because the degree-capped ranging plan omitted
    it. Positive neighbor evidence may still apply the configured upper bound.
    """

    result = solve_visibility_branching_tuned(
        pairs,
        missing_pairs=missing_pairs,
        neighbor_pairs=neighbor_pairs,
        seed=seed,
        current_positions_m=current_positions_m,
        nonneighbor_min_m=nonneighbor_min_m,
        neighbor_max_m=neighbor_max_m,
        random_seed=random_seed,
        neighbor_evidence_precedence=True,
    )
    return replace(
        result,
        algorithm=result.algorithm.replace(
            VISIBILITY_BRANCHING_TUNED_ALGORITHM,
            VISIBILITY_BRANCHING_NEIGHBOR_AWARE_ALGORITHM,
            1,
        ),
    )
