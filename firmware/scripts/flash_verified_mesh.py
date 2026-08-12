#!/usr/bin/env python3
"""Stage and promote production-candidate mesh firmware transactionally.

``--stage-only`` performs the one permitted target write and leaves a durable
qualification journal.  A later invocation with ``--hardware-manifest``
promotes that already-running image without programming it again.  A failed
candidate must be explicitly rejected before another image can be staged.
If the target was deliberately disconnected or replaced, an explicit abandon
archives the journal and backup without claiming anything about target state.
``--stage-only --initialize-storage`` is the explicit first-migration path for
durable presets: it backs up the whole target, journals the storage preimage,
and erases only the compiled storage partition before the candidate is reset.
``--complete-bench-qualification`` closes a non-promotable forced-hop bench
journal only after its exact capture and three-role topology are validated.
``--supersede-staged-candidate`` retires a stale journal only after a live
readback proves that its recorded code has already been replaced out of band.
"""

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
from datetime import datetime, timezone
from pathlib import Path

from intelhex import IntelHex

import artifact_cohort as cohort
import verify_stack_evidence as verifier

REPO_ROOT = Path(__file__).resolve().parents[2]
FLASH_FREQUENCY_HZ = 4_000_000
WEST_EXECUTABLE = REPO_ROOT / ".venv" / "bin" / "west"
PYOCD_EXECUTABLE = REPO_ROOT / ".venv" / "bin" / "pyocd"
CAPTURE_LEDGER = REPO_ROOT / "logs" / "stack-evidence" / "verified-capture-ledger.jsonl"
TRANSACTION_DIRECTORY = REPO_ROOT / "logs" / "stack-evidence" / "flash-transactions"
COHORT_DIRECTORY = REPO_ROOT / "logs" / "stack-evidence" / "cohorts"
BENCH_QUALIFICATION_DIRECTORY = (
    REPO_ROOT / "logs" / "stack-evidence" / "bench-qualifications"
)

TARGET_NAME = "nrf52833"
TARGET_FLASH_ADDRESS = 0x00000000
TARGET_FLASH_SIZE = 512 * 1024
TARGET_FLASH_SECTOR_SIZE = 4096
STORAGE_PARTITION_ADDRESS = 0x7A000
STORAGE_PARTITION_END = 0x80000
STORAGE_PARTITION_SIZE = STORAGE_PARTITION_END - STORAGE_PARTITION_ADDRESS
TRANSACTION_SCHEMA = 1
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
_SECTOR_ADDRESS_RE = re.compile(r"^0x[0-9a-f]{8}$")

class TransactionError(RuntimeError):
    """A transaction could not be safely completed or recovered."""


def _utc_text() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


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


def _write_json_once(path: Path, data: dict[str, object]) -> None:
    """Durably create an archive, accepting only an identical recovery copy."""
    if path.exists():
        try:
            existing = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise TransactionError(f"existing archive is unreadable: {path}") from exc
        if existing != data:
            raise TransactionError(f"existing archive differs from recovery state: {path}")
        _durable_sync(path)
        return
    _atomic_json(path, data)


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


def _commander(
    probe_id: str,
    *commands: str,
    connect_mode: str = "halt",
    resume_on_disconnect: bool | None = None,
) -> None:
    command = [
        str(PYOCD_EXECUTABLE), "commander", "--no-config", "-t", TARGET_NAME,
        "-u", probe_id, "-f", str(FLASH_FREQUENCY_HZ), "-M", connect_mode,
    ]
    if resume_on_disconnect is not None:
        command.extend([
            "-O",
            "resume_on_disconnect=" +
            ("true" if resume_on_disconnect else "false"),
        ])
    for item in commands:
        command.extend(["-c", item])
    result = _run(command, capture_output=True)
    combined = f"{result.stdout}\n{result.stderr}"
    if result.returncode or re.search(r"(?:^|\n)\s*(?:Error|Traceback)\b", combined, re.IGNORECASE):
        detail = result.stderr.strip() or result.stdout.strip()
        raise TransactionError(
            f"pyOCD commander failed with exit status {result.returncode}: {detail}"
        )


def _read_target_flash(
    probe_id: str,
    destination: Path,
    *,
    reset_after: bool = False,
    resume_after: bool = False,
    connect_mode: str = "halt",
    keep_halted: bool = False,
) -> str:
    if reset_after and resume_after:
        raise TransactionError("target readback cannot both reset and resume")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.unlink(missing_ok=True)
    save = (
        f"savemem 0x{TARGET_FLASH_ADDRESS:x} 0x{TARGET_FLASH_SIZE:x} "
        f"{shlex.quote(str(destination))}"
    )
    if resume_after:
        # pyOCD commander connects in halt mode. Keep resume separate so a
        # failed savemem cannot skip the best-effort release in this finally.
        try:
            _commander(probe_id, save, connect_mode=connect_mode)
        finally:
            _commander(probe_id, "continue")
    else:
        commands = (save, "reset") if reset_after else (save,)
        _commander(
            probe_id,
            *commands,
            connect_mode=connect_mode,
            resume_on_disconnect=False if keep_halted else None,
        )
    return _verified_readback_sha256(destination)


def _verified_readback_sha256(destination: Path) -> str:
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


def _erase_storage_partition(probe_id: str) -> None:
    """Erase storage while preventing the application from resuming."""
    command = [
        str(PYOCD_EXECUTABLE), "erase", "--no-config", "-t", TARGET_NAME,
        "-u", probe_id, "-f", str(FLASH_FREQUENCY_HZ),
        "-M", "under-reset", "-O", "resume_on_disconnect=false",
        "--sector",
        f"0x{STORAGE_PARTITION_ADDRESS:x}-0x{STORAGE_PARTITION_END:x}",
    ]
    result = _run(command, capture_output=True)
    combined = f"{result.stdout}\n{result.stderr}"
    if result.returncode or re.search(
        r"(?:^|\n)\s*(?:Error|Traceback)\b", combined, re.IGNORECASE
    ):
        detail = result.stderr.strip() or result.stdout.strip()
        raise TransactionError(
            f"pyOCD storage erase failed with exit status {result.returncode}: {detail}"
        )


def _expected_staged_image(
    backup: Path,
    hex_path: Path,
    *,
    erase_storage_partition: bool = False,
) -> bytes:
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
    if erase_storage_partition:
        expected[STORAGE_PARTITION_ADDRESS:STORAGE_PARTITION_END] = (
            b"\xff" * STORAGE_PARTITION_SIZE
        )
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


def _valid_code_sector_hashes(expected: object) -> bool:
    if not isinstance(expected, dict) or not expected:
        return False
    for address_text, expected_sha256 in expected.items():
        if (not isinstance(address_text, str) or
                _SECTOR_ADDRESS_RE.fullmatch(address_text) is None or
                not isinstance(expected_sha256, str) or
                _SHA256_RE.fullmatch(expected_sha256) is None):
            return False
        address = int(address_text, 16)
        if (address % TARGET_FLASH_SECTOR_SIZE or
                address + TARGET_FLASH_SECTOR_SIZE > TARGET_FLASH_SIZE):
            return False
    return True


def _code_sectors_match(readback: Path, expected: object) -> bool:
    content = readback.read_bytes()
    if len(content) != TARGET_FLASH_SIZE or not _valid_code_sector_hashes(expected):
        return False
    assert isinstance(expected, dict)
    for address_text, expected_sha256 in expected.items():
        address = int(address_text, 16)
        actual = hashlib.sha256(content[address:address + TARGET_FLASH_SECTOR_SIZE]).hexdigest()
        if actual != expected_sha256:
            return False
    return True


def _storage_partition_sha256(content: bytes) -> str:
    if len(content) != TARGET_FLASH_SIZE:
        raise TransactionError("storage hash requires a complete target flash image")
    return hashlib.sha256(
        content[STORAGE_PARTITION_ADDRESS:STORAGE_PARTITION_END]
    ).hexdigest()


