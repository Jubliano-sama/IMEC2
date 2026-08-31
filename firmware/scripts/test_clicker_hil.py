#!/usr/bin/env python3
"""Capture the four-probe click path with three anchors and a BLE gateway."""

from __future__ import annotations

import argparse
import os
import pty
import select
import signal
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
LOGS_DIR = REPO_ROOT / "logs" / "clicker_three_anchor_qualification_20260827"

CLICKER_PROBE = "E46070D247233537"
ANCHOR_DIRECT_PROBE = "E4645C15CB365D30"
ANCHOR_B_PROBE = "E46070D247394D36"
ANCHOR_C_PROBE = "E4645C15CB0F3B37"

PROBES = {
    "clicker": CLICKER_PROBE,
    "anchor_a": ANCHOR_DIRECT_PROBE,
    "anchor_b": ANCHOR_B_PROBE,
    "anchor_c": ANCHOR_C_PROBE,
}

RTT_INPUT_CHAR_DELAY_S = 0.05
DEFAULT_GATEWAY_NAME = "IMEC Mesh Test Gateway"
BLE_CONSUMER_CONNECTED_MARKER = b"BLE_CONNECTED "
BLE_CONSUMER_MONITOR_MARKER = b"command=monitor"
CLICKER_RTT_READY_MARKER = "DBG_CLICKER_RTT ready=1"
BLE_CONSUMER_SHUTDOWN_TIMEOUT_S = 5.0


def write_rtt_command(master_fd: int, command: bytes) -> None:
    """Pace PTY input so pyOCD's one-character stdin reader sees every byte."""
    for byte in command:
        os.write(master_fd, bytes((byte,)))
        time.sleep(RTT_INPUT_CHAR_DELAY_S)


def start_rtt_capture(role: str, probe_id: str) -> tuple[subprocess.Popen, int]:
    """Start pyocd rtt in a pseudo-terminal and return (process, master_fd)."""
    master_fd, slave_fd = pty.openpty()
    
    cmd = [
        str(REPO_ROOT / ".venv" / "bin" / "pyocd"), "rtt",
        "-t", "nrf52833",
        "-M", "attach",
        "-u", probe_id,
        "-a", "0x20000410",
        "-s", "0x100",
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


def start_ble_receipt_consumer(
    gateway: str,
    duration_s: float,
    connect_timeout_s: float,
) -> subprocess.Popen:
    """Start the repository's BLE monitor, which returns exact host receipts."""
    cmd = [
        str(REPO_ROOT / ".venv" / "bin" / "python"),
        str(REPO_ROOT / "firmware" / "scripts" / "provision_mesh_anchor.py"),
        "--gateway", gateway,
        "--command", "monitor",
        "--duration", str(duration_s),
        "--connect-timeout", str(connect_timeout_s),
    ]
    env = os.environ.copy()
    env["PYTHONUNBUFFERED"] = "1"
    return subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
        env=env,
    )


def read_process_output(
    proc: subprocess.Popen,
    output: list[bytes],
    timeout_s: float,
) -> bytes:
    """Read one available binary-output chunk without blocking indefinitely."""
    if proc.stdout is None:
        raise RuntimeError("BLE receipt consumer has no captured stdout")
    readable, _, _ = select.select([proc.stdout.fileno()], [], [], timeout_s)
    if not readable:
        return b""
    data = os.read(proc.stdout.fileno(), 4096)
    if data:
        output.append(data)
    return data


def wait_for_ble_receipt_consumer(
    proc: subprocess.Popen,
    output: list[bytes],
    timeout_s: float,
) -> None:
    """Prove the host-receipt consumer is connected before clicks are allowed."""
    deadline = time.monotonic() + timeout_s
    tail = b""
    while time.monotonic() < deadline:
        data = read_process_output(
            proc,
            output,
            min(0.1, max(0.0, deadline - time.monotonic())),
        )
        if data:
            tail = (tail + data)[-512:]
            if (
                BLE_CONSUMER_CONNECTED_MARKER in tail
                and BLE_CONSUMER_MONITOR_MARKER in tail
            ):
                print("BLE host-receipt consumer connected; click injection may arm")
                return
        status = proc.poll()
        if status is not None:
            detail = b"".join(output)[-1024:].decode("utf-8", errors="replace")
            raise RuntimeError(
                "BLE host-receipt consumer exited before connection "
                f"(status={status}): {detail}"
            )
    raise RuntimeError(
        "BLE host-receipt consumer did not connect before the bounded timeout"
    )


