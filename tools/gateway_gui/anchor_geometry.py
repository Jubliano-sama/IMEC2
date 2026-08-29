"""Anchor-to-anchor spring layout solver.

The clicker survey provides distances between anchors, not absolute anchor
coordinates. This module treats those distances as springs and finds the 2D
anchor layout with the lowest spring energy. The solver is dependency-free so
the GUI can run on the same lightweight install as the capture tools.
"""

from __future__ import annotations

from dataclasses import dataclass
import math
import random
from statistics import median
from typing import Iterable, TypedDict


ANCHOR_LAYOUT_ALGORITHM = "Spring energy basin hopping (multi-seed LM)"
DISTANCE_ONLY_REFINEMENT_ALGORITHM = "Seeded measured-distance least squares"
MANUALLY_EDITED_LAYOUT_ALGORITHM = "Manually edited layout"


class _PairAggregate(TypedDict):
    weighted_distance: float
    weight: float
    sources: set[str]


@dataclass(frozen=True)
class AnchorPairDistance:
    """One enabled anchor-to-anchor distance measurement."""

    anchor_a_id: str
    anchor_b_id: str
    distance_m: float
    sigma_m: float = 0.05
    enabled: bool = True
    source: str = "survey"


@dataclass(frozen=True)
class ProcessedAnchorPair:
    """Validated and de-duplicated spring constraint."""

    anchor_a_id: str
    anchor_b_id: str
    distance_m: float
    sigma_m: float
    weight: float
    source: str


@dataclass(frozen=True)
class AnchorLayoutResult:
    """Solved anchor layout and residual diagnostics."""

    algorithm: str
    energy: float
    rmse_m: float
    max_residual_m: float
    positions_m: dict[str, tuple[float, float]]
    processed_pairs: tuple[ProcessedAnchorPair, ...]
    residuals_m: dict[str, float]
    warnings: tuple[str, ...]
    seed_count: int
    basin_hop_count: int


def solve_anchor_layout(
    pairs: Iterable[AnchorPairDistance],
    *,
    seed_count: int = 24,
    basin_hops: int = 10,
    max_iterations: int = 80,
    random_seed: int = 1337,
    min_sigma_m: float = 0.02,
    min_distance_m: float = 0.05,
) -> AnchorLayoutResult:
    """Solve anchor coordinates from pair distances.

    Distances only constrain shape, so the result is arbitrary up to
    translation, rotation, and mirror reflection. The returned layout is
    canonicalized with the first two sorted anchor IDs on the same Y coordinate.
    """

    processed = _preprocess_pairs(
        pairs,
        min_sigma_m=min_sigma_m,
        min_distance_m=min_distance_m,
    )
    anchor_ids = _anchor_ids(processed)
    _validate_connected(anchor_ids, processed)

    scale = _layout_scale(processed)
    parameterization = _Parameterization(anchor_ids)
    rng = random.Random(random_seed)
    initial_seeds = _initial_parameters(
        parameterization,
        processed,
        seed_count=max(seed_count, 1),
        scale=scale,
        rng=rng,
    )

    best_params: list[float] | None = None
    best_energy = math.inf
    accepted_hops = 0
    temperature = max(scale * scale * 1e-5, 1e-8)
    hop_scale = max(scale * 0.35, 0.05)

    for seed_params in initial_seeds:
        current_params, current_energy = _local_minimize(
            seed_params,
            parameterization,
            processed,
            max_iterations=max_iterations,
        )
        if current_energy < best_energy:
            best_params = current_params
            best_energy = current_energy

        for _hop_index in range(max(basin_hops, 0)):
            hopped = [
                value + rng.gauss(0.0, hop_scale)
                for value in current_params
            ]
            candidate_params, candidate_energy = _local_minimize(
                hopped,
                parameterization,
                processed,
                max_iterations=max_iterations,
            )
            accept = candidate_energy <= current_energy
            if not accept:
                probability = math.exp(
                    max(min((current_energy - candidate_energy) / temperature, 0.0), -60.0)
                )
                accept = rng.random() < probability
            if accept:
                accepted_hops += 1
                current_params = candidate_params
                current_energy = candidate_energy
            if candidate_energy < best_energy:
                best_params = candidate_params
                best_energy = candidate_energy

    if best_params is None:
        raise ValueError("Could not solve anchor layout.")

    positions = parameterization.to_positions(best_params)
    positions = rotate_layout_to_level(positions, anchor_ids[0], anchor_ids[1])
    residuals = pair_residuals(positions, processed)
    rmse = _rmse(residuals.values())
    max_residual = max((abs(value) for value in residuals.values()), default=0.0)
    warnings = _layout_warnings(anchor_ids, processed, rmse, max_residual)
    return AnchorLayoutResult(
        algorithm=ANCHOR_LAYOUT_ALGORITHM,
        energy=best_energy,
        rmse_m=rmse,
        max_residual_m=max_residual,
        positions_m=positions,
        processed_pairs=tuple(processed),
        residuals_m=residuals,
        warnings=tuple(warnings),
        seed_count=len(initial_seeds),
        basin_hop_count=accepted_hops,
    )