def _erased_storage_partition_sha256() -> str:
    return hashlib.sha256(b"\xff" * STORAGE_PARTITION_SIZE).hexdigest()


def _storage_initialization(data: dict[str, object]) -> dict[str, object] | None:
    if data.get("storage_initialized") is not True:
        return None
    value = data.get("storage_initialization")
    return value if isinstance(value, dict) else None


def _valid_storage_initialization(data: dict[str, object]) -> bool:
    initialized = data.get("storage_initialized")
    if initialized is None:
        return "storage_initialization" not in data
    if not isinstance(initialized, bool):
        return False
    if not initialized:
        return "storage_initialization" not in data

    value = _storage_initialization(data)
    if value is None:
        return False
    if (
        value.get("intent") != "erase_storage_partition"
        or value.get("range_start") != STORAGE_PARTITION_ADDRESS
        or value.get("range_end") != STORAGE_PARTITION_END
        or value.get("size") != STORAGE_PARTITION_SIZE
        or value.get("erased_storage_sha256") != _erased_storage_partition_sha256()
        or not isinstance(value.get("pre_storage_sha256"), str)
        or _SHA256_RE.fullmatch(str(value.get("pre_storage_sha256"))) is None
        or value.get("phase") not in {
            "not_started", "programmed_not_erased", "erase_started",
            "erased_not_verified", "erased_not_reset", "complete",
        }
    ):
        return False
    return all(
        timestamp is None or (isinstance(timestamp, str) and bool(timestamp))
        for timestamp in (
            value.get("erase_started_at_utc"),
            value.get("erase_completed_at_utc"),
            value.get("erase_verified_at_utc"),
        )
    )


def _storage_erase_is_verified(data: dict[str, object]) -> bool:
    value = _storage_initialization(data)
    return value is not None and value.get("phase") in {
        "erased_not_reset", "complete",
    } and all(
        isinstance(value.get(key), str) and bool(value.get(key))
        for key in (
            "erase_started_at_utc",
            "erase_completed_at_utc",
            "erase_verified_at_utc",
        )
    )


def _storage_is_erased(readback: Path, data: dict[str, object]) -> bool:
    value = _storage_initialization(data)
    if value is None:
        return True
    try:
        content = readback.read_bytes()
    except OSError:
        return False
    return _storage_partition_sha256(content) == value["erased_storage_sha256"]


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
        capture_directory = manifest.parent.resolve()
        transcript = (capture_directory / relative).resolve()
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise TransactionError("trusted capture manifest cannot be synchronized") from exc
    if transcript == capture_directory or capture_directory not in transcript.parents:
        raise TransactionError("trusted capture transcript escapes its capture directory")
    if not transcript.is_file():
        raise TransactionError("trusted capture transcript is missing before promotion")
    for path in (transcript, manifest):
        with path.open("rb") as source:
            os.fsync(source.fileno())
        _fsync_directory(path.parent)


def _sync_topology_capture_artifacts(topology: dict[str, object]) -> None:
    roles = topology.get("roles")
    if not isinstance(roles, dict):
        raise TransactionError("validated topology lacks role captures")
    for preset in sorted(cohort.BENCH_TOPOLOGY_PRESETS):
        role = roles.get(preset)
        if not isinstance(role, dict):
            raise TransactionError(f"validated topology lacks {preset} capture")
        manifest = Path(str(role.get("capture_manifest_path", "")))
        try:
            capture = json.loads(manifest.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise TransactionError(
                f"topology capture cannot be synchronized for {preset}"
            ) from exc
        if not isinstance(capture, dict):
            raise TransactionError(
                f"topology capture is invalid for {preset}"
            )
        if "transcript" in capture:
            _sync_capture_artifacts(manifest)
        else:
            _durable_sync(manifest)


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
        "build_dir", "backup_path", "backup_sha256", "build_elf_sha256",
        "build_hex_sha256", "expected_staged_sha256", "code_sector_sha256",
    }
    allowed_states = {
        "prepared", "staging", "staged", "awaiting_qualification",
        "promotion_intent", "bench_completion_intent", "supersede_intent",
        "committed", "rejected",
    }
    if (not isinstance(data, dict) or set(data) < required
            or data.get("schema") != TRANSACTION_SCHEMA
            or data.get("probe_id") != probe_id
            or data.get("state") not in allowed_states
            or any(not isinstance(data.get(key), str) or not data.get(key) for key in (
                "transaction_id", "preset", "build_dir", "backup_path",
            ))
            or any(not isinstance(data.get(key), str) or
                   _SHA256_RE.fullmatch(str(data.get(key))) is None for key in (
                       "backup_sha256", "build_elf_sha256", "build_hex_sha256",
                       "expected_staged_sha256",
                   ))
            or not _valid_code_sector_hashes(data.get("code_sector_sha256"))
            or not _valid_storage_initialization(data)
            or (data.get("state") in {
                    "awaiting_qualification", "promotion_intent",
                    "bench_completion_intent", "supersede_intent", "committed",
                } and (not isinstance(data.get("staged_flash_sha256"), str) or
                       _SHA256_RE.fullmatch(str(data.get("staged_flash_sha256"))) is None))):
        raise TransactionError("active flash transaction journal is invalid")
    if (data.get("storage_initialized") is True and
            data.get("state") in {
                "awaiting_qualification", "promotion_intent",
                "bench_completion_intent", "supersede_intent", "committed",
            } and not _storage_erase_is_verified(data)):
        raise TransactionError(
            "active storage-initialization transaction lacks an erased-storage verification"
        )
    cohort_keys = {
        "cohort_manifest_path", "cohort_id", "source_id", "artifact_id",
        "evidence_mode", "promotion_allowed",
    }
    present_cohort_keys = cohort_keys.intersection(data)
    if present_cohort_keys and (
        present_cohort_keys != cohort_keys
        or not isinstance(data.get("cohort_manifest_path"), str)
        or not data.get("cohort_manifest_path")
        or any(
            not isinstance(data.get(key), str)
            or _SHA256_RE.fullmatch(str(data.get(key))) is None
            for key in ("cohort_id", "source_id", "artifact_id")
        )
        or data.get("evidence_mode") not in {
            "production_candidate", "bench_only",
        }
        or not isinstance(data.get("promotion_allowed"), bool)
        or (
            data.get("evidence_mode") == "bench_only"
            and data.get("promotion_allowed") is not False
        )
    ):
        raise TransactionError("active flash transaction cohort binding is invalid")
    backup = Path(str(data["backup_path"])).resolve()
    allowed = (TRANSACTION_DIRECTORY / _probe_key(probe_id)).resolve()
    if backup.parent.parent != allowed:
        raise TransactionError("active flash transaction backup is outside the transaction directory")
    if data.get("state") == "bench_completion_intent":
        record = data.get("bench_qualification_record")
        record_path = data.get("bench_qualification_path")
        if (
            not isinstance(record, dict)
            or data.get("bench_qualification_record_sha256")
            != _record_sha256(record)
            or not _valid_bench_qualification_record(data, record)
            or not isinstance(record_path, str)
            or not record_path
        ):
            raise TransactionError(
                "active bench-completion transaction record is invalid"
            )
    if data.get("state") == "supersede_intent":
        readback_path = data.get("supersede_readback_path")
        readback_sha256 = data.get("supersede_readback_sha256")
        if (
            data.get("supersede_reason") != "target_replaced_out_of_band"
            or not isinstance(data.get("superseded_at_utc"), str)
            or not data.get("superseded_at_utc")
            or not isinstance(data.get("supersede_resume_completed_at_utc"), str)
            or not data.get("supersede_resume_completed_at_utc")
            or not isinstance(readback_path, str)
            or Path(readback_path).resolve()
            != (backup.parent / "supersede-readback.bin").resolve()
            or not isinstance(readback_sha256, str)
            or _SHA256_RE.fullmatch(readback_sha256) is None
        ):
            raise TransactionError("active supersede transaction record is invalid")
    return data