def stop_process(proc: subprocess.Popen | None) -> None:
    """Let the BLE client disconnect cleanly before bounded hard fallbacks."""
    if proc is None or proc.poll() is not None:
        return

    # asyncio.run() converts the first SIGINT into cancellation of its main
    # task.  That unwinds provision_mesh_anchor.py's BleakClient context and
    # waits for BlueZ disconnect, whereas Popen.terminate() skips that cleanup
    # and can leave the gateway connected with no owning monitor process.
    try:
        proc.send_signal(signal.SIGINT)
        proc.wait(timeout=BLE_CONSUMER_SHUTDOWN_TIMEOUT_S)
        return
    except Exception:
        pass

    try:
        proc.terminate()
        proc.wait(timeout=BLE_CONSUMER_SHUTDOWN_TIMEOUT_S)
        return
    except Exception:
        pass

    try:
        proc.kill()
        proc.wait(timeout=BLE_CONSUMER_SHUTDOWN_TIMEOUT_S)
    except Exception:
        pass


def run_click_test(
    config_name: str,
    duration_s: float = 30.0,
    pre_click_delay_s: float = 10.0,
    click_count: int = 1,
    click_interval_s: float = 20.0,
    command_sequence: tuple[str, ...] | None = None,
    gateway: str = DEFAULT_GATEWAY_NAME,
    ble_connect_timeout_s: float = 30.0,
) -> dict[str, str]:
    commands = command_sequence or ("CLICK",) * click_count
    if not commands:
        raise ValueError("at least one RTT command is required")
    if any(command not in ("CLICK", "LONG") for command in commands):
        raise ValueError("RTT commands must be CLICK or LONG")
    if click_interval_s <= 0.0:
        raise ValueError("click_interval_s must be positive")
    if ble_connect_timeout_s <= 0.0:
        raise ValueError("ble_connect_timeout_s must be positive")

    config_dir = LOGS_DIR / config_name
    config_dir.mkdir(parents=True, exist_ok=True)
    
    print(f"\n=======================================================")
    print(f"Starting test for configuration: {config_name}")
    print(f"=======================================================")
    
    procs: dict[str, subprocess.Popen] = {}
    fds: dict[str, int] = {}
    buffers: dict[str, list[bytes]] = {r: [] for r in PROBES}
    scan_tails: dict[str, str] = {r: "" for r in PROBES}
    ble_buffer: list[bytes] = []
    ble_proc: subprocess.Popen | None = None
    
    try:
        # Own the receipt consumer so a test can never inject traffic while the
        # gateway is correctly withholding ACKs from an absent host.
        expected_runtime_s = (
            pre_click_delay_s
            + max(0, len(commands) - 1) * click_interval_s
            + duration_s
        )
        ble_proc = start_ble_receipt_consumer(
            gateway,
            duration_s=max(60.0, expected_runtime_s + 120.0),
            connect_timeout_s=ble_connect_timeout_s,
        )
        wait_for_ble_receipt_consumer(
            ble_proc,
            ble_buffer,
            timeout_s=ble_connect_timeout_s * 2.0 + 5.0,
        )

        # Start all 4 RTT monitors
        for role, probe_id in PROBES.items():
            proc, master_fd = start_rtt_capture(role, probe_id)
            procs[role] = proc
            fds[role] = master_fd
            print(f"Started RTT monitor for {role} ({probe_id})")
        
        commands_sent = 0
        last_command_sent_at: float | None = None
        connected_roles: set[str] = set()
        clicker_down_channel_ready = False
        clicker_firmware_ready = False
        clicker_ready_query_sent = False
        injection_ready_at: float | None = None
        start_time = time.time()
        
        while time.time() - start_time < duration_s:
            if ble_proc.poll() is not None:
                read_process_output(ble_proc, ble_buffer, 0.0)
                detail = b"".join(ble_buffer)[-1024:].decode(
                    "utf-8", errors="replace"
                )
                raise RuntimeError(
                    "BLE host-receipt consumer stopped during the click test: "
                    f"{detail}"
                )
            if ble_proc.stdout is None:
                raise RuntimeError("BLE receipt consumer lost its captured stdout")

            ble_fd = ble_proc.stdout.fileno()
            read_list = [*fds.values(), ble_fd]
            r, _, _ = select.select(read_list, [], [], 0.05)
            for fd in r:
                if fd == ble_fd:
                    data = os.read(fd, 4096)
                    if data:
                        ble_buffer.append(data)
                    continue
                for role, role_fd in fds.items():
                    if fd == role_fd:
                        try:
                            data = os.read(fd, 4096)
                            if data:
                                buffers[role].append(data)
                                text = data.decode("utf-8", errors="replace")
                                scan_tails[role] = (scan_tails[role] + text)[-512:]
                                if "Reading from up channel 0" in scan_tails[role]:
                                    connected_roles.add(role)
                                if role == "clicker":
                                    for line in text.splitlines():
                                        if "DBG_CLICKER_RTT" in line or "DBG_CLICK" in line or "DBG_DS" in line:
                                            print(f"[{role}] {line}")
                                    if CLICKER_RTT_READY_MARKER in scan_tails[role]:
                                        clicker_firmware_ready = True
                                    if "Writing to down channel 0" in scan_tails[role]:
                                        clicker_down_channel_ready = True
                        except OSError:
                            pass

            if (not clicker_ready_query_sent and
                    not clicker_firmware_ready and
                    clicker_down_channel_ready and
                    connected_roles == set(PROBES)):
                write_rtt_command(fds["clicker"], b"READY\n")
                clicker_ready_query_sent = True
                print("All four RTT readers attached; querying clicker firmware readiness")
            
            if (injection_ready_at is None and
                    clicker_firmware_ready and
                    clicker_down_channel_ready and
                    connected_roles == set(PROBES)):
                injection_ready_at = time.time()
                print("All four RTT readers attached; click injection armed")

            now = time.time()
            first_command_due = (
                commands_sent == 0 and injection_ready_at is not None and
                now - injection_ready_at >= pre_click_delay_s
            )
            later_command_due = (
                0 < commands_sent < len(commands) and
                last_command_sent_at is not None and
                now - last_command_sent_at >= click_interval_s
            )
            if first_command_due or later_command_due:
                command_index = commands_sent
                command = commands[command_index]
                print(
                    f"\n>>> Injecting {command} {command_index + 1}/{len(commands)} "
                    f"at host_monotonic={now:.3f} <<<\n"
                )
                # Clear any unterminated input left by an interrupted prior run.
                write_rtt_command(fds["clicker"], b"\n")
                write_rtt_command(fds["clicker"], command.encode("ascii") + b"\n")
                commands_sent += 1
                last_command_sent_at = now
                # Keep the runner alive through a paced series and capture a full
                # duration after the final command.
                start_time = time.time()
        
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
        stop_process(ble_proc)
        if ble_proc is not None and ble_proc.stdout is not None:
            while read_process_output(ble_proc, ble_buffer, 0.0):
                pass
            ble_proc.stdout.close()

        ble_log_path = config_dir / "ble-monitor.typescript"
        ble_log_path.write_text(
            b"".join(ble_buffer).decode("utf-8", errors="replace"),
            encoding="utf-8",
        )
        print(f"Saved BLE receipt log to {ble_log_path}")

    results = {}
    for role, chunks in buffers.items():
        text = b"".join(chunks).decode("utf-8", errors="replace")
        log_path = config_dir / f"{role}.typescript"
        log_path.write_text(text, encoding="utf-8")
        results[role] = text
        print(f"Saved {role} log to {log_path} ({len(text)} bytes)")

    if commands_sent != len(commands):
        raise RuntimeError(
            f"only {commands_sent}/{len(commands)} commands were injected: "
            "RTT attachment or cadence gate was not met"
        )
    
    return results


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", choices=["direct", "forced"], default="direct")
    parser.add_argument(
        "--output-name",
        help="optional evidence subdirectory (defaults to --config)",
    )
    parser.add_argument("--duration", type=float, default=30.0)
    parser.add_argument("--pre-click-delay", type=float, default=10.0)
    parser.add_argument("--click-count", type=int, default=1)
    parser.add_argument("--click-interval", type=float, default=20.0)
    parser.add_argument("--gateway", default=DEFAULT_GATEWAY_NAME)
    parser.add_argument("--ble-connect-timeout", type=float, default=30.0)
    parser.add_argument(
        "--commands",
        help="comma-separated RTT gesture sequence, for example LONG,CLICK",
    )
    args = parser.parse_args()

    output_name = args.output_name or args.config
    output_path = Path(output_name)
    if output_path.is_absolute() or len(output_path.parts) != 1:
        parser.error("--output-name must be one relative directory name")
    command_sequence = None
    if args.commands is not None:
        command_sequence = tuple(
            command.strip().upper()
            for command in args.commands.split(",")
            if command.strip()
        )
    run_click_test(
        output_name,
        args.duration,
        args.pre_click_delay,
        args.click_count,
        args.click_interval,
        command_sequence,
        args.gateway,
        args.ble_connect_timeout,
    )