def refine_anchor_layout_from_seed(
    pairs: Iterable[AnchorPairDistance],
    seed_positions_m: dict[str, tuple[float, float]],
    *,
    max_iterations: int = 200,
) -> AnchorLayoutResult:
    """Refine one solved layout against measured pair distances only.

    The seed fixes the selected layout basin. No missing-edge, radio-radius,
    graph-shortest, or other visibility constraint enters this pass.
    """

    processed = _preprocess_pairs(
        pairs,
        min_sigma_m=0.02,
        min_distance_m=0.05,
    )
    anchor_ids = _anchor_ids(processed)
    _validate_connected(anchor_ids, processed)
    if set(seed_positions_m) != set(anchor_ids):
        raise ValueError("Seed layout does not match the measured anchor set.")
    seed = rotate_layout_to_level(
        seed_positions_m,
        anchor_ids[0],
        anchor_ids[1],
    )
    parameterization = _Parameterization(anchor_ids)
    params, energy = _local_minimize(
        _positions_to_params(parameterization, seed),
        parameterization,
        processed,
        max_iterations=max_iterations,
    )
    positions = rotate_layout_to_level(
        parameterization.to_positions(params),
        anchor_ids[0],
        anchor_ids[1],
    )
    positions = _clean_positions(positions)
    residuals = pair_residuals(positions, processed)
    rmse = _rmse(residuals.values())
    max_residual = max((abs(value) for value in residuals.values()), default=0.0)
    return AnchorLayoutResult(
        algorithm=DISTANCE_ONLY_REFINEMENT_ALGORITHM,
        energy=energy,
        rmse_m=rmse,
        max_residual_m=max_residual,
        positions_m=positions,
        processed_pairs=tuple(processed),
        residuals_m=residuals,
        warnings=tuple(_layout_warnings(anchor_ids, processed, rmse, max_residual)),
        seed_count=1,
        basin_hop_count=0,
    )


def pair_residuals(
    positions_m: dict[str, tuple[float, float]],
    pairs: Iterable[ProcessedAnchorPair | AnchorPairDistance],
) -> dict[str, float]:
    """Return signed measured-minus-model residuals for each pair."""

    residuals: dict[str, float] = {}
    for pair in pairs:
        if pair.anchor_a_id not in positions_m or pair.anchor_b_id not in positions_m:
            continue
        ax, ay = positions_m[pair.anchor_a_id]
        bx, by = positions_m[pair.anchor_b_id]
        model_distance = math.hypot(ax - bx, ay - by)
        residuals[_pair_label(pair.anchor_a_id, pair.anchor_b_id)] = (
            model_distance - float(pair.distance_m)
        )
    return residuals


