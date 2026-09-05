"""One-sided NLOS-tolerant anchor solver for survey ranging graphs.

Recovered from Claude's 2026-09-05 prototype and implementation plan; see
Documentation/Reviews/nlos-recovery-2026-09-05 for the original evidence.
NLOS propagation introduces positive range bias; measurement noise and
calibration errors can still have either sign. The symmetric solvers
treat a too-long range like a too-short one, so an anchor whose far links
cross a wall is pushed away from those anchors until it mirrors across its
near neighbours. This solver keeps the ranging sigma when the model distance
is longer than the measured one and lets the penalty saturate when the model
is shorter, so a biased link stops pulling. An explicit reflection search
visits alternative placements separated by a high local optimization cost.
"""

from __future__ import annotations

from collections.abc import Iterable
import itertools
import math
import random

import numpy as np

from .anchor_geometry import (
    AnchorLayoutResult,
    AnchorPairDistance,
    ProcessedAnchorPair,
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
)
from .anchor_geometry_connectivity import (
    DEFAULT_NEIGHBOR_MAX_M,
    DEFAULT_NONNEIGHBOR_MIN_M,
    PairKey,
    _normalize_constraints,
    _seed_families,
    pair_key,
)
from .anchor_geometry_seeds import GEOMETRY_SEEDS, SEED_AUTO, SEED_CURRENT


NLOS_ONE_SIDED_ALGORITHM = "NLOS one-sided intervals"
DEFAULT_PLATEAU = 16.0
DEFAULT_DISTANCE_WEIGHT_POWER = 1.0
DEFAULT_BIAS_CAP_M = 8.0
NLOS_SUSPECT_SIGMAS = 3.0


def one_sided_residual(
    residual_m: np.ndarray,
    sigma_m: np.ndarray,
    *,
    plateau: float,
    bias_cap_m: float,
) -> np.ndarray:
    """Return signed residual terms whose squares form the one-sided cost.

    ``residual_m`` is model minus measured distance. A positive residual means
    the model is longer than the range, which NLOS cannot explain, so it keeps
    the symmetric ``residual / sigma`` scale. A negative residual saturates at
    ``plateau`` sigma-squared units so a biased link stops pulling, and grows
    again once the implied bias exceeds ``bias_cap_m``.
    """

    scaled = residual_m / sigma_m
    positive = scaled >= 0.0
    terms = np.empty_like(scaled)
    terms[positive] = scaled[positive]
    negative = scaled[~positive]
    cost = -plateau * np.expm1(-(negative * negative) / plateau)
    over = np.maximum(0.0, -residual_m[~positive] - bias_cap_m) / sigma_m[~positive]
    terms[~positive] = -np.sqrt(cost + over * over)
    return terms


def _reflect_across(point: np.ndarray, line_a: np.ndarray, line_b: np.ndarray) -> np.ndarray:
    """Mirror ``point`` across the infinite line through ``line_a`` and ``line_b``."""

    direction = line_b - line_a
    unit = direction / float(np.linalg.norm(direction))
    offset = point - line_a
    return line_a + 2.0 * float(np.dot(offset, unit)) * unit - offset


