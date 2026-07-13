#!/usr/bin/env python3
"""Flash and qualify production-candidate mesh firmware in place."""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import uuid
from contextlib import contextmanager
from pathlib import Path

from intelhex import IntelHex

import verify_stack_evidence as verifier

REPO_ROOT = Path(__file__).resolve().parents[2]
FLASH_FREQUENCY_HZ = 4_000_000
WEST_EXECUTABLE = REPO_ROOT / ".venv" / "bin" / "west"
PYOCD_EXECUTABLE = REPO_ROOT / ".venv" / "bin" / "pyocd"
CAPTURE_EXECUTABLE = REPO_ROOT / "firmware" / "scripts" / "capture_stack_evidence.py"
CAPTURE_LEDGER = REPO_ROOT / "logs" / "stack-evidence" / "verified-capture-ledger.jsonl"
TRANSACTION_DIRECTORY = REPO_ROOT / "logs" / "stack-evidence" / "flash-transactions"

TARGET_NAME = "nrf52833"
TARGET_FLASH_ADDRESS = 0x00000000
TARGET_FLASH_SIZE = 512 * 1024
TARGET_FLASH_SECTOR_SIZE = 4096
TRANSACTION_SCHEMA = 1

# capture_stack_evidence.py inherits the TTY-backed pyOCD stream.  pyOCD can
# leave an SGR reset on stdout without a newline, so the capture tool's final
# manifest path may be immediately prefixed or suffixed by colour decoration.
# Accept only SGR sequences here; cursor movement, hyperlinks, and other
# terminal controls remain invalid rather than being silently rewritten.
_ANSI_SGR_RE = re.compile(r"\x1b\[[0-9:;]*m")


class TransactionError(RuntimeError):
    """A transaction could not be safely completed or recovered."""


def _checkpoint(_name: str) -> None:
    """Test seam for simulating power loss after durable boundaries."""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _probe_key(probe_id: str) -> str:
    return hashlib.sha256(probe_id.encode("utf-8")).hexdigest()[:24]


def _journal_path(probe_id: str) -> Path:
    return TRANSACTION_DIRECTORY / f"{_probe_key(probe_id)}.json"


def _fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _atomic_json(path: Path, data: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.{uuid.uuid4().hex}.tmp")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(temporary, flags, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as destination:
            json.dump(data, destination, indent=2, sort_keys=True)
            destination.write("\n")
            destination.flush()
            os.fsync(destination.fileno())
        os.replace(temporary, path)
        _fsync_directory(path.parent)
    finally:
        temporary.unlink(missing_ok=True)


def _durable_unlink(path: Path) -> None:
    if path.exists():
        path.unlink()
        _fsync_directory(path.parent)


def _durable_sync(path: Path) -> None:
    with path.open("rb") as source:
        os.fsync(source.fileno())
    _fsync_directory(path.parent)


@contextmanager
def _ledger_lock(path: Path | None = None):
    path = path or CAPTURE_LEDGER
    lock_path = path.with_suffix(path.suffix + ".lock")
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open("a+", encoding="utf-8") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(lock.fileno(), fcntl.LOCK_UN)


def _ledger_records(path: Path | None = None) -> list[dict[str, object]]:
    path = path or CAPTURE_LEDGER
    if not path.is_file():
        return []
    records: list[dict[str, object]] = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if not line.strip():
            continue
        try:
            item = json.loads(line)
        except ValueError as exc:
            raise TransactionError(f"capture ledger is corrupt at line {line_number}") from exc
        if not isinstance(item, dict) or not isinstance(item.get("capture_id"), str):
            raise TransactionError(f"capture ledger has an invalid record at line {line_number}")
        records.append(item)
    return records


def _consumed_capture_ids(path: Path | None = None) -> set[str]:
    return {str(item["capture_id"]) for item in _ledger_records(path)}


def _ledger_line(record: dict[str, object]) -> str:
    return json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n"


def _record_sha256(record: dict[str, object]) -> str:
    return hashlib.sha256(_ledger_line(record).encode("utf-8")).hexdigest()


def _record_consumed_capture(record: dict[str, object], path: Path | None = None) -> None:
    path = path or CAPTURE_LEDGER
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as ledger:
        ledger.write(_ledger_line(record))
        ledger.flush()
        os.fsync(ledger.fileno())
    _fsync_directory(path.parent)


def _run(command: list[str], *, capture_output: bool = False) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, capture_output=capture_output, text=True, check=False)