def evaluate_anchor_layout(
    pairs: Iterable[AnchorPairDistance],
    positions_m: dict[str, tuple[float, float]],
    *,
    algorithm: str = MANUALLY_EDITED_LAYOUT_ALGORITHM,
) -> AnchorLayoutResult:
    """Evaluate fixed user-supplied coordinates without moving any anchor."""

    processed = _preprocess_pairs(
        pairs,
        min_sigma_m=0.02,
        min_distance_m=0.05,
    )
    anchor_ids = _anchor_ids(processed)
    _validate_connected(anchor_ids, processed)
    if set(positions_m) != set(anchor_ids):
        raise ValueError("Edited layout does not match the measured anchor graph.")
    positions: dict[str, tuple[float, float]] = {}
    for anchor_id in anchor_ids:
        raw_x, raw_y = positions_m[anchor_id]
        x_m = float(raw_x)
        y_m = float(raw_y)
        if not math.isfinite(x_m) or not math.isfinite(y_m):
            raise ValueError("Edited anchor coordinates must be finite.")
        positions[anchor_id] = (x_m, y_m)
    positions = _clean_positions(positions)
    residuals = pair_residuals(positions, processed)
    rmse = _rmse(residuals.values())
    max_residual = max(
        (abs(value) for value in residuals.values()),
        default=0.0,
    )
    energy = sum(
        processed_pair.weight
        * residuals[_pair_label(
            processed_pair.anchor_a_id,
            processed_pair.anchor_b_id,
        )]
        ** 2
        for processed_pair in processed
    )
    return AnchorLayoutResult(
        algorithm=algorithm,
        energy=energy,
        rmse_m=rmse,
        max_residual_m=max_residual,
        positions_m=positions,
        processed_pairs=tuple(processed),
        residuals_m=residuals,
        warnings=tuple(
            _layout_warnings(anchor_ids, processed, rmse, max_residual)
        ),
        seed_count=1,
        basin_hop_count=0,
    )


def rotate_layout_to_level(
    positions_m: dict[str, tuple[float, float]],
    anchor_a_id: str,
    anchor_b_id: str,
) -> dict[str, tuple[float, float]]:
    """Rotate and translate a layout so two anchors lie on the same Y.

    The first anchor becomes the origin. If the second anchor ends up to the
    left of it, the layout is rotated another 180 degrees so the pair reads as a
    left-to-right baseline.
    """

    if anchor_a_id not in positions_m or anchor_b_id not in positions_m:
        raise ValueError("Both selected anchors must exist in the layout.")
    ax, ay = positions_m[anchor_a_id]
    bx, by = positions_m[anchor_b_id]
    dx = bx - ax
    dy = by - ay
    if math.hypot(dx, dy) <= 1e-12:
        raise ValueError("Selected anchors are at the same position.")

    angle = -math.atan2(dy, dx)
    cos_a = math.cos(angle)
    sin_a = math.sin(angle)
    rotated: dict[str, tuple[float, float]] = {}
    for anchor_id, (x_m, y_m) in positions_m.items():
        tx = x_m - ax
        ty = y_m - ay
        rotated[anchor_id] = (
            tx * cos_a - ty * sin_a,
            tx * sin_a + ty * cos_a,
        )
    if rotated[anchor_b_id][0] < 0:
        rotated = rotate_layout(rotated, 180.0, origin=(0.0, 0.0))
    return _clean_positions(rotated)


def rotate_layout(
    positions_m: dict[str, tuple[float, float]],
    angle_degrees: float,
    *,
    origin: tuple[float, float] | None = None,
) -> dict[str, tuple[float, float]]:
    """Rotate a layout around ``origin`` or its center."""

    ox, oy = origin if origin is not None else _layout_center(positions_m)
    radians = math.radians(angle_degrees)
    cos_a = math.cos(radians)
    sin_a = math.sin(radians)
    rotated = {}
    for anchor_id, (x_m, y_m) in positions_m.items():
        tx = x_m - ox
        ty = y_m - oy
        rotated[anchor_id] = (
            ox + tx * cos_a - ty * sin_a,
            oy + tx * sin_a + ty * cos_a,
        )
    return _clean_positions(rotated)


