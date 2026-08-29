"""Shared names and deterministic seed construction for geometry solvers."""

from __future__ import annotations

import numpy as np

from .anchor_geometry import AnchorPairDistance, rotate_layout_to_level


SEED_AUTO = "Auto (best of all)"
SEED_CURRENT = "Current layout"
SEED_VISIBILITY = "Visibility branching"
SEED_SPRING = "Measured-distance spring"
SEED_GRAPH_MDS = "Graph MDS"
GEOMETRY_SEEDS = (
    SEED_AUTO,
    SEED_CURRENT,
    SEED_VISIBILITY,
    SEED_SPRING,
    SEED_GRAPH_MDS,
)


def graph_mds_seed(
    pairs: list[AnchorPairDistance],
    anchor_ids: list[str],
) -> dict[str, tuple[float, float]]:
    """Embed measured shortest-path distances as a deterministic seed."""

    count = len(anchor_ids)
    index = {anchor_id: position for position, anchor_id in enumerate(anchor_ids)}
    distances = np.full((count, count), np.inf, dtype=float)
    np.fill_diagonal(distances, 0.0)
    for pair in pairs:
        left = index[pair.anchor_a_id]
        right = index[pair.anchor_b_id]
        distances[left, right] = distances[right, left] = min(
            distances[left, right],
            pair.distance_m,
        )
    for intermediate in range(count):
        distances = np.minimum(
            distances,
            distances[:, intermediate : intermediate + 1]
            + distances[intermediate : intermediate + 1, :],
        )
    if not np.all(np.isfinite(distances)):
        raise ValueError("Measured distance graph is disconnected.")
    squared = distances * distances
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
