#!/usr/bin/env python3
"""Monte Carlo model for the IMEC2 BLE courtesy detection table.

The model is intentionally small and deterministic so the documentation table can
be regenerated after changing firmware timing constants. It models two clickers
that enter BLE courtesy at the same wall-clock time, with the scan started before
advertising as in firmware/app/src/main.c.
"""

from __future__ import annotations

import argparse
import random
import re
from pathlib import Path

BLE_UNIT_US = 625
DEFAULT_WINDOWS_MS = (10, 15, 20, 25, 27, 30, 40, 50, 60, 75, 90, 100, 125, 150, 200)
DEFAULT_TRIALS = 1_000_000
DEFAULT_SEED = 0x1A2B3C4D
# Conservative channel-37 radio event occupancy used by the architecture docs.
DEFAULT_ADV_EVENT_US = 1_000
DEFAULT_ADV_DELAY_US = 10_000
INF_US = 10**18

DEFINE_RE = re.compile(r"^#define\s+(BLE_COURTESY_[A-Z0-9_]+)\s+([^\s\\]+)", re.MULTILINE)


def parse_define_value(raw: str) -> int:
    value = raw.strip().rstrip("uUlL")
    return int(value, 0)


def load_firmware_constants(main_c: Path) -> dict[str, int]:
    text = main_c.read_text(encoding="utf-8")
    values = {name: parse_define_value(raw) for name, raw in DEFINE_RE.findall(text)}
    required = (
        "BLE_COURTESY_ADV_INTERVAL_MIN_UNITS",
        "BLE_COURTESY_ADV_INTERVAL_MAX_UNITS",
        "BLE_COURTESY_SCAN_INTERVAL_UNITS",
        "BLE_COURTESY_SCAN_WINDOW_UNITS",
        "BLE_COURTESY_MIN_WINDOW_MS",
    )
    missing = [name for name in required if name not in values]
    if missing:
        raise SystemExit(f"missing firmware constants in {main_c}: {', '.join(missing)}")
    return values


def advertising_events(rng: random.Random,
                       max_window_us: int,
                       adv_min_us: int,
                       adv_max_us: int,
                       adv_delay_us: int,
                       adv_event_us: int) -> list[tuple[float, float]]:
    # The firmware starts advertising after scan start, but the controller does
    # not expose the first radio anchor. Treat two independent controllers as
    # having independent first-event phase over one possible advertising period.
    t = rng.uniform(0.0, float(adv_max_us + adv_delay_us))
    events: list[tuple[float, float]] = []
    while t + adv_event_us <= max_window_us:
        events.append((t, t + adv_event_us))
        t += rng.uniform(float(adv_min_us), float(adv_max_us))
        t += rng.uniform(0.0, float(adv_delay_us))
    return events


def event_is_inside_scan(tx_start_us: float,
                         tx_end_us: float,
                         scan_interval_us: int,
                         scan_window_us: int) -> bool:
    # Scan starts at t=0. There is no random scan phase in this model.
    scan_phase_us = tx_start_us % scan_interval_us
    return scan_phase_us + (tx_end_us - tx_start_us) <= scan_window_us


def overlaps_any(event: tuple[float, float], blockers: list[tuple[float, float]]) -> bool:
    start_us, end_us = event
    for block_start_us, block_end_us in blockers:
        if block_end_us <= start_us:
            continue
        if block_start_us >= end_us:
            return False
        return True
    return False


def earliest_full_packet_detection_us(peer_tx_events: list[tuple[float, float]],
                                      local_tx_events: list[tuple[float, float]],
                                      scan_interval_us: int,
                                      scan_window_us: int) -> float:
    for event in peer_tx_events:
        if event_is_inside_scan(event[0], event[1], scan_interval_us, scan_window_us):
            # The nRF BLE radio is single-event. A local advertising event removes
            # that time from passive scan RX, even if the host has scan enabled.
            if not overlaps_any(event, local_tx_events):
                return event[1]
    return INF_US