def mirror_layout(
    positions_m: dict[str, tuple[float, float]],
    axis: str,
    *,
    origin: tuple[float, float] | None = None,
) -> dict[str, tuple[float, float]]:
    """Mirror a layout across its center X or Y axis."""

    normalized_axis = axis.lower().strip()
    if normalized_axis not in {"x", "y"}:
        raise ValueError("Mirror axis must be 'x' or 'y'.")
    ox, oy = origin if origin is not None else _layout_center(positions_m)
    mirrored = {}
    for anchor_id, (x_m, y_m) in positions_m.items():
        if normalized_axis == "x":
            mirrored[anchor_id] = (2.0 * ox - x_m, y_m)
        else:
            mirrored[anchor_id] = (x_m, 2.0 * oy - y_m)
    return _clean_positions(mirrored)


def _preprocess_pairs(
    pairs: Iterable[AnchorPairDistance],
    *,
    min_sigma_m: float,
    min_distance_m: float,
) -> list[ProcessedAnchorPair]:
    aggregates: dict[tuple[str, str], _PairAggregate] = {}
    for pair in pairs:
        if not pair.enabled:
            continue
        anchor_a = str(pair.anchor_a_id).strip()
        anchor_b = str(pair.anchor_b_id).strip()
        if not anchor_a or not anchor_b or anchor_a == anchor_b:
            continue
        distance = float(pair.distance_m)
        sigma = max(abs(float(pair.sigma_m)), min_sigma_m)
        if not math.isfinite(distance) or not math.isfinite(sigma):
            continue
        if distance <= min_distance_m:
            continue
        key = (anchor_a, anchor_b) if anchor_a < anchor_b else (anchor_b, anchor_a)
        weight = 1.0 / (sigma * sigma)
        aggregate = aggregates.setdefault(
            key,
            {"weighted_distance": 0.0, "weight": 0.0, "sources": set()},
        )
        aggregate["weighted_distance"] = float(aggregate["weighted_distance"]) + distance * weight
        aggregate["weight"] = float(aggregate["weight"]) + weight
        sources = aggregate["sources"]
        if isinstance(sources, set) and pair.source:
            sources.add(str(pair.source))

    processed: list[ProcessedAnchorPair] = []
    for (anchor_a, anchor_b), aggregate in sorted(aggregates.items()):
        weight = float(aggregate["weight"])
        if weight <= 0.0:
            continue
        sigma = max(math.sqrt(1.0 / weight), min_sigma_m)
        sources = aggregate["sources"]
        source_text = ", ".join(sorted(sources)) if isinstance(sources, set) and sources else "survey"
        processed.append(
            ProcessedAnchorPair(
                anchor_a_id=anchor_a,
                anchor_b_id=anchor_b,
                distance_m=float(aggregate["weighted_distance"]) / weight,
                sigma_m=sigma,
                weight=weight,
                source=source_text,
            )
        )

    if len(processed) < 1:
        raise ValueError("At least one valid anchor-to-anchor distance is required.")
    if len(_anchor_ids(processed)) < 2:
        raise ValueError("At least two anchors are required.")
    return processed


def _anchor_ids(pairs: Iterable[ProcessedAnchorPair]) -> list[str]:
    ids: set[str] = set()
    for pair in pairs:
        ids.add(pair.anchor_a_id)
        ids.add(pair.anchor_b_id)
    return sorted(ids)


def _validate_connected(anchor_ids: list[str], pairs: list[ProcessedAnchorPair]) -> None:
    neighbors: dict[str, set[str]] = {anchor_id: set() for anchor_id in anchor_ids}
    for pair in pairs:
        neighbors[pair.anchor_a_id].add(pair.anchor_b_id)
        neighbors[pair.anchor_b_id].add(pair.anchor_a_id)
    seen = {anchor_ids[0]}
    queue = [anchor_ids[0]]
    while queue:
        current = queue.pop(0)
        for neighbor in neighbors[current]:
            if neighbor not in seen:
                seen.add(neighbor)
                queue.append(neighbor)
    if len(seen) != len(anchor_ids):
        missing = ", ".join(anchor_id for anchor_id in anchor_ids if anchor_id not in seen)
        raise ValueError(f"Anchor distance graph is disconnected; missing {missing}.")


