#!/usr/bin/env python3
"""Run HIL clicker tests across DDD, F1F1D, and F2F1D configurations."""

from __future__ import annotations

import argparse
import os
import pty
import select
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
LOGS_DIR = REPO_ROOT / "logs" / "clicker_evaluation_20260818"

CLICKER_PROBE = "E46070D247233537"
ANCHOR_DIRECT_PROBE = "E4645C15CB365D30"
ANCHOR_B_PROBE = "E46070D247394D36"
ANCHOR_C_PROBE = "E4645C15CB0F3B37"

PROBES = {
    "clicker": CLICKER_PROBE,
    "direct": ANCHOR_DIRECT_PROBE,
    "anchor_b": ANCHOR_B_PROBE,
    "anchor_c": ANCHOR_C_PROBE,
}


def start_rtt_capture(role: str, probe_id: str) -> tuple[subprocess.Popen, int]:
    """Start pyocd rtt in a pseudo-terminal and return (process, master_fd)."""
    master_fd, slave_fd = pty.openpty()
    
    cmd = [
        "pyocd", "rtt",
        "-t", "nrf52833",
        "-M", "pre-reset",
        "-u", probe_id,
        "--up-channel-id", "0",
        "--down-channel-id", "0",
    ]
    
    env = os.environ.copy()
    env["PATH"] = f"{REPO_ROOT}/.venv/bin:{env.get('PATH', '')}"
    env["SHELL"] = "/bin/bash"

    proc = subprocess.Popen(
        cmd,
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=slave_fd,
        close_fds=True,
        env=env,
    )
    os.close(slave_fd)
    return proc, master_fd


def run_click_test(config_name: str, duration_s: float = 12.0) -> dict[str, str]:
    config_dir = LOGS_DIR / config_name
    config_dir.mkdir(parents=True, exist_ok=True)
    
    print(f"\n=======================================================")
    print(f"Starting test for configuration: {config_name}")
    print(f"=======================================================")
    
    procs: dict[str, subprocess.Popen] = {}
    fds: dict[str, int] = {}
    buffers: dict[str, list[bytes]] = {r: [] for r in PROBES}
    
    try:
        # Start all 4 RTT monitors
        for role, probe_id in PROBES.items():
            proc, master_fd = start_rtt_capture(role, probe_id)
            procs[role] = proc
            fds[role] = master_fd
            print(f"Started RTT monitor for {role} ({probe_id})")
        
        click_sent = False
        ready_seen = False
        start_time = time.time()
        
        while time.time() - start_time < duration_s:
            read_list = list(fds.values())
            r, _, _ = select.select(read_list, [], [], 0.05)
            for fd in r:
                for role, role_fd in fds.items():
                    if fd == role_fd:
                        try:
                            data = os.read(fd, 4096)
                            if data:
                                buffers[role].append(data)
                                text = data.decode("utf-8", errors="replace")
                                if role == "clicker":
                                    for line in text.splitlines():
                                        if "DBG_CLICKER_RTT" in line or "DBG_CLICK" in line or "DBG_DS" in line:
                                            print(f"[{role}] {line}")
                                    if "DBG_CLICKER_RTT ready=1" in text:
                                        ready_seen = True
                        except OSError:
                            pass
            
            if ready_seen and not click_sent:
                time.sleep(0.5)
                print("\n>>> Injecting CLICK command to clicker RTT <<<\n")
                os.write(fds["clicker"], b"CLICK\n")
                click_sent = True
                # extend duration to record after click
                start_time = time.time()
                duration_s = 10.0
        
    finally:
        for role, proc in procs.items():
            try:
                proc.terminate()
                proc.wait(timeout=2.0)
            except Exception:
                proc.kill()
        for role, fd in fds.items():
            try:
                os.close(fd)
            except Exception:
                pass

    results = {}
    for role, chunks in buffers.items():
        text = b"".join(chunks).decode("utf-8", errors="replace")
        log_path = config_dir / f"{role}.typescript"
        log_path.write_text(text, encoding="utf-8")
        results[role] = text
        print(f"Saved {role} log to {log_path} ({len(text)} bytes)")
    
    return results


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", choices=["ddd", "f1f1d", "f2f1d"], default="ddd")
    parser.add_argument("--duration", type=float, default=12.0)
    args = parser.parse_args()
    
    run_click_test(args.config, args.duration)
