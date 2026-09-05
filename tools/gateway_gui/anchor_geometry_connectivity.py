"""Neighbor-interval anchor solver for capped survey ranging graphs."""

from __future__ import annotations

from collections.abc import Iterable
import math
import random

import numpy as np

from .anchor_geometry import (
    AnchorLayoutResult,
    AnchorPairDistance,
    _Parameterization,
    _anchor_ids,
    _clean_positions,
    _layout_warnings,
    _positions_to_params,
    _preprocess_pairs,
    _rmse,
    _validate_connected,
    pair_residuals,
    rotate_layout_to_level,
    solve_anchor_layout,
)
from .anchor_geometry_visibility import solve_visibility_branching_tuned
from .anchor_geometry_seeds import (
    GEOMETRY_SEEDS,
    SEED_AUTO,
    SEED_CURRENT,
    SEED_GRAPH_MDS,
    SEED_SPRING,
    SEED_VISIBILITY,
    graph_mds_seed,
)


CONNECTIVITY_INTERVAL_ALGORITHM = "Neighbor intervals"
DEFAULT_NONNEIGHBOR_MIN_M = 7.0
# Retain the old public name for callers outside the GUI package.
NONNEIGHBOR_MIN_M = DEFAULT_NONNEIGHBOR_MIN_M
DEFAULT_NEIGHBOR_MAX_M = 15.0
CONNECTIVITY_SEEDS = GEOMETRY_SEEDS

PairKey = tuple[str, str]


def pair_key(anchor_a: str, anchor_b: str) -> PairKey:
    return (anchor_a, anchor_b) if anchor_a < anchor_b else (anchor_b, anchor_a)


def _normalize_constraints(
    pairs: Iterable[tuple[str, str]],
    anchors: set[str],
) -> frozenset[PairKey]:
    normalized: set[PairKey] = set()
    for anchor_a, anchor_b in pairs:
        if anchor_a == anchor_b or anchor_a not in anchors or anchor_b not in anchors:
            raise ValueError("Neighbor constraints must name two solved anchors.")
        normalized.add(pair_key(anchor_a, anchor_b))
    return frozenset(normalized)


def _seed_families(
    pairs: list[AnchorPairDistance],
    anchor_ids: list[str],
    nonneighbor_pairs: frozenset[PairKey],
    seed: str,
    current_positions_m: dict[str, tuple[float, float]] | None,
) -> list[tuple[str, dict[str, tuple[float, float]]]]:
    factories = {
        SEED_VISIBILITY: lambda: solve_visibility_branching_tuned(
            pairs,
            missing_pairs=nonneighbor_pairs,
        ).positions_m,
        SEED_SPRING: lambda: solve_anchor_layout(pairs).positions_m,
        SEED_GRAPH_MDS: lambda: graph_mds_seed(pairs, anchor_ids),
    }
    seeds: list[tuple[str, dict[str, tuple[float, float]]]] = []
    requested = (SEED_VISIBILITY, SEED_SPRING, SEED_GRAPH_MDS) if seed == SEED_AUTO else (seed,)
    if seed in (SEED_AUTO, SEED_CURRENT) and current_positions_m is not None:
        if set(current_positions_m) != set(anchor_ids):
            raise ValueError("Current layout seed does not match the anchor graph.")
        seeds.append((SEED_CURRENT, dict(current_positions_m)))
    elif seed == SEED_CURRENT:
        raise ValueError("No current solved layout is available as a seed.")
    for name in requested:
        factory = factories.get(name)
        if factory is None:
            if name != SEED_CURRENT:
                raise ValueError(f"Unknown geometry seed: {name}")
            continue
        try:
            seeds.append((name, factory()))
        except Exception:
            if seed != SEED_AUTO:
                raise
    if not seeds:
        raise ValueError("No geometry seed could be produced.")
    return seeds


