"""Optional distance-based confidence heuristic, independent of radio evidence."""

from dataclasses import replace
import math

from .anchor_geometry import AnchorPairDistance


def parse_distance_weight_power(value: str) -> float:
    try:
        power = float(value)
    except ValueError as exc:
        raise ValueError("Distance weight power must be a number from 0 to 4.") from exc
    if not math.isfinite(power) or not 0.0 <= power <= 4.0:
        raise ValueError("Distance weight power must be a number from 0 to 4.")
    return power


def distance_weighted_pairs(
    pairs: tuple[AnchorPairDistance, ...], power: float,
) -> tuple[AnchorPairDistance, ...]:
    """Multiply each measured weight by (shortest enabled range / range)**power.

    The closest range retains its original weight. Longer ranges lose weight;
    measurements, topology, and the original survey objects remain unchanged.
    Power zero preserves the original solver inputs exactly. This is a heuristic,
    not an NLOS classifier or a model of calibrated measurement uncertainty.
    """
    parse_distance_weight_power(str(power))
    if power == 0.0:
        return pairs
    enabled = [pair for pair in pairs if pair.enabled]
    if not enabled:
        return pairs
    for pair in enabled:
        if not math.isfinite(pair.distance_m) or pair.distance_m <= 0.0:
            raise ValueError("Distance weighting requires finite positive ranges.")
    # Match the solver's distance and sigma floors before multiplying weights.
    shortest = min(max(pair.distance_m, 0.05) for pair in enabled)
    return tuple(
        replace(
            pair,
            sigma_m=max(abs(pair.sigma_m), 0.02)
            * (max(pair.distance_m, 0.05) / shortest) ** (power / 2.0),
        ) if pair.enabled else pair
        for pair in pairs
    )