def _probe_is_visible(probe_id: str) -> None:
    result = _run([str(PYOCD_EXECUTABLE), "list", "--json"], capture_output=True)
    visible = False
    if result.returncode == 0:
        try:
            devices = json.loads(result.stdout)
        except ValueError as exc:
            raise TransactionError("pyOCD probe enumeration did not produce JSON") from exc
        visible = isinstance(devices, list) and any(
            isinstance(device, dict) and probe_id in {
                str(device.get("unique_id", "")),
                str(device.get("uid", "")),
                str(device.get("id", "")),
            }
            for device in devices
        )
    else:
        plain = _run([str(PYOCD_EXECUTABLE), "list"], capture_output=True)
        if plain.returncode:
            raise TransactionError(f"cannot enumerate pyOCD probes: {plain.stderr.strip()}")
        visible = any(probe_id in line.split() for line in plain.stdout.splitlines())
    if not visible:
        raise TransactionError(f"requested probe {probe_id} is not attached")


def _commander(probe_id: str, *commands: str) -> None:
    command = [
        str(PYOCD_EXECUTABLE), "commander", "--no-config", "-t", TARGET_NAME,
        "-u", probe_id, "-f", str(FLASH_FREQUENCY_HZ), "-M", "halt",
    ]
    for item in commands:
        command.extend(["-c", item])
    result = _run(command, capture_output=True)
    combined = f"{result.stdout}\n{result.stderr}"
    if result.returncode or re.search(r"(?:^|\n)\s*(?:Error|Traceback)\b", combined, re.IGNORECASE):
        detail = result.stderr.strip() or result.stdout.strip()
        raise TransactionError(
            f"pyOCD commander failed with exit status {result.returncode}: {detail}"
        )


def _read_target_flash(probe_id: str, destination: Path, *, reset_after: bool = False) -> str:
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.unlink(missing_ok=True)
    save = (
        f"savemem 0x{TARGET_FLASH_ADDRESS:x} 0x{TARGET_FLASH_SIZE:x} "
        f"{shlex.quote(str(destination))}"
    )
    commands = (save, "reset") if reset_after else (save,)
    _commander(probe_id, *commands)
    if not destination.is_file() or destination.stat().st_size != TARGET_FLASH_SIZE:
        actual = destination.stat().st_size if destination.is_file() else 0
        raise TransactionError(
            f"target flash readback has {actual} bytes, expected {TARGET_FLASH_SIZE}"
        )
    os.chmod(destination, 0o600)
    with destination.open("rb") as backup:
        os.fsync(backup.fileno())
    _fsync_directory(destination.parent)
    return _sha256(destination)


