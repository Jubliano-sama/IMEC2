#!/usr/bin/env python3
"""Verify repository-owned stack evidence for deployable mesh firmware.

The runtime part deliberately accepts only a transcript created by
``capture_stack_evidence.py``. This is strong local provenance, not remote or
cryptographic probe attestation: somebody with write access to this checkout
can still replace the tool, artifact, transcript, and local replay ledger.
"""

from __future__ import annotations

import argparse
import hashlib
import heapq
import json
import re
import subprocess
import sys
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
POLICY_HEADER = REPO_ROOT / "firmware" / "include" / "stack_budget.h"
CAPTURE_TOOL_RELATIVE = "firmware/scripts/capture_stack_evidence.py"
CAPTURE_TOOL = REPO_ROOT / CAPTURE_TOOL_RELATIVE
POLICY_BEGIN = "/* STACK_BUDGET_POLICY_BEGIN */"
POLICY_END = "/* STACK_BUDGET_POLICY_END */"
ROOT_BEGIN = "/* STACK_BUDGET_THREAD_ROOTS_BEGIN */"
ROOT_END = "/* STACK_BUDGET_THREAD_ROOTS_END */"
WORKLOAD_BEGIN = "/* STACK_BUDGET_WORKLOAD_POLICY_BEGIN */"
WORKLOAD_END = "/* STACK_BUDGET_WORKLOAD_POLICY_END */"
DEPLOYABLE_PRESETS = frozenset({
    "mesh_clicker", "mesh_anchor", "mesh_gateway",
})
WATCHDOG_BYPASS_BENCH_PRESETS = frozenset({"mesh_anchor_forcedhop"})
DURABLE_STATE_PRESETS = DEPLOYABLE_PRESETS | WATCHDOG_BYPASS_BENCH_PRESETS
DURABLE_STATE_REQUIRED_CONFIG = (
    "CONFIG_FLASH",
    "CONFIG_FLASH_MAP",
    "CONFIG_FLASH_PAGE_LAYOUT",
    "CONFIG_NVS",
    "CONFIG_NVS_DATA_CRC",
    "CONFIG_IMEC_DURABLE_STATE",
)
DURABLE_STATE_SECTOR_BYTES = 4096
DURABLE_STATE_MIN_SECTORS = 2
DURABLE_STATE_FLASH_LIMIT = 0x7A000
DURABLE_STATE_MIN_FLASH_HEADROOM_BYTES = 0
GATEWAY_FIT_REQUIRED_CONFIG = {
    "CONFIG_ADC": False,
    "CONFIG_BT_CTLR_ECDH": False,
    "CONFIG_BT_CTLR_LE_ENC": False,
    "CONFIG_BT_ASSERT_VERBOSE": False,
    "CONFIG_BT_GATT_CACHING": False,
    "CONFIG_BT_GATT_READ_MULTIPLE": False,
    "CONFIG_BT_GATT_READ_MULT_VAR_LEN": False,
    "CONFIG_BT_GATT_SERVICE_CHANGED": True,
    "CONFIG_BT_CTLR_CRYPTO": True,
}
KNOWN_WORKLOADS = frozenset({
    "click_spam", "cir_handling", "relay_retry", "ble_backpressure",
    "click_activity", "anchor_scan", "gateway_report_ingress",
    "gateway_priority_control",
})
THREAD_ROOTS = frozenset({
    "main", "system_workqueue", "clicker_action", "anchor_uwb_scan", "mesh_route", "mesh_test", "isr", "bt_rx",
    "fatal_context",
})
KNOWN_DYNAMIC_THREAD_NAMES = frozenset({"clicker_action", "anchor_uwb_scan", "mesh_route", "mesh_test", "bt_rx", "shared_min", "system_workqueue", "unknown"})
CAPTURE_SCHEMA = 3
CAPTURE_WORKFLOW = "pyocd-rtt-pre-reset-v1"
MAX_CAPTURE_AGE = timedelta(hours=24)
MAX_FUTURE_SKEW = timedelta(minutes=5)
MAX_CAPTURE_DURATION = timedelta(minutes=15)
# ``capture_stack_evidence.py`` bounds the RTT child itself to 15 minutes.
# Its UTC timestamps wrap process startup/teardown and are serialized only to
# whole seconds, so the observed wall clock can legitimately be a few seconds
# longer without extending the hardware workload.
MAX_CAPTURE_PROCESS_OVERHEAD = timedelta(seconds=5)


@dataclass(frozen=True)
class PresetPolicy:
    role_name: str
    preset: str
    main_bytes: int
    system_workqueue_bytes: int
    mesh_route_bytes: int
    isr_bytes: int
    idle_bytes: int
    log_processor_bytes: int
    bt_hci_tx_bytes: int
    bt_rx_bytes: int
    minimum_static_ram_headroom_bytes: int
    init_stacks: bool
    hw_stack_protection: bool
    mpu_stack_guard: bool
    thread_stack_info: bool
    stack_sentinel: bool
    deployable: bool


@dataclass(frozen=True)
class WorkloadRequirement:
    kind: str
    owner: str
    minimum_successes: int
    ordered_sequence: bool


@dataclass(frozen=True)
class StackUsage:
    source: Path
    line: int
    function: str
    bytes_used: int
    qualifier: str
    # GCC emits inline/header records into the .su file belonging to the
    # compiling translation unit.  Keep that provenance instead of treating
    # the header as an independent object; otherwise same-named platform
    # helpers merge and their ownership becomes unverifiable.
    translation_unit: str = ""
    object_path: str = ""


@dataclass(frozen=True)
class CompileEdge:
    output: Path
    source: Path


@dataclass(frozen=True)
class ArchiveEdge:
    output: Path
    members: tuple[Path, ...]


@dataclass(frozen=True)
class LinkedObject:
    output: Path
    source: Path
    archive: Path | None = None


@dataclass
class BuildEvidence:
    build_dir: Path
    preset: str = ""
    config: dict[str, Any] = field(default_factory=dict)
    ram_used: int = 0
    ram_size: int = 0
    ram_headroom: int = 0
    app_object_count: int = 0
    linked_usage_count: int = 0
    attributed_usage_count: int = 0
    synchronous_usage_bytes: dict[str, int] = field(default_factory=dict)
    elf_path: Path | None = None
    hex_path: Path | None = None
    elf_sha256: str = ""
    hex_sha256: str = ""
    build_identity: str = ""
    issues: list[str] = field(default_factory=list)


@dataclass
class HardwareEvidence:
    preset: str
    manifest: Path
    probe_id: str = ""
    capture_id: str = ""
    sample_count: int = 0
    rtt_sha256: str = ""
    issues: list[str] = field(default_factory=list)


class EvidenceError(ValueError):
    pass


def _uint(value: str) -> int:
    match = re.fullmatch(r"(0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]*", value.strip())
    if match is None:
        raise EvidenceError(f"invalid policy integer {value!r}")
    return int(match.group(1), 0)


def _bool(value: str) -> bool:
    if value.strip() == "true":
        return True
    if value.strip() == "false":
        return False
    raise EvidenceError(f"invalid policy boolean {value!r}")


def _split_macro_args(value: str) -> list[str]:
    return [part.replace("\\", "").strip() for part in value.split(",")]


def load_policy(header: Path = POLICY_HEADER) -> tuple[dict[str, PresetPolicy], int]:
    text = header.read_text(encoding="utf-8")
    try:
        policy_text = text.split(POLICY_BEGIN, 1)[1].split(POLICY_END, 1)[0]
    except IndexError as exc:
        raise EvidenceError("stack policy markers are missing") from exc
    policies: dict[str, PresetPolicy] = {}
    for match in re.finditer(r"\bX\(([^()]*)\)", policy_text, re.DOTALL):
        fields = _split_macro_args(match.group(1))
        if len(fields) != 16:
            raise EvidenceError("malformed stack policy row")
        role, preset = fields[:2]
        policy = PresetPolicy(
            role_name=role,
            preset=preset.strip('"'),
            main_bytes=_uint(fields[2]),
            system_workqueue_bytes=_uint(fields[3]),
            mesh_route_bytes=_uint(fields[4]),
            isr_bytes=_uint(fields[5]),
            idle_bytes=_uint(fields[6]),
            log_processor_bytes=_uint(fields[7]),
            bt_hci_tx_bytes=_uint(fields[8]),
            bt_rx_bytes=_uint(fields[9]),
            minimum_static_ram_headroom_bytes=_uint(fields[10]),
            init_stacks=_bool(fields[11]),
            hw_stack_protection=_bool(fields[12]),
            mpu_stack_guard=_bool(fields[13]),
            thread_stack_info=_bool(fields[14]),
            stack_sentinel=_bool(fields[15]),
            deployable=preset.strip('"') in DEPLOYABLE_PRESETS,
        )
        if policy.preset in policies:
            raise EvidenceError(f"duplicate stack policy for {policy.preset}")
        policies[policy.preset] = policy
    if set(policy for policy in policies if policy in DEPLOYABLE_PRESETS) != DEPLOYABLE_PRESETS:
        raise EvidenceError("stack policy must cover exactly all deployable mesh presets")
    limit = re.search(r"#define\s+STACK_BUDGET_LARGE_LOCAL_FRAME_MAX_BYTES\s+(\S+)", text)
    if limit is None:
        raise EvidenceError("stack frame limit is missing")
    return policies, _uint(limit.group(1))


def load_workload_policy(
    header: Path = POLICY_HEADER,
) -> dict[str, tuple[WorkloadRequirement, ...]]:
    text = header.read_text(encoding="utf-8")
    try:
        workload_text = text.split(WORKLOAD_BEGIN, 1)[1].split(WORKLOAD_END, 1)[0]
    except IndexError as exc:
        raise EvidenceError("hardware workload policy markers are missing") from exc
    requirements: dict[str, list[WorkloadRequirement]] = {
        preset: [] for preset in DEPLOYABLE_PRESETS
    }
    for match in re.finditer(r"\bX\(([^()]*)\)", workload_text, re.DOTALL):
        fields = _split_macro_args(match.group(1))
        if len(fields) != 5:
            raise EvidenceError("malformed hardware workload policy row")
        preset = fields[0].strip('"')
        requirement = WorkloadRequirement(
            kind=fields[1].strip('"'),
            owner=fields[2].strip('"'),
            minimum_successes=_uint(fields[3]),
            ordered_sequence=_bool(fields[4]),
        )
        if preset not in DEPLOYABLE_PRESETS:
            raise EvidenceError(f"workload policy names non-deployable preset {preset}")
        if requirement.kind not in KNOWN_WORKLOADS:
            raise EvidenceError(f"workload policy names unknown kind {requirement.kind}")
        if requirement.owner not in KNOWN_DYNAMIC_THREAD_NAMES:
            raise EvidenceError(f"workload policy names unknown owner {requirement.owner}")
        if requirement.minimum_successes == 0:
            raise EvidenceError("workload policy minimum must be positive")
        if any(item.kind == requirement.kind for item in requirements[preset]):
            raise EvidenceError(f"duplicate workload policy for {preset}:{requirement.kind}")
        requirements[preset].append(requirement)
    missing = sorted(preset for preset, rows in requirements.items() if not rows)
    if missing:
        raise EvidenceError("hardware workload policy misses " + ",".join(missing))
    return {preset: tuple(rows) for preset, rows in requirements.items()}


def load_thread_roots(header: Path = POLICY_HEADER) -> dict[tuple[str, str], set[str]]:
    text = header.read_text(encoding="utf-8")
    try:
        annotation_text = text.split(ROOT_BEGIN, 1)[1].split(ROOT_END, 1)[0]
    except IndexError as exc:
        raise EvidenceError("stack thread-root annotation markers are missing") from exc
    roots: dict[tuple[str, str], set[str]] = {}
    for source, function, owner in re.findall(
        r'X\("([^"/]+)",\s*"([^" ]+)",\s*"([^" ]+)"\)', annotation_text
    ):
        key = (source, function)
        if owner not in THREAD_ROOTS:
            raise EvidenceError(f"invalid stack thread root for {source}:{function}")
        roots.setdefault(key, set()).add(owner)
    if not roots:
        raise EvidenceError("no stack thread-root annotations")
    return roots


def parse_kconfig(path: Path) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if raw.startswith("# CONFIG_") and raw.endswith(" is not set"):
            result[raw[2:-11]] = False
        elif raw.startswith("CONFIG_") and "=" in raw:
            key, value = raw.split("=", 1)
            result[key] = True if value == "y" else _uint(value) if re.fullmatch(r"0x[0-9a-fA-F]+|[0-9]+", value) else value
    return result


