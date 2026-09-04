#!/usr/bin/env python3
"""Bench click run with one DK flashed as an RTT-controllable clicker.

Probe layout while the production clicker has no probe: the gateway stays on
E46070D247233537, two DKs run the anchor image, and the third DK runs the
mesh_clicker image with the RTT control overlay so CLICK can be injected.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import test_clicker_hil as hil  # noqa: E402

hil.PROBES = {
    "clicker": "E4645C15CB0F3B37",
    "anchor_a": "E4645C15CB365D30",
    "anchor_b": "E46070D247394D36",
    "gateway": "E46070D247233537",
}
hil.LOGS_DIR = hil.REPO_ROOT / "logs" / "click_two_anchor_bench_20260903"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-name", default="run")
    parser.add_argument("--duration", type=float, default=30.0)
    parser.add_argument("--pre-click-delay", type=float, default=10.0)
    parser.add_argument("--click-count", type=int, default=1)
    parser.add_argument("--click-interval", type=float, default=20.0)
    args = parser.parse_args()
    hil.run_click_test(
        args.output_name,
        args.duration,
        args.pre_click_delay,
        args.click_count,
        args.click_interval,
    )


if __name__ == "__main__":
    main()