class _OneSidedProblem:
    """Vectorised one-sided objective over the shared anchor parameterisation."""

    def __init__(
        self,
        anchor_ids: list[str],
        processed: list[ProcessedAnchorPair],
        neighbors: frozenset[PairKey],
        nonneighbors: frozenset[PairKey],
        *,
        neighbor_max_m: float,
        nonneighbor_min_m: float,
        interval_sigma_m: float,
        plateau: float,
        distance_weight_power: float,
        bias_cap_m: float,
        fixed_positions_m: dict[str, tuple[float, float]] | None = None,
    ) -> None:
        self.anchor_ids = list(anchor_ids)
        self.index = {anchor_id: row for row, anchor_id in enumerate(self.anchor_ids)}
        self.count = len(self.anchor_ids)
        self.parameterization = _Parameterization(self.anchor_ids, fixed_positions_m)
        self.neighbor_max_m = neighbor_max_m
        self.nonneighbor_min_m = nonneighbor_min_m
        self.interval_sigma_m = interval_sigma_m
        self.plateau = plateau
        self.bias_cap_m = bias_cap_m
        self._fixed_points = np.asarray(list(self.parameterization.to_positions(
            [0.0] * self.parameterization.dimension,
        ).values()))
        variables = self.parameterization.variable_index
        self._variable_rows = np.asarray([self.index[a] for a, _axis in variables], dtype=int)
        self._variable_axes = np.asarray([0 if axis == "x" else 1 for _a, axis in variables], dtype=int)
        self.pair_a = np.asarray([self.index[item.anchor_a_id] for item in processed], dtype=int)
        self.pair_b = np.asarray([self.index[item.anchor_b_id] for item in processed], dtype=int)
        self.measured = np.asarray([item.distance_m for item in processed], dtype=float)
        self.sigma = np.asarray([item.sigma_m for item in processed], dtype=float)
        reference = float(np.median(self.measured)) if self.measured.size else 1.0
        weights = (reference / np.maximum(self.measured, 0.3)) ** distance_weight_power
        self.sqrt_weights = np.sqrt(weights)
        neighbor_list = sorted(neighbors)
        nonneighbor_list = sorted(nonneighbors)
        self.neighbor_a = np.asarray([self.index[a] for a, _b in neighbor_list], dtype=int)
        self.neighbor_b = np.asarray([self.index[b] for _a, b in neighbor_list], dtype=int)
        self.nonneighbor_a = np.asarray([self.index[a] for a, _b in nonneighbor_list], dtype=int)
        self.nonneighbor_b = np.asarray([self.index[b] for _a, b in nonneighbor_list], dtype=int)
        self.nearest: dict[int, list[int]] = {row: [] for row in range(self.count)}
        for edge in np.argsort(self.measured, kind="stable"):
            first = int(self.pair_a[edge])
            second = int(self.pair_b[edge])
            self.nearest[first].append(second)
            self.nearest[second].append(first)

    def positions_array(self, params: np.ndarray) -> np.ndarray:
        points = self._fixed_points.copy()
        points[self._variable_rows, self._variable_axes] = params
        return points

    def positions(self, params: np.ndarray) -> dict[str, tuple[float, float]]:
        points = self.positions_array(params)
        return {
            anchor_id: (float(points[row, 0]), float(points[row, 1]))
            for anchor_id, row in self.index.items()
        }

    def params_from_array(self, points: np.ndarray) -> np.ndarray | None:
        positions = {
            anchor_id: (float(points[row, 0]), float(points[row, 1]))
            for anchor_id, row in self.index.items()
        }
        try:
            levelled = positions if self.parameterization.fixed_positions_m else rotate_layout_to_level(
                positions, self.anchor_ids[0], self.anchor_ids[1],
            )
        except ValueError:
            return None
        return np.asarray(_positions_to_params(self.parameterization, levelled), dtype=float)

    def residual_terms(self, params: np.ndarray) -> np.ndarray:
        points = self.positions_array(params)
        model = np.linalg.norm(points[self.pair_a] - points[self.pair_b], axis=1)
        terms = [
            self.sqrt_weights
            * one_sided_residual(
                model - self.measured,
                self.sigma,
                plateau=self.plateau,
                bias_cap_m=self.bias_cap_m,
            )
        ]
        if self.neighbor_a.size:
            neighbor_distance = np.linalg.norm(points[self.neighbor_a] - points[self.neighbor_b], axis=1)
            terms.append(np.maximum(0.0, neighbor_distance - self.neighbor_max_m) / self.interval_sigma_m)
        if self.nonneighbor_a.size:
            nonneighbor_distance = np.linalg.norm(
                points[self.nonneighbor_a] - points[self.nonneighbor_b], axis=1
            )
            terms.append(
                np.maximum(0.0, self.nonneighbor_min_m - nonneighbor_distance) / self.interval_sigma_m
            )
        return np.concatenate(terms)

    def objective(self, params: np.ndarray) -> float:
        terms = self.residual_terms(params)
        return float(np.dot(terms, terms))

    def near_fit_rank(self, params: np.ndarray) -> tuple[int, int]:
        """Count locally constrained dimensions using only near-fit ranges.

        This detects lost geometric support, not discrete mirror ambiguity.
        Radio hinges and relaxed ranges may still influence the final layout.
        """
        points = self.positions_array(params)
        delta = points[self.pair_a] - points[self.pair_b]
        distances = np.linalg.norm(delta, axis=1)
        near = np.abs(distances - self.measured) <= NLOS_SUSPECT_SIGMAS * self.sigma
        directions = delta / np.maximum(distances[:, None], 1e-9)
        incidence = (self.pair_a[:, None] == self._variable_rows).astype(float)
        incidence -= self.pair_b[:, None] == self._variable_rows
        jacobian = (incidence * directions[:, self._variable_axes])[near]
        rank = int(np.linalg.matrix_rank(jacobian)) if jacobian.size else 0
        required = self.parameterization.dimension
        fixed = self.parameterization.fixed_positions_m
        if fixed and len(set(fixed.values())) == 1 and required:
            required -= 1  # One distinct pin leaves a free global rotation.
        return rank, required

    def local_solve(self, params: np.ndarray, *, max_nfev: int) -> tuple[np.ndarray, float]:
        from scipy.optimize import least_squares  # type: ignore[import-untyped]

        start = np.asarray(params, dtype=float)
        if start.size == 0:
            return start, self.objective(start)
        result = least_squares(
            self.residual_terms,
            start,
            method="trf",
            x_scale="jac",
            max_nfev=max(1, max_nfev),
        )
        solved = np.asarray(result.x, dtype=float)
        return solved, self.objective(solved)

    def reflection_search(
        self,
        params: np.ndarray,
        objective: float,
        *,
        max_sweeps: int = 3,
        nearest_count: int = 4,
        max_nfev: int = 300,
    ) -> tuple[np.ndarray, float, int]:
        """Try each anchor's mirror across lines through its nearest neighbours."""

        best_params = np.asarray(params, dtype=float)
        best_objective = objective
        accepted = 0
        for _sweep in range(max_sweeps):
            improved = False
            for anchor in range(self.count):
                if self.anchor_ids[anchor] in self.parameterization.fixed_positions_m:
                    continue
                candidates = self.nearest[anchor][:nearest_count]
                for first, second in itertools.combinations(candidates, 2):
                    points = self.positions_array(best_params)
                    if float(np.linalg.norm(points[second] - points[first])) <= 0.1:
                        continue
                    reflected = _reflect_across(points[anchor], points[first], points[second])
                    if float(np.linalg.norm(reflected - points[anchor])) < 0.2:
                        continue
                    moved = points.copy()
                    moved[anchor] = reflected
                    start = self.params_from_array(moved)
                    if start is None:
                        continue
                    candidate, value = self.local_solve(start, max_nfev=max_nfev)
                    if value < best_objective - 1e-6:
                        best_params = candidate
                        best_objective = value
                        accepted += 1
                        improved = True
            if not improved:
                break
        return best_params, best_objective, accepted


