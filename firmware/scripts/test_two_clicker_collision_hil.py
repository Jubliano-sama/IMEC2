#!/usr/bin/env python3
"""Inject two RTT clicks together and prove their DS-TWR work is serialized."""

from __future__ import annotations

import argparse
import os
import re
import select
import time
from pathlib import Path

from test_clicker_hil import (
    DEFAULT_GATEWAY_NAME,
    read_process_output,
    start_ble_receipt_consumer,
    start_rtt_capture,
    stop_process,
    wait_for_ble_receipt_consumer,
)


REPO_ROOT = Path(__file__).resolve().parents[2]
PROBES = {
    "clicker_1": "E4645C15CB0F3B37",
    "clicker_2": "E4645C15CB365D30",
    "anchor_b": "E46070D247394D36",
    "anchor_c": "E46070D247233537",
}
CLICKER_ROLES = ("clicker_1", "clicker_2")
RTT_INPUT_CHAR_DELAY_S = 0.05
MAX_ACCEPT_SKEW_S = 0.100


def _event_times(timeline: list[str], role: str, marker: str) -> list[float]:
    prefix = f" {role} "
    return [
        float(line.split(" ", 1)[0])
        for line in timeline
        if prefix in line and marker in line
    ]


def _range_intervals(timeline: list[str], role: str) -> list[tuple[float, float]]:
    starts = _event_times(timeline, role, "DBG_CLICKER_RANGE state=START")
    ends = _event_times(timeline, role, "DBG_CLICKER_RANGE state=END")
    if len(starts) != len(ends):
        raise RuntimeError(
            f"{role} left an incomplete ranging interval "
            f"(starts={len(starts)} ends={len(ends)})"
        )
    intervals = list(zip(starts, ends, strict=True))
    if any(end < start for start, end in intervals):
        raise RuntimeError(f"{role} emitted an inverted ranging interval")
    return intervals


def inject_simultaneous_clicks(fds: dict[str, int]) -> None:
    """Finish both RTT command lines back-to-back in the same poll interval."""
    for role in CLICKER_ROLES:
        os.write(fds[role], b"\n")
    time.sleep(RTT_INPUT_CHAR_DELAY_S)
    for byte in b"CLICK":
        for role in CLICKER_ROLES:
            os.write(fds[role], bytes((byte,)))
        time.sleep(RTT_INPUT_CHAR_DELAY_S)
    for role in CLICKER_ROLES:
        os.write(fds[role], b"\n")


