"""Recognize rigid frame changes without running localization again."""
from __future__ import annotations

import math
from typing import Callable


def rigid_motion(
    before: dict[str, tuple[float, float]],
    after: dict[str, tuple[float, float]],
) -> Callable[[float, float], tuple[float, float]] | None:
    """Return a rotation/translation only when every anchor agrees."""
    if set(before) != set(after) or len(before) < 2:
        return None
    first = next(iter(before))
    second = max(before, key=lambda key: math.dist(before[first], before[key]))
    x0, y0 = before[first]
    u0, v0 = after[first]
    dx, dy = before[second][0] - x0, before[second][1] - y0
    du, dv = after[second][0] - u0, after[second][1] - v0
    length2 = dx * dx + dy * dy
    if length2 < 1e-12:
        return None
    cosine = (dx * du + dy * dv) / length2
    sine = (dx * dv - dy * du) / length2
    if not math.isclose(cosine * cosine + sine * sine, 1.0, rel_tol=1e-10, abs_tol=1e-10):
        return None

    def transform(x: float, y: float) -> tuple[float, float]:
        return u0 + cosine * (x - x0) - sine * (y - y0), v0 + sine * (x - x0) + cosine * (y - y0)

    if any(math.dist(transform(*point), after[key]) > 1e-8 for key, point in before.items()):
        return None
    return transform


def rotation_safe_bounds(positions: dict[str, tuple[float, float]]) -> tuple[tuple[float, float], ...]:
    """Keep auto-fit steady while a layout rotates about its centroid."""
    if not positions:
        return ((0.0, 0.0),)
    cx = sum(point[0] for point in positions.values()) / len(positions)
    cy = sum(point[1] for point in positions.values()) / len(positions)
    radius = max(0.5, max(math.hypot(x - cx, y - cy) for x, y in positions.values()))
    return ((cx - radius, cy - radius), (cx + radius, cy + radius), (0.0, 0.0))
