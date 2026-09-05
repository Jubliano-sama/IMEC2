"""Adaptive host-side estimates based on the firmware's survey radio schedules.

These estimates drive display only.  Reaching zero never expires an operation;
the command and survey owners keep using their independent safety deadlines.
Keep the constants here aligned with ``mesh_relay.h``, ``survey.h``, and
``enumeration_response_lane.h`` whenever the production radio schedule changes.
"""

from __future__ import annotations

from dataclasses import dataclass
import json
import math
import os
from pathlib import Path
import tempfile


SURVEY_MAX_HOPS = 8

# Relays retain the complete gateway-origin, eight-depth, weak-fallback wave.
# The gateway can release its local command owner at 20 s because the wave is
# already in depth four and continues outward on its immutable clock.  Allow
# another three empirically observed seconds for terminal telemetry in the GUI
# countdown; this estimate never controls the firmware or command deadline.
ROUTE_DEPTH_BLOCK_MS = 4_500
ROUTE_FULL_WAVE_BLOCKS = SURVEY_MAX_HOPS + 2
ROUTE_FINAL_GUARD_MS = 150
ROUTE_FULL_WAVE_MS = (
    ROUTE_FULL_WAVE_BLOCKS * ROUTE_DEPTH_BLOCK_MS
    + ROUTE_FINAL_GUARD_MS
)
ROUTE_REFRESH_NOMINAL_MS = 20_000
ROUTE_REFRESH_TERMINAL_MARGIN_MS = 3_000
ROUTE_REFRESH_SCHEDULED_MS = (
    ROUTE_REFRESH_NOMINAL_MS + ROUTE_REFRESH_TERMINAL_MARGIN_MS
)

# Compact survey control and response timing.  The gateway command may spend
# up to this long obtaining the first RF origin; later calculations begin at
# the RF origin published by firmware and therefore omit it explicitly.
CONTROL_ORIGIN_BUDGET_MS = 10_000
CONTROL_PROPAGATION_MARGIN_MS = 150
CONTROL_PER_HOP_MS = 540
CONTROL_LISTENER_REDUNDANCY_MS = 2_000
RADIO_GUARD_MS = 60

RESPONSE_ROUND_MS = 125
RESPONSE_SOURCE_ROUNDS_PER_DEPTH = 12
RESPONSE_FORWARD_ROUNDS_PER_HOP = 2
HIA_RESPONSE_LEAD_DEPTHS = 2
HIA_RESPONSE_CLOCK_GUARD_MS = 100

NEIGHBOR_SLOT_MS = 1_000
RESULT_PREPARE_MS = 40
RANGE_WAVE_MS = 600
RANGE_EXTRA_DRAIN_STRIDES = 2


def _hop_count(value: int) -> int:
    if isinstance(value, bool) or not 1 <= value <= SURVEY_MAX_HOPS:
        raise ValueError(f"maximum hop count must be in 1..{SURVEY_MAX_HOPS}")
    return value


def _slot_span(value: int) -> int:
    if isinstance(value, bool) or not 1 <= value <= 50:
        raise ValueError("survey slot span must be in 1..50")
    return value


def survey_control_delivery_ms(max_hop_count: int) -> int:
    depth = _hop_count(max_hop_count)
    return (
        CONTROL_PROPAGATION_MARGIN_MS
        + depth * CONTROL_PER_HOP_MS
        + CONTROL_LISTENER_REDUNDANCY_MS
        + RADIO_GUARD_MS
    )


def survey_response_lane_ms(max_hop_count: int) -> int:
    depth = _hop_count(max_hop_count)
    rounds = (
        depth * RESPONSE_SOURCE_ROUNDS_PER_DEPTH
        + RESPONSE_FORWARD_ROUNDS_PER_HOP * depth * (depth - 1) // 2
    )
    return RESPONSE_ROUND_MS * rounds


def survey_enumeration_phase_ms(max_hop_count: int = SURVEY_MAX_HOPS) -> int:
    """Return prearmed ASSIGN dispatch through the TABLE quiet edge.

    Enumeration Here-I-Am starts its compact identity lane while the route
    wave is still running.  The later ASSIGN command consumes that prearm and
    never sends a second CLAIM, so only the compact lane's overlap tail plus
    TABLE delivery and propagation remain at target dispatch.
    """

    depth = _hop_count(max_hop_count)
    compact_start_ms = (
        (HIA_RESPONSE_LEAD_DEPTHS + 1) * ROUTE_DEPTH_BLOCK_MS
        + HIA_RESPONSE_CLOCK_GUARD_MS
    )
    compact_end_ms = compact_start_ms + SURVEY_MAX_HOPS * ROUTE_DEPTH_BLOCK_MS
    compact_tail_ms = max(0, compact_end_ms - ROUTE_REFRESH_SCHEDULED_MS)
    table_propagation_ms = CONTROL_PROPAGATION_MARGIN_MS + depth * CONTROL_PER_HOP_MS
    return compact_tail_ms + CONTROL_ORIGIN_BUDGET_MS + table_propagation_ms


