#!/usr/bin/env python3
"""Multi-probe RTT capture with both up channels, plus RTT click injection.

Uses the pyOCD Python API so every board's status markers (up channel 0) and
firmware log (up channel 1) land in one timestamped file per role.  With
``--click-role`` it queries READY and injects CLICK gestures on that role's
down channel 0, and it runs the repository BLE receipt monitor so the gateway
is never left with an absent host.
"""

from __future__ import annotations

import argparse
import os
import re
import select
import signal
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / ".venv" / "lib"))

from pyocd.core.helpers import ConnectHelper  # noqa: E402
from pyocd.debug.rtt import RTTControlBlock  # noqa: E402

ANSI = re.compile(rb"\x1b\[[0-9;]*m")


class Probe:
    def __init__(self, role: str, uid: str, out_path: Path, reset: bool):
        self.role = role
        self.uid = uid
        self.out = out_path.open("wb")
        self.session = ConnectHelper.session_with_chosen_probe(
            unique_id=uid,
            target_override="nrf52833",
            connect_mode="pre-reset" if reset else "attach",
            options={"resume_on_disconnect": True, "frequency": 4000000},
        )
        self.session.open()
        target = self.session.board.target
        if reset:
            target.reset_and_halt()
            target.resume()
        # Let the firmware initialise the control block after a reset.
        deadline = time.monotonic() + 8.0
        last_error = None
        while True:
            try:
                # Search the complete padded ID in one snapshot. pyOCD's
                # bytewise search can lose its address after a partial ID
                # match in a live log buffer preceding the control block.
                ram = bytes(target.read_memory_block8(0x20000000, 0x8000))
                offset = ram.find(b"SEGGER RTT" + bytes(6))
                if offset < 0:
                    raise RuntimeError("RTT signature absent")
                counts = [int.from_bytes(ram[offset+i:offset+i+4], "little")
                          for i in (16, 20)]
                if not (0 < counts[0] <= 16 and 0 <= counts[1] <= 16):
                    raise RuntimeError(f"invalid RTT channel counts: {counts}")
                self.cb = RTTControlBlock.from_target(
                    target, address=0x20000000 + offset, size=0)
                self.cb.start()
                if self.cb.up_channels:
                    break
            except Exception as exc:
                last_error = exc
            if time.monotonic() > deadline:
                self.session.close()
                self.out.close()
                raise RuntimeError(f"{role}: RTT control block not found: {last_error}")
            time.sleep(0.2)
        self.up = list(self.cb.up_channels[:2])
        self.down = self.cb.down_channels[0] if self.cb.down_channels else None
        self.lock = threading.Lock()
        self.tail = b""

    def poll(self) -> None:
        with self.lock:
            for index, channel in enumerate(self.up):
                data = channel.read()
                if not data:
                    continue
                stamp = f"{time.monotonic():.3f} ".encode()
                for line in ANSI.sub(b"", data).split(b"\n"):
                    if line.strip():
                        self.out.write(stamp + b"[" + str(index).encode() + b"] " + line + b"\n")
                if index == 0:
                    self.tail = (self.tail + data)[-4096:]
            self.out.flush()

    def write(self, text: bytes) -> None:
        if self.down is None:
            raise RuntimeError(f"{self.role}: no down channel")
        with self.lock:
            for byte in text:
                self.down.write(bytes((byte,)))
                time.sleep(0.02)

    def close(self) -> None:
        try:
            self.poll()
        finally:
            self.out.close()
            self.session.close()