def _write_journal(data: dict[str, object]) -> None:
    _atomic_json(_journal_path(str(data["probe_id"])), data)


def _erase_initialized_storage(
    data: dict[str, object],
    probe_id: str,
    readback: Path | None = None,
) -> str | None:
    """Durably record the exact erase before asking pyOCD to perform it."""
    storage = _storage_initialization(data)
    if storage is None:
        return None
    if storage.get("erase_started_at_utc") is None:
        storage["erase_started_at_utc"] = _utc_text()
        storage["phase"] = "erase_started"
        _write_journal(data)
        _checkpoint("storage_erase_started_durable")
    _erase_storage_partition(probe_id)
    staged_sha256 = None
    if readback is not None:
        staged_sha256 = _read_target_flash(
            probe_id,
            readback,
            connect_mode="under-reset",
            keep_halted=True,
        )
    storage["erase_completed_at_utc"] = _utc_text()
    storage["phase"] = "erased_not_verified"
    _write_journal(data)
    _checkpoint("storage_erase_completed_durable")
    return staged_sha256


def _record_verified_storage_erase(
    data: dict[str, object],
    readback: Path,
    staged_sha256: str,
) -> None:
    storage = _storage_initialization(data)
    if storage is None:
        return
    if staged_sha256 != data["expected_staged_sha256"]:
        raise TransactionError(
            "storage-initialization readback differs from the expected staged image"
        )
    if not _storage_is_erased(readback, data):
        raise TransactionError(
            "storage-initialization readback does not contain an erased storage partition"
        )
    if storage.get("erase_completed_at_utc") is None:
        # A power loss can happen after pyOCD succeeds but before its completion
        # journal update. The physical readback is the only safe recovery proof.
        storage["erase_completed_at_utc"] = _utc_text()
    if storage.get("erase_verified_at_utc") is None:
        storage["erase_verified_at_utc"] = _utc_text()
    storage["phase"] = "erased_not_reset"
    _write_journal(data)
    _checkpoint("storage_erase_verified_durable")


def _mark_awaiting_qualification(
    data: dict[str, object],
    staged_sha256: str,
    probe_id: str,
) -> None:
    data["state"] = "awaiting_qualification"
    data["staged_flash_sha256"] = staged_sha256
    data["readback_completed_at_utc"] = _utc_text()
    _write_journal(data)
    _checkpoint("awaiting_qualification_durable")
    _commander(probe_id, "reset")
    data["reset_completed_at_utc"] = _utc_text()
    storage = _storage_initialization(data)
    if storage is not None:
        storage["phase"] = "complete"
    _write_journal(data)


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


def _recover_interrupted_transaction(probe_id: str) -> str:
    data = _load_journal(probe_id)
    if data is None:
        return "none"
    if data["state"] == "awaiting_qualification":
        storage_initialization = _storage_initialization(data)
        if (storage_initialization is not None and
                storage_initialization.get("phase") != "complete"):
            # The storage readback was already durably recorded before this
            # state was published. A second reset is safe if power failed
            # after the first reset but before its completion was journaled.
            _probe_is_visible(probe_id)
            _commander(probe_id, "reset")
            data["reset_completed_at_utc"] = _utc_text()
            storage_initialization["phase"] = "complete"
            _write_journal(data)
        return "awaiting_qualification"
    if data["state"] == "bench_completion_intent":
        if _bench_qualification_record_is_durable(data):
            _cleanup_transaction(data)
            return "bench_completed"
        if _bench_qualification_ledger_is_durable(data):
            raise TransactionError(
                "consumed bench capture lacks its immutable qualification record"
            )
        if _bench_qualification_file_is_durable(data):
            record = data["bench_qualification_record"]
            assert isinstance(record, dict)
            capture_id = str(record["capture_id"])
            if capture_id in _consumed_capture_ids():
                raise TransactionError(
                    "bench capture was consumed by a different ledger record"
                )
            _record_consumed_capture(record)
            _checkpoint("bench_qualification_recovery_ledger_durable")
            if not _bench_qualification_record_is_durable(data):
                raise TransactionError(
                    "bench qualification recovery did not reach a durable record"
                )
            _cleanup_transaction(data)
            return "bench_completed"
        _restore_awaiting_qualification(data)
        return "awaiting_qualification"
    if data["state"] == "supersede_intent":
        _finish_supersede(data)
        return "superseded"
    _probe_is_visible(probe_id)
    if data["state"] == "promotion_intent":
        record = data.get("deployment_record")
        record_sha256 = data.get("deployment_record_sha256")
        if (isinstance(record, dict) and
                record_sha256 == _record_sha256(record) and
                any(item == record for item in _ledger_records())):
            data["state"] = "committed"
            _write_journal(data)
            _cleanup_transaction(data)
            return "committed"
        for key in (
            "capture_id", "manifest_path", "deployment_record",
            "deployment_record_sha256",
        ):
            data.pop(key, None)
        data["state"] = "awaiting_qualification"
        _write_journal(data)
        _commander(probe_id, "reset")
        return "awaiting_qualification"
    if data["state"] in {"staging", "staged"}:
        backup = Path(str(data["backup_path"]))
        readback = backup.parent / "recovery-readback.bin"
        storage_initialization = _storage_initialization(data)
        try:
            staged_sha256 = _read_target_flash(probe_id, readback)
            if _code_sectors_match(
                readback,
                data["code_sector_sha256"],  # type: ignore[arg-type]
            ):
                if storage_initialization is not None:
                    # A completed journal timestamp alone is not evidence that
                    # the exact range was erased. If pyOCD did complete before
                    # an interruption, the first readback proves it; otherwise
                    # repeat the idempotent sector erase under the same intent.
                    if (storage_initialization.get("erase_completed_at_utc") is None
                            or not _storage_is_erased(readback, data)):
                        erased_sha256 = _erase_initialized_storage(
                            data, probe_id, readback
                        )
                        if erased_sha256 is None:
                            raise TransactionError(
                                "storage recovery erase produced no target readback"
                            )
                        staged_sha256 = erased_sha256
                    _record_verified_storage_erase(data, readback, staged_sha256)
                _mark_awaiting_qualification(data, staged_sha256, probe_id)
                return "awaiting_qualification"
            if storage_initialization is not None:
                raise TransactionError(
                    "storage-initialization recovery cannot verify staged code; "
                    "the backup and journal are retained without reset"
                )
        finally:
            readback.unlink(missing_ok=True)
        _commander(probe_id, "reset")
        _cleanup_transaction(data)
        return "none"
    if _storage_initialization(data) is not None:
        if data["state"] == "committed":
            _cleanup_transaction(data)
            return "committed"
        if data["state"] == "rejected":
            _cleanup_transaction(data)
            return "none"
        raise TransactionError(
            "storage-initialization transaction has not reached a verified staged image; "
            "the backup and journal are retained without reset"
        )
    # An interrupted or rejected candidate is deliberately left on the target
    # so the same image can be debugged and iterated.  The journal protects
    # local evidence/ledger bookkeeping only; it is never an instruction to
    # restore the previous firmware.
    _commander(probe_id, "reset")
    _cleanup_transaction(data)
    return "none"


def _verify_promotion_candidate(
    build_dir: Path,
    manifest: Path,
    probe_id: str,
) -> tuple[verifier.BuildEvidence | None, str | None, list[str]]:
    build, capture_id, issues = verify_flash(build_dir, manifest, probe_id)
    if not PYOCD_EXECUTABLE.is_file():
        issues.append(f"repository pyOCD is missing: {PYOCD_EXECUTABLE}")
    if not issues:
        try:
            _probe_is_visible(probe_id)
        except TransactionError as exc:
            issues.append(str(exc))
    return build, capture_id, issues