def survey_neighbor_phase_ms(max_hop_count: int, slot_span: int) -> int:
    """Return a conservative GUI-dispatch-to-neighbor-graph duration."""

    return (
        CONTROL_ORIGIN_BUDGET_MS
        + survey_control_delivery_ms(max_hop_count)
        + _slot_span(slot_span) * NEIGHBOR_SLOT_MS
        + RESULT_PREPARE_MS
        + survey_response_lane_ms(max_hop_count)
    )


def survey_wave_stride_ms(max_hop_count: int) -> int:
    return (
        RANGE_WAVE_MS
        + RESULT_PREPARE_MS
        + survey_response_lane_ms(max_hop_count)
        + RADIO_GUARD_MS
    )


def survey_ranging_phase_ms(max_hop_count: int, wave_count: int) -> int:
    """Return PLAN-origin-to-terminal time after PLAN_ACCEPTED is observed."""

    if isinstance(wave_count, bool) or not 0 <= wave_count <= 100:
        raise ValueError("survey wave count must be in 0..100")
    return (
        survey_control_delivery_ms(max_hop_count)
        + (wave_count + RANGE_EXTRA_DRAIN_STRIDES)
        * survey_wave_stride_ms(max_hop_count)
    )


@dataclass(frozen=True)
class ScheduledPhaseSnapshot:
    key: str
    label: str
    duration_ms: int
    elapsed_ms: int

    @property
    def remaining_ms(self) -> int:
        return max(0, self.duration_ms - self.elapsed_ms)

    @property
    def fraction(self) -> float:
        return min(max(self.elapsed_ms / self.duration_ms, 0.0), 1.0)

    @property
    def elapsed(self) -> bool:
        return self.elapsed_ms >= self.duration_ms


@dataclass(frozen=True)
class ScheduledPhaseEstimate:
    key: str
    label: str
    started_at: float
    duration_ms: int
    nominal_duration_ms: int | None = None

    def __post_init__(self) -> None:
        if not self.key or not self.label:
            raise ValueError("scheduled phase requires a key and label")
        if self.duration_ms <= 0:
            raise ValueError("scheduled phase duration must be positive")

    def snapshot(self, now: float) -> ScheduledPhaseSnapshot:
        elapsed_ms = max(0, int((now - self.started_at) * 1_000))
        return ScheduledPhaseSnapshot(
            self.key,
            self.label,
            self.duration_ms,
            elapsed_ms,
        )


class PhaseTimingCalibration:
    """Small, persisted corrections to display estimates, never radio deadlines.

    Keep the topology-derived schedule as the baseline. Successful observations
    learn its additive error (first sample directly, then a 35% moving average).
    Bound corrections to five minutes and each displayed duration to 10%..200%
    of its baseline, so a previous topology or delayed BLE delivery cannot make
    the next estimate nonsensical. Aborts and incomplete results aren't samples.
    """

    PHASES = frozenset({"routes", "enumeration", "neighbors", "ranging"})
    MAX_OFFSET_MS = 300_000

    def __init__(self, path: Path | None = None) -> None:
        self.path = path
        self.offsets_ms: dict[str, float] = {}
        if path is not None:
            try:
                if path.stat().st_size > 4096:
                    return
                data = json.loads(path.read_text(encoding="utf-8"))
                if not isinstance(data, dict) or data.get("version") != 1:
                    return
                offsets = data.get("offsets_ms")
                if not isinstance(offsets, dict):
                    return
                for key, value in offsets.items():
                    if (
                        key in self.PHASES
                        and type(value) in (int, float)
                        and math.isfinite(value)
                        and abs(value) <= self.MAX_OFFSET_MS
                    ):
                        self.offsets_ms[key] = float(value)
            except (OSError, ValueError, OverflowError):
                pass

    @classmethod
    def for_user(cls) -> PhaseTimingCalibration:
        return cls(
            Path.home() / ".config" / "imec2-gateway-gui" / "survey-timing.json"
        )

    def duration_ms(self, key: str, nominal_ms: int) -> int:
        return round(max(
            1, nominal_ms * 0.1,
            min(nominal_ms * 2, nominal_ms + self.offsets_ms.get(key, 0.0)),
        ))

    def observe(self, key: str, nominal_ms: int, actual_ms: int) -> bool:
        if (
            key not in self.PHASES or nominal_ms <= 0 or actual_ms <= 0
            or not math.isfinite(actual_ms)
        ):
            return False
        error = max(-self.MAX_OFFSET_MS, min(
            self.MAX_OFFSET_MS, actual_ms - nominal_ms,
        ))
        previous = self.offsets_ms.get(key)
        self.offsets_ms[key] = (
            float(error) if previous is None else previous + 0.35 * (error - previous)
        )
        self._save()
        return True

    def _save(self) -> None:
        if self.path is None:
            return
        temporary: str | None = None
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(
                mode="w", encoding="utf-8", dir=self.path.parent,
                prefix=".survey-timing-", delete=False,
            ) as handle:
                temporary = handle.name
                json.dump({"version": 1, "offsets_ms": self.offsets_ms}, handle)
            os.replace(temporary, self.path)
        except OSError:
            # An unwritable preferences directory must not interrupt a survey;
            # the calibration remains usable for this GUI session.
            pass
        finally:
            if temporary is not None:
                try:
                    os.unlink(temporary)
                except OSError:
                    pass