def solve_nlos_one_sided_layout(
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
    plateau: float = DEFAULT_PLATEAU,
    distance_weight_power: float = DEFAULT_DISTANCE_WEIGHT_POWER,
    bias_cap_m: float = DEFAULT_BIAS_CAP_M,
    max_nfev: int = 600,
) -> AnchorLayoutResult:
    """Solve measured ranges with a one-sided NLOS-tolerant loss.

    The negative residual cost saturates, then grows again past ``bias_cap_m``.
    Short-link weighting is an explicit assumption, not an NLOS classifier.
    Radio neighbour intervals apply as in ``Neighbor intervals``; unknown
    or unscheduled edges are not negative radio evidence.
    """

    if not (math.isfinite(neighbor_max_m) and 0.0 < nonneighbor_min_m <= neighbor_max_m):
        raise ValueError("Radio interval must satisfy 0 < minimum <= maximum.")
    if not math.isfinite(interval_sigma_m) or interval_sigma_m <= 0.0:
        raise ValueError("Radio interval sigma must be positive.")
    if not math.isfinite(plateau) or plateau <= 0.0:
        raise ValueError("NLOS plateau must be positive.")
    if not math.isfinite(distance_weight_power) or not 0.0 <= distance_weight_power <= 4.0:
        raise ValueError("Distance weight power must be between 0 and 4.")
    if not math.isfinite(bias_cap_m) or bias_cap_m <= 0.0:
        raise ValueError("NLOS bias cap must be positive.")
    if seed not in GEOMETRY_SEEDS:
        raise ValueError(f"Unknown geometry seed: {seed}")
    known_pairs = list(pairs)
    processed = _preprocess_pairs(known_pairs, min_sigma_m=0.02, min_distance_m=0.05)
    anchor_ids = _anchor_ids(processed)
    _validate_connected(anchor_ids, processed)
    anchor_set = set(anchor_ids)
    neighbors = _normalize_constraints(neighbor_pairs, anchor_set)
    nonneighbors = _normalize_constraints(nonneighbor_pairs, anchor_set)
    if neighbors & nonneighbors:
        raise ValueError("A pair cannot be both a neighbor and a non-neighbor.")
    measured = {pair_key(item.anchor_a_id, item.anchor_b_id) for item in processed}
    if measured & nonneighbors:
        raise ValueError("A measured pair cannot be constrained as a non-neighbor.")

    problem = _OneSidedProblem(
        anchor_ids,
        processed,
        neighbors,
        nonneighbors,
        neighbor_max_m=neighbor_max_m,
        nonneighbor_min_m=nonneighbor_min_m,
        interval_sigma_m=interval_sigma_m,
        plateau=plateau,
        distance_weight_power=distance_weight_power,
        bias_cap_m=bias_cap_m,
        fixed_positions_m=fixed_positions_m,
    )
    if problem.parameterization.dimension == 0:
        base_seeds = [(SEED_CURRENT, problem.parameterization.fixed_positions_m)]
    else:
        base_seeds = _seed_families(known_pairs, anchor_ids, nonneighbors, seed, current_positions_m)
    rng = random.Random(1337)
    scale = max((item.distance_m for item in processed), default=1.0)
    candidates: list[tuple[float, str, np.ndarray]] = []
    for seed_name, base_positions in base_seeds:
        oriented = base_positions if fixed_positions_m else rotate_layout_to_level(
            base_positions, anchor_ids[0], anchor_ids[1],
        )
        first = np.asarray(_positions_to_params(problem.parameterization, oriented), dtype=float)
        starts = [first]
        if seed == SEED_AUTO and first.size:
            for _ in range(2):
                starts.append(first + np.asarray([rng.gauss(0.0, 0.08 * scale) for _ in first]))
        for start in starts:
            params, objective = problem.local_solve(start, max_nfev=max_nfev)
            candidates.append((objective, seed_name, params))
    objective, selected_seed, params = min(candidates, key=lambda item: item[0])
    params, objective, accepted_reflections = problem.reflection_search(params, objective)

    positions = problem.positions(params)
    if not fixed_positions_m:
        positions = rotate_layout_to_level(positions, anchor_ids[0], anchor_ids[1])
    positions = _clean_positions(positions)
    positions.update(problem.parameterization.fixed_positions_m)
    residual_map = pair_residuals(positions, processed)
    rmse = _rmse(residual_map.values())
    max_residual = max((abs(value) for value in residual_map.values()), default=0.0)
    warnings = list(_layout_warnings(anchor_ids, processed, rmse, max_residual))
    neighbor_violations = sum(
        1 for a, b in neighbors if math.dist(positions[a], positions[b]) > neighbor_max_m + 1e-6
    )
    nonneighbor_violations = sum(
        1 for a, b in nonneighbors if math.dist(positions[a], positions[b]) < nonneighbor_min_m - 1e-6
    )
    if neighbor_violations:
        warnings.append(f"{neighbor_violations} neighbor interval(s) exceed {neighbor_max_m:.1f} m")
    if nonneighbor_violations:
        warnings.append(
            f"{nonneighbor_violations} non-neighbor interval(s) are below {nonneighbor_min_m:.1f} m"
        )
    suspects: list[tuple[float, str]] = []
    for item in processed:
        residual = residual_map[f"{item.anchor_a_id}-{item.anchor_b_id}"]
        if residual < -NLOS_SUSPECT_SIGMAS * item.sigma_m:
            suspects.append((-residual, f"{item.anchor_a_id}-{item.anchor_b_id} (+{-residual:.2f} m)"))
    if suspects:
        listed = ", ".join(text for _excess, text in sorted(suspects, reverse=True))
        warnings.append(f"{len(suspects)} possible NLOS link(s), measured excess: {listed}")
    rank, required_rank = problem.near_fit_rank(params)
    if rank < required_rank:
        warnings.append(
            f"Near-fit ranges constrain {rank}/{required_rank} shape dimensions; "
            "result depends on relaxed ranges or radio bounds."
        )
    return AnchorLayoutResult(
        algorithm=(
            f"{NLOS_ONE_SIDED_ALGORITHM} ({nonneighbor_min_m:g}-{neighbor_max_m:g} m); "
            f"seed {selected_seed}"
        ),
        energy=objective,
        rmse_m=rmse,
        max_residual_m=max_residual,
        positions_m=positions,
        processed_pairs=tuple(processed),
        residuals_m=residual_map,
        warnings=tuple(warnings),
        seed_count=len(candidates),
        basin_hop_count=accepted_reflections,
    )