def _verify_stage_candidate(
    build_dir: Path,
    probe_id: str,
    bench_only: bool = False,
) -> tuple[verifier.BuildEvidence | None, list[str]]:
    try:
        policies, frame_limit = verifier.load_policy()
        build = verifier.verify_build(
            build_dir,
            policies,
            frame_limit,
            allow_watchdog_bypass=True,
        )
        _consumed_capture_ids()
    except (OSError, verifier.EvidenceError, TransactionError) as exc:
        return None, [str(exc)]
    issues = list(build.issues)
    policy = policies.get(build.preset)
    deployable = (
        build.preset in verifier.DEPLOYABLE_PRESETS and
        policy is not None and policy.deployable
    )
    bench_preset = build.preset in verifier.WATCHDOG_BYPASS_BENCH_PRESETS
    if bench_only and not bench_preset:
        issues.append(
            f"{build.preset or '<missing>'} is not an allowed bench-only preset"
        )
    elif not bench_only and not deployable:
        suffix = "; pass --bench-only for verified non-promotable staging" if bench_preset else ""
        issues.append(
            f"{build.preset or '<missing>'} is not an allowed deployment preset{suffix}"
        )
    if not WEST_EXECUTABLE.is_file():
        issues.append(f"repository west is missing: {WEST_EXECUTABLE}")
    if not PYOCD_EXECUTABLE.is_file():
        issues.append(f"repository pyOCD is missing: {PYOCD_EXECUTABLE}")
    if not issues:
        try:
            _probe_is_visible(probe_id)
        except TransactionError as exc:
            issues.append(str(exc))
    return build, issues


def _storage_initialization_issues(
    build: verifier.BuildEvidence,
) -> list[str]:
    """Ensure the destructive migration targets the compiled durable layout."""
    issues: list[str] = []
    if build.preset not in verifier.DURABLE_STATE_PRESETS:
        return [
            f"{build.preset or '<missing>'} is not a durable preset and cannot initialize storage"
        ]
    for key in verifier.DURABLE_STATE_REQUIRED_CONFIG:
        if build.config.get(key) is not True:
            issues.append(
                f"storage initialization requires generated {key}=True"
            )
    if (build.config.get("CONFIG_FLASH_LOAD_SIZE") !=
            verifier.DURABLE_STATE_FLASH_LIMIT):
        issues.append(
            "storage initialization requires generated "
            "CONFIG_FLASH_LOAD_SIZE="
            f"0x{verifier.DURABLE_STATE_FLASH_LIMIT:x}"
        )

    dts = build.build_dir / "zephyr" / "zephyr.dts"
    try:
        text = dts.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        issues.append(f"generated storage partition is unreadable: {exc}")
        return issues
    node = re.search(
        r"\bstorage_partition\s*:\s*[^\s{]+\s*\{(?P<body>.*?)^\s*\};",
        text,
        re.MULTILINE | re.DOTALL,
    )
    reg = re.search(r"\breg\s*=\s*<\s*([^>]+)\s*>\s*;", node.group("body")) if node else None
    try:
        cells = [int(value, 0) for value in reg.group(1).split()] if reg else []
    except ValueError:
        cells = []
    if cells != [STORAGE_PARTITION_ADDRESS, STORAGE_PARTITION_SIZE]:
        issues.append(
            "storage initialization requires generated storage_partition "
            f"[0x{STORAGE_PARTITION_ADDRESS:x},0x{STORAGE_PARTITION_END:x})"
        )
    return issues


def _resolve_cohort(
    build_dir: Path,
    manifest: Path | None,
) -> dict[str, object]:
    selected = manifest
    if selected is None:
        selected = cohort.create_manifest(
            REPO_ROOT, [build_dir], COHORT_DIRECTORY,
        )
    return cohort.validate_build(selected, REPO_ROOT, build_dir)


def _journal_cohort_issues(
    data: dict[str, object],
    binding: dict[str, object] | None,
) -> list[str]:
    recorded = data.get("cohort_id")
    if recorded is None:
        # Legacy journals remain recoverable and rejectable, but they cannot
        # be promoted because they never bound the staged bytes to a source
        # snapshot. Restaging is the only way to obtain that proof.
        return [
            "staged candidate predates cohort provenance; reject and restage it"
        ]
    if binding is None:
        return ["staged candidate requires its content-addressed cohort manifest"]
    issues: list[str] = []
    for key in ("cohort_id", "source_id", "artifact_id"):
        if data.get(key) != binding.get(key):
            issues.append(f"staged candidate {key} differs from the cohort manifest")
    if data.get("cohort_manifest_path") != binding.get("manifest_path"):
        issues.append("staged candidate cohort manifest path differs from the request")
    return issues


def _capture_cohort_issues(
    data: dict[str, object],
    manifest: Path,
) -> list[str]:
    if data.get("cohort_id") is None:
        return []
    try:
        capture = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return ["trusted capture cohort binding is unreadable"]
    binding = capture.get("cohort") if isinstance(capture, dict) else None
    if not isinstance(binding, dict):
        return ["trusted capture lacks a cohort binding"]
    issues: list[str] = []
    for key in (
        "manifest_path", "cohort_id", "source_id", "artifact_id",
    ):
        journal_key = "cohort_manifest_path" if key == "manifest_path" else key
        if binding.get(key) != data.get(journal_key):
            issues.append(f"trusted capture {key} differs from the staged candidate")
    artifact = capture.get("artifact") if isinstance(capture, dict) else None
    target = capture.get("target") if isinstance(capture, dict) else None
    if not isinstance(artifact, dict) or artifact.get("artifact_id") != data.get("artifact_id"):
        issues.append("trusted capture artifact identity differs from the staged candidate")
    expected_sector_map = _sector_hash_map_sha256(
        data["code_sector_sha256"]  # type: ignore[arg-type]
    )
    if not isinstance(target, dict) or target.get("code_sector_map_sha256") != expected_sector_map:
        issues.append("trusted capture target-sector identity differs from the staged candidate")
    try:
        expected_binding_id = cohort.capture_binding_id(capture)
    except cohort.CohortError as exc:
        issues.append(str(exc))
    else:
        if capture.get("cohort_capture_id") != expected_binding_id:
            issues.append("trusted capture cohort/readback binding identity is invalid")
    return issues


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


def _verify_bench_capture(
    build_dir: Path,
    manifest: Path,
    probe_id: str,
) -> tuple[
    verifier.BuildEvidence | None,
    str | None,
    dict[str, object] | None,
    list[str],
]:
    """Revalidate a forced-hop capture without treating it as deployable."""
    try:
        policies, frame_limit = verifier.load_policy()
        build = verifier.verify_build(
            build_dir,
            policies,
            frame_limit,
            allow_watchdog_bypass=True,
        )
        consumed = _consumed_capture_ids()
    except (OSError, verifier.EvidenceError, TransactionError) as exc:
        return None, None, None, [str(exc)]

    issues = list(build.issues)
    policy = policies.get(build.preset)
    if (
        build.preset != "mesh_anchor_forcedhop"
        or build.preset not in verifier.WATCHDOG_BYPASS_BENCH_PRESETS
        or policy is None
    ):
        issues.append(
            f"{build.preset or '<missing>'} is not the forced-hop bench preset"
        )
        return build, None, None, issues

    try:
        capture = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        issues.append(f"trusted bench capture is unreadable: {exc}")
        return build, None, None, issues
    if not isinstance(capture, dict):
        issues.append("trusted bench capture is invalid")
        return build, None, None, issues

    result = verifier._load_hardware_manifest(manifest, build, policy)
    issues.extend(result.issues)
    if result.probe_id and result.probe_id != probe_id:
        issues.append(
            f"trusted capture probe {result.probe_id} does not match selected probe {probe_id}"
        )
    if capture.get("evidence_mode") != "bench_only":
        issues.append("trusted capture is not bench-only evidence")
    if capture.get("promotion_allowed") is not False:
        issues.append("trusted bench capture must be non-promotable")
    if result.capture_id and result.capture_id in consumed:
        issues.append("trusted bench capture was already consumed")
    return build, result.capture_id, capture, issues