def _layout_scale(pairs: list[ProcessedAnchorPair]) -> float:
    distances = [pair.distance_m for pair in pairs if pair.distance_m > 0.0]
    if not distances:
        return 1.0
    return max(median(distances), 0.25)


class _Parameterization:
    def __init__(self, anchor_ids: list[str]) -> None:
        self.anchor_ids = anchor_ids
        self.variable_index: dict[tuple[str, str], int] = {}
        index = 0
        for anchor_position, anchor_id in enumerate(anchor_ids):
            if anchor_position == 0:
                continue
            self.variable_index[(anchor_id, "x")] = index
            index += 1
            if anchor_position > 1:
                self.variable_index[(anchor_id, "y")] = index
                index += 1
        self.dimension = index

    def to_positions(self, params: list[float]) -> dict[str, tuple[float, float]]:
        positions: dict[str, tuple[float, float]] = {}
        for anchor_position, anchor_id in enumerate(self.anchor_ids):
            if anchor_position == 0:
                positions[anchor_id] = (0.0, 0.0)
                continue
            x_index = self.variable_index[(anchor_id, "x")]
            x_m = params[x_index]
            y_index = self.variable_index.get((anchor_id, "y"))
            y_m = params[y_index] if y_index is not None else 0.0
            positions[anchor_id] = (x_m, y_m)
        return positions

    def derivative_index(self, anchor_id: str, axis: str) -> int | None:
        return self.variable_index.get((anchor_id, axis))


def _initial_parameters(
    parameterization: _Parameterization,
    pairs: list[ProcessedAnchorPair],
    *,
    seed_count: int,
    scale: float,
    rng: random.Random,
) -> list[list[float]]:
    seeds: list[list[float]] = []
    seeds.append(_triangulated_seed(parameterization, pairs, scale))
    seeds.append(_circle_seed(parameterization, scale, flip_y=False))
    seeds.append(_circle_seed(parameterization, scale, flip_y=True))

    while len(seeds) < seed_count:
        seeds.append(_random_seed(parameterization, scale, rng))
    return seeds[:seed_count]


def _triangulated_seed(
    parameterization: _Parameterization,
    pairs: list[ProcessedAnchorPair],
    scale: float,
) -> list[float]:
    anchor_ids = parameterization.anchor_ids
    positions: dict[str, tuple[float, float]] = {anchor_ids[0]: (0.0, 0.0)}
    base_distance = _pair_distance(anchor_ids[0], anchor_ids[1], pairs) or scale
    positions[anchor_ids[1]] = (base_distance, 0.0)

    for index, anchor_id in enumerate(anchor_ids[2:], start=2):
        d0 = _pair_distance(anchor_ids[0], anchor_id, pairs)
        d1 = _pair_distance(anchor_ids[1], anchor_id, pairs)
        if d0 is not None and d1 is not None and base_distance > 1e-9:
            x_m = (d0 * d0 + base_distance * base_distance - d1 * d1) / (2.0 * base_distance)
            y_sq = max(d0 * d0 - x_m * x_m, 0.0)
            y_m = math.sqrt(y_sq)
            if index % 2:
                y_m = -y_m
            positions[anchor_id] = (x_m, y_m)
        else:
            angle = 2.0 * math.pi * (index - 1) / max(len(anchor_ids) - 1, 1)
            positions[anchor_id] = (
                math.cos(angle) * scale,
                math.sin(angle) * scale,
            )
    return _positions_to_params(parameterization, positions)


def _circle_seed(
    parameterization: _Parameterization,
    scale: float,
    *,
    flip_y: bool,
) -> list[float]:
    anchor_ids = parameterization.anchor_ids
    positions = {anchor_ids[0]: (0.0, 0.0), anchor_ids[1]: (scale, 0.0)}
    radius = max(scale, 0.25)
    for index, anchor_id in enumerate(anchor_ids[2:], start=2):
        angle = 2.0 * math.pi * (index - 1) / max(len(anchor_ids) - 1, 1)
        y_sign = -1.0 if flip_y else 1.0
        positions[anchor_id] = (
            radius * math.cos(angle),
            y_sign * radius * math.sin(angle),
        )
    return _positions_to_params(parameterization, positions)