def start_ble_monitor(gateway: str, duration_s: float, log_path: Path,
                      notification_hold_s: float = 0.0) -> subprocess.Popen:
    cmd = [str(REPO_ROOT / ".venv" / "bin" / "python"),
           str(REPO_ROOT / "firmware" / "scripts" / "provision_mesh_anchor.py"),
           "--gateway", gateway, "--command", "monitor",
           "--duration", str(duration_s), "--connect-timeout", "30",
           "--notification-hold-s", str(notification_hold_s)]
    env = os.environ.copy()
    env["PYTHONUNBUFFERED"] = "1"
    return subprocess.Popen(cmd, stdout=log_path.open("wb"), stderr=subprocess.STDOUT, env=env)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", action="append", required=True,
                        metavar="ROLE=UID", help="repeatable role=probe id")
    parser.add_argument("--duration", type=float, default=40.0)
    parser.add_argument("--reset", action="store_true", help="reset every board first")
    parser.add_argument("--click-role", help="role whose down channel receives CLICK")
    parser.add_argument("--clicks", type=int, default=0)
    parser.add_argument("--commands", help="comma-separated CLICK/LONG gesture sequence")
    parser.add_argument("--click-interval", type=float, default=15.0)
    parser.add_argument("--first-click-delay", type=float, default=8.0)
    parser.add_argument("--gateway-name", default="IMEC Mesh Test Gateway")
    parser.add_argument("--no-ble", action="store_true")
    parser.add_argument("--notification-hold-s", type=float, default=0.0,
                        help="hold host notifications initially to exercise backpressure")
    parser.add_argument("--output-dir")
    args = parser.parse_args()
    commands = ([item.strip().upper() for item in args.commands.split(",")]
                if args.commands else ["CLICK"] * args.clicks)
    if any(command not in ("CLICK", "LONG") for command in commands):
        parser.error("--commands accepts only CLICK and LONG")

    stamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    out_dir = Path(args.output_dir) if args.output_dir else REPO_ROOT / "logs" / f"bench_rtt_{stamp}"
    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"logging to {out_dir}")

    ble = None
    if not args.no_ble:
        ble_log = out_dir / "ble-monitor.log"
        ble = start_ble_monitor(args.gateway_name, args.duration + 60.0, ble_log,
                                args.notification_hold_s)
        # Clicks must not start before the host is connected: with no host the
        # gateway's RAM stream fills after a few reports and it (correctly)
        # withholds ACKs, which would make every later click look like a miss.
        ble_deadline = time.monotonic() + 45.0
        while time.monotonic() < ble_deadline:
            if ble_log.exists() and b"BLE_CONNECTED" in ble_log.read_bytes():
                print("BLE monitor connected")
                break
            if ble.poll() is not None:
                raise RuntimeError("BLE monitor exited before connecting")
            time.sleep(0.5)
        else:
            raise RuntimeError("BLE monitor did not connect within 45 s")

    stop_requested = False

    def request_stop(signum, frame):
        nonlocal stop_requested
        stop_requested = True

    # Finish an in-flight USB transaction before closing the capture.
    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    probes: list[Probe] = []
    try:
        for spec in args.probe:
            role, uid = spec.split("=", 1)
            probes.append(Probe(role, uid, out_dir / f"{role}.log", args.reset))
            print(f"attached {role} ({uid})")
        clicker = next((p for p in probes if p.role == args.click_role), None)
        start = time.monotonic()
        clicks_sent = 0
        next_click = start + args.first_click_delay
        ready_sent = False
        while not stop_requested and time.monotonic() - start < args.duration:
            for p in probes:
                p.poll()
            if clicker is not None and not ready_sent and time.monotonic() - start > 2.0:
                clicker.write(b"\nREADY\n")
                ready_sent = True
            if clicker is not None and clicks_sent < len(commands) and time.monotonic() >= next_click:
                command = commands[clicks_sent]
                clicker.write(f"\n{command}\n".encode())
                clicks_sent += 1
                next_click = time.monotonic() + args.click_interval
                print(f"{command} {clicks_sent}/{len(commands)} at +{time.monotonic()-start:.1f}s")
            time.sleep(0.02)
    finally:
        for p in probes:
            try:
                p.close()
            except Exception as exc:  # pragma: no cover - bench tooling
                print(f"close {p.role}: {exc}")
        if ble is not None:
            ble.send_signal(signal.SIGINT)
            try:
                ble.wait(timeout=10)
            except Exception:
                ble.kill()
    print("done")


if __name__ == "__main__":
    main()
