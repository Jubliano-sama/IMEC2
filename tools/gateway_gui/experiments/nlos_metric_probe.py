"""Conservative triangle-bound preprocessing as an alternative experiment."""

from dataclasses import replace
import math

import numpy as np


def metric_bounds(pairs):
    """Cap only ranges exceeding a measured path plus three-sigma slack.

    If path measurements contain nonnegative biases, their sum is an upper
    bound on direct geometric distance, subject to measurement noise. The
    original input records are preserved. No missing range is fabricated.
    """
    ids = sorted({a for p in pairs for a in (p.anchor_a_id, p.anchor_b_id)})
    index = {a: i for i, a in enumerate(ids)}
    size = len(ids)
    upper = np.full((size, size), np.inf)
    variance = np.zeros((size, size))
    np.fill_diagonal(upper, 0.)
    for p in pairs:
        a, b = index[p.anchor_a_id], index[p.anchor_b_id]
        if p.distance_m < upper[a, b]:
            upper[a, b] = upper[b, a] = p.distance_m
            variance[a, b] = variance[b, a] = p.sigma_m ** 2
    for k in range(size):
        candidate = upper[:, k, None] + upper[None, k, :]
        better = candidate < upper
        new_variance = variance[:, k, None] + variance[None, k, :]
        upper = np.where(better, candidate, upper)
        variance = np.where(better, new_variance, variance)
    adjusted, changes = [], []
    for p in pairs:
        a, b = index[p.anchor_a_id], index[p.anchor_b_id]
        bound = upper[a, b] + 3 * math.sqrt(variance[a, b] + p.sigma_m ** 2)
        value = min(p.distance_m, bound)
        adjusted.append(replace(p, distance_m=value))
        if value < p.distance_m:
            changes.append({"pair": [p.anchor_a_id, p.anchor_b_id], "before_m": p.distance_m, "after_m": value})
    return adjusted, changes