def _validated_topology_manifest(path: Path) -> dict[str, object]:
    """Recompute a persisted topology from its three exact capture paths."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise TransactionError(f"topology manifest is unreadable: {path}") from exc
    if not isinstance(data, dict) or data.get("schema") != 1:
        raise TransactionError("topology manifest schema is invalid")
    roles = data.get("roles")
    if not isinstance(roles, dict) or set(roles) != cohort.BENCH_TOPOLOGY_PRESETS:
        raise TransactionError(
            "topology manifest must bind gateway, anchor, and forced-hop"
        )
    bindings: list[tuple[str, str, Path]] = []
    try:
        for preset in sorted(cohort.BENCH_TOPOLOGY_PRESETS):
            role = roles[preset]
            if not isinstance(role, dict):
                raise TypeError
            probe_id = role["probe_id"]
            capture_path = role["capture_manifest_path"]
            if not isinstance(probe_id, str) or not isinstance(capture_path, str):
                raise TypeError
            bindings.append((preset, probe_id, Path(capture_path)))
        recomputed = cohort.validate_topology(
            bindings, cohort.BENCH_TOPOLOGY_PRESETS,
        )
    except (AssertionError, KeyError, TypeError, cohort.CohortError) as exc:
        raise TransactionError("topology manifest binding is invalid") from exc
    if data != recomputed:
        raise TransactionError(
            "topology manifest differs from its exact three capture bindings"
        )
    topology_id = data.get("topology_id")
    if (
        path.name != f"topology-{topology_id}.json"
        or path.stat().st_mode & 0o222
    ):
        raise TransactionError(
            "topology manifest is not the immutable content-addressed artifact"
        )
    return data


def _bench_topology_issues(
    data: dict[str, object],
    capture_manifest: Path,
    capture_id: str,
    topology_manifest: Path,
) -> tuple[dict[str, object] | None, list[str]]:
    try:
        topology = _validated_topology_manifest(topology_manifest)
    except TransactionError as exc:
        return None, [str(exc)]
    roles = topology["roles"]
    assert isinstance(roles, dict)
    forcedhop = roles.get("mesh_anchor_forcedhop")
    if not isinstance(forcedhop, dict):
        return None, ["topology lacks the forced-hop role binding"]
    expected = {
        "probe_id": data.get("probe_id"),
        "capture_id": capture_id,
        "capture_manifest_path": str(capture_manifest.resolve()),
        "capture_manifest_sha256": _sha256(capture_manifest),
        "artifact_id": data.get("artifact_id"),
        "cohort_id": data.get("cohort_id"),
        "target_code_sector_map_sha256": _sector_hash_map_sha256(
            data["code_sector_sha256"]  # type: ignore[arg-type]
        ),
    }
    issues = [
        f"topology forced-hop {key} differs from the staged candidate"
        for key, value in expected.items()
        if forcedhop.get(key) != value
    ]
    if topology.get("source_id") != data.get("source_id"):
        issues.append("topology source identity differs from the staged candidate")
    return topology, issues


def _stage_candidate(build_dir: Path, probe_id: str) -> None:
    command = [
        str(WEST_EXECUTABLE), "flash", "--runner", "pyocd", "--build-dir", str(build_dir),
        "--", "--dev-id", probe_id, "--frequency", str(FLASH_FREQUENCY_HZ),
        "--flash-opt=--no-reset",
    ]
    result = _run(command)
    if result.returncode:
        raise TransactionError(f"candidate staging failed with exit status {result.returncode}")


def _start_transaction(
    build: verifier.BuildEvidence,
    build_dir: Path,
    probe_id: str,
    cohort_binding: dict[str, object],
    *,
    bench_only: bool,
    initialize_storage: bool,
) -> dict[str, object]:
    transaction_id = uuid.uuid4().hex
    transaction_directory = TRANSACTION_DIRECTORY / _probe_key(probe_id) / transaction_id
    transaction_directory.mkdir(parents=True, exist_ok=False)
    _fsync_directory(transaction_directory.parent)
    backup = transaction_directory / "target-flash-backup.bin"
    try:
        created_at = _utc_text()
        backup_sha256 = _read_target_flash(probe_id, backup)
        backup_completed_at = _utc_text()
        _elf_path, hex_path = _artifact_paths(build_dir)
        expected_staged = _expected_staged_image(
            backup,
            hex_path,
            erase_storage_partition=initialize_storage,
        )
        expected_staged_sha256 = hashlib.sha256(expected_staged).hexdigest()
        code_sector_sha256 = _code_sector_hashes(expected_staged, hex_path)
        artifact = cohort_binding.get("artifact")
        if (
            not isinstance(artifact, dict)
            or artifact.get("programmed_sector_sha256") != code_sector_sha256
        ):
            raise TransactionError(
                "cohort programmed-sector identity differs from the staged HEX"
            )
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
            "cohort_manifest_path": str(cohort_binding["manifest_path"]),
            "cohort_id": str(cohort_binding["cohort_id"]),
            "source_id": str(cohort_binding["source_id"]),
            "artifact_id": str(cohort_binding["artifact_id"]),
            "evidence_mode": "bench_only" if bench_only else "production_candidate",
            "promotion_allowed": not bench_only,
            "storage_initialized": initialize_storage,
            "created_at_utc": created_at,
            "backup_completed_at_utc": backup_completed_at,
        }
        if initialize_storage:
            data["storage_initialization"] = {
                "intent": "erase_storage_partition",
                "range_start": STORAGE_PARTITION_ADDRESS,
                "range_end": STORAGE_PARTITION_END,
                "size": STORAGE_PARTITION_SIZE,
                "pre_storage_sha256": _storage_partition_sha256(backup.read_bytes()),
                "erased_storage_sha256": _erased_storage_partition_sha256(),
                "phase": "not_started",
                "erase_started_at_utc": None,
                "erase_completed_at_utc": None,
                "erase_verified_at_utc": None,
            }
        _write_journal(data)
        _checkpoint("journal_durable")
        return data
    except Exception:
        if initialize_storage:
            # No target write is allowed before this journal exists. Preserve
            # any complete backup even if the local journal write itself was
            # interrupted, rather than resetting away forensic state.
            raise
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


def _stage_for_qualification(
    build: verifier.BuildEvidence,
    build_dir: Path,
    probe_id: str,
    cohort_binding: dict[str, object],
    *,
    bench_only: bool,
    initialize_storage: bool,
) -> None:
    data = _start_transaction(
        build,
        build_dir,
        probe_id,
        cohort_binding,
        bench_only=bench_only,
        initialize_storage=initialize_storage,
    )
    backup = Path(str(data["backup_path"]))
    transaction_directory = backup.parent
    try:
        data["state"] = "staging"
        data["program_started_at_utc"] = _utc_text()
        _assert_artifacts_unchanged(data, build_dir)
        _write_journal(data)
        _checkpoint("staging_durable")
        _stage_candidate(build_dir, probe_id)
        _assert_artifacts_unchanged(data, build_dir)
        data["state"] = "staged"
        data["program_completed_at_utc"] = _utc_text()
        storage_initialization = _storage_initialization(data)
        if storage_initialization is not None:
            storage_initialization["phase"] = "programmed_not_erased"
        _write_journal(data)
        _checkpoint("candidate_staged")

        staged = transaction_directory / "staged-readback.bin"
        if storage_initialization is not None:
            staged_sha256 = _erase_initialized_storage(data, probe_id, staged)
            if staged_sha256 is None:
                raise TransactionError("storage erase produced no target readback")
        else:
            staged_sha256 = _read_target_flash(probe_id, staged)
        if not _code_sectors_match(staged, data["code_sector_sha256"]):
            raise TransactionError(
                "staged code sectors do not match the immutable cohort"
            )
        if storage_initialization is not None:
            if staged_sha256 != data["expected_staged_sha256"]:
                raise TransactionError(
                    "storage-initialization readback differs from the expected staged image"
                )
            _record_verified_storage_erase(data, staged, staged_sha256)
        staged.unlink(missing_ok=True)
        _mark_awaiting_qualification(data, staged_sha256, probe_id)
    except BaseException as exc:
        if not isinstance(exc, Exception):
            raise
        if data.get("state") == "awaiting_qualification":
            raise
        if _storage_initialization(data) is not None:
            # This mode intentionally destroys prior durable state. Keep its
            # forensic backup and phase journal intact, and never reset a
            # target whose exact erase/readback state is still uncertain.
            raise
        try:
            _commander(probe_id, "reset")
            _cleanup_transaction(data)
        except Exception as cleanup_error:
            raise TransactionError(
                f"staging failed and local transaction cleanup remains pending: {cleanup_error}"
            ) from cleanup_error
        raise


def _reject_staged_candidate(data: dict[str, object], _build_dir: Path,
                             probe_id: str) -> None:
    if data.get("state") != "awaiting_qualification":
        raise TransactionError("selected probe has no staged candidate awaiting qualification")
    # Rejection identifies the running candidate from the journaled target
    # sector hashes below.  The local build directory may already contain the
    # corrected successor; requiring its ELF/HEX to remain byte-identical
    # would make an explicitly failed staged image impossible to retire.
    _probe_is_visible(probe_id)
    backup = Path(str(data["backup_path"]))
    readback = backup.parent / "rejection-readback.bin"
    try:
        _read_target_flash(probe_id, readback, reset_after=True)
        if not _code_sectors_match(
            readback,
            data["code_sector_sha256"],  # type: ignore[arg-type]
        ):
            raise TransactionError(
                "current target code sectors differ from the staged candidate"
            )
    finally:
        readback.unlink(missing_ok=True)

    data["state"] = "rejected"
    data["rejection_reason"] = "qualification_failed"
    _write_journal(data)
    _checkpoint("rejection_durable")
    _cleanup_transaction(data)


def _supersede_archive(data: dict[str, object]) -> dict[str, object]:
    archived = dict(data)
    archived["state"] = "superseded"
    return archived


def _finish_supersede(data: dict[str, object]) -> None:
    """Finish a journal close from already-durable live replacement proof."""
    if data.get("state") != "supersede_intent":
        raise TransactionError("stale-journal supersede intent is missing")
    backup = Path(str(data["backup_path"])).resolve()
    readback = Path(str(data["supersede_readback_path"])).resolve()
    if readback != (backup.parent / "supersede-readback.bin").resolve():
        raise TransactionError("supersede readback path is outside its transaction")
    if (
        not readback.is_file()
        or readback.stat().st_size != TARGET_FLASH_SIZE
        or _sha256(readback) != data.get("supersede_readback_sha256")
    ):
        raise TransactionError("supersede readback proof is missing or changed")
    if _code_sectors_match(
        readback,
        data["code_sector_sha256"],  # type: ignore[arg-type]
    ):
        raise TransactionError(
            "supersede readback still matches the staged candidate; use rejection"
        )
    archive = backup.parent / "superseded-journal.json"
    _write_json_once(archive, _supersede_archive(data))
    _checkpoint("supersede_archive_durable")
    _durable_unlink(_journal_path(str(data["probe_id"])))


def _supersede_staged_candidate(
    data: dict[str, object],
    probe_id: str,
) -> None:
    if data.get("state") != "awaiting_qualification":
        raise TransactionError(
            "selected probe has no staged candidate awaiting qualification"
        )
    _probe_is_visible(probe_id)
    backup = Path(str(data["backup_path"])).resolve()
    readback = backup.parent / "supersede-readback.bin"
    try:
        observed_sha256 = _read_target_flash(
            probe_id, readback, resume_after=True,
        )
        if _code_sectors_match(
            readback,
            data["code_sector_sha256"],  # type: ignore[arg-type]
        ):
            raise TransactionError(
                "current target still matches the staged candidate; reject it instead"
            )
    except BaseException:
        if data.get("state") == "awaiting_qualification":
            readback.unlink(missing_ok=True)
        raise

    data["state"] = "supersede_intent"
    data["supersede_reason"] = "target_replaced_out_of_band"
    data["superseded_at_utc"] = _utc_text()
    data["supersede_resume_completed_at_utc"] = _utc_text()
    data["supersede_readback_path"] = str(readback.resolve())
    data["supersede_readback_sha256"] = observed_sha256
    _write_journal(data)
    _checkpoint("supersede_intent_durable")
    _finish_supersede(data)


def _abandon_staged_candidate(data: dict[str, object]) -> None:
    if data.get("state") != "awaiting_qualification":
        raise TransactionError(
            "selected probe has no staged candidate awaiting qualification"
        )
    archived = dict(data)
    archived["state"] = "abandoned"
    archived["abandon_reason"] = "operator_confirmed_target_unavailable"
    backup = Path(str(data["backup_path"]))
    archive = backup.parent / "abandoned-journal.json"
    _atomic_json(archive, archived)
    _checkpoint("abandonment_durable")
    _durable_unlink(_journal_path(str(data["probe_id"])))


def _journal_matches_candidate(
    data: dict[str, object],
    build: verifier.BuildEvidence,
    build_dir: Path,
) -> list[str]:
    issues: list[str] = []
    if data.get("state") != "awaiting_qualification":
        issues.append("target has no candidate awaiting qualification")
    if data.get("preset") != build.preset:
        issues.append("staged candidate preset differs from the exact build")
    if (data.get("build_elf_sha256") != build.elf_sha256 or
            data.get("build_hex_sha256") != build.hex_sha256):
        issues.append("staged candidate artifact differs from the exact build")
    if data.get("build_dir") != str(build_dir.resolve()):
        issues.append("staged candidate build directory differs from the promotion request")
    if not isinstance(data.get("staged_flash_sha256"), str):
        issues.append("staged candidate lacks a verified readback identity")
    return issues


def _promotion_record_is_durable(data: dict[str, object]) -> bool:
    record = data.get("deployment_record")
    return (isinstance(record, dict) and
            data.get("deployment_record_sha256") == _record_sha256(record) and
            any(item == record for item in _ledger_records()))


def _restore_awaiting_qualification(data: dict[str, object]) -> None:
    for key in (
        "capture_id", "manifest_path", "deployment_record",
        "deployment_record_sha256", "topology_manifest_path",
        "bench_qualification_record", "bench_qualification_record_sha256",
        "bench_qualification_path",
    ):
        data.pop(key, None)
    data["state"] = "awaiting_qualification"
    _write_journal(data)


def _deployment_storage_record(data: dict[str, object]) -> dict[str, object]:
    storage = _storage_initialization(data)
    if storage is None:
        return {"initialized": False}
    return {
        "initialized": True,
        "range_start": storage["range_start"],
        "range_end": storage["range_end"],
        "pre_storage_sha256": storage["pre_storage_sha256"],
        "post_storage_sha256": storage["erased_storage_sha256"],
    }


def _valid_bench_qualification_record(
    data: dict[str, object],
    record: dict[str, object],
) -> bool:
    payload = dict(record)
    identity = payload.pop("bench_qualification_id", None)
    if (
        not isinstance(identity, str)
        or _SHA256_RE.fullmatch(identity) is None
        or identity != cohort._content_id(payload)
        or record.get("schema") != 1
        or record.get("record_type") != "bench_qualification"
        or record.get("preset") != "mesh_anchor_forcedhop"
        or record.get("preset") != data.get("preset")
        or record.get("probe_id") != data.get("probe_id")
        or record.get("transaction_id") != data.get("transaction_id")
        or record.get("capture_id") != data.get("capture_id")
        or record.get("capture_manifest_path") != data.get("manifest_path")
        or record.get("topology_manifest_path")
        != data.get("topology_manifest_path")
        or record.get("cohort_manifest_path")
        != data.get("cohort_manifest_path")
        or record.get("staged_flash_sha256")
        != data.get("staged_flash_sha256")
        or record.get("code_sector_map_sha256")
        != _sector_hash_map_sha256(
            data["code_sector_sha256"]  # type: ignore[arg-type]
        )
        or record.get("storage_initialized")
        is not (data.get("storage_initialized") is True)
        or record.get("storage_initialization")
        != _deployment_storage_record(data)
        or record.get("evidence_mode") != "bench_only"
        or record.get("promotion_allowed") is not False
    ):
        return False
    for key in (
        "capture_id", "cohort_capture_id", "capture_manifest_sha256",
        "topology_id", "topology_manifest_sha256", "inventory_id",
        "cohort_id", "source_id", "artifact_id",
        "staged_flash_sha256", "code_sector_map_sha256",
    ):
        value = record.get(key)
        if not isinstance(value, str) or _SHA256_RE.fullmatch(value) is None:
            return False
    return (
        all(record.get(key) == data.get(key)
            for key in ("cohort_id", "source_id", "artifact_id"))
        and isinstance(record.get("qualified_at_utc"), str)
        and bool(record.get("qualified_at_utc"))
    )


def _bench_qualification_ledger_is_durable(
    data: dict[str, object],
) -> bool:
    record = data.get("bench_qualification_record")
    if (
        not isinstance(record, dict)
        or data.get("bench_qualification_record_sha256")
        != _record_sha256(record)
        or not _valid_bench_qualification_record(data, record)
    ):
        return False
    return any(item == record for item in _ledger_records())


def _bench_qualification_file_is_durable(
    data: dict[str, object],
) -> bool:
    record = data.get("bench_qualification_record")
    record_path = data.get("bench_qualification_path")
    if (
        not isinstance(record, dict)
        or data.get("bench_qualification_record_sha256")
        != _record_sha256(record)
        or not isinstance(record_path, str)
    ):
        return False
    expected_path = (
        BENCH_QUALIFICATION_DIRECTORY
        / f"bench-qualification-{record.get('bench_qualification_id')}.json"
    ).resolve()
    if (
        Path(record_path).resolve() != expected_path
        or not expected_path.is_file()
        or expected_path.stat().st_mode & 0o222
    ):
        return False
    try:
        persisted = json.loads(expected_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    return persisted == record


def _bench_qualification_record_is_durable(
    data: dict[str, object],
) -> bool:
    return (
        _bench_qualification_file_is_durable(data)
        and _bench_qualification_ledger_is_durable(data)
    )


def _complete_bench_qualification(
    data: dict[str, object],
    build: verifier.BuildEvidence,
    build_dir: Path,
    capture_manifest: Path,
    capture_id: str,
    capture: dict[str, object],
    topology_manifest: Path,
    topology: dict[str, object],
) -> Path:
    """Close one exact non-promotable bench journal without target access."""
    _assert_artifacts_unchanged(data, build_dir)
    _sync_topology_capture_artifacts(topology)
    _durable_sync(topology_manifest)
    record: dict[str, object] = {
        "schema": 1,
        "record_type": "bench_qualification",
        "preset": build.preset,
        "probe_id": str(data["probe_id"]),
        "transaction_id": str(data["transaction_id"]),
        "capture_id": capture_id,
        "cohort_capture_id": capture.get("cohort_capture_id"),
        "capture_manifest_path": str(capture_manifest.resolve()),
        "capture_manifest_sha256": _sha256(capture_manifest),
        "topology_id": topology.get("topology_id"),
        "topology_manifest_path": str(topology_manifest.resolve()),
        "topology_manifest_sha256": _sha256(topology_manifest),
        "inventory_id": topology.get("inventory_id"),
        "cohort_id": data.get("cohort_id"),
        "source_id": data.get("source_id"),
        "artifact_id": data.get("artifact_id"),
        "cohort_manifest_path": data.get("cohort_manifest_path"),
        "staged_flash_sha256": str(data["staged_flash_sha256"]),
        "code_sector_map_sha256": _sector_hash_map_sha256(
            data["code_sector_sha256"]  # type: ignore[arg-type]
        ),
        "storage_initialized": data.get("storage_initialized") is True,
        "storage_initialization": _deployment_storage_record(data),
        "evidence_mode": "bench_only",
        "promotion_allowed": False,
        "qualified_at_utc": _utc_text(),
    }
    record["bench_qualification_id"] = cohort._content_id(record)
    record_path = (
        BENCH_QUALIFICATION_DIRECTORY
        / f"bench-qualification-{record['bench_qualification_id']}.json"
    ).resolve()
    data["state"] = "bench_completion_intent"
    data["capture_id"] = capture_id
    data["manifest_path"] = str(capture_manifest.resolve())
    data["topology_manifest_path"] = str(topology_manifest.resolve())
    data["bench_qualification_record"] = record
    data["bench_qualification_record_sha256"] = _record_sha256(record)
    data["bench_qualification_path"] = str(record_path)
    _write_journal(data)
    _checkpoint("bench_completion_intent_durable")
    try:
        persisted = cohort._persist_content_addressed(
            record,
            BENCH_QUALIFICATION_DIRECTORY,
            "bench-qualification",
            "bench_qualification_id",
        )
        if persisted.resolve() != record_path:
            raise TransactionError("bench qualification record path is invalid")
        _checkpoint("bench_qualification_record_durable")
        _record_consumed_capture(record)
        _checkpoint("bench_qualification_ledger_durable")
        _cleanup_transaction(data)
    except BaseException as exc:
        if not isinstance(exc, Exception):
            raise
        if _bench_qualification_record_is_durable(data):
            _cleanup_transaction(data)
        elif _bench_qualification_ledger_is_durable(data):
            # Capture consumption is irreversible. Retain the intent so
            # recovery cannot silently relabel a consumed capture as pending.
            pass
        elif _bench_qualification_file_is_durable(data):
            # The exact immutable record can be appended idempotently by the
            # intent recovery path; keep that intent instead of orphaning it.
            pass
        else:
            _restore_awaiting_qualification(data)
        raise
    return record_path


def _promote_staged_candidate(
    data: dict[str, object],
    build: verifier.BuildEvidence,
    build_dir: Path,
    manifest: Path,
    capture_id: str,
    probe_id: str,
) -> None:
    backup = Path(str(data["backup_path"]))
    transaction_directory = backup.parent
    readback = transaction_directory / "promotion-readback.bin"

    _assert_artifacts_unchanged(data, build_dir)
    _sync_capture_artifacts(manifest)
    try:
        _read_target_flash(probe_id, readback, reset_after=True)
        if not _code_sectors_match(
            readback,
            data["code_sector_sha256"],  # type: ignore[arg-type]
        ):
            raise TransactionError(
                "current target code sectors differ from the staged candidate"
            )
    finally:
        readback.unlink(missing_ok=True)

    deployment_record: dict[str, object] = {
        "capture_id": capture_id,
        "preset": build.preset,
        "probe_id": probe_id,
        "staged_flash_sha256": str(data["staged_flash_sha256"]),
        "code_sector_map_sha256": _sector_hash_map_sha256(
            data["code_sector_sha256"]  # type: ignore[arg-type]
        ),
        "transaction_id": str(data["transaction_id"]),
        "cohort_id": data.get("cohort_id"),
        "source_id": data.get("source_id"),
        "artifact_id": data.get("artifact_id"),
        "cohort_manifest_path": data.get("cohort_manifest_path"),
        "storage_initialized": data.get("storage_initialized") is True,
        "storage_initialization": _deployment_storage_record(data),
        "promoted_at_utc": _utc_text(),
    }
    try:
        data["state"] = "promotion_intent"
        data["capture_id"] = capture_id
        data["manifest_path"] = str(manifest.resolve())
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
        if not isinstance(exc, Exception):
            raise
        if _promotion_record_is_durable(data):
            data["state"] = "committed"
            _write_journal(data)
            _cleanup_transaction(data)
        else:
            _restore_awaiting_qualification(data)
        raise


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--hardware-manifest", type=Path)
    parser.add_argument("--cohort-manifest", type=Path)
    parser.add_argument("--topology-manifest", type=Path)
    parser.add_argument("--probe-id", required=True)
    parser.add_argument("--stage-only", action="store_true")
    parser.add_argument("--initialize-storage", action="store_true")
    parser.add_argument("--bench-only", action="store_true")
    parser.add_argument("--complete-bench-qualification", action="store_true")
    parser.add_argument("--reject-staged-candidate", action="store_true")
    parser.add_argument("--supersede-staged-candidate", action="store_true")
    parser.add_argument("--abandon-staged-candidate", action="store_true")
    args = parser.parse_args(argv)
    if sum((args.stage_only,
            args.reject_staged_candidate,
            args.supersede_staged_candidate,
            args.abandon_staged_candidate,
            args.complete_bench_qualification,
            args.hardware_manifest is not None
            and not args.complete_bench_qualification)) != 1:
        parser.error(
            "select exactly one of --stage-only, production --hardware-manifest, "
            "--complete-bench-qualification, --reject-staged-candidate, "
            "--supersede-staged-candidate, or --abandon-staged-candidate"
        )
    if args.complete_bench_qualification and (
        args.hardware_manifest is None or args.topology_manifest is None
    ):
        parser.error(
            "--complete-bench-qualification requires --hardware-manifest and "
            "--topology-manifest"
        )
    if args.topology_manifest is not None and not args.complete_bench_qualification:
        parser.error(
            "--topology-manifest is valid only with --complete-bench-qualification"
        )
    if args.bench_only and not args.stage_only:
        parser.error("--bench-only is valid only with --stage-only")
    if args.initialize_storage and not args.stage_only:
        parser.error("--initialize-storage is valid only with --stage-only")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        with _ledger_lock():
            data = _load_journal(args.probe_id)
            recovery = "none"
            storage_initialization = (
                _storage_initialization(data) if data is not None else None
            )
            if data is not None and (
                    data["state"] != "awaiting_qualification"
                    or (storage_initialization is not None
                        and storage_initialization.get("phase") != "complete")):
                recovery = _recover_interrupted_transaction(args.probe_id)
                data = _load_journal(args.probe_id)
            if (
                recovery == "bench_completed"
                and args.complete_bench_qualification
            ) or (
                recovery == "superseded"
                and args.supersede_staged_candidate
            ):
                return 0
            if args.stage_only:
                if data is not None and data["state"] == "awaiting_qualification":
                    if recovery == "awaiting_qualification":
                        return 0
                    raise TransactionError(
                        "selected probe already has a candidate awaiting qualification"
                    )
                build, issues = _verify_stage_candidate(
                    args.build_dir, args.probe_id, args.bench_only,
                )
                if args.initialize_storage and build is not None:
                    issues.extend(_storage_initialization_issues(build))
                if issues or build is None:
                    print("verified staging blocked:", file=sys.stderr)
                    for issue in issues:
                        print(f"  {issue}", file=sys.stderr)
                    return 1
                cohort_binding = _resolve_cohort(
                    args.build_dir, args.cohort_manifest,
                )
                _stage_for_qualification(
                    build,
                    args.build_dir,
                    args.probe_id,
                    cohort_binding,
                    bench_only=args.bench_only,
                    initialize_storage=args.initialize_storage,
                )
                return 0

            if args.reject_staged_candidate:
                if data is None:
                    raise TransactionError(
                        "selected probe has no staged candidate awaiting qualification"
                    )
                _reject_staged_candidate(data, args.build_dir, args.probe_id)
                return 0

            if args.supersede_staged_candidate:
                if data is None:
                    raise TransactionError(
                        "selected probe has no staged candidate awaiting qualification"
                    )
                _supersede_staged_candidate(data, args.probe_id)
                return 0

            if args.abandon_staged_candidate:
                if data is None:
                    raise TransactionError(
                        "selected probe has no staged candidate awaiting qualification"
                    )
                _abandon_staged_candidate(data)
                return 0

            if args.complete_bench_qualification:
                if data is None or data.get("state") != "awaiting_qualification":
                    raise TransactionError(
                        "selected probe has no staged candidate awaiting qualification"
                    )
                if (
                    data.get("preset") != "mesh_anchor_forcedhop"
                    or data.get("evidence_mode") != "bench_only"
                    or data.get("promotion_allowed") is not False
                ):
                    raise TransactionError(
                        "only an awaiting non-promotable forced-hop bench journal can be completed"
                    )
                if data.get("cohort_id") is None:
                    raise TransactionError(
                        "bench completion requires a cohort-bound staged candidate"
                    )
                assert args.hardware_manifest is not None
                assert args.topology_manifest is not None
                cohort_manifest = args.cohort_manifest
                if cohort_manifest is None and isinstance(
                    data.get("cohort_manifest_path"), str
                ):
                    cohort_manifest = Path(str(data["cohort_manifest_path"]))
                cohort_binding = _resolve_cohort(args.build_dir, cohort_manifest)
                build, capture_id, capture, issues = _verify_bench_capture(
                    args.build_dir, args.hardware_manifest, args.probe_id,
                )
                topology: dict[str, object] | None = None
                if build is not None:
                    issues.extend(_journal_matches_candidate(
                        data, build, args.build_dir,
                    ))
                    issues.extend(_journal_cohort_issues(data, cohort_binding))
                    issues.extend(_capture_cohort_issues(
                        data, args.hardware_manifest,
                    ))
                if capture_id is not None:
                    topology, topology_issues = _bench_topology_issues(
                        data,
                        args.hardware_manifest,
                        capture_id,
                        args.topology_manifest,
                    )
                    issues.extend(topology_issues)
                if (
                    issues
                    or build is None
                    or capture_id is None
                    or capture is None
                    or topology is None
                ):
                    print("verified bench completion blocked:", file=sys.stderr)
                    for issue in issues:
                        print(f"  {issue}", file=sys.stderr)
                    return 1
                record_path = _complete_bench_qualification(
                    data,
                    build,
                    args.build_dir,
                    args.hardware_manifest,
                    capture_id,
                    capture,
                    args.topology_manifest,
                    topology,
                )
                print(record_path)
                return 0

            if recovery == "committed":
                return 0
            if data is None or data["state"] != "awaiting_qualification":
                raise TransactionError(
                    "selected probe has no staged candidate awaiting qualification"
                )
            assert args.hardware_manifest is not None
            if data.get("promotion_allowed") is False:
                raise TransactionError(
                    "bench-only staged artifacts can never be promoted"
                )
            cohort_manifest: Path | None = args.cohort_manifest
            if cohort_manifest is None and isinstance(
                data.get("cohort_manifest_path"), str
            ):
                cohort_manifest = Path(str(data["cohort_manifest_path"]))
            cohort_binding = (
                _resolve_cohort(args.build_dir, cohort_manifest)
                if data.get("cohort_id") is not None else None
            )
            build, capture_id, issues = _verify_promotion_candidate(
                args.build_dir, args.hardware_manifest, args.probe_id
            )
            if build is not None:
                issues.extend(_journal_matches_candidate(
                    data, build, args.build_dir
                ))
                issues.extend(_journal_cohort_issues(data, cohort_binding))
                issues.extend(_capture_cohort_issues(
                    data, args.hardware_manifest,
                ))
            if issues or build is None or capture_id is None:
                print("verified deployment blocked:", file=sys.stderr)
                for issue in issues:
                    print(f"  {issue}", file=sys.stderr)
                return 1
            _promote_staged_candidate(
                data, build, args.build_dir, args.hardware_manifest,
                capture_id, args.probe_id,
            )
    except (OSError, cohort.CohortError, verifier.EvidenceError, TransactionError) as exc:
        print(f"verified deployment failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