def run_simulation(trials: int,
                   seed: int,
                   windows_ms: tuple[int, ...],
                   adv_min_us: int,
                   adv_max_us: int,
                   adv_delay_us: int,
                   adv_event_us: int,
                   scan_interval_us: int,
                   scan_window_us: int) -> list[tuple[int, float, float, float]]:
    windows_us = tuple(window_ms * 1000 for window_ms in windows_ms)
    max_window_us = max(windows_us)
    rng = random.Random(seed)
    counts = [[0, 0, 0] for _ in windows_us]

    for _ in range(trials):
        higher_tx = advertising_events(rng, max_window_us, adv_min_us, adv_max_us,
                                       adv_delay_us, adv_event_us)
        lower_tx = advertising_events(rng, max_window_us, adv_min_us, adv_max_us,
                                      adv_delay_us, adv_event_us)
        lower_hears_higher_at = earliest_full_packet_detection_us(
            higher_tx, lower_tx, scan_interval_us, scan_window_us)
        higher_hears_lower_at = earliest_full_packet_detection_us(
            lower_tx, higher_tx, scan_interval_us, scan_window_us)

        for index, window_us in enumerate(windows_us):
            lower_hears_higher = lower_hears_higher_at <= window_us
            higher_hears_lower = higher_hears_lower_at <= window_us
            counts[index][0] += int(lower_hears_higher)
            counts[index][1] += int(lower_hears_higher or higher_hears_lower)
            counts[index][2] += int(lower_hears_higher and higher_hears_lower)

    return [
        (
            window_ms,
            100.0 * lower_hears_higher / trials,
            100.0 * at_least_one / trials,
            100.0 * mutual / trials,
        )
        for window_ms, (lower_hears_higher, at_least_one, mutual)
        in zip(windows_ms, counts)
    ]


def markdown_table(rows: list[tuple[int, float, float, float]], highlight_ms: int) -> str:
    lines = [
        "| Courtesy window | Lower hears higher | At least one direction hears | Mutual detection |",
        "| ---: | ---: | ---: | ---: |",
    ]
    for window_ms, lower_hears_higher, at_least_one, mutual in rows:
        cells = [
            f"{window_ms} ms",
            f"{lower_hears_higher:.1f}%",
            f"{at_least_one:.1f}%",
            f"{mutual:.1f}%",
        ]
        if window_ms == highlight_ms:
            cells = [f"**{cell}**" for cell in cells]
        lines.append("| " + " | ".join(cells) + " |")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--main-c", type=Path, default=Path("firmware/app/src/main.c"))
    parser.add_argument("--trials", type=int, default=DEFAULT_TRIALS)
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=DEFAULT_SEED)
    parser.add_argument("--adv-event-us", type=int, default=DEFAULT_ADV_EVENT_US)
    parser.add_argument("--adv-delay-us", type=int, default=DEFAULT_ADV_DELAY_US)
    args = parser.parse_args()

    constants = load_firmware_constants(args.main_c)
    adv_min_us = constants["BLE_COURTESY_ADV_INTERVAL_MIN_UNITS"] * BLE_UNIT_US
    adv_max_us = constants["BLE_COURTESY_ADV_INTERVAL_MAX_UNITS"] * BLE_UNIT_US
    scan_interval_us = constants["BLE_COURTESY_SCAN_INTERVAL_UNITS"] * BLE_UNIT_US
    scan_window_us = constants["BLE_COURTESY_SCAN_WINDOW_UNITS"] * BLE_UNIT_US
    highlight_ms = constants["BLE_COURTESY_MIN_WINDOW_MS"]

    rows = run_simulation(
        trials=args.trials,
        seed=args.seed,
        windows_ms=DEFAULT_WINDOWS_MS,
        adv_min_us=adv_min_us,
        adv_max_us=adv_max_us,
        adv_delay_us=args.adv_delay_us,
        adv_event_us=args.adv_event_us,
        scan_interval_us=scan_interval_us,
        scan_window_us=scan_window_us,
    )

    print(f"trials: {args.trials}")
    print(f"seed: 0x{args.seed:x}")
    print(f"advertising interval: {adv_min_us / 1000:.3f}-{adv_max_us / 1000:.3f} ms")
    print(f"advertising delay: 0-{args.adv_delay_us / 1000:.3f} ms")
    print(f"advertising radio event: {args.adv_event_us / 1000:.3f} ms")
    print(f"passive scan: {scan_window_us / 1000:.3f} ms every {scan_interval_us / 1000:.3f} ms")
    print()
    print(markdown_table(rows, highlight_ms))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