def _expect_boolean(evidence: BuildEvidence, key: str, expected: bool) -> None:
    # Zephyr omits disabled bool symbols from .config.  That is false, not a
    # missing required value; numeric and string settings retain strict absence.
    actual = evidence.config.get(key, False)
    if actual is not expected:
        evidence.issues.append(f"generated {key}={actual!r}, expected {expected!r}")


def parse_cmake_cache(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if ":" in line and "=" in line and not line.startswith("//"):
            key, value = line.split("=", 1)
            values[key.split(":", 1)[0]] = value
    return values


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def extract_build_identity(elf: Path) -> str:
    match = re.search(rb"imec-stack-v1:[A-Za-z0-9_.-]+:[0-9a-f]{64}", elf.read_bytes())
    if match is None:
        raise EvidenceError("exact ELF lacks the target stack build identity")
    return match.group(0).decode("ascii")


def _expect(evidence: BuildEvidence, key: str, expected: Any) -> None:
    if evidence.config.get(key) != expected:
        evidence.issues.append(f"generated {key}={evidence.config.get(key)!r}, expected {expected!r}")


def _verify_config(
    evidence: BuildEvidence,
    policy: PresetPolicy,
    *,
    allow_watchdog_bypass: bool,
) -> None:
    for field, key in {
        "main_bytes": "CONFIG_MAIN_STACK_SIZE",
        "system_workqueue_bytes": "CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE",
        "isr_bytes": "CONFIG_ISR_STACK_SIZE",
        "idle_bytes": "CONFIG_IDLE_STACK_SIZE",
    }.items():
        _expect(evidence, key, getattr(policy, field))
    for field, key in {
        "init_stacks": "CONFIG_INIT_STACKS",
        "hw_stack_protection": "CONFIG_HW_STACK_PROTECTION",
        "mpu_stack_guard": "CONFIG_MPU_STACK_GUARD",
        "thread_stack_info": "CONFIG_THREAD_STACK_INFO",
        "stack_sentinel": "CONFIG_STACK_SENTINEL",
    }.items():
        _expect_boolean(evidence, key, getattr(policy, field))
    if policy.log_processor_bytes:
        _expect(evidence, "CONFIG_LOG_PROCESS_THREAD_STACK_SIZE", policy.log_processor_bytes)
    if policy.bt_hci_tx_bytes:
        _expect(evidence, "CONFIG_BT_HCI_TX_STACK_SIZE", policy.bt_hci_tx_bytes)
    if policy.bt_rx_bytes:
        _expect(evidence, "CONFIG_BT_RX_STACK_SIZE", policy.bt_rx_bytes)
    if evidence.preset == "mesh_gateway":
        for key, expected in GATEWAY_FIT_REQUIRED_CONFIG.items():
            _expect_boolean(evidence, key, expected)
    watchdog_bypass = (
        evidence.config.get("CONFIG_IMEC_WATCHDOG_BYPASS") is True
    )
    if watchdog_bypass:
        if policy.deployable and not allow_watchdog_bypass:
            evidence.issues.append(
                "generated CONFIG_IMEC_WATCHDOG_BYPASS=True is bench-only "
                "and cannot qualify for production promotion"
            )
        elif (
            not policy.deployable
            and evidence.preset not in WATCHDOG_BYPASS_BENCH_PRESETS
        ):
            evidence.issues.append(
                "generated CONFIG_IMEC_WATCHDOG_BYPASS=True is not allowed "
                f"for preset {evidence.preset}"
            )
    if policy.deployable:
        for key in ("CONFIG_IMEC_STACK_DIAGNOSTICS", "CONFIG_THREAD_MONITOR", "CONFIG_THREAD_NAME", "CONFIG_USE_SEGGER_RTT"):
            _expect(evidence, key, True)
    if evidence.preset in DURABLE_STATE_PRESETS:
        for key in DURABLE_STATE_REQUIRED_CONFIG:
            _expect(evidence, key, True)
        _expect(evidence, "CONFIG_FLASH_LOAD_SIZE", DURABLE_STATE_FLASH_LIMIT)


def _verify_storage_partition(evidence: BuildEvidence) -> None:
    if evidence.preset not in DURABLE_STATE_PRESETS:
        return

    path = evidence.build_dir / "zephyr" / "zephyr.dts"
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        evidence.issues.append(f"generated devicetree is unreadable: {exc}")
        return

    node = re.search(
        r"\bstorage_partition\s*:\s*[^\s{]+\s*\{(?P<body>.*?)^\s*\};",
        text,
        re.MULTILINE | re.DOTALL,
    )
    if node is None:
        evidence.issues.append("generated devicetree lacks storage_partition")
        return

    body = node.group("body")
    status = re.search(r'\bstatus\s*=\s*"([^"]+)"\s*;', body)
    if status is not None and status.group(1) != "okay":
        evidence.issues.append(
            f"generated storage_partition status is {status.group(1)!r}, expected 'okay'"
        )

    reg = re.search(r"\breg\s*=\s*<\s*([^>]+)\s*>\s*;", body)
    if reg is None:
        evidence.issues.append("generated storage_partition lacks a single reg range")
        return
    try:
        cells = [int(value, 0) for value in reg.group(1).split()]
    except ValueError:
        cells = []
    if len(cells) != 2:
        evidence.issues.append("generated storage_partition reg is malformed")
        return

    address, size = cells
    minimum_size = DURABLE_STATE_SECTOR_BYTES * DURABLE_STATE_MIN_SECTORS
    if address % DURABLE_STATE_SECTOR_BYTES:
        evidence.issues.append(
            f"generated storage_partition address 0x{address:x} is not "
            f"{DURABLE_STATE_SECTOR_BYTES}-byte aligned"
        )
    if size < minimum_size:
        evidence.issues.append(
            f"generated storage_partition size {size} is below {minimum_size}"
        )
    elif size % DURABLE_STATE_SECTOR_BYTES:
        evidence.issues.append(
            f"generated storage_partition size {size} is not a multiple of "
            f"{DURABLE_STATE_SECTOR_BYTES}"
        )

    map_path = evidence.build_dir / "zephyr" / "zephyr.map"
    try:
        map_text = map_path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        evidence.issues.append(f"linker map is unreadable for storage check: {exc}")
        return
    flash = re.search(
        r"^FLASH\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)",
        map_text,
        re.MULTILINE,
    )
    used = re.search(
        r"^\s*0x([0-9a-fA-F]+)\s+_flash_used\b",
        map_text,
        re.MULTILINE,
    )
    if flash is None or used is None:
        evidence.issues.append(
            "linker map lacks FLASH/_flash_used bounds for storage_partition"
        )
        return
    code_start = int(flash.group(1), 16)
    code_end = code_start + int(used.group(1), 16)
    partition_end = address + size
    if code_start < partition_end and address < code_end:
        evidence.issues.append(
            f"linked image [0x{code_start:x},0x{code_end:x}) overlaps "
            f"storage_partition [0x{address:x},0x{partition_end:x})"
        )
    else:
        headroom = address - code_end
        if headroom < DURABLE_STATE_MIN_FLASH_HEADROOM_BYTES:
            evidence.issues.append(
                f"linked FLASH headroom {headroom} bytes below "
                f"storage_partition is below required "
                f"{DURABLE_STATE_MIN_FLASH_HEADROOM_BYTES} bytes"
            )


def _ram_map(path: Path) -> tuple[int, int, int, set[str]]:
    text = path.read_text(encoding="utf-8", errors="replace")
    ram = re.search(r"^RAM\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)", text, re.MULTILINE)
    end = re.search(r"0x([0-9a-fA-F]+)\s+_image_ram_end", text)
    if ram is None or end is None:
        raise EvidenceError("linker map lacks RAM bounds")
    origin, size, image_end = int(ram.group(1), 16), int(ram.group(2), 16), int(end.group(1), 16)
    if image_end < origin or image_end > origin + size:
        raise EvidenceError("linker RAM end lies outside RAM")
    symbols = set(re.findall(r"^\s*0x[0-9a-fA-F]+\s+([A-Za-z_][A-Za-z0-9_.$]*)", text, re.MULTILINE))
    # Local/static functions do not always receive a standalone linker symbol.
    # Their non-zero app/libapp text section is the linker provenance that proves
    # they survived section GC and must therefore be attributed as well.
    app_text_patterns = (
        r"^\s*\.text\.([A-Za-z_][A-Za-z0-9_.$]*)\s+0x([0-9a-fA-F]+)\s+0x[0-9a-fA-F]+\s+app/libapp\.a\([^)]*\.obj\)",
        r"^\s*\.text\.([A-Za-z_][A-Za-z0-9_.$]*)\s*\n\s*0x([0-9a-fA-F]+)\s+0x[0-9a-fA-F]+\s+app/libapp\.a\([^)]*\.obj\)",
    )
    for pattern in app_text_patterns:
        symbols.update(name for name, address in re.findall(pattern, text, re.MULTILINE) if int(address, 16) != 0)
    return image_end - origin, size, origin + size - image_end, symbols


def _ninja_logical_lines(text: str) -> list[str]:
    lines: list[str] = []
    pending = ""
    for physical in text.splitlines():
        logical = pending + (physical.lstrip() if pending else physical)
        trailing_dollars = len(logical) - len(logical.rstrip("$"))
        if trailing_dollars % 2:
            pending = logical[:-1]
            continue
        lines.append(logical)
        pending = ""
    if pending:
        raise EvidenceError("unterminated Ninja line continuation")
    return lines


def _ninja_path_tokens(value: str) -> list[str]:
    tokens: list[str] = []
    token: list[str] = []
    index = 0
    while index < len(value):
        char = value[index]
        if char in " \t":
            if token:
                tokens.append("".join(token))
                token = []
            index += 1
            continue
        if char != "$":
            token.append(char)
            index += 1
            continue
        if value.startswith("${cmake_ninja_workdir}", index):
            # CMake emits a duplicate absolute-output spelling on link edges;
            # the ordinary relative output is retained alongside it. Keep
            # the variable token opaque so it cannot masquerade as a source
            # path, while still allowing the final-link edge to be parsed.
            token.append("${cmake_ninja_workdir}")
            index += len("${cmake_ninja_workdir}")
            continue
        if index + 1 >= len(value) or value[index + 1] not in " $:":
            raise EvidenceError(
                f"unsupported Ninja path escape near {value[index:index + 16]!r}"
            )
        token.append(value[index + 1])
        index += 2
    if token:
        tokens.append("".join(token))
    return tokens


def _ninja_build_parts(line: str) -> tuple[list[str], list[str]]:
    body = line[len("build "):]
    delimiter = None
    index = 0
    while index < len(body):
        if body[index] == "$":
            index += 2
            continue
        if body[index] == ":":
            delimiter = index
            break
        index += 1
    if delimiter is None:
        raise EvidenceError("malformed Ninja build edge lacks ':'")
    outputs = _ninja_path_tokens(body[:delimiter])
    rule_and_inputs = _ninja_path_tokens(body[delimiter + 1:])
    if not outputs or not rule_and_inputs:
        raise EvidenceError("malformed Ninja build edge")
    inputs: list[str] = []
    for item in rule_and_inputs[1:]:
        if item in {"|", "||", "|@"}:
            break
        inputs.append(item)
    return outputs, inputs


def _ninja_objects(path: Path) -> tuple[str, list[tuple[Path, Path]]]:
    text = path.read_text(encoding="utf-8", errors="replace")
    objects: list[tuple[Path, Path]] = []
    app_archive_members: set[Path] | None = None
    for line in _ninja_logical_lines(text):
        if not line.startswith("build "):
            continue
        if line.startswith("build app/libapp.a:"):
            outputs, inputs = _ninja_build_parts(line)
            if "app/libapp.a" in outputs:
                if app_archive_members is not None:
                    raise EvidenceError("build graph has multiple app/libapp.a edges")
                app_archive_members = {
                    Path(item) for item in inputs
                    if item.endswith(".obj") and
                    "CMakeFiles/app.dir/" in item
                }
        if "CMakeFiles/app.dir/" not in line or ".obj" not in line:
            continue
        outputs, inputs = _ninja_build_parts(line)
        app_outputs = [
            output for output in outputs
            if output.endswith(".obj") and "CMakeFiles/app.dir/" in output
        ]
        if not app_outputs:
            continue
        sources = [item for item in inputs if item.endswith(".c")]
        if len(sources) != 1:
            raise EvidenceError(
                "application Ninja compile edge must name exactly one C source: "
                + ",".join(app_outputs)
            )
        objects.extend((Path(output), Path(sources[0]))
                       for output in app_outputs)
    if not objects:
        raise EvidenceError("build graph has no application C objects")
    if app_archive_members is None:
        raise EvidenceError("build graph lacks app/libapp.a membership edge")
    compiled_objects = {output for output, _source in objects}
    if compiled_objects != app_archive_members:
        missing = sorted(str(path) for path in app_archive_members - compiled_objects)
        unexpected = sorted(str(path) for path in compiled_objects - app_archive_members)
        raise EvidenceError(
            "application Ninja compile edges differ from app/libapp.a members: "
            f"missing={missing}, unexpected={unexpected}"
        )
    return text, objects


def _ninja_compile_edges(text: str) -> dict[Path, Path]:
    """Return every C compile edge, including Zephyr and module targets.

    The app-only parser above is intentionally kept as a compatibility seam
    for the existing application archive checks.  Full stack closure uses
    this independent graph so a platform object cannot disappear merely
    because it is outside ``CMakeFiles/app.dir``.
    """
    edges: dict[Path, Path] = {}
    for line in _ninja_logical_lines(text):
        if not line.startswith("build "):
            continue
        raw_outputs, raw_inputs = _ninja_build_parts(line)
        outputs = [Path(output) for output in raw_outputs]
        inputs = [Path(input_path) for input_path in raw_inputs]
        object_outputs = [
            output for output in outputs
            if output.suffix in {".obj", ".o"}
        ]
        if not object_outputs:
            continue
        sources = [source for source in inputs if source.suffix == ".c"]
        if len(sources) != 1:
            # Assembly and generated/object-only edges are outside ordinary C
            # TU evidence. A C-looking edge without one source is ambiguous,
            # so reject it rather than silently dropping its provenance.
            if any(source.suffix in {".c", ".cc", ".cpp", ".cxx"}
                   for source in inputs):
                raise EvidenceError(
                    "C Ninja compile edge must name exactly one C source: "
                    + ",".join(str(output) for output in object_outputs)
                )
            continue
        for output in object_outputs:
            if output in edges and edges[output] != sources[0]:
                raise EvidenceError(
                    f"Ninja compile output has conflicting sources {output}"
                )
            edges[output] = sources[0]
    if not edges:
        raise EvidenceError("build graph has no C compile edges")
    return edges


def _ninja_archive_edges(text: str) -> dict[Path, ArchiveEdge]:
    archives: dict[Path, ArchiveEdge] = {}
    for line in _ninja_logical_lines(text):
        if not line.startswith("build "):
            continue
        raw_outputs, raw_inputs = _ninja_build_parts(line)
        outputs = [Path(output) for output in raw_outputs]
        inputs = [Path(input_path) for input_path in raw_inputs]
        archive_outputs = [output for output in outputs
                           if output.suffix == ".a"]
        if not archive_outputs:
            continue
        members = tuple(input_path for input_path in inputs
                        if input_path.suffix in {".obj", ".o"})
        for output in archive_outputs:
            if output in archives:
                raise EvidenceError(f"build graph has multiple archive edges {output}")
            archives[output] = ArchiveEdge(output, members)
    return archives


def _ninja_final_link_inputs(text: str) -> tuple[set[Path], set[Path]]:
    """Find the exact final ELF link edge and its archive/object inputs."""
    matches: list[tuple[set[Path], set[Path]]] = []
    for line in _ninja_logical_lines(text):
        if not line.startswith("build "):
            continue
        body = line[len("build "):]
        delimiter = None
        index = 0
        while index < len(body):
            if body[index] == "$":
                index += 2
                continue
            if body[index] == ":":
                delimiter = index
                break
            index += 1
        if delimiter is None:
            raise EvidenceError("malformed final Ninja link edge")
        outputs = [Path(output) for output in
                   _ninja_path_tokens(body[:delimiter])]
        # Archive inputs are commonly placed after Ninja's order-only `|`
        # separator even though they are real linker inputs. The app parser
        # intentionally stops at that separator; the final-link pass must
        # retain every archive/object token and discard only separators.
        all_items = _ninja_path_tokens(body[delimiter + 1:])
        inputs = [Path(item) for item in all_items
                  if item not in {"|", "||", "|@"}]
        if Path("zephyr/zephyr.elf") not in outputs:
            continue
        archives = {item for item in inputs if item.suffix == ".a"}
        objects = {item for item in inputs
                   if item.suffix in {".obj", ".o"}}
        matches.append((archives, objects))
    if len(matches) != 1:
        raise EvidenceError(
            "build graph must contain exactly one final zephyr/zephyr.elf link edge"
        )
    return matches[0]


def _map_archive_members(path: Path) -> list[tuple[Path, str]]:
    """Read GNU ld's linked archive-member table from the final map."""
    text = path.read_text(encoding="utf-8", errors="replace")
    members: list[tuple[Path, str]] = []
    pattern = re.compile(r"^(?P<archive>.+\.a)\((?P<member>[^()]+)\)")
    in_archive_table = False
    saw_member = False
    for raw in text.splitlines():
        if raw.startswith("Archive member included to satisfy reference by file"):
            in_archive_table = True
            continue
        if not in_archive_table:
            continue
        # The section dump later in the map repeats archive(member) in every
        # input-section row.  Only the initial archive-member table is linker
        # extraction provenance; stop before the first blank separator.
        if not raw.strip():
            if saw_member:
                break
            continue
        match = pattern.match(raw.strip())
        if match is not None:
            members.append((Path(match.group("archive")), match.group("member")))
            saw_member = True
    if not members:
        raise EvidenceError("linker map lacks linked archive-member provenance")
    return members


def _ninja_path_key(path: Path) -> str:
    return str(path.resolve()) if path.is_absolute() else path.as_posix()


def _linked_application_objects(
    build_dir: Path,
    ninja_text: str,
    app_objects: list[tuple[Path, Path]],
) -> list[LinkedObject]:
    """Prove the exact final ELF contains the complete application archive."""
    app_archive = Path("app/libapp.a")
    linked_archives, _linked_direct_objects = _ninja_final_link_inputs(
        ninja_text
    )
    if app_archive not in linked_archives:
        raise EvidenceError(
            "final zephyr/zephyr.elf link does not contain app/libapp.a"
        )

    objects_by_member: dict[str, tuple[Path, Path]] = {}
    for output, source in app_objects:
        if output.name in objects_by_member:
            raise EvidenceError(
                "app/libapp.a has ambiguous duplicate object member "
                f"{output.name}"
            )
        objects_by_member[output.name] = (output, source)

    linked_members = {
        member
        for archive, member in _map_archive_members(
            build_dir / "zephyr" / "zephyr.map"
        )
        if archive == app_archive
    }
    expected_members = set(objects_by_member)
    if linked_members != expected_members:
        missing = sorted(expected_members - linked_members)
        unexpected = sorted(linked_members - expected_members)
        raise EvidenceError(
            "final linker map app/libapp.a members differ from Ninja archive: "
            f"missing={missing}, unexpected={unexpected}"
        )
    return [
        LinkedObject(output, source, app_archive)
        for output, source in app_objects
    ]


def _full_linked_objects(
    build_dir: Path,
    ninja_text: str,
) -> list[LinkedObject]:
    """Reconcile final-map members with every source-built C object.

    The final map is the linker truth for archive extraction; Ninja is the
    source/provenance truth. Both are required. This deliberately leaves
    opaque prebuilt archive members for named ABI contracts instead of
    pretending that a generic compiler reserve proves their stack use.
    """
    compile_edges = _ninja_compile_edges(ninja_text)
    archive_edges = _ninja_archive_edges(ninja_text)
    linked_archives, linked_direct_objects = _ninja_final_link_inputs(ninja_text)
    map_members = _map_archive_members(build_dir / "zephyr" / "zephyr.map")

    compile_by_key = {_ninja_path_key(output): (output, source)
                      for output, source in compile_edges.items()}
    archive_by_key = {_ninja_path_key(output): edge
                      for output, edge in archive_edges.items()}
    linked_archive_keys = {_ninja_path_key(path) for path in linked_archives}
    result: list[LinkedObject] = []
    seen: set[Path] = set()

    def add_object(output: Path, source: Path, archive: Path | None) -> None:
        if source.suffix != ".c":
            return
        if output in seen:
            return
        seen.add(output)
        result.append(LinkedObject(output, source, archive))

    for archive, member in map_members:
        archive_key = _ninja_path_key(archive)
        if archive_key not in linked_archive_keys:
            if _is_implicit_toolchain_archive(archive):
                # GCC/picolibc inject these archives through the driver/specs,
                # so they have no ordinary Ninja archive edge. Their symbols
                # are checked by the exact compiler/libc ABI contracts.
                continue
            raise EvidenceError(
                f"linker map archive member is absent from final link: {archive}"
            )
        edge = archive_by_key.get(archive_key)
        if edge is None:
            # A member from an absolute/vendor archive has no source compile
            # edge by design. Its call boundary is checked against an explicit
            # ABI contract after the source-built graph is loaded.
            continue
        # This closure owns ordinary C TUs. Assembly members have no .su or
        # IPA cgraph record and are covered by the linker/ABI boundary rather
        # than silently being mistaken for a missing C compile edge.
        member_stem = member[:-4] if member.endswith((".obj", ".o")) else member
        if Path(member_stem).suffix.lower() in {".s", ".asm"}:
            continue
        candidates = [
            output for output in edge.members
            if output.name == member
        ]
        if len(candidates) != 1:
            raise EvidenceError(
                f"linker map member {archive}({member}) does not map to one "
                f"Ninja archive member: {len(candidates)} matches"
            )
        output = candidates[0]
        compiled = compile_by_key.get(_ninja_path_key(output))
        if compiled is None:
            raise EvidenceError(
                f"linked C archive member lacks a Ninja compile edge: "
                f"{archive}({member})"
            )
        _output, source = compiled
        add_object(output, source, archive)

    for output in linked_direct_objects:
        compiled = compile_by_key.get(_ninja_path_key(output))
        if compiled is None:
            raise EvidenceError(
                f"linked direct C object lacks a Ninja compile edge: {output}"
            )
        _output, source = compiled
        add_object(output, source, None)

    if not result:
        raise EvidenceError("final link has no source-built C translation units")
    return result


def _parse_su(
    path: Path,
    *,
    translation_unit: str = "",
    object_path: Path | None = None,
    allow_empty: bool = False,
    application_source_dir: Path | None = None,
    west_topdir: Path | None = None,
) -> list[StackUsage]:
    records: list[StackUsage] = []
    text = path.read_text(encoding="utf-8", errors="replace")
    if not text.strip():
        if allow_empty:
            return records
        raise EvidenceError(f"empty compiler stack evidence {path}")
    for line in text.splitlines():
        columns = line.split("\t")
        location = re.fullmatch(r"(.*):(\d+):(\d+):(.+)", columns[0]) if len(columns) >= 3 else None
        if location is None or not columns[1].isdigit():
            raise EvidenceError(f"malformed compiler stack row in {path}: {line!r}")
        records.append(StackUsage(
            _resolve_compiler_source(
                Path(location.group(1)),
                application_source_dir=application_source_dir,
                west_topdir=west_topdir,
            ),
            int(location.group(2)),
            location.group(4),
            int(columns[1]),
            ",".join(columns[2:]),
            translation_unit,
            str(object_path) if object_path is not None else "",
        ))
    return records


def _resolve_compiler_source(
    path: Path,
    *,
    application_source_dir: Path | None = None,
    west_topdir: Path | None = None,
) -> Path:
    """Resolve GCC paths after Zephyr's source-prefix remapping."""
    if not path.is_absolute() and path.parts[:1] == ("CMAKE_SOURCE_DIR",):
        if application_source_dir is not None:
            return application_source_dir.joinpath(*path.parts[1:]).resolve()
    if not path.is_absolute() and path.parts[:1] == ("WEST_TOPDIR",):
        root = REPO_ROOT if west_topdir is None else west_topdir
        return root.joinpath(*path.parts[1:]).resolve()
    return path.resolve()


def _translation_unit_key(source: Path, object_path: Path) -> str:
    """Return a stable, path-qualified identity for one compiled TU."""
    try:
        source_text = str(source.resolve().relative_to(REPO_ROOT))
    except ValueError:
        source_text = str(source.resolve())
    return f"{source_text} [{object_path.as_posix()}]"


def _is_inline_stack_source(path: Path, build_dir: Path) -> bool:
    """Accept project/platform inline records while excluding host headers."""
    if path.suffix not in {".h", ".inc"}:
        return False
    try:
        resolved = path.resolve()
    except OSError:
        return False
    roots = (REPO_ROOT.resolve(), build_dir.resolve())
    return any(root == resolved or root in resolved.parents for root in roots)


def _record_source_key(record: StackUsage) -> str:
    return record.translation_unit or record.source.name


_GCC_CLONE_SUFFIX_RE = re.compile(
    r"(?:\.(?:isra|constprop|part)(?:\.\d+)?)+$"
)


def _canonical_function(function: str) -> str:
    return _GCC_CLONE_SUFFIX_RE.sub("", function)


_CGRAPH_VARIABLE_PREFIX = "<variable>:"
_CGRAPH_REFERENCE_PREFIX = "<reference>:"
_CGRAPH_DATA_REFERENCE_PREFIX = "<data-reference>:"
_CGRAPH_UNKNOWN_REFERENCE_PREFIX = "<unknown-reference>:"
_CGRAPH_INDIRECT_CALL = "<indirect-call>"

# These are deliberately named contracts, not a blanket allowance for every
# missing call edge.  Compiler intrinsics and the small set of C-library
# entrypoints below have no ordinary source TU in the exact linked image; all
# other live unresolved calls remain verification failures.  Prebuilt vendor
# symbols have no default contract: a future exception must be an exact,
# map-proven per-symbol contract supplied by the caller.
_COMPILER_ABI_PREFIXES = ("__builtin_", "__atomic_")
_COMPILER_ABI_SYMBOLS = frozenset({
    "__aeabi_memclr4", "__aeabi_memclr8", "__aeabi_memcpy4",
    "__aeabi_memcpy8", "__aeabi_memset4", "__aeabi_memset8",
    "__aeabi_uidiv", "__aeabi_uidivmod", "__aeabi_idiv", "__aeabi_idivmod",
    "__aeabi_uldivmod", "__aeabi_ldivmod", "__gnu_thumb1_case_uqi",
})
_LIBC_ABI_SYMBOLS = frozenset({
    "memcpy", "memmove", "memset", "memcmp", "strlen", "strcpy",
    "strncpy", "strnlen", "strchr", "strcmp", "snprintf", "vsnprintf",
    "__errno", "__errno_location",
})
_ZEPHYR_INLINE_ABI_SYMBOLS = frozenset({
    "assert_post_action", "assert_print", "z_spin_lock_set_owner",
    "z_spin_lock_valid", "z_spin_unlock_valid",
})
_ZEPHYR_METADATA_ABI_SYMBOLS = frozenset({"z_tls_current"})
_ZEPHYR_METADATA_ABI_PREFIXES = ("__device_dts_ord_",)
_IMPLICIT_TOOLCHAIN_ARCHIVES = frozenset({"libc.a", "libgcc.a"})


def _abi_contract_reason(
    symbol: str,
    contracts: dict[str, str] | None = None,
) -> str | None:
    if contracts is not None and symbol in contracts:
        return contracts[symbol]
    if symbol in _COMPILER_ABI_SYMBOLS:
        return "compiler ABI"
    if any(symbol.startswith(prefix) for prefix in _COMPILER_ABI_PREFIXES):
        return "compiler intrinsic ABI"
    if symbol in _LIBC_ABI_SYMBOLS:
        return "picolibc ABI"
    if symbol in _ZEPHYR_INLINE_ABI_SYMBOLS:
        return "Zephyr inline ABI"
    if symbol in _ZEPHYR_METADATA_ABI_SYMBOLS:
        return "Zephyr metadata ABI"
    if any(symbol.startswith(prefix) for prefix in _ZEPHYR_METADATA_ABI_PREFIXES):
        return "Zephyr device metadata ABI"
    return None


def _is_implicit_toolchain_archive(path: Path) -> bool:
    """Recognize only the compiler's named libc/libgcc inputs.

    Zephyr's final Ninja edge does not list the archives injected by
    ``picolibc.specs`` and the compiler driver. Their map members are still
    covered by exact libc/compiler symbol contracts; an arbitrary project
    archive with the same basename must not receive that exemption.
    """
    if path.name not in _IMPLICIT_TOOLCHAIN_ARCHIVES:
        return False
    text = path.as_posix()
    return "zephyr-sdk" in text or "/gcc/" in text or "/picolibc/" in text


def _compiler_function_name_symbol(symbol: str) -> bool:
    return symbol == "__func__" or symbol.startswith("__func__.")


def _owner_capacity(policy: PresetPolicy, owner: str) -> int:
    if owner == "fatal_context":
        # A Zephyr fatal override runs on whichever context faulted, including
        # the small idle thread. Bound its application path by the smallest
        # configured execution stack; disabled services have zero capacity and
        # are excluded from the minimum.
        capacities = (
            policy.main_bytes,
            policy.system_workqueue_bytes,
            policy.mesh_route_bytes,
            8192 if policy.preset in {
                "mesh_transmitter", "mesh_transmitter_forcedhop",
            } else 0,
            policy.isr_bytes,
            policy.idle_bytes,
            policy.log_processor_bytes,
            policy.bt_hci_tx_bytes,
            policy.bt_rx_bytes,
        )
        return min(capacity for capacity in capacities if capacity > 0)
    capacities = {
        "main": policy.main_bytes,
        "system_workqueue": policy.system_workqueue_bytes,
        "clicker_action": 8192 if policy.preset == "mesh_clicker" else 0,
        "anchor_uwb_scan": (
            8192 if policy.preset in {
                "mesh_anchor", "mesh_anchor_forcedhop",
            } else 0
        ),
        "mesh_route": policy.mesh_route_bytes,
        "mesh_test": (
            8192 if policy.preset in {
                "mesh_transmitter", "mesh_transmitter_forcedhop",
            } else 0
        ),
        "isr": policy.isr_bytes,
        "bt_rx": policy.bt_rx_bytes,
    }
    return capacities.get(owner, 0)


def _cgraph_path(build_dir: Path, object_path: Path) -> Path:
    name = object_path.name
    if not name.endswith(".obj"):
        raise EvidenceError(f"unexpected application object {object_path}")
    return build_dir / object_path.parent / f"{name[:-4]}.c.000i.cgraph"


def _parse_cgraph(path: Path, source_name: str) -> dict[tuple[str, str], set[str]]:
    text = path.read_text(encoding="utf-8", errors="replace")
    optimized_markers = list(re.finditer(
        r"^Optimized Symbol table:\s*$", text, re.MULTILINE
    ))
    if not optimized_markers:
        raise EvidenceError(
            f"compiler call graph lacks an optimized symbol table {path}"
        )
    start = optimized_markers[-1].end()
    end_marker = re.search(
        r"^(?:Removing variables:|Final Symbol table:)\s*$",
        text[start:],
        re.MULTILINE,
    )
    end = start + end_marker.start() if end_marker is not None else len(text)
    optimized = text[start:end]

    nodes: dict[tuple[str, str], set[str]] = {}
    pattern = re.compile(
        r"^([^\s/]+)/(\d+) \(([^)]+)\).*?(?=^[^\s/]+/\d+ \(|\Z)",
        re.MULTILINE | re.DOTALL,
    )
    blocks = list(pattern.finditer(optimized))
    kinds: dict[tuple[str, str], set[str]] = {}
    for match in blocks:
        type_match = re.search(
            r"^  Type: (function|variable)(?:\s|$)",
            match.group(0),
            re.MULTILINE,
        )
        if type_match is not None:
            kinds.setdefault((match.group(1), match.group(2)), set()).add(
                type_match.group(1)
            )

    for match in blocks:
        block = match.group(0)
        function_definition = "Type: function definition analyzed" in block
        variable_definition = "Type: variable definition analyzed" in block
        if not function_definition and not variable_definition:
            continue
        symbol = _canonical_function(match.group(1))
        if variable_definition and _compiler_function_name_symbol(symbol):
            # Every C function may own a distinct compiler-generated
            # `__func__` string with the same source-level symbol. Joining
            # those constants would invent call edges between unrelated work
            # queues; they can never contain a callback.
            continue
        node_symbol = (_CGRAPH_VARIABLE_PREFIX + symbol
                       if variable_definition else symbol)
        # GCC may put IPA diagnostics immediately after an empty ``Calls:``
        # field.  ``\s`` also consumes newlines, which would turn a diagnostic
        # such as ``updating call of caller/7`` into an invented call edge.
        calls = re.search(r"^  Calls:[ \t]*(.*)$", block, re.MULTILINE)
        targets = set()
        if calls is not None:
            targets = {
                _canonical_function(target)
                for target in re.findall(r"([^\s/]+)/\d+", calls.group(1))
            }
        references = re.search(r"^  References:[ \t]*(.*)$", block,
                               re.MULTILINE)
        if references is not None:
            for target, target_id in re.findall(
                r"([^\s/]+)/(\d+)(?:\s+\([^)]+\))?",
                references.group(1),
            ):
                canonical = _canonical_function(target)
                if _compiler_function_name_symbol(canonical):
                    continue
                target_kinds = kinds.get((target, target_id), set())
                if target_kinds == {"function"}:
                    targets.add(_CGRAPH_REFERENCE_PREFIX + canonical)
                elif target_kinds == {"variable"}:
                    # A symbol address/read is data unless GCC proves the
                    # referenced node is a function. Keep the data edge so a
                    # local application ops table can lead to its callbacks;
                    # an external variable simply terminates at the platform
                    # boundary later.
                    targets.add(_CGRAPH_DATA_REFERENCE_PREFIX + canonical)
                else:
                    targets.add(
                        _CGRAPH_UNKNOWN_REFERENCE_PREFIX + canonical
                    )
        if re.search(r"^   Indirect call", block, re.MULTILINE):
            # GCC emits targetless function-pointer dispatch separately from
            # References. Do not confuse ordinary data addresses with calls,
            # and do not silently bless a real unresolved application call.
            targets.add(_CGRAPH_INDIRECT_CALL)
        nodes.setdefault((source_name, node_symbol), set()).update(targets)
    if not nodes:
        raise EvidenceError(f"compiler call graph has no analyzed definitions {path}")
    return nodes


def _cgraph_proves_empty_optimized_tu(path: Path) -> bool:
    """Recognize a complete GCC dump whose optimized TU emits no code/data."""
    text = path.read_text(encoding="utf-8", errors="replace")
    if "Initial Symbol table:" not in text or "Final Symbol table:" not in text:
        return False
    optimized_markers = list(re.finditer(
        r"^Optimized Symbol table:\s*$", text, re.MULTILINE
    ))
    if not optimized_markers:
        return False
    start = optimized_markers[-1].end()
    end_marker = re.search(
        r"^(?:Removing variables:|Final Symbol table:)\s*$",
        text[start:],
        re.MULTILINE,
    )
    if end_marker is None:
        return False
    optimized = text[start:start + end_marker.start()]
    administrative = {"Trivially needed variables:"}
    return all(
        not line.strip() or line.strip() in administrative
        for line in optimized.splitlines()
    )


def _map_proves_no_live_text(
    path: Path,
    linked_object: LinkedObject,
) -> bool:
    """Prove that one linked source object contributes no allocated text."""
    text = path.read_text(encoding="utf-8", errors="replace")
    marker = "Linker script and memory map"
    if text.count(marker) != 1:
        return False
    owner = (
        f"{linked_object.archive.as_posix()}({linked_object.output.name})"
        if linked_object.archive is not None
        else linked_object.output.as_posix()
    )
    if owner not in text:
        return False

    live_lines = text.split(marker, 1)[1].splitlines()
    for index, line in enumerate(live_lines):
        if owner not in line:
            continue
        location = re.search(
            r"0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)\s+" +
            re.escape(owner) + r"\s*$",
            line,
        )
        if location is None:
            continue
        section = re.match(r"^\s+(\.[^\s]+)", line)
        if section is None and index:
            section = re.match(r"^\s+(\.[^\s]+)\s*$", live_lines[index - 1])
        if section is None:
            continue
        name = section.group(1)
        if (name == ".text" or name.startswith(".text.")) and int(
            location.group(1), 16
        ):
            return False
    return True


def _parse_tu_compiler_evidence(
    usage_path: Path,
    graph_path: Path,
    translation_unit: str,
    object_path: Path,
    *,
    final_map_path: Path | None = None,
    linked_object: LinkedObject | None = None,
    application_source_dir: Path | None = None,
    west_topdir: Path | None = None,
) -> tuple[
    list[StackUsage],
    dict[tuple[str, str], set[str]],
    dict[tuple[str, str], set[str]],
]:
    """Parse one TU's frame and graph evidence with empty-data validation."""
    usage_empty = not usage_path.read_text(
        encoding="utf-8", errors="replace"
    ).strip()
    try:
        graph = _parse_cgraph(graph_path, translation_unit)
    except EvidenceError as exc:
        if (
            usage_empty
            and "compiler call graph has no analyzed definitions" in str(exc)
            and _cgraph_proves_empty_optimized_tu(graph_path)
            and final_map_path is not None
            and linked_object is not None
            and _map_proves_no_live_text(final_map_path, linked_object)
        ):
            graph = {}
        else:
            raise
    has_function_definition = any(
        not node[1].startswith(_CGRAPH_VARIABLE_PREFIX)
        for node in graph
    )
    records = _parse_su(
        usage_path,
        translation_unit=translation_unit,
        object_path=object_path,
        allow_empty=not has_function_definition,
        application_source_dir=application_source_dir,
        west_topdir=west_topdir,
    )
    synchronous = (
        _parse_synchronous_cgraph(graph_path, translation_unit)
        if graph else {}
    )
    return records, graph, synchronous


def _parse_synchronous_cgraph(
    path: Path, source_name: str
) -> dict[tuple[str, str], set[str]]:
    """Parse the final optimized, non-inlined Calls edges from a GCC dump."""
    text = path.read_text(encoding="utf-8", errors="replace")
    optimized_markers = list(re.finditer(
        r"^Optimized Symbol table:\s*$", text, re.MULTILINE
    ))
    if not optimized_markers:
        raise EvidenceError(
            f"compiler call graph lacks an optimized symbol table {path}"
        )
    start = optimized_markers[-1].end()
    end_marker = re.search(
        r"^(?:Removing variables:|Final Symbol table:)\s*$",
        text[start:],
        re.MULTILINE,
    )
    end = start + end_marker.start() if end_marker is not None else len(text)
    optimized = text[start:end]

    nodes: dict[tuple[str, str], set[str]] = {}
    pattern = re.compile(
        r"^([^\s/]+)/\d+ \(([^)]+)\).*?(?=^[^\s/]+/\d+ \(|\Z)",
        re.MULTILINE | re.DOTALL,
    )
    optimized_has_analyzed_definition = False
    for match in pattern.finditer(optimized):
        block = match.group(0)
        if (
            "Type: function definition analyzed" in block
            or "Type: variable definition analyzed" in block
        ):
            optimized_has_analyzed_definition = True
        if "Type: function definition analyzed" not in block:
            continue
        symbol = match.group(1)
        targets: set[str] = set()
        calls = re.search(r"^  Calls:[ \t]*(.*)$", block, re.MULTILINE)
        if calls is not None:
            call_line = calls.group(1)
            call_matches = list(re.finditer(r"([^\s/]+)/\d+", call_line))
            for index, target_match in enumerate(call_matches):
                metadata_end = (
                    call_matches[index + 1].start()
                    if index + 1 < len(call_matches)
                    else len(call_line)
                )
                metadata = call_line[target_match.end():metadata_end]
                if "(inlined)" in metadata:
                    continue
                targets.add(target_match.group(1))
        nodes.setdefault((source_name, symbol), set()).update(targets)
    if not nodes:
        if not optimized_has_analyzed_definition:
            raise EvidenceError(
                f"compiler optimized call graph has no analyzed definitions {path}"
            )
        # A generated data-only TU can have an optimized symbol table with
        # only variable definitions. Its .su file is legitimately empty; the
        # ordinary parser has already validated that the graph is non-empty.
        return nodes
    return nodes


def _synchronous_cgraph(
    cgraphs: dict[tuple[str, str], set[str]],
    linked_by_key: dict[tuple[str, str], list[StackUsage]],
) -> dict[tuple[str, str], set[tuple[str, str]]]:
    """Resolve only compiler-proved synchronous calls between app functions."""
    graph_nodes = set(cgraphs)
    by_function: dict[str, set[tuple[str, str]]] = {}
    for node in linked_by_key:
        by_function.setdefault(node[1], set()).add(node)

    resolved: dict[tuple[str, str], set[tuple[str, str]]] = {
        node: set()
        for node in graph_nodes
        if not node[1].startswith(_CGRAPH_VARIABLE_PREFIX)
    }
    for node in resolved:
        for target in cgraphs.get(node, set()):
            if target.startswith(_CGRAPH_REFERENCE_PREFIX):
                continue
            local = (node[0], target)
            if local in resolved:
                candidates = (local,)
            else:
                candidates = by_function.get(target, ())
            resolved[node].update(
                candidate for candidate in candidates if candidate in resolved
            )
    return resolved


def _strongly_connected_components(
    adjacency: dict[tuple[str, str], set[tuple[str, str]]],
    nodes: set[tuple[str, str]],
) -> list[set[tuple[str, str]]]:
    """Return SCCs with iterative Kosaraju passes bounded by nodes plus edges."""
    visited: set[tuple[str, str]] = set()
    finish_order: list[tuple[str, str]] = []
    for start in sorted(nodes):
        if start in visited:
            continue
        visited.add(start)
        pending: list[
            tuple[tuple[str, str], list[tuple[str, str]], int]
        ] = [(start, sorted(adjacency.get(start, set())), 0)]
        while pending:
            node, targets, index = pending[-1]
            if index >= len(targets):
                pending.pop()
                finish_order.append(node)
                continue
            target = targets[index]
            pending[-1] = (node, targets, index + 1)
            if target in nodes and target not in visited:
                visited.add(target)
                pending.append((
                    target,
                    sorted(adjacency.get(target, set())),
                    0,
                ))

    reverse: dict[tuple[str, str], set[tuple[str, str]]] = {
        node: set() for node in nodes
    }
    for node in nodes:
        for target in adjacency.get(node, set()):
            if target in nodes:
                reverse[target].add(node)

    components: list[set[tuple[str, str]]] = []
    visited.clear()
    for start in reversed(finish_order):
        if start in visited:
            continue
        component: set[tuple[str, str]] = set()
        pending = [start]
        visited.add(start)
        while pending:
            node = pending.pop()
            component.add(node)
            for target in reverse[node]:
                if target not in visited:
                    visited.add(target)
                    pending.append(target)
        components.append(component)
    return components


def _format_stack_path(
    start: tuple[str, str],
    successor: dict[tuple[str, str], tuple[str, str] | None],
) -> str:
    path: list[str] = []
    node: tuple[str, str] | None = start
    seen: set[tuple[str, str]] = set()
    while node is not None and node not in seen and len(path) < 12:
        seen.add(node)
        path.append(f"{node[0]}:{node[1]}")
        node = successor.get(node)
    if node is not None:
        path.append("...")
    return " -> ".join(path)


def _validate_synchronous_stack_chains(
    evidence: BuildEvidence,
    policy: PresetPolicy,
    linked: list[StackUsage],
    cgraphs: dict[tuple[str, str], set[str]],
    owners: dict[tuple[str, str], set[str]],
) -> None:
    linked_by_key: dict[tuple[str, str], list[StackUsage]] = {}
    for record in linked:
        linked_by_key.setdefault(
            (_record_source_key(record), record.function),
            [],
        ).append(record)
    adjacency = _synchronous_cgraph(cgraphs, linked_by_key)
    synchronous_owners = {
        node: owners.get((node[0], _canonical_function(node[1])), set())
        for node in adjacency
    }
    weights = {
        node: max((record.bytes_used for record in linked_by_key.get(node, ())),
                  default=0)
        for node in adjacency
    }
    owner_names = sorted({
        owner for node_owners in synchronous_owners.values()
        for owner in node_owners
    })
    for owner in owner_names:
        owner_nodes = {
            node for node in adjacency
            if owner in synchronous_owners.get(node, set())
        }
        owner_adjacency = {
            node: {
                target for target in adjacency[node]
                if target in owner_nodes
            }
            for node in owner_nodes
        }
        if not owner_nodes:
            continue

        cyclic_components = []
        for component in _strongly_connected_components(
            owner_adjacency, owner_nodes
        ):
            if len(component) > 1 or any(
                node in owner_adjacency.get(node, set())
                for node in component
            ):
                cyclic_components.append(component)
        if cyclic_components:
            for component in cyclic_components:
                functions = sorted(
                    f"{node[0]}:{node[1]}" for node in component
                )
                if len(functions) > 8:
                    functions = functions[:8] + ["..."]
                evidence.issues.append(
                    "recursive synchronous compiler call graph "
                    f"owner={owner}: {', '.join(functions)}"
                )
            continue

        indegree = {node: 0 for node in owner_nodes}
        for targets in owner_adjacency.values():
            for target in targets:
                indegree[target] += 1
        ready = [node for node, count in indegree.items() if count == 0]
        heapq.heapify(ready)
        topological: list[tuple[str, str]] = []
        while ready:
            node = heapq.heappop(ready)
            topological.append(node)
            for target in sorted(owner_adjacency[node]):
                indegree[target] -= 1
                if indegree[target] == 0:
                    heapq.heappush(ready, target)
        if len(topological) != len(owner_nodes):
            evidence.issues.append(
                "synchronous compiler call graph could not be bounded "
                f"owner={owner}"
            )
            continue

        depths: dict[tuple[str, str], int] = {}
        successor: dict[tuple[str, str], tuple[str, str] | None] = {}
        for node in reversed(topological):
            next_node = max(
                owner_adjacency[node],
                key=lambda target: (depths[target], target),
                default=None,
            )
            successor[node] = next_node
            depths[node] = weights[node] + (
                depths[next_node] if next_node else 0
            )

        depth, start = max(
            (depths[node], node) for node in owner_nodes
        )
        evidence.synchronous_usage_bytes[owner] = depth
        capacity = _owner_capacity(policy, owner)
        required_free = _required_free(
            capacity,
            service=owner in {"isr", "bt_rx", "fatal_context"},
        )
        if capacity == 0 or depth + required_free > capacity:
            evidence.issues.append(
                f"compiler synchronous stack chain {depth} plus required "
                f"free {required_free} exceeds configured {capacity} "
                f"owner={owner}: {_format_stack_path(start, successor)}"
            )


def _root_owners_for_node(
    node: tuple[str, str],
    roots: dict[tuple[str, str], set[str]],
) -> set[str]:
    """Match policy roots against a path-qualified compiler TU identity."""
    owners: set[str] = set()
    node_source = node[0].split(" [", 1)[0]
    for (root_source, root_function), root_owners in roots.items():
        if node[1] != _canonical_function(root_function):
            continue
        if (node[0] == root_source or
                Path(node_source).name == Path(root_source).name):
            owners.update(root_owners)
    return owners


def _attribute_linked_functions(
    evidence: BuildEvidence,
    policy: PresetPolicy,
    linked: list[StackUsage],
    cgraphs: dict[tuple[str, str], set[str]],
    roots: dict[tuple[str, str], set[str]],
    frame_limit: int,
    synchronous_cgraphs: dict[tuple[str, str], set[str]] | None = None,
    abi_contracts: dict[str, str] | None = None,
    platform_symbols: set[str] | None = None,
) -> None:
    linked_by_key: dict[tuple[str, str], list[StackUsage]] = {}
    for record in linked:
        linked_by_key.setdefault(
            (_record_source_key(record), _canonical_function(record.function)),
            [],
        ).append(record)
    graph_nodes = set(cgraphs)
    linked_by_function: dict[str, set[tuple[str, str]]] = {}
    for node in linked_by_key:
        linked_by_function.setdefault(node[1], set()).add(node)
    graph_by_function: dict[str, set[tuple[str, str]]] = {}
    graph_by_variable: dict[str, set[tuple[str, str]]] = {}
    for node in graph_nodes:
        symbol = (
            node[1][len(_CGRAPH_VARIABLE_PREFIX):]
            if node[1].startswith(_CGRAPH_VARIABLE_PREFIX)
            else node[1]
        )
        if node[1].startswith(_CGRAPH_VARIABLE_PREFIX):
            graph_by_variable.setdefault(symbol, set()).add(node)
        else:
            graph_by_function.setdefault(node[1], set()).add(node)
    for node in linked_by_key:
        if node not in cgraphs:
            evidence.issues.append(f"missing compiler call-graph node for {node[0]}:{node[1]}")

    owners: dict[tuple[str, str], set[str]] = {node: set() for node in graph_nodes}
    for root, root_owners in roots.items():
        root_nodes = [
            node for node in graph_nodes
            if node in linked_by_key and
            root_owners <= _root_owners_for_node(node, {root: root_owners})
        ]
        if not root_nodes:
            continue
        for owner in root_owners:
            # Root annotations are shared across exact role builds. A service
            # with zero configured stack capacity does not exist in that
            # preset and cannot authorize otherwise unowned linked code.
            if _owner_capacity(policy, owner) == 0:
                continue
            pending = root_nodes[:]
            seen: set[tuple[str, str]] = set()
            while pending:
                node = pending.pop()
                if node in seen:
                    continue
                seen.add(node)
                owners[node].add(owner)
                for target in cgraphs.get(node, set()):
                    if target == _CGRAPH_INDIRECT_CALL:
                        # GCC cannot identify a target for a vtable/function-
                        # pointer dispatch. It terminates the application
                        # static graph here; exact typed workload captures own
                        # the dynamic platform/interface stack beyond it.
                        continue
                    reference_edge = target.startswith(
                        _CGRAPH_REFERENCE_PREFIX
                    )
                    data_edge = target.startswith(
                        _CGRAPH_DATA_REFERENCE_PREFIX
                    )
                    unknown_edge = target.startswith(
                        _CGRAPH_UNKNOWN_REFERENCE_PREFIX
                    )
                    if reference_edge:
                        target = target[len(_CGRAPH_REFERENCE_PREFIX):]
                    elif data_edge:
                        target = target[len(_CGRAPH_DATA_REFERENCE_PREFIX):]
                    elif unknown_edge:
                        target = target[
                            len(_CGRAPH_UNKNOWN_REFERENCE_PREFIX):
                        ]
                        evidence.issues.append(
                            "unresolved live compiler symbol reference "
                            f"owner={owner}: {node[0]}:{node[1]} -> {target}"
                        )
                        continue
                    # Prefer exact translation-unit definitions, including
                    # immutable callback/ops variables and non-linked compiler
                    # intermediates. Only an unresolved external name fans out
                    # to linked cross-object candidates. This stays
                    # conservative without joining unrelated static helpers or
                    # compiler constants which happen to share a name.
                    local: list[tuple[str, str]] = []
                    local_function = (node[0], target)
                    local_variable = (node[0], _CGRAPH_VARIABLE_PREFIX + target)
                    if not data_edge and local_function in graph_nodes:
                        local.append(local_function)
                    if data_edge and local_variable in graph_nodes:
                        local.append(local_variable)
                    if local:
                        candidates = local
                    else:
                        graph_candidates = (
                            graph_by_variable.get(target, set())
                            if data_edge else
                            graph_by_function.get(target, set())
                        )
                        # A cross-translation-unit callee may have been inlined
                        # and therefore have no surviving linker symbol or .su
                        # row of its own. Follow its compiler graph when the
                        # definition is unique; that preserves ownership across
                        # the inlined intermediary. Ambiguous same-name static
                        # definitions still fail closed unless the linker leaves
                        # an exact surviving candidate.
                        if reference_edge or data_edge:
                            # A data/callback reference may legitimately have
                            # several compiler snapshots or a same-name
                            # external definition; retaining all proved
                            # candidates preserves custody without treating a
                            # data symbol as an unresolved call.
                            candidates = list(graph_candidates)
                        elif len(graph_candidates) == 1:
                            candidates = list(graph_candidates)
                        else:
                            candidates = list(
                                linked_by_function.get(target, ())
                            )
                    resolved_candidates = bool(candidates)
                    if reference_edge:
                        candidates = [
                            candidate for candidate in candidates
                            if not _root_owners_for_node(candidate, roots)
                        ]
                    if not candidates:
                        reason = _abi_contract_reason(target, abi_contracts)
                        platform_boundary = (
                            platform_symbols is not None and
                            target in platform_symbols
                        )
                        # Ordinary data references which do not resolve to an
                        # application variable end at the platform/linker data
                        # boundary. Function references and direct calls need
                        # an exact linked symbol or named compiler ABI proof.
                        if (not data_edge and reason is None and
                                not platform_boundary and
                                not (reference_edge and resolved_candidates)):
                            edge_kind = (
                                "indirect call" if reference_edge else "call"
                            )
                            evidence.issues.append(
                                f"unresolved live compiler {edge_kind} "
                                f"owner={owner}: {node[0]}:{node[1]} -> {target}"
                            )
                    pending.extend(candidates)
    for node, records in linked_by_key.items():
        applicable = owners.get(node, set())
        if not applicable:
            evidence.issues.append(f"unattributed linked application function {node[0]}:{node[1]}")
            continue
        for record in records:
            if "dynamic" in record.qualifier and "bounded" not in record.qualifier:
                evidence.issues.append(f"unbounded compiler stack use in {record.function}")
            if record.bytes_used > frame_limit:
                evidence.issues.append(f"compiler frame {record.bytes_used} exceeds {frame_limit}: {record.function}")
            for owner in applicable:
                capacity = _owner_capacity(policy, owner)
                if capacity == 0 or record.bytes_used > capacity:
                    evidence.issues.append(
                        f"compiler owner bound failed for {record.source.name}:{record.function} owner={owner}"
                    )
        evidence.attributed_usage_count += len(records)
    _validate_synchronous_stack_chains(
        evidence,
        policy,
        linked,
        synchronous_cgraphs if synchronous_cgraphs is not None else cgraphs,
        owners,
    )


def _required_free(size: int, service: bool = False) -> int:
    return max((size * 20 + 99) // 100, 256 if service else 1024)


def _build_graph_clean(build_dir: Path, cache: dict[str, str]) -> str | None:
    make = cache.get("CMAKE_MAKE_PROGRAM")
    if not make:
        return "CMake cache lacks CMAKE_MAKE_PROGRAM"
    try:
        result = subprocess.run([make, "-C", str(build_dir), "-n"], capture_output=True, text=True, timeout=30, check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return f"cannot dry-run exact build graph: {exc}"
    output = "\n".join(part.strip() for part in (result.stdout, result.stderr) if part.strip())
    if result.returncode or "ninja: no work to do." not in output:
        return "exact build graph is dirty; rebuild before verification"
    return None


def _reachable_graph_nodes(
    cgraphs: dict[tuple[str, str], set[str]],
    seeds: set[tuple[str, str]],
) -> set[tuple[str, str]]:
    """Resolve conservative direct/callback reachability across TUs."""
    nodes = set(cgraphs)
    by_function: dict[str, set[tuple[str, str]]] = {}
    by_variable: dict[str, set[tuple[str, str]]] = {}
    for node in nodes:
        symbol = (
            node[1][len(_CGRAPH_VARIABLE_PREFIX):]
            if node[1].startswith(_CGRAPH_VARIABLE_PREFIX)
            else node[1]
        )
        if node[1].startswith(_CGRAPH_VARIABLE_PREFIX):
            by_variable.setdefault(_canonical_function(symbol), set()).add(node)
        else:
            by_function.setdefault(_canonical_function(node[1]), set()).add(node)
    reachable: set[tuple[str, str]] = set()
    pending = list(seeds)
    while pending:
        node = pending.pop()
        if node in reachable or node not in nodes:
            continue
        reachable.add(node)
        for raw_target in cgraphs.get(node, set()):
            if raw_target == _CGRAPH_INDIRECT_CALL or raw_target.startswith(
                _CGRAPH_UNKNOWN_REFERENCE_PREFIX
            ):
                continue
            reference = raw_target.startswith(_CGRAPH_REFERENCE_PREFIX)
            data_reference = raw_target.startswith(
                _CGRAPH_DATA_REFERENCE_PREFIX
            )
            if reference:
                target = raw_target[len(_CGRAPH_REFERENCE_PREFIX):]
            elif data_reference:
                target = raw_target[len(_CGRAPH_DATA_REFERENCE_PREFIX):]
            else:
                target = raw_target
            target = _canonical_function(target)
            candidates = []
            local = (node[0], target)
            local_variable = (node[0], _CGRAPH_VARIABLE_PREFIX + target)
            if not data_reference and local in nodes:
                candidates.append(local)
            if data_reference and local_variable in nodes:
                candidates.append(local_variable)
            if not candidates:
                candidates.extend(
                    by_variable.get(target, ()) if data_reference
                    else by_function.get(target, ())
                )
            pending.extend(candidates)
    return reachable


def _select_live_records(
    records_by_tu: dict[str, tuple[Path, list[StackUsage]]],
    symbols: set[str],
    cgraphs: dict[tuple[str, str], set[str]],
    build_dir: Path,
) -> list[StackUsage]:
    """Keep linked C rows plus live inline/header rows with TU provenance."""
    seeds: set[tuple[str, str]] = set()
    primary: list[StackUsage] = []
    for tu, (source_path, records) in records_by_tu.items():
        source_resolved = _resolve_compiler_source(source_path)
        for record in records:
            record_source = _resolve_compiler_source(record.source)
            is_primary = record_source == source_resolved
            if is_primary:
                primary.append(record)
                if (record.function in symbols or
                        _canonical_function(record.function) in symbols):
                    seeds.add((tu, _canonical_function(record.function)))

    reachable = _reachable_graph_nodes(cgraphs, seeds)
    selected: list[StackUsage] = []
    for tu, (source_path, records) in records_by_tu.items():
        source_resolved = _resolve_compiler_source(source_path)
        for record in records:
            record_source = _resolve_compiler_source(record.source)
            if record_source == source_resolved:
                if (record.function in symbols or
                        _canonical_function(record.function) in symbols):
                    selected.append(record)
                continue
            if _is_inline_stack_source(
                _resolve_compiler_source(record.source), build_dir
            ):
                node = (tu, _canonical_function(record.function))
                if node in reachable:
                    selected.append(record)
    return selected


def verify_build(
    build_dir: Path,
    policies: dict[str, PresetPolicy],
    frame_limit: int,
    *,
    allow_watchdog_bypass: bool = False,
) -> BuildEvidence:
    build_dir = build_dir.resolve()
    evidence = BuildEvidence(build_dir)
    try:
        cache = parse_cmake_cache(build_dir / "CMakeCache.txt")
        application_source_dir = Path(
            cache.get("APPLICATION_SOURCE_DIR", str(REPO_ROOT / "firmware" / "app"))
        ).resolve()
        zephyr_base_text = cache.get("ZEPHYR_BASE")
        west_topdir = (
            Path(zephyr_base_text).resolve().parent
            if zephyr_base_text else REPO_ROOT
        )
        evidence.preset = cache.get("IMEC_BUILD_PRESET", "")
        policy = policies.get(evidence.preset)
        if policy is None:
            evidence.issues.append(f"no stack policy for preset {evidence.preset!r}")
            return evidence
        if not policy.deployable and evidence.preset in DEPLOYABLE_PRESETS:
            evidence.issues.append("deployable preset is not marked deployable")
        clean = _build_graph_clean(build_dir, cache)
        if clean:
            evidence.issues.append(clean)
        evidence.config = parse_kconfig(build_dir / "zephyr" / ".config")
        _verify_config(
            evidence,
            policy,
            allow_watchdog_bypass=allow_watchdog_bypass,
        )
        _verify_storage_partition(evidence)
        evidence.ram_used, evidence.ram_size, evidence.ram_headroom, symbols = _ram_map(build_dir / "zephyr" / "zephyr.map")
        if evidence.config.get("CONFIG_SRAM_SIZE") != evidence.ram_size // 1024:
            evidence.issues.append("generated CONFIG_SRAM_SIZE differs from linker RAM")
        if evidence.ram_headroom < policy.minimum_static_ram_headroom_bytes:
            evidence.issues.append(f"static RAM headroom {evidence.ram_headroom} is below {policy.minimum_static_ram_headroom_bytes}")
        evidence.elf_path = build_dir / "zephyr" / "zephyr.elf"
        evidence.hex_path = build_dir / "zephyr" / "zephyr.hex"
        evidence.elf_sha256 = _sha256(evidence.elf_path)
        evidence.hex_sha256 = _sha256(evidence.hex_path)
        evidence.build_identity = extract_build_identity(evidence.elf_path)
        ninja, app_objects = _ninja_objects(build_dir / "build.ninja")
        evidence.app_object_count = len(app_objects)
        if "-fstack-usage" not in ninja:
            evidence.issues.append("build graph does not enable -fstack-usage")
        if "-fdump-ipa-cgraph" not in ninja:
            evidence.issues.append("build graph does not enable -fdump-ipa-cgraph")
        linked_objects = _linked_application_objects(
            build_dir, ninja, app_objects
        )
        roots = load_thread_roots()
        records_by_tu: dict[str, tuple[Path, list[StackUsage]]] = {}
        cgraphs: dict[tuple[str, str], set[str]] = {}
        synchronous_cgraphs: dict[tuple[str, str], set[str]] = {}
        for linked_object in linked_objects:
            object_path, source_path = linked_object.output, linked_object.source
            usage_path = (build_dir / object_path).with_suffix(".su")
            if not usage_path.is_file():
                evidence.issues.append(
                    f"missing compiler stack evidence {usage_path.relative_to(build_dir)}"
                )
                continue
            graph_path = _cgraph_path(build_dir, object_path)
            if not graph_path.is_file():
                evidence.issues.append(
                    f"missing compiler call graph {graph_path.relative_to(build_dir)}"
                )
                continue
            if source_path.is_file() and usage_path.stat().st_mtime + 0.001 < source_path.stat().st_mtime:
                evidence.issues.append(f"stale compiler stack evidence for {source_path}")
            if source_path.is_file() and graph_path.stat().st_mtime + 0.001 < source_path.stat().st_mtime:
                evidence.issues.append(f"stale compiler call graph for {source_path}")
            tu = _translation_unit_key(source_path, object_path)
            source_records, source_cgraph, source_synchronous_cgraph = (
                _parse_tu_compiler_evidence(
                    usage_path,
                    graph_path,
                    tu,
                    object_path,
                    final_map_path=build_dir / "zephyr" / "zephyr.map",
                    linked_object=linked_object,
                    application_source_dir=application_source_dir,
                    west_topdir=west_topdir,
                )
            )
            records_by_tu[tu] = (source_path, source_records)
            cgraphs.update(source_cgraph)
            synchronous_cgraphs.update(source_synchronous_cgraph)
        linked = _select_live_records(records_by_tu, symbols, cgraphs, build_dir)
        evidence.linked_usage_count = len(linked)
        if not linked:
            evidence.issues.append("no linked C functions matched compiler evidence")
        application_functions = {
            node[1] for node in cgraphs
            if not node[1].startswith(_CGRAPH_VARIABLE_PREFIX)
        }
        platform_symbols = symbols - application_functions
        _attribute_linked_functions(
            evidence,
            policy,
            linked,
            cgraphs,
            roots,
            frame_limit,
            synchronous_cgraphs,
            platform_symbols=platform_symbols,
        )
    except (OSError, EvidenceError, ValueError) as exc:
        evidence.issues.append(str(exc))
    return evidence


def _runtime_kernel_stack_size(build: BuildEvidence, configured_size: int) -> int:
    """Mirror K_KERNEL_STACK_SIZEOF for the exact ARM Zephyr build."""
    pointer_alignment = 8 if build.config.get("CONFIG_STACK_ALIGN_DOUBLE_WORD") else 4
    reserved = 0
    object_alignment = pointer_alignment
    if build.config.get("CONFIG_MPU_STACK_GUARD"):
        minimum = int(build.config.get(
            "CONFIG_ARM_MPU_REGION_MIN_ALIGN_AND_SIZE", 32
        ))
        reserved = max(64, minimum)
        object_alignment = reserved

    adjusted = ((configured_size + pointer_alignment - 1) //
                pointer_alignment) * pointer_alignment + reserved
    allocated = ((adjusted + object_alignment - 1) //
                 object_alignment) * object_alignment
    return allocated - reserved


def _required_threads(build: BuildEvidence, policy: PresetPolicy) -> dict[str, int]:
    # Runtime watermarks cover threads which still exist when the workload is
    # sampled. The startup main thread has returned by then, while the
    # integrated controller's HCI TX size is a synchronous call-stack setting,
    # not a live thread. Both remain checked by exact config/compiler evidence.
    required = {
        "sysworkq": _runtime_kernel_stack_size(
            build, policy.system_workqueue_bytes
        ),
        "mesh_route": _runtime_kernel_stack_size(
            build, policy.mesh_route_bytes
        ),
    }
    clicker_action_bytes = _owner_capacity(policy, "clicker_action")
    if clicker_action_bytes:
        required["clicker_action"] = clicker_action_bytes
    anchor_uwb_scan_bytes = _owner_capacity(policy, "anchor_uwb_scan")
    if anchor_uwb_scan_bytes:
        required["anchor_uwb_scan"] = _runtime_kernel_stack_size(
            build, anchor_uwb_scan_bytes
        )
    if policy.idle_bytes:
        required["idle"] = policy.idle_bytes
    if policy.log_processor_bytes:
        required["logging"] = policy.log_processor_bytes
    if policy.bt_rx_bytes:
        if build.config.get("CONFIG_BT_RECV_WORKQ_BT"):
            required["BT RX WQ"] = policy.bt_rx_bytes
        elif not build.config.get("CONFIG_BT_RECV_WORKQ_SYS"):
            required["BT RX"] = policy.bt_rx_bytes
        if build.config.get("CONFIG_BT_LONG_WQ"):
            required["BT LW WQ"] = _runtime_kernel_stack_size(
                build,
                int(build.config.get("CONFIG_BT_LONG_WQ_STACK_SIZE", 0)),
            )
        if build.config.get("CONFIG_MPSL"):
            required["MPSL Work"] = int(build.config.get("CONFIG_MPSL_WORK_STACK_SIZE", 0))
    return required


def _parse_time(value: Any) -> datetime:
    if not isinstance(value, str):
        raise EvidenceError("capture timestamp is missing")
    parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        raise EvidenceError("capture timestamp lacks a timezone")
    return parsed.astimezone(timezone.utc)


def _fields(line: str, prefix: str, required: set[str]) -> dict[str, str]:
    if not line.startswith(prefix + " "):
        raise EvidenceError(f"expected {prefix}")
    values: dict[str, str] = {}
    for token in line[len(prefix) + 1:].split():
        if "=" not in token:
            raise EvidenceError(f"malformed {prefix} token")
        key, value = token.split("=", 1)
        if key in values or not key or not value:
            raise EvidenceError(f"malformed {prefix} field")
        values[key] = value
    missing = required - values.keys()
    if missing:
        raise EvidenceError(f"{prefix} misses {','.join(sorted(missing))}")
    return values


def _u32(fields: dict[str, str], key: str) -> int:
    value = fields.get(key, "")
    if not value.isdigit() or int(value) > 0xFFFFFFFF:
        raise EvidenceError(f"invalid {key}")
    return int(value)


def _u64(fields: dict[str, str], key: str) -> int:
    value = fields.get(key, "")
    if not value.isdigit() or int(value) > 0xFFFFFFFFFFFFFFFF:
        raise EvidenceError(f"invalid {key}")
    return int(value)


def _int(fields: dict[str, str], key: str) -> int:
    try:
        return int(fields[key])
    except (KeyError, ValueError) as exc:
        raise EvidenceError(f"invalid {key}") from exc


def _check_sample_rows(rows: dict[str, tuple[int, int, int]], policy: PresetPolicy, build: BuildEvidence) -> list[str]:
    issues: list[str] = []
    required = _required_threads(build, policy)
    allowed = set(required) | set(KNOWN_DYNAMIC_THREAD_NAMES)
    for name, (used, free, size) in rows.items():
        if name not in allowed:
            issues.append(f"unknown RTT thread owner {name}")
        if used + free != size:
            issues.append(f"invalid RTT stack row for {name}")
    for name, size in required.items():
        row = rows.get(name)
        if row is None:
            issues.append(f"RTT sample misses configured thread {name}")
            continue
        service = name in {"logging", "BT RX", "BT RX WQ", "BT LW WQ", "MPSL Work", "idle"}
        if row[2] != size:
            issues.append(f"RTT stack size for {name} differs from generated config")
        if row[1] < _required_free(size, service):
            issues.append(f"RTT stack free space below policy for {name}")
    return issues


def parse_typed_transcript(log: str, policy: PresetPolicy, build: BuildEvidence) -> tuple[int, list[str]]:
    issues: list[str] = []
    requirements = load_workload_policy().get(policy.preset, ())
    requirements_by_kind = {item.kind: item for item in requirements}
    boot: dict[str, str] | None = None
    runs: dict[int, dict[str, Any]] = {}
    completed_ids: set[int] = set()
    completed: list[dict[str, Any]] = []
    open_sample: dict[str, Any] | None = None
    sample_count = 0
    for raw in log.splitlines():
        line = raw.strip()
        if not line or not line.startswith("DBG_STACK"):
            continue
        try:
            if line.startswith("DBG_STACK_BOOT "):
                if boot is not None:
                    raise EvidenceError("duplicate target boot identity")
                boot = _fields(line, "DBG_STACK_BOOT", {"preset", "build", "epoch", "uptime"})
                if _u64(boot, "epoch") == 0:
                    raise EvidenceError("target boot epoch is zero")
                _u32(boot, "uptime")
            elif line.startswith("DBG_STACK_RUN_BEGIN "):
                fields = _fields(line, "DBG_STACK_RUN_BEGIN", {"epoch", "run", "kind", "owner", "queue", "custody", "credit", "retry", "drain", "src", "dst", "session", "seq", "type", "sequence", "previous", "uptime"})
                _u32(fields, "uptime")
                run_id = _u32(fields, "run")
                identity = (_u64(fields, "src"), _u64(fields, "dst"), _u32(fields, "session"), _u32(fields, "seq"), _u32(fields, "type"))
                if boot is None or _u64(fields, "epoch") != _u64(boot, "epoch") or run_id == 0 or run_id == 0xFFFFFFFF or run_id in runs or run_id in completed_ids or fields["kind"] not in KNOWN_WORKLOADS or fields["owner"] not in KNOWN_DYNAMIC_THREAD_NAMES or identity == (0, 0, 0, 0, 0):
                    raise EvidenceError("invalid typed workload run")
                requirement = requirements_by_kind.get(fields["kind"])
                if requirement is not None and fields["owner"] != requirement.owner:
                    raise EvidenceError("typed workload owner differs from preset policy")
                sequence, previous = _u32(fields, "sequence"), _u32(fields, "previous")
                if fields["kind"] == "click_spam":
                    if sequence == 0 or sequence == 0xFFFFFFFF:
                        raise EvidenceError("click sequence has wraparound ambiguity")
                    if previous != 0:
                        retained = runs.get(previous)
                        if retained is None or retained["kind"] != "click_spam":
                            raise EvidenceError("click previous link is forged or non-overlapping")
                        if retained["identity"] == identity:
                            raise EvidenceError("click previous link does not retain a distinct report identity")
                elif sequence != 0 or previous != 0:
                    raise EvidenceError("non-click workload has forged click linkage")
                runs[run_id] = {"id": run_id, "epoch": _u64(fields, "epoch"), "kind": fields["kind"], "owner": fields["owner"], "identity": identity, "sequence": sequence, "previous": previous, "samples": [], "start": (_u32(fields, "queue"), _u32(fields, "custody"), _u32(fields, "credit"), _u32(fields, "retry"), _u32(fields, "drain"))}
            elif line.startswith("DBG_STACK_SAMPLE_BEGIN "):
                if open_sample is not None:
                    raise EvidenceError("overlapping RTT stack samples")
                fields = _fields(line, "DBG_STACK_SAMPLE_BEGIN", {"epoch", "run", "sample", "kind", "owner", "queue", "custody", "credit", "retry", "drain", "src", "dst", "session", "seq", "type", "uptime"})
                _u32(fields, "uptime")
                run_id, sample_id = _u32(fields, "run"), _u32(fields, "sample")
                run = runs.get(run_id)
                identity = (_u64(fields, "src"), _u64(fields, "dst"), _u32(fields, "session"), _u32(fields, "seq"), _u32(fields, "type"))
                if run is None or _u64(fields, "epoch") != run["epoch"] or sample_id == 0 or fields["kind"] != run["kind"] or fields["owner"] != run["owner"] or identity != run["identity"]:
                    raise EvidenceError("sample is not correlated to its active workload")
                open_sample = {"run": run_id, "sample": sample_id, "rows": {}, "isr_config": None, "state": (_u32(fields, "queue"), _u32(fields, "custody"), _u32(fields, "credit"), _u32(fields, "retry"), _u32(fields, "drain"))}
            elif line.startswith("DBG_STACK_ISR_CONFIG "):
                fields = _fields(line, "DBG_STACK_ISR_CONFIG", {"size", "run", "sample"})
                if open_sample is None or _u32(fields, "run") != open_sample["run"] or _u32(fields, "sample") != open_sample["sample"]:
                    raise EvidenceError("ISR configuration lies outside its typed sample")
                if open_sample["isr_config"] is not None:
                    raise EvidenceError("duplicate ISR configuration record")
                open_sample["isr_config"] = _u32(fields, "size")
            elif line.startswith("DBG_STACK name="):
                row = re.fullmatch(r"DBG_STACK name=(.*?) tid=\S+ used=(\d+) free=(\d+) size=(\d+) ret=(-?\d+) run=(\d+) sample=(\d+)", line)
                if row is None or open_sample is None:
                    raise EvidenceError("malformed or uncorrelated RTT stack row")
                name, used, free, size, ret, run_id, sample_id = row.groups()
                if int(run_id) != open_sample["run"] or int(sample_id) != open_sample["sample"] or int(ret) != 0 or name in open_sample["rows"]:
                    raise EvidenceError("invalid RTT stack row")
                open_sample["rows"][name] = (int(used), int(free), int(size))
            elif line.startswith("DBG_STACK_SAMPLE_END "):
                fields = _fields(line, "DBG_STACK_SAMPLE_END", {"run", "sample"})
                if open_sample is None or _u32(fields, "run") != open_sample["run"] or _u32(fields, "sample") != open_sample["sample"]:
                    raise EvidenceError("unmatched RTT stack sample end")
                if open_sample["isr_config"] != policy.isr_bytes:
                    raise EvidenceError("ISR configuration differs from generated config")
                issues.extend(_check_sample_rows(open_sample["rows"], policy, build))
                runs[open_sample["run"]]["samples"].append(open_sample)
                sample_count += 1
                open_sample = None
            elif line.startswith("DBG_STACK_RUN_END "):
                fields = _fields(line, "DBG_STACK_RUN_END", {"epoch", "run", "kind", "owner", "outcome", "queue", "custody", "credit", "retry", "drain", "src", "dst", "session", "seq", "type", "samples", "attempts", "errors", "last_error", "mutex_drops", "short_drops", "sequence", "previous", "uptime"})
                _u32(fields, "uptime")
                run_id = _u32(fields, "run")
                run = runs.pop(run_id, None)
                if run is None or open_sample is not None and open_sample["run"] == run_id:
                    raise EvidenceError("unmatched workload completion")
                identity = (_u64(fields, "src"), _u64(fields, "dst"), _u32(fields, "session"), _u32(fields, "seq"), _u32(fields, "type"))
                samples = _u32(fields, "samples")
                sample_attempts = _u32(fields, "attempts")
                sample_errors = _u32(fields, "errors")
                last_sample_error = _int(fields, "last_error")
                _u32(fields, "mutex_drops")
                _u32(fields, "short_drops")
                if (sample_attempts != samples + sample_errors or
                        (sample_errors == 0 and last_sample_error != 0) or
                        (sample_errors > 0 and not (-0x8000 <= last_sample_error < 0))):
                    raise EvidenceError("workload completion has inconsistent sample accounting")
                if _u64(fields, "epoch") != run["epoch"] or fields["kind"] != run["kind"] or fields["owner"] != run["owner"] or identity != run["identity"] or _u32(fields, "sequence") != run["sequence"] or _u32(fields, "previous") != run["previous"] or samples != len(run["samples"]) or fields["outcome"] not in {"ack", "custody_drop", "direct_ack_failure", "preempted", "timeout_drop", "disconnect", "error"}:
                    raise EvidenceError("workload completion does not match its run")
                run["outcome"] = fields["outcome"]
                run["end"] = (_u32(fields, "queue"), _u32(fields, "custody"), _u32(fields, "credit"), _u32(fields, "retry"), _u32(fields, "drain"))
                completed.append(run)
                completed_ids.add(run_id)
            else:
                raise EvidenceError("marker-only or unsupported stack evidence")
        except EvidenceError as exc:
            issues.append(str(exc))
    if boot is None:
        issues.append("transcript lacks target-reported boot identity")
    elif boot.get("preset") != policy.preset or boot.get("build") != build.build_identity:
        issues.append("target-reported boot identity does not match the exact artifact")
    if open_sample is not None:
        issues.append("unterminated typed stack sample")
    if runs:
        issues.append("unterminated typed workload run")
    for requirement in requirements:
        successful = [
            run for run in completed
            if run["kind"] == requirement.kind and run["outcome"] == "ack" and
            run["samples"] and run["owner"] == requirement.owner
        ]
        if len(successful) < requirement.minimum_successes:
            issues.append(
                "missing completed typed workload evidence for "
                f"{requirement.kind}: need {requirement.minimum_successes}, "
                f"have {len(successful)}"
            )
            continue
        if not requirement.ordered_sequence:
            continue
        ordered = sorted(successful, key=lambda run: run["sequence"])
        for previous, current in zip(ordered, ordered[1:]):
            if current["sequence"] != previous["sequence"] + 1 or current["previous"] != previous["id"]:
                issues.append("sequential-click run IDs are not an ordered real sequence")
                break
            if previous["identity"] == current["identity"]:
                issues.append("sequential-click report identity was not retained distinctly")
                break
            if not isinstance(previous.get("end"), tuple) or not isinstance(current.get("start"), tuple):
                issues.append("sequential-click queue/custody state is missing")
                break
    return sample_count, issues


def _capture_id(data: dict[str, Any]) -> str:
    artifact, transcript, provenance = data["artifact"], data["transcript"], data["provenance"]
    payload = "|".join((artifact["elf_sha256"], artifact["hex_sha256"], transcript["sha256"], data["preset"], data["probe_id"], provenance["started_at_utc"], provenance["ended_at_utc"]))
    return hashlib.sha256(payload.encode("ascii")).hexdigest()


def _manifest_path(base: Path, value: Any) -> Path:
    if not isinstance(value, str):
        raise EvidenceError("transcript path is missing")
    path = (base / value).resolve()
    if path == base.resolve() or base.resolve() not in path.parents:
        raise EvidenceError("transcript path escapes capture directory")
    return path


def _load_hardware_manifest(path: Path, build: BuildEvidence, policy: PresetPolicy) -> HardwareEvidence:
    evidence = HardwareEvidence(policy.preset, path)
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(data, dict) or data.get("schema") != CAPTURE_SCHEMA:
            raise EvidenceError("manifest is not a trusted schema-3 capture")
        for key in ("preset", "probe_id", "capture_id", "artifact", "target", "transcript", "provenance"):
            if key not in data:
                raise EvidenceError(f"manifest misses {key}")
        if data["preset"] != policy.preset or not isinstance(data["probe_id"], str) or not data["probe_id"]:
            raise EvidenceError("manifest preset or probe identity is invalid")
        artifact, target, transcript, provenance = data["artifact"], data["target"], data["transcript"], data["provenance"]
        if not all(isinstance(value, dict) for value in (artifact, target, transcript, provenance)):
            raise EvidenceError("manifest provenance sections are invalid")
        if artifact.get("elf_sha256") != build.elf_sha256 or artifact.get("hex_sha256") != build.hex_sha256:
            raise EvidenceError("manifest artifact hash differs from exact build")
        if target.get("preset") != policy.preset or target.get("build_identity") != build.build_identity:
            raise EvidenceError("manifest target identity differs from exact artifact")
        if provenance.get("tool") != CAPTURE_TOOL_RELATIVE or provenance.get("workflow") != CAPTURE_WORKFLOW:
            raise EvidenceError("manifest was not created by the repository capture workflow")
        if provenance.get("tool_sha256") != _sha256(CAPTURE_TOOL):
            raise EvidenceError("capture tool provenance does not match this repository")
        expected_command = [
            "pyocd", "rtt", "-t", "nrf52833", "-M", "pre-reset",
            "-a", "0x20000410", "-s", "0x100",
            "-u", data["probe_id"], "--up-channel-id", "0",
        ]
        if provenance.get("rtt_command") != expected_command or provenance.get("tty_wrapper") != "script":
            raise EvidenceError("capture did not use the required pyOCD RTT pre-reset TTY workflow")
        started, ended = _parse_time(provenance.get("started_at_utc")), _parse_time(provenance.get("ended_at_utc"))
        now = datetime.now(timezone.utc)
        if ended > now + MAX_FUTURE_SKEW:
            raise EvidenceError("capture timestamp is in the future")
        if (started > ended or
                ended - started >
                MAX_CAPTURE_DURATION + MAX_CAPTURE_PROCESS_OVERHEAD):
            raise EvidenceError("capture wall-clock bounds are invalid")
        if ended < now - MAX_CAPTURE_AGE:
            raise EvidenceError("capture is stale")
        log_path = _manifest_path(path.parent, transcript.get("path"))
        if transcript.get("sha256") != _sha256(log_path):
            raise EvidenceError("RTT transcript SHA-256 mismatch")
        if data["capture_id"] != _capture_id(data):
            raise EvidenceError("capture ID does not bind transcript, artifact, probe, and wall-clock bounds")
        evidence.probe_id = data["probe_id"]
        evidence.capture_id = data["capture_id"]
        evidence.rtt_sha256 = transcript["sha256"]
        evidence.sample_count, runtime_issues = parse_typed_transcript(log_path.read_text(encoding="utf-8", errors="replace"), policy, build)
        evidence.issues.extend(runtime_issues)
    except (OSError, KeyError, TypeError, ValueError, EvidenceError) as exc:
        evidence.issues.append(str(exc))
    return evidence


def verify_hardware(manifest_paths: list[Path], builds: list[BuildEvidence], policies: dict[str, PresetPolicy], require_hardware: bool, targets: set[str] | None = None, consumed_capture_ids: set[str] | None = None) -> tuple[list[HardwareEvidence], list[str]]:
    by_preset = {build.preset: build for build in builds}
    wanted = targets or set(by_preset)
    issues: list[str] = []
    results: list[HardwareEvidence] = []
    manifests_by_preset: dict[str, Path] = {}
    for path in manifest_paths:
        try:
            value = json.loads(path.read_text(encoding="utf-8"))
            preset = value.get("preset") if isinstance(value, dict) else None
            if not isinstance(preset, str) or preset in manifests_by_preset:
                issues.append(f"duplicate or unreadable hardware manifest {path}")
            else:
                manifests_by_preset[preset] = path
        except (OSError, ValueError):
            issues.append(f"cannot read hardware manifest {path}")
    for preset in sorted(wanted):
        build = by_preset.get(preset)
        policy = policies.get(preset)
        if build is None or policy is None:
            issues.append(f"no exact build policy for hardware target {preset}")
            continue
        if preset not in DEPLOYABLE_PRESETS or not policy.deployable:
            issues.append(f"{preset} is not a deployable verified-flash target")
            continue
        path = manifests_by_preset.get(preset)
        if path is None:
            if require_hardware:
                issues.append(f"missing trusted hardware capture for {preset}")
            continue
        result = _load_hardware_manifest(path, build, policy)
        if consumed_capture_ids is not None and result.capture_id in consumed_capture_ids:
            result.issues.append("capture replay was already consumed by verified deployment flashing")
        results.append(result)
    return results, issues


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", action="append", type=Path, required=True)
    parser.add_argument("--hardware-manifest", action="append", type=Path)
    parser.add_argument("--require-hardware", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        policies, frame_limit = load_policy()
    except EvidenceError as exc:
        print(f"stack evidence policy error: {exc}", file=sys.stderr)
        return 1
    builds = [verify_build(path, policies, frame_limit) for path in args.build_dir]
    issues = [f"{build.build_dir}: {issue}" for build in builds for issue in build.issues]
    manifests, hardware_issues = verify_hardware(args.hardware_manifest or [], builds, policies, args.require_hardware)
    issues.extend(hardware_issues)
    issues.extend(f"{item.manifest}: {issue}" for item in manifests for issue in item.issues)
    if issues:
        print("stack evidence rejected:", file=sys.stderr)
        print("\n".join(f"  {issue}" for issue in issues), file=sys.stderr)
        return 1
    print("stack evidence verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