def _expected_staged_image(backup: Path, hex_path: Path) -> bytes:
    original = backup.read_bytes()
    if len(original) != TARGET_FLASH_SIZE:
        raise TransactionError("target backup is not a complete 512 KiB flash image")
    try:
        image = IntelHex(str(hex_path))
    except Exception as exc:
        raise TransactionError(f"candidate HEX cannot be parsed: {exc}") from exc
    addresses = image.addresses()
    if not addresses:
        raise TransactionError("candidate HEX contains no flash data")
    if min(addresses) < TARGET_FLASH_ADDRESS or max(addresses) >= TARGET_FLASH_SIZE:
        raise TransactionError("candidate HEX contains an address outside the 512 KiB application flash")
    expected = bytearray(original)
    sectors = {address // TARGET_FLASH_SECTOR_SIZE for address in addresses}
    for sector in sectors:
        start = sector * TARGET_FLASH_SECTOR_SIZE
        expected[start:start + TARGET_FLASH_SECTOR_SIZE] = b"\xff" * TARGET_FLASH_SECTOR_SIZE
    for address in addresses:
        expected[address] = image[address]
    return bytes(expected)


def _code_sector_hashes(expected: bytes, hex_path: Path) -> dict[str, str]:
    try:
        image = IntelHex(str(hex_path))
    except Exception as exc:
        raise TransactionError(f"candidate HEX cannot be parsed: {exc}") from exc
    addresses = image.addresses()
    if not addresses or min(addresses) < 0 or max(addresses) >= TARGET_FLASH_SIZE:
        raise TransactionError("candidate HEX contains an address outside the 512 KiB application flash")
    sectors = sorted({address // TARGET_FLASH_SECTOR_SIZE for address in addresses})
    return {
        f"0x{sector * TARGET_FLASH_SECTOR_SIZE:08x}": hashlib.sha256(
            expected[
                sector * TARGET_FLASH_SECTOR_SIZE:
                (sector + 1) * TARGET_FLASH_SECTOR_SIZE
            ]
        ).hexdigest()
        for sector in sectors
    }


def _sector_hash_map_sha256(sectors: dict[str, str]) -> str:
    encoded = json.dumps(sectors, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _code_sectors_match(readback: Path, expected: dict[str, str]) -> bool:
    content = readback.read_bytes()
    if len(content) != TARGET_FLASH_SIZE or not expected:
        return False
    for address_text, expected_sha256 in expected.items():
        try:
            address = int(address_text, 0)
        except (TypeError, ValueError):
            return False
        if (address < 0 or address % TARGET_FLASH_SECTOR_SIZE
                or address + TARGET_FLASH_SECTOR_SIZE > TARGET_FLASH_SIZE
                or not isinstance(expected_sha256, str)):
            return False
        actual = hashlib.sha256(content[address:address + TARGET_FLASH_SECTOR_SIZE]).hexdigest()
        if actual != expected_sha256:
            return False
    return True


def _artifact_paths(build_dir: Path) -> tuple[Path, Path]:
    return build_dir / "zephyr" / "zephyr.elf", build_dir / "zephyr" / "zephyr.hex"


def _assert_artifacts_unchanged(data: dict[str, object], build_dir: Path) -> None:
    elf, hex_path = _artifact_paths(build_dir)
    if (_sha256(elf) != data["build_elf_sha256"]
            or _sha256(hex_path) != data["build_hex_sha256"]):
        raise TransactionError("candidate ELF or HEX changed during the deployment transaction")


def _sync_capture_artifacts(manifest: Path) -> None:
    try:
        data = json.loads(manifest.read_text(encoding="utf-8"))
        relative = data["transcript"]["path"]
        transcript = (manifest.parent / relative).resolve()
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise TransactionError("qualified capture manifest cannot be synchronized") from exc
    if not transcript.is_file():
        raise TransactionError("qualified capture transcript is missing before promotion")
    for path in (transcript, manifest):
        with path.open("rb") as source:
            os.fsync(source.fileno())
        _fsync_directory(path.parent)


def _load_journal(probe_id: str) -> dict[str, object] | None:
    path = _journal_path(probe_id)
    if not path.is_file():
        return None
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise TransactionError("active flash transaction journal is unreadable") from exc
    required = {
        "schema", "transaction_id", "state", "probe_id", "preset",
        "backup_path", "backup_sha256", "build_elf_sha256", "build_hex_sha256",
        "expected_staged_sha256", "code_sector_sha256",
    }
    allowed_states = {"prepared", "staging", "staged", "promotion_intent", "committed"}
    if (not isinstance(data, dict) or set(data) < required
            or data.get("schema") != TRANSACTION_SCHEMA
            or data.get("probe_id") != probe_id
            or data.get("state") not in allowed_states):
        raise TransactionError("active flash transaction journal is invalid")
    backup = Path(str(data["backup_path"])).resolve()
    allowed = (TRANSACTION_DIRECTORY / _probe_key(probe_id)).resolve()
    if backup.parent.parent != allowed:
        raise TransactionError("active flash transaction backup is outside the transaction directory")
    return data


def _write_journal(data: dict[str, object]) -> None:
    _atomic_json(_journal_path(str(data["probe_id"])), data)


def _cleanup_transaction(data: dict[str, object]) -> None:
    backup = Path(str(data["backup_path"]))
    transaction_directory = backup.parent
    # The journal is removed first. A crash after this point leaves only an
    # inert orphan backup, never an unrecoverable active transaction.
    _durable_unlink(_journal_path(str(data["probe_id"])))
    for candidate in transaction_directory.iterdir() if transaction_directory.is_dir() else ():
        candidate.unlink(missing_ok=True)
    if transaction_directory.is_dir():
        transaction_directory.rmdir()
        _fsync_directory(transaction_directory.parent)
    probe_directory = transaction_directory.parent
    if probe_directory.is_dir() and not any(probe_directory.iterdir()):
        probe_directory.rmdir()
        _fsync_directory(probe_directory.parent)


def _cleanup_unjournaled_transaction(transaction_directory: Path) -> None:
    for candidate in transaction_directory.iterdir() if transaction_directory.is_dir() else ():
        candidate.unlink(missing_ok=True)
    if transaction_directory.is_dir():
        transaction_directory.rmdir()
        _fsync_directory(transaction_directory.parent)
    probe_directory = transaction_directory.parent
    if probe_directory.is_dir() and not any(probe_directory.iterdir()):
        probe_directory.rmdir()
        _fsync_directory(probe_directory.parent)


def _recover_interrupted_transaction(probe_id: str) -> None:
    data = _load_journal(probe_id)
    if data is None:
        return
    _probe_is_visible(probe_id)
    # Qualification is observational: an interrupted or rejected candidate is
    # deliberately left on the target so the same image can be debugged and
    # iterated.  The journal protects local capture/ledger bookkeeping only;
    # it is never an instruction to restore the previous firmware.
    _commander(probe_id, "reset")
    _cleanup_transaction(data)


def _verify_candidate(build_dir: Path, probe_id: str) -> tuple[verifier.BuildEvidence | None, list[str]]:
    issues: list[str] = []
    try:
        policies, frame_limit = verifier.load_policy()
        build = verifier.verify_build(build_dir, policies, frame_limit)
    except (OSError, verifier.EvidenceError) as exc:
        return None, [str(exc)]
    issues.extend(build.issues)
    policy = policies.get(build.preset)
    if build.preset not in verifier.DEPLOYABLE_PRESETS or policy is None or not policy.deployable:
        issues.append(f"{build.preset or '<missing>'} is not an allowed deployment preset")
    if not WEST_EXECUTABLE.is_file():
        issues.append(f"repository west is missing: {WEST_EXECUTABLE}")
    if not PYOCD_EXECUTABLE.is_file():
        issues.append(f"repository pyOCD is missing: {PYOCD_EXECUTABLE}")
    if not CAPTURE_EXECUTABLE.is_file():
        issues.append(f"repository capture tool is missing: {CAPTURE_EXECUTABLE}")
    try:
        _consumed_capture_ids()
    except TransactionError as exc:
        issues.append(str(exc))
    if not issues:
        try:
            _probe_is_visible(probe_id)
        except TransactionError as exc:
            issues.append(str(exc))
    return build, issues


def verify_flash(build_dir: Path, manifest: Path, probe_id: str) -> tuple[verifier.BuildEvidence | None, str | None, list[str]]:
    try:
        policies, frame_limit = verifier.load_policy()
        build = verifier.verify_build(build_dir, policies, frame_limit)
        consumed = _consumed_capture_ids()
    except (OSError, verifier.EvidenceError, TransactionError) as exc:
        return None, None, [str(exc)]
    issues = list(build.issues)
    policy = policies.get(build.preset)
    if build.preset not in verifier.DEPLOYABLE_PRESETS or policy is None or not policy.deployable:
        issues.append(f"{build.preset or '<missing>'} is not an allowed deployment preset")
    results, hardware_issues = verifier.verify_hardware(
        [manifest], [build], policies, True, {build.preset}, consumed
    )
    issues.extend(hardware_issues)
    capture_id: str | None = None
    for result in results:
        issues.extend(result.issues)
        if result.probe_id and result.probe_id != probe_id:
            issues.append(f"trusted capture probe {result.probe_id} does not match selected probe {probe_id}")
        capture_id = result.capture_id
    if not results:
        issues.append("trusted capture is missing for the exact deployment preset")
    return build, capture_id, issues


def _stage_candidate(build_dir: Path, probe_id: str) -> None:
    command = [
        str(WEST_EXECUTABLE), "flash", "--runner", "pyocd", "--build-dir", str(build_dir),
        "--", "--dev-id", probe_id, "--frequency", str(FLASH_FREQUENCY_HZ),
        "--flash-opt=--no-reset",
    ]
    result = _run(command)
    if result.returncode:
        raise TransactionError(f"candidate staging failed with exit status {result.returncode}")


def _capture_candidate(build_dir: Path, probe_id: str, output_dir: Path,
                       duration_seconds: int) -> Path:
    command = [
        sys.executable, str(CAPTURE_EXECUTABLE),
        "--build-dir", str(build_dir),
        "--probe-id", probe_id,
        "--output-dir", str(output_dir),
        "--duration-seconds", str(duration_seconds),
    ]
    result = _run(command, capture_output=True)
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise TransactionError(f"candidate qualification capture failed: {detail}")
    return _capture_manifest_path(result.stdout, output_dir)


def _capture_manifest_path(stdout: str, output_dir: Path) -> Path:
    undecorated = _ANSI_SGR_RE.sub("", stdout)
    candidate = ""
    for raw_line in reversed(undecorated.split("\n")):
        line = raw_line[:-1] if raw_line.endswith("\r") else raw_line
        line = line.strip(" ")
        if line:
            candidate = line
            break
    if not candidate:
        raise TransactionError("candidate qualification capture returned no manifest")
    if any(ord(char) < 0x20 or 0x7f <= ord(char) <= 0x9f for char in candidate):
        raise TransactionError("candidate qualification manifest path contains unsupported control data")

    manifest = Path(candidate).resolve()
    capture_directory = output_dir.resolve()
    if manifest.suffix != ".json":
        raise TransactionError("candidate qualification manifest path is not a JSON file")
    if manifest.parent != capture_directory:
        raise TransactionError(
            "candidate qualification manifest is outside the requested output directory"
        )
    if not manifest.is_file():
        raise TransactionError(f"candidate qualification manifest is missing: {manifest}")
    return manifest


def _qualify_staged_candidate(build_dir: Path, probe_id: str, output_dir: Path,
                              duration_seconds: int) -> Path:
    """Run the bounded repository-owned qualification phase.

    This single seam intentionally owns qualification orchestration so a
    preset-specific workload driver can later run alongside RTT without
    accepting a user-authored command or changing transaction semantics.
    """
    return _capture_candidate(build_dir, probe_id, output_dir, duration_seconds)


def _start_transaction(build: verifier.BuildEvidence, build_dir: Path,
                       probe_id: str) -> dict[str, object]:
    transaction_id = uuid.uuid4().hex
    transaction_directory = TRANSACTION_DIRECTORY / _probe_key(probe_id) / transaction_id
    transaction_directory.mkdir(parents=True, exist_ok=False)
    _fsync_directory(transaction_directory.parent)
    backup = transaction_directory / "target-flash-backup.bin"
    try:
        backup_sha256 = _read_target_flash(probe_id, backup)
        _elf_path, hex_path = _artifact_paths(build_dir)
        expected_staged = _expected_staged_image(backup, hex_path)
        expected_staged_sha256 = hashlib.sha256(expected_staged).hexdigest()
        code_sector_sha256 = _code_sector_hashes(expected_staged, hex_path)
        data: dict[str, object] = {
            "schema": TRANSACTION_SCHEMA,
            "transaction_id": transaction_id,
            "state": "prepared",
            "probe_id": probe_id,
            "preset": build.preset,
            "build_dir": str(build_dir.resolve()),
            "build_elf_sha256": build.elf_sha256,
            "build_hex_sha256": build.hex_sha256,
            "backup_path": str(backup.resolve()),
            "backup_sha256": backup_sha256,
            "expected_staged_sha256": expected_staged_sha256,
            "code_sector_sha256": code_sector_sha256,
        }
        _write_journal(data)
        _checkpoint("journal_durable")
        return data
    except Exception:
        try:
            if _journal_path(probe_id).is_file():
                _recover_interrupted_transaction(probe_id)
            else:
                _commander(probe_id, "reset")
                _cleanup_unjournaled_transaction(transaction_directory)
        except Exception as cleanup_error:
            raise TransactionError(
                f"transaction preparation failed and target cleanup failed: {cleanup_error}"
            ) from cleanup_error
        raise


def _deploy(build: verifier.BuildEvidence, build_dir: Path, probe_id: str,
            output_dir: Path, duration_seconds: int) -> None:
    data = _start_transaction(build, build_dir, probe_id)
    backup = Path(str(data["backup_path"]))
    transaction_directory = backup.parent
    try:
        data["state"] = "staging"
        _assert_artifacts_unchanged(data, build_dir)
        _write_journal(data)
        _checkpoint("staging_durable")
        _stage_candidate(build_dir, probe_id)
        _checkpoint("candidate_staged")
        _assert_artifacts_unchanged(data, build_dir)

        staged = transaction_directory / "staged-readback.bin"
        staged_sha256 = _read_target_flash(probe_id, staged)
        staged.unlink(missing_ok=True)
        if staged_sha256 != data["expected_staged_sha256"]:
            raise TransactionError("staged 512 KiB flash does not match the sector-erase HEX overlay")
        data["state"] = "staged"
        data["staged_flash_sha256"] = staged_sha256
        _write_journal(data)
        _checkpoint("staged_hash_durable")
        _commander(probe_id, "reset")

        manifest = _qualify_staged_candidate(
            build_dir, probe_id, output_dir, duration_seconds
        )
        verified_build, capture_id, issues = verify_flash(build_dir, manifest, probe_id)
        if issues or verified_build is None or capture_id is None:
            raise TransactionError("captured candidate was rejected: " + "; ".join(issues))
        if verified_build.hex_sha256 != data["build_hex_sha256"] or verified_build.preset != data["preset"]:
            raise TransactionError("candidate build changed during qualification")
        _assert_artifacts_unchanged(data, build_dir)
        _sync_capture_artifacts(manifest)
        deployment_record: dict[str, object] = {
            "capture_id": capture_id,
            "preset": build.preset,
            "probe_id": probe_id,
            "staged_flash_sha256": staged_sha256,
            "code_sector_map_sha256": _sector_hash_map_sha256(
                data["code_sector_sha256"]  # type: ignore[arg-type]
            ),
            "transaction_id": str(data["transaction_id"]),
        }
        data["state"] = "promotion_intent"
        data["capture_id"] = capture_id
        data["manifest_path"] = str(manifest)
        data["deployment_record"] = deployment_record
        data["deployment_record_sha256"] = _record_sha256(deployment_record)
        _write_journal(data)
        _checkpoint("promotion_intent_durable")

        _record_consumed_capture(deployment_record)
        _checkpoint("promotion_ledger_durable")
        data["state"] = "committed"
        _write_journal(data)
        _checkpoint("commit_durable")
        _cleanup_transaction(data)
    except BaseException as exc:
        # BaseException represents abrupt process loss in tests; the durable
        # journal survives so the next invocation can reset the retained image
        # and discard incomplete local bookkeeping without restoring firmware.
        if not isinstance(exc, Exception):
            raise
        try:
            _commander(probe_id, "reset")
            _cleanup_transaction(data)
        except Exception as cleanup_error:
            raise TransactionError(
                f"deployment failed and local transaction cleanup remains pending: {cleanup_error}"
            ) from cleanup_error
        raise


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--probe-id", required=True)
    parser.add_argument("--output-dir", type=Path, default=CAPTURE_LEDGER.parent)
    parser.add_argument("--duration-seconds", type=int, default=300)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if args.duration_seconds < 1 or args.duration_seconds > int(verifier.MAX_CAPTURE_DURATION.total_seconds()):
        print("verified deployment blocked: duration must be between 1 and 900 seconds", file=sys.stderr)
        return 1
    try:
        with _ledger_lock():
            _recover_interrupted_transaction(args.probe_id)
            build, issues = _verify_candidate(args.build_dir, args.probe_id)
            if issues or build is None:
                print("verified deployment blocked:", file=sys.stderr)
                for issue in issues:
                    print(f"  {issue}", file=sys.stderr)
                return 1
            _deploy(build, args.build_dir, args.probe_id, args.output_dir, args.duration_seconds)
    except (OSError, verifier.EvidenceError, TransactionError) as exc:
        print(f"verified deployment failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
