#!/usr/bin/env python3
import argparse
import os
import pty
import select
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

PROBES = {
    "gateway": "E46070D247233537",
    "direct": "E4645C15CB365D30",
    "anchor_b": "E46070D247394D36",
    "anchor_c": "E4645C15CB0F3B37",
}


def start_rtt_capture(role: str, probe_id: str) -> tuple[subprocess.Popen, int]:
    master_fd, slave_fd = pty.openpty()
    cmd = [
        str(REPO_ROOT / ".venv" / "bin" / "pyocd"),
        "rtt",
        "-t",
        "nrf52833",
        "-M",
        "pre-reset",
        "-u",
        probe_id,
        "-a",
        "0x20000410",
        "-s",
        "0x200",
        "--up-channel-id",
        "0",
        "--down-channel-id",
        "0",
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


def main():
    parser = argparse.ArgumentParser(description="Run 4-board bench test with full RTT logging")
    parser.add_argument("--command", choices=["here-i-am", "assign-slots", "survey"], default="survey")
    parser.add_argument("--gateway-name", default="IMEC Mesh Test Gateway")
    parser.add_argument("--expected-anchors", type=int, default=3)
    parser.add_argument("--expected-pairs", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--erase-storage", action="store_true", help="Erase NVS storage partition (0x7a000-0x80000) on all boards before starting")
    parser.add_argument("--deepest-hop", type=int, default=None)
    args = parser.parse_args()

    timestamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    log_dir = REPO_ROOT / "logs" / f"four_board_{args.command}_{timestamp}"
    log_dir.mkdir(parents=True, exist_ok=True)
    print(f"=== Starting 4-board {args.command} bench test ===")
    print(f"Logging to: {log_dir}")

    procs: dict[str, subprocess.Popen] = {}
    fds: dict[str, int] = {}
    buffers: dict[str, list[bytes]] = {r: [] for r in PROBES}
    # Clear any stale BlueZ BLE connections
    out = subprocess.run(["bluetoothctl", "devices", "Connected"], capture_output=True, text=True).stdout
    for line in out.strip().splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0] == "Device":
            subprocess.run(["bluetoothctl", "disconnect", parts[1]], capture_output=True, timeout=5)
    time.sleep(1.0)
    if args.erase_storage:
        print("Erasing NVS storage partition on all probes...")
        pyocd_bin = str(REPO_ROOT / ".venv" / "bin" / "pyocd")
        for role, probe_id in PROBES.items():
            subprocess.run([pyocd_bin, "erase", "-t", "nrf52833", "-u", probe_id, "--sector", "0x7a000-0x80000"], capture_output=True)
            subprocess.run([pyocd_bin, "reset", "-t", "nrf52833", "-u", probe_id], capture_output=True)
            print(f"  Erased & reset {role} ({probe_id})")
        time.sleep(1.0)

    try:
        for role, probe_id in PROBES.items():
            proc, master_fd = start_rtt_capture(role, probe_id)
            procs[role] = proc
            fds[role] = master_fd
            print(f"  Started RTT monitor for {role} ({probe_id})")

        # Let RTT monitors connect and catch reset logs
        time.sleep(2.0)

        # Build provision command
        prov_cmd = [
            str(REPO_ROOT / ".venv" / "bin" / "python"),
            str(REPO_ROOT / "firmware" / "scripts" / "provision_mesh_anchor.py"),
            "--gateway",
            args.gateway_name,
            "--command",
            args.command,
            "--connect-timeout",
            "30",
            "--route-refresh-timeout",
            "90",
            "--assignment-timeout",
            "180",
        ]
        if args.command == "survey":
            prov_cmd.extend([
                "--require-survey-success",
                "--expected-anchors",
                str(args.expected_anchors),
                "--expected-pairs",
                str(args.expected_pairs),
            ])
        elif args.command == "assign-slots":
            prov_cmd.extend([
                "--require-assignment-success",
                "--expected-anchors",
                str(args.expected_anchors),
            ])
        if args.deepest_hop is not None:
            prov_cmd.extend(["--deepest-hop", str(args.deepest_hop)])

        print(f"\nRunning command: {' '.join(prov_cmd)}\n")
        prov_proc = subprocess.Popen(
            prov_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )

        prov_output = []
        start_time = time.time()

        while True:
            # Check provision proc stdout
            if prov_proc.poll() is not None:
                # Drain remaining stdout
                for line in prov_proc.stdout:
                    sys.stdout.write(f"[PROV] {line}")
                    prov_output.append(line)
                break

            # Read available stdout lines non-blocking
            rlist, _, _ = select.select([prov_proc.stdout], [], [], 0.1)
            if rlist:
                line = prov_proc.stdout.readline()
                if line:
                    sys.stdout.write(f"[PROV] {line}")
                    prov_output.append(line)

            # Drain RTT buffers
            for role, fd in fds.items():
                r_fds, _, _ = select.select([fd], [], [], 0.01)
                if r_fds:
                    try:
                        data = os.read(fd, 4096)
                        if data:
                            buffers[role].append(data)
                    except OSError:
                        pass

            if time.time() - start_time > args.timeout:
                print("\nTIMEOUT reached! Terminating provision process...")
                prov_proc.terminate()
                break

        # Save provision output
        (log_dir / "provision.log").write_text("".join(prov_output))

        # Drain final RTT
        time.sleep(1.0)
        for role, fd in fds.items():
            r_fds, _, _ = select.select([fd], [], [], 0.1)
            if r_fds:
                try:
                    data = os.read(fd, 4096)
                    if data:
                        buffers[role].append(data)
                except OSError:
                    pass
            # Write out RTT log
            raw_data = b"".join(buffers[role])
            text = raw_data.decode("utf-8", errors="replace")
            (log_dir / f"{role}.log").write_text(text)
            print(f"Saved {role}.log ({len(text)} chars)")

        retcode = prov_proc.returncode if prov_proc.poll() is not None else -1
        print(f"\n=== Test completed with exit code: {retcode} ===")
        return retcode

    finally:
        for role, proc in procs.items():
            try:
                proc.terminate()
                proc.wait(timeout=1.0)
            except Exception:
                try:
                    proc.kill()
                except Exception:
                    pass
        for fd in fds.values():
            try:
                os.close(fd)
            except Exception:
                pass


if __name__ == "__main__":
    sys.exit(main())