def _random_seed(
    parameterization: _Parameterization,
    scale: float,
    rng: random.Random,
) -> list[float]:
    anchor_ids = parameterization.anchor_ids
    positions = {
        anchor_ids[0]: (0.0, 0.0),
        anchor_ids[1]: (max(scale + rng.gauss(0.0, scale * 0.25), 0.1), 0.0),
    }
    radius = max(scale, 0.25)
    for anchor_id in anchor_ids[2:]:
        angle = rng.uniform(-math.pi, math.pi)
        distance = rng.uniform(0.35 * radius, 1.75 * radius)
        positions[anchor_id] = (
            distance * math.cos(angle),
            distance * math.sin(angle),
        )
    return _positions_to_params(parameterization, positions)


def _positions_to_params(
    parameterization: _Parameterization,
    positions: dict[str, tuple[float, float]],
) -> list[float]:
    params = [0.0] * parameterization.dimension
    for anchor_id, (x_m, y_m) in positions.items():
        x_index = parameterization.derivative_index(anchor_id, "x")
        if x_index is not None:
            params[x_index] = x_m
        y_index = parameterization.derivative_index(anchor_id, "y")
        if y_index is not None:
            params[y_index] = y_m
    return params


def _pair_distance(
    anchor_a_id: str,
    anchor_b_id: str,
    pairs: list[ProcessedAnchorPair],
) -> float | None:
    wanted = set((anchor_a_id, anchor_b_id))
    for pair in pairs:
        if {pair.anchor_a_id, pair.anchor_b_id} == wanted:
            return pair.distance_m
    return None


def _local_minimize(
    initial_params: list[float],
    parameterization: _Parameterization,
    pairs: list[ProcessedAnchorPair],
    *,
    max_iterations: int,
) -> tuple[list[float], float]:
    params = list(initial_params)
    energy = _spring_energy(params, parameterization, pairs)
    damping = 1e-3

    for _iteration in range(max(max_iterations, 1)):
        normal, rhs = _normal_equations(params, parameterization, pairs)
        if not normal:
            break
        damped = [row[:] for row in normal]
        for index in range(len(damped)):
            damped[index][index] += damping * max(normal[index][index], 1.0)
        try:
            delta = _solve_linear_system(damped, rhs)
        except ValueError:
            damping *= 10.0
            if damping > 1e12:
                break
            continue
        if _vector_norm(delta) <= 1e-10:
            break
        candidate = [value + step for value, step in zip(params, delta)]
        candidate_energy = _spring_energy(candidate, parameterization, pairs)
        if candidate_energy <= energy:
            params = candidate
            if abs(energy - candidate_energy) <= 1e-14:
                energy = candidate_energy
                break
            energy = candidate_energy
            damping = max(damping * 0.35, 1e-12)
        else:
            damping *= 4.0
            if damping > 1e12:
                break
    return params, energy


def _spring_energy(
    params: list[float],
    parameterization: _Parameterization,
    pairs: list[ProcessedAnchorPair],
) -> float:
    positions = parameterization.to_positions(params)
    energy = 0.0
    for pair in pairs:
        ax, ay = positions[pair.anchor_a_id]
        bx, by = positions[pair.anchor_b_id]
        residual = math.hypot(ax - bx, ay - by) - pair.distance_m
        energy += 0.5 * pair.weight * residual * residual
    return energy


