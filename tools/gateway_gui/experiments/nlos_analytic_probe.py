"""Test exact derivatives as a cheaper way to retain all reflection trials."""

import argparse
import json
from multiprocessing import Pool
from pathlib import Path
import time
from unittest.mock import patch

import numpy as np
from scipy.optimize import least_squares  # type: ignore[import-untyped]

from ..anchor_geometry_nlos import _OneSidedProblem, solve_nlos_one_sided_layout
from .verify_nlos_recovery import cases, score


class NumericalProblem(_OneSidedProblem):
    """Keep the original numerical baseline reproducible after GUI changes."""
    def local_solve(self, params, *, max_nfev):
        start = np.asarray(params, dtype=float)
        if start.size == 0:
            return start, self.objective(start)
        result = least_squares(self.residual_terms, start, jac="2-point",
                               method="trf", x_scale="jac", max_nfev=max(1, max_nfev))
        return result.x, self.objective(result.x)


class AnalyticProblem(_OneSidedProblem):
    def distance_jacobian(self, points, first, second):
        delta = points[first] - points[second]
        distance = np.linalg.norm(delta, axis=1)
        direction = delta / np.maximum(distance[:, None], 1e-15)
        incidence = (first[:, None] == self._variable_rows).astype(float)
        incidence -= second[:, None] == self._variable_rows
        return distance, incidence * direction[:, self._variable_axes]

    def jacobian(self, params):
        points = self.positions_array(params)
        distances, jac = self.distance_jacobian(points, self.pair_a, self.pair_b)
        r = distances - self.measured
        derivative = 1 / self.sigma
        negative = r < -1e-10
        e = -r[negative]
        sigma = self.sigma[negative]
        exponent = np.exp(-e**2 / (self.plateau * sigma**2))
        over = np.maximum(0, e - self.bias_cap_m)
        cost = -self.plateau * np.expm1(-e**2 / (self.plateau * sigma**2)) + (over / sigma)**2
        derivative[negative] = (e * exponent + over) / (sigma**2 * np.sqrt(cost))
        blocks = [jac * (derivative * self.sqrt_weights)[:, None]]
        if self.neighbor_a.size:
            distances, jac = self.distance_jacobian(points, self.neighbor_a, self.neighbor_b)
            blocks.append(jac * ((distances > self.neighbor_max_m) / self.interval_sigma_m)[:, None])
        if self.nonneighbor_a.size:
            distances, jac = self.distance_jacobian(points, self.nonneighbor_a, self.nonneighbor_b)
            blocks.append(-jac * ((distances < self.nonneighbor_min_m) / self.interval_sigma_m)[:, None])
        return np.concatenate(blocks, axis=0)

    def local_solve(self, params, *, max_nfev):
        start = np.asarray(params, dtype=float)
        if start.size == 0:
            return start, self.objective(start)
        result = least_squares(self.residual_terms, start, jac=self.jacobian,
                               method="trf", x_scale="jac", max_nfev=max(1, max_nfev))
        return result.x, self.objective(result.x)


def run(job):
    index, scene = job
    rows = []
    for method in (("baseline", "analytic") if index % 2 else ("analytic", "baseline")):
        start = time.perf_counter()
        row = {"scene": scene.name, "group": scene.meta["group"], "method": method}
        try:
            implementation = AnalyticProblem if method == "analytic" else NumericalProblem
            with patch("tools.gateway_gui.anchor_geometry_nlos._OneSidedProblem", implementation):
                result = solve_nlos_one_sided_layout(scene.pairs, neighbor_pairs=scene.neighbor_pairs,
                    nonneighbor_pairs=scene.nonneighbor_pairs,
                    neighbor_max_m=20 if scene.name == "indistinguishable" else 15)
            row.update(status="ok", positions=result.positions_m, energy=result.energy,
                       **score(scene, result.positions_m))
        except Exception as exc:
            row.update(status="error", error=f"{type(exc).__name__}: {exc}")
        row["runtime_s"] = time.perf_counter() - start
        rows.append(row)
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=6)
    args = parser.parse_args()
    scenes = cases()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w") as stream, Pool(args.workers) as pool:
        for i, rows in enumerate(pool.imap_unordered(run, enumerate(scenes)), 1):
            for row in rows:
                stream.write(json.dumps(row, allow_nan=False) + "\n")
            stream.flush()
            if i % 20 == 0 or i == len(scenes):
                print(f"{i}/{len(scenes)} scenes completed", flush=True)


if __name__ == "__main__":
    main()
