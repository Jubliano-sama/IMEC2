#!/usr/bin/env python3
"""Run programmatic HIL clicker tests using pyOCD RTT across DDD, F1F1D, and F2F1D."""

from __future__ import annotations

import argparse
import sys
import threading
import time
from pathlib import Path

from pyocd.core.helpers import ConnectHelper
from pyocd.debug.rtt import RTTControlBlock

REPO_ROOT = Path(__file__).resolve().parents[2]
LOGS_DIR = REPO_ROOT / "logs" / "clicker_evaluation_20260818"

PROBES = {
    "clicker": "E46070D247233537",
    "direct": "E4645C15CB365D30",
    "anchor_b": "E46070D247394D36",
    "anchor_c": "E4645C15CB0F3B37",
}


class ProbeMonitor(threading.Thread):
    def __init__(self, role: str, probe_id: str):
        super().__init__(daemon=True)
        self.role = role
        self.probe_id = probe_id
        self.running = True
        self.buffer = bytearray()
        self.session = None
        self.control_block = None
        self.up_chan = None
        self.down_chan = None
        self.error = None

    def run(self):
        try:
            self.session = ConnectHelper.session_with_chosen_probe(
                unique_id=self.probe_id,
                target_override="nrf52833",
                connect_mode="attach",
            )
            self.session.open()
            target = self.session.board.target
            self.control_block = RTTControlBlock.from_target(
                target, address=0x20000410, size=0x100
            )
            self.control_block.start()
            if self.control_block.up_channels:
                self.up_chan = self.control_block.up_channels[0]
            if self.control_block.down_channels:
                self.down_chan = self.control_block.down_channels[0]

            while self.running:
                if self.up_chan:
                    data = self.up_chan.read()
                    if data:
                        self.buffer.extend(data)
                time.sleep(0.01)

        except Exception as exc:
            self.error = exc
        finally:
            if self.session:
                try:
                    self.session.close()
                except Exception:
                    pass

    def write_down(self, data: bytes):
        if self.down_chan:
            self.down_chan.write(data)

    def stop(self):
        self.running = False


def run_configuration_test(config_name: str, wait_before_s: float = 2.0, capture_s: float = 8.0) -> dict[str, str]:
    config_dir = LOGS_DIR / config_name
    config_dir.mkdir(parents=True, exist_ok=True)

    print(f"\n=======================================================")
    print(f"Executing Clicker Evaluation on Configuration: {config_name.upper()}")
    print(f"=======================================================")

    monitors: dict[str, ProbeMonitor] = {}
    for role, probe_id in PROBES.items():
        m = ProbeMonitor(role, probe_id)
        monitors[role] = m
        m.start()
        print(f"Connected monitor for {role} ({probe_id})")

    # Let monitors sync and gather initial logs
    time.sleep(wait_before_s)

    print(f"\n>>> Injecting CLICK command to clicker via RTT <<<\n")
    clicker_mon = monitors["clicker"]
    clicker_mon.write_down(b"\nCLICK\n")

    # Capture logs for capture_s
    time.sleep(capture_s)

    # Stop monitors
    for m in monitors.values():
        m.stop()
    for m in monitors.values():
        m.join(timeout=2.0)

    results = {}
    for role, m in monitors.items():
        text = m.buffer.decode("utf-8", errors="replace")
        log_file = config_dir / f"{role}.typescript"
        log_file.write_text(text, encoding="utf-8")
        results[role] = text
        print(f"Saved {role} log to {log_file} ({len(text)} bytes, error={m.error})")

    return results


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", choices=["ddd", "f1f1d", "f2f1d"], required=True)
    args = parser.parse_args()
    
    run_configuration_test(args.config)