def solve_connectivity_interval_layout(
    pairs: Iterable[AnchorPairDistance],
    *,
    neighbor_pairs: Iterable[tuple[str, str]],
    nonneighbor_pairs: Iterable[tuple[str, str]],
    seed: str = SEED_AUTO,
    current_positions_m: dict[str, tuple[float, float]] | None = None,
    fixed_positions_m: dict[str, tuple[float, float]] | None = None,
    neighbor_max_m: float = DEFAULT_NEIGHBOR_MAX_M,
    nonneighbor_min_m: float = DEFAULT_NONNEIGHBOR_MIN_M,
    interval_sigma_m: float = 0.75,
    max_nfev: int = 600,
) -> AnchorLayoutResult:
    """Solve measured ranges plus radio-neighbor interval constraints."""

    if not 0.0 < nonneighbor_min_m <= neighbor_max_m:
        raise ValueError("Radio interval must satisfy 0 < minimum <= maximum.")
    if interval_sigma_m <= 0.0:
        raise ValueError("Radio interval sigma must be positive.")
    known_pairs = list(pairs)
    processed = _preprocess_pairs(known_pairs, min_sigma_m=0.02, min_distance_m=0.05)
    anchor_ids = _anchor_ids(processed)
    _validate_connected(anchor_ids, processed)
    anchor_set = set(anchor_ids)
    neighbors = _normalize_constraints(neighbor_pairs, anchor_set)
    nonneighbors = _normalize_constraints(nonneighbor_pairs, anchor_set)
    overlap = neighbors & nonneighbors
    if overlap:
        raise ValueError("A pair cannot be both a neighbor and a non-neighbor.")
    measured = {pair_key(pair.anchor_a_id, pair.anchor_b_id) for pair in processed}
    if measured & nonneighbors:
        raise ValueError("A measured pair cannot be constrained as a non-neighbor.")

    try:
        from scipy.optimize import least_squares  # type: ignore[import-untyped]
    except Exception as exc:  # pragma: no cover - scipy is a GUI requirement.
        raise RuntimeError("neighbor-interval solver requires scipy") from exc

    parameterization = _Parameterization(anchor_ids, fixed_positions_m)
    base_seeds = _seed_families(
        known_pairs,
        anchor_ids,
        nonneighbors,
        seed,
        current_positions_m,
    )
    candidates: list[tuple[float, str, dict[str, tuple[float, float]]]] = []
    rng = random.Random(1337)
    for seed_name, base_positions in base_seeds:
        oriented = base_positions if fixed_positions_m else rotate_layout_to_level(
            base_positions, anchor_ids[0], anchor_ids[1],
        )
        starts = [_positions_to_params(parameterization, oriented)]
        if seed == SEED_AUTO:
            scale = max(
                (pair.distance_m for pair in processed),
                default=1.0,
            )
            for _ in range(2):
                starts.append([
                    value + rng.gauss(0.0, 0.08 * scale)
                    for value in starts[0]
                ])

        def residuals(params: np.ndarray) -> np.ndarray:
            positions = parameterization.to_positions(params.tolist())
            values: list[float] = []
            for pair in processed:
                distance = math.dist(
                    positions[pair.anchor_a_id],
                    positions[pair.anchor_b_id],
                )
                values.append((distance - pair.distance_m) / pair.sigma_m)
            for anchor_a, anchor_b in neighbors:
                distance = math.dist(positions[anchor_a], positions[anchor_b])
                values.append(max(0.0, distance - neighbor_max_m) / interval_sigma_m)
            for anchor_a, anchor_b in nonneighbors:
                distance = math.dist(positions[anchor_a], positions[anchor_b])
                values.append(max(0.0, nonneighbor_min_m - distance) / interval_sigma_m)
            return np.asarray(values, dtype=float)

        for start in starts:
            params = np.asarray(start, dtype=float)
            if params.size:
                params = least_squares(
                    residuals,
                    params,
                    max_nfev=max(1, max_nfev),
                    method="trf",
                    x_scale="jac",
                ).x
            positions = parameterization.to_positions(params.tolist())
            errors = residuals(params)
            objective = float(np.dot(errors, errors))
            candidates.append((objective, seed_name, positions))

    objective, selected_seed, positions = min(candidates, key=lambda item: item[0])
    if not fixed_positions_m:
        positions = rotate_layout_to_level(positions, anchor_ids[0], anchor_ids[1])
    positions = _clean_positions(positions)
    positions.update(parameterization.fixed_positions_m)
    residual_map = pair_residuals(positions, processed)
    rmse = _rmse(residual_map.values())
    max_residual = max((abs(value) for value in residual_map.values()), default=0.0)
    interval_warnings: list[str] = []
    neighbor_violations = [
        math.dist(positions[a], positions[b]) - neighbor_max_m
        for a, b in neighbors
        if math.dist(positions[a], positions[b]) > neighbor_max_m + 1e-6
    ]
    nonneighbor_violations = [
        nonneighbor_min_m - math.dist(positions[a], positions[b])
        for a, b in nonneighbors
        if math.dist(positions[a], positions[b]) < nonneighbor_min_m - 1e-6
    ]
    if neighbor_violations:
        interval_warnings.append(
            f"{len(neighbor_violations)} neighbor interval(s) exceed {neighbor_max_m:.1f} m"
        )
    if nonneighbor_violations:
        interval_warnings.append(
            f"{len(nonneighbor_violations)} non-neighbor interval(s) are below {nonneighbor_min_m:.1f} m"
        )
    warnings = [
        *_layout_warnings(anchor_ids, processed, rmse, max_residual),
        *interval_warnings,
    ]
    return AnchorLayoutResult(
        algorithm=(
            f"{CONNECTIVITY_INTERVAL_ALGORITHM} "
            f"({nonneighbor_min_m:g}-{neighbor_max_m:g} m); seed {selected_seed}"
        ),
        energy=objective,
        rmse_m=rmse,
        max_residual_m=max_residual,
        positions_m=positions,
        processed_pairs=tuple(processed),
        residuals_m=residual_map,
        warnings=tuple(warnings),
        seed_count=len(candidates),
        basin_hop_count=0,
    )