def run(output_dir: Path, gateway: str, capture_after_s: float) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    procs = {}
    fds: dict[str, int] = {}
    buffers: dict[str, list[bytes]] = {role: [] for role in PROBES}
    tails: dict[str, str] = {role: "" for role in PROBES}
    timeline: list[str] = []
    ble_buffer: list[bytes] = []
    ble_proc = None
    injected_at: float | None = None
    ready_at: float | None = None
    connected_roles: set[str] = set()
    down_ready: set[str] = set()

    try:
        ble_proc = start_ble_receipt_consumer(
            gateway,
            duration_s=max(90.0, capture_after_s + 60.0),
            connect_timeout_s=30.0,
        )
        wait_for_ble_receipt_consumer(ble_proc, ble_buffer, timeout_s=65.0)

        for role, probe_id in PROBES.items():
            proc, master_fd = start_rtt_capture(role, probe_id)
            procs[role] = proc
            fds[role] = master_fd
            print(f"RTT attached role={role} probe={probe_id}")

        deadline = time.monotonic() + capture_after_s + 45.0
        while time.monotonic() < deadline:
            if ble_proc.poll() is not None:
                raise RuntimeError("BLE receipt consumer stopped during capture")
            if ble_proc.stdout is None:
                raise RuntimeError("BLE receipt consumer lost stdout")

            ble_fd = ble_proc.stdout.fileno()
            readable, _, _ = select.select([*fds.values(), ble_fd], [], [], 0.02)
            for fd in readable:
                if fd == ble_fd:
                    data = os.read(fd, 4096)
                    if data:
                        ble_buffer.append(data)
                    continue
                for role, role_fd in fds.items():
                    if fd != role_fd:
                        continue
                    try:
                        data = os.read(fd, 4096)
                    except OSError:
                        data = b""
                    if not data:
                        continue
                    buffers[role].append(data)
                    text = data.decode("utf-8", errors="replace")
                    tails[role] = (tails[role] + text)[-1024:]
                    timestamp = time.monotonic()
                    for line in text.splitlines():
                        timeline.append(f"{timestamp:.6f} {role} {line}")
                    if "Reading from up channel 0" in tails[role]:
                        connected_roles.add(role)
                    if (
                        role in CLICKER_ROLES
                        and "Writing to down channel 0" in tails[role]
                    ):
                        down_ready.add(role)

            if (
                ready_at is None
                and connected_roles == set(PROBES)
                and down_ready == set(CLICKER_ROLES)
            ):
                ready_at = time.monotonic()
                print("All four RTT streams attached; dual injection armed")

            if (
                injected_at is None
                and ready_at is not None
                and time.monotonic() - ready_at >= 2.0
            ):
                injected_at = time.monotonic()
                inject_simultaneous_clicks(fds)
                print(f"DUAL_CLICK_INJECTED host_monotonic={injected_at:.6f}")

            if injected_at is not None and time.monotonic() - injected_at >= capture_after_s:
                break
    finally:
        for proc in procs.values():
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=2.0)
                except Exception:
                    proc.kill()
        for fd in fds.values():
            try:
                os.close(fd)
            except OSError:
                pass
        stop_process(ble_proc)
        if ble_proc is not None and ble_proc.stdout is not None:
            while read_process_output(ble_proc, ble_buffer, 0.0):
                pass
            ble_proc.stdout.close()

        for role, chunks in buffers.items():
            (output_dir / f"{role}.typescript").write_text(
                b"".join(chunks).decode("utf-8", errors="replace"),
                encoding="utf-8",
            )
        (output_dir / "ble-monitor.typescript").write_text(
            b"".join(ble_buffer).decode("utf-8", errors="replace"),
            encoding="utf-8",
        )
        (output_dir / "timeline.log").write_text(
            "\n".join(timeline) + "\n", encoding="utf-8"
        )

    if injected_at is None:
        raise RuntimeError("dual click was not injected")

    clicker_text = {
        role: b"".join(buffers[role]).decode("utf-8", errors="replace")
        for role in CLICKER_ROLES
    }
    for role, text in clicker_text.items():
        if "DBG_CLICKER_RTT command=CLICK accepted=1 ret=0" not in text:
            raise RuntimeError(f"{role} did not accept its RTT click")
        if "DBG_TX_WAKE_CLAIM_SENT" not in text:
            raise RuntimeError(f"{role} never transmitted a wake claim")
        if "DBG_CLICKER_CLICK state=COMPLETE ret=0" not in text:
            raise RuntimeError(f"{role} did not complete a successful click")

    accepted_at = {
        role: _event_times(
            timeline, role, "DBG_CLICKER_RTT command=CLICK accepted=1 ret=0"
        )[0]
        for role in CLICKER_ROLES
    }
    accept_skew_s = abs(accepted_at["clicker_1"] - accepted_at["clicker_2"])
    if accept_skew_s > MAX_ACCEPT_SKEW_S:
        raise RuntimeError(
            "RTT clicks were not simultaneous enough to test arbitration "
            f"(accept_skew_ms={accept_skew_s * 1000.0:.1f})"
        )

    range_intervals = {
        role: _range_intervals(timeline, role) for role in CLICKER_ROLES
    }
    for role, intervals in range_intervals.items():
        if not intervals:
            raise RuntimeError(f"{role} never entered DS-TWR ranging")
    for first_start, first_end in range_intervals["clicker_1"]:
        for second_start, second_end in range_intervals["clicker_2"]:
            if max(first_start, second_start) < min(first_end, second_end):
                raise RuntimeError(
                    "clicker DS-TWR intervals overlapped "
                    f"(clicker_1=[{first_start:.6f},{first_end:.6f}] "
                    f"clicker_2=[{second_start:.6f},{second_end:.6f}])"
                )

    first_role, second_role = sorted(
        CLICKER_ROLES, key=lambda role: range_intervals[role][0][0]
    )
    first_complete = range_intervals[first_role][-1][1]
    second_start = range_intervals[second_role][0][0]

    ble_deferred = [
        role
        for role, text in clicker_text.items()
        if re.search(r"DBG_BLE_COURTESY_DECISION .*defer=1", text)
    ]
    gap_ms = (second_start - first_complete) * 1000.0
    print(
        "DUAL_CLICK_COLLISION_AVOIDANCE_OK "
        f"accept_skew_ms={accept_skew_s * 1000.0:.1f} "
        f"first_range={first_role} second_range={second_role} "
        f"range_non_overlap_gap_ms={gap_ms:.1f} "
        f"range_attempts={','.join(f'{role}:{len(range_intervals[role])}' for role in CLICKER_ROLES)} "
        f"ble_defer_log={','.join(ble_deferred) or 'not-captured'}"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=REPO_ROOT / "logs" / "two_clicker_collision_20260827" / "run-01",
    )
    parser.add_argument("--gateway", default=DEFAULT_GATEWAY_NAME)
    parser.add_argument("--capture-after", type=float, default=35.0)
    args = parser.parse_args()
    if args.capture_after <= 0.0:
        parser.error("--capture-after must be positive")
    run(args.output_dir, args.gateway, args.capture_after)


if __name__ == "__main__":
    main()