def _normal_equations(
    params: list[float],
    parameterization: _Parameterization,
    pairs: list[ProcessedAnchorPair],
) -> tuple[list[list[float]], list[float]]:
    dimension = parameterization.dimension
    normal = [[0.0 for _col in range(dimension)] for _row in range(dimension)]
    rhs = [0.0 for _row in range(dimension)]
    positions = parameterization.to_positions(params)

    for pair in pairs:
        ax, ay = positions[pair.anchor_a_id]
        bx, by = positions[pair.anchor_b_id]
        dx = ax - bx
        dy = ay - by
        length = max(math.hypot(dx, dy), 1e-9)
        residual = length - pair.distance_m
        sqrt_weight = math.sqrt(pair.weight)
        weighted_residual = sqrt_weight * residual
        derivatives: dict[int, float] = {}

        for anchor_id, sign in ((pair.anchor_a_id, 1.0), (pair.anchor_b_id, -1.0)):
            x_index = parameterization.derivative_index(anchor_id, "x")
            y_index = parameterization.derivative_index(anchor_id, "y")
            if x_index is not None:
                derivatives[x_index] = derivatives.get(x_index, 0.0) + sign * sqrt_weight * dx / length
            if y_index is not None:
                derivatives[y_index] = derivatives.get(y_index, 0.0) + sign * sqrt_weight * dy / length

        for row_index, row_value in derivatives.items():
            rhs[row_index] -= row_value * weighted_residual
            for col_index, col_value in derivatives.items():
                normal[row_index][col_index] += row_value * col_value
    return normal, rhs


def _solve_linear_system(matrix: list[list[float]], rhs: list[float]) -> list[float]:
    n = len(rhs)
    a = [row[:] + [rhs[index]] for index, row in enumerate(matrix)]
    for col in range(n):
        pivot = max(range(col, n), key=lambda row: abs(a[row][col]))
        if abs(a[pivot][col]) <= 1e-14:
            raise ValueError("Singular normal equation matrix.")
        if pivot != col:
            a[col], a[pivot] = a[pivot], a[col]
        pivot_value = a[col][col]
        for entry in range(col, n + 1):
            a[col][entry] /= pivot_value
        for row in range(n):
            if row == col:
                continue
            factor = a[row][col]
            if abs(factor) <= 1e-18:
                continue
            for entry in range(col, n + 1):
                a[row][entry] -= factor * a[col][entry]
    return [a[row][n] for row in range(n)]


def _vector_norm(values: list[float]) -> float:
    return math.sqrt(sum(value * value for value in values))


def _rmse(values: Iterable[float]) -> float:
    collected = list(values)
    if not collected:
        return 0.0
    return math.sqrt(sum(value * value for value in collected) / len(collected))


def _layout_warnings(
    anchor_ids: list[str],
    pairs: list[ProcessedAnchorPair],
    rmse_m: float,
    max_residual_m: float,
) -> list[str]:
    warnings: list[str] = []
    minimum_rigid_pair_count = max(1, 2 * len(anchor_ids) - 3)
    if len(anchor_ids) >= 3 and len(pairs) < minimum_rigid_pair_count:
        warnings.append(
            "Anchor graph is underconstrained; multiple layouts may fit the same distances."
        )
    if rmse_m > 0.10:
        warnings.append("Spring RMSE is high; check bad pair ranges or NLOS measurements.")
    if max_residual_m > 0.25:
        warnings.append("At least one anchor pair residual exceeds 0.25 m.")
    return warnings


def _layout_center(positions_m: dict[str, tuple[float, float]]) -> tuple[float, float]:
    if not positions_m:
        return (0.0, 0.0)
    return (
        sum(x for x, _y in positions_m.values()) / len(positions_m),
        sum(y for _x, y in positions_m.values()) / len(positions_m),
    )


def _clean_positions(
    positions_m: dict[str, tuple[float, float]]
) -> dict[str, tuple[float, float]]:
    cleaned = {}
    for anchor_id, (x_m, y_m) in positions_m.items():
        cleaned[anchor_id] = (
            0.0 if abs(x_m) < 1e-12 else x_m,
            0.0 if abs(y_m) < 1e-12 else y_m,
        )
    return cleaned


def _pair_label(anchor_a_id: str, anchor_b_id: str) -> str:
    return f"{anchor_a_id}-{anchor_b_id}"
