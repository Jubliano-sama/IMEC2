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
KNOWN_WORKLOADS = frozenset({
    "click_spam", "cir_handling", "relay_retry", "ble_backpressure",
    "click_activity", "anchor_survey_report", "gateway_report_ingress",
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


def _verify_config(evidence: BuildEvidence, policy: PresetPolicy) -> None:
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
    if policy.deployable:
        for key in ("CONFIG_IMEC_STACK_DIAGNOSTICS", "CONFIG_THREAD_MONITOR", "CONFIG_THREAD_NAME", "CONFIG_USE_SEGGER_RTT"):
            _expect(evidence, key, True)


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


def _ninja_objects(path: Path) -> tuple[str, list[tuple[Path, Path]]]:
    text = path.read_text(encoding="utf-8", errors="replace")
    objects = [
        (Path(obj), Path(source))
        for obj, source in re.findall(
            r"^build\s+(\S+\.obj):[^\n]*?\s(\S+\.c)(?:\s|$)", text, re.MULTILINE
        )
        if "CMakeFiles/app.dir/" in obj
    ]
    if not objects:
        raise EvidenceError("build graph has no application C objects")
    return text, objects


def _parse_su(path: Path) -> list[StackUsage]:
    records: list[StackUsage] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        columns = line.split("\t")
        location = re.fullmatch(r"(.*):(\d+):(\d+):(.+)", columns[0]) if len(columns) >= 3 else None
        if location is None or not columns[1].isdigit():
            raise EvidenceError(f"malformed compiler stack row in {path}: {line!r}")
        records.append(StackUsage(Path(location.group(1)), int(location.group(2)), location.group(4), int(columns[1]), ",".join(columns[2:])))
    if not records:
        raise EvidenceError(f"empty compiler stack evidence {path}")
    return records


def _resolve_compiler_source(path: Path) -> Path:
    """Resolve GCC paths after Zephyr's WEST_TOPDIR prefix remapping."""
    if not path.is_absolute() and path.parts[:1] == ("WEST_TOPDIR",):
        return (REPO_ROOT.joinpath(*path.parts[1:])).resolve()
    return path.resolve()


_GCC_CLONE_SUFFIX_RE = re.compile(
    r"(?:\.(?:isra|constprop|part)(?:\.\d+)?)+$"
)


def _canonical_function(function: str) -> str:
    return _GCC_CLONE_SUFFIX_RE.sub("", function)


_CGRAPH_VARIABLE_PREFIX = "<variable>:"
_CGRAPH_REFERENCE_PREFIX = "<reference>:"


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
            12288 if policy.preset in {
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
    nodes: dict[tuple[str, str], set[str]] = {}
    pattern = re.compile(r"^([^\s/]+)/\d+ \(([^)]+)\).*?(?=^[^\s/]+/\d+ \(|\Z)", re.MULTILINE | re.DOTALL)
    for match in pattern.finditer(text):
        block = match.group(0)
        function_definition = "Type: function definition analyzed" in block
        immutable_variable = (
            "Type: variable definition analyzed" in block and
            re.search(r"^  Varpool flags:.*\bread-only\b", block, re.MULTILINE) is not None
        )
        if not function_definition and not immutable_variable:
            continue
        symbol = _canonical_function(match.group(1))
        if immutable_variable and _compiler_function_name_symbol(symbol):
            # Every C function may own a distinct compiler-generated
            # `__func__` string with the same source-level symbol. Joining
            # those constants would invent call edges between unrelated work
            # queues; they can never contain a callback.
            continue
        node_symbol = (_CGRAPH_VARIABLE_PREFIX + symbol
                       if immutable_variable else symbol)
        calls = re.search(r"^  Calls:\s*(.*)$", block, re.MULTILINE)
        targets = set()
        if calls is not None:
            targets = {
                _canonical_function(target)
                for target in re.findall(r"([^\s/]+)/\d+", calls.group(1))
            }
        references = re.search(r"^  References:\s*(.*)$", block, re.MULTILINE)
        if references is not None:
            # Callback and ops tables are compiler variable nodes between the
            # function which uses the table and its address-taken functions.
            # Preserve every compiler-proved reference so ownership can cross
            # that table without a source-wide fallback.
            targets.update(
                _CGRAPH_REFERENCE_PREFIX + canonical
                for target in re.findall(r"([^\s/]+)/\d+", references.group(1))
                if not _compiler_function_name_symbol(
                    canonical := _canonical_function(target)
                )
            )
        # GCC emits several IPA snapshots for one symbol.  Preserve every
        # compiler-proved edge; later optimization snapshots may legitimately
        # show an empty Calls line after inlining or cloning.
        nodes.setdefault((source_name, node_symbol), set()).update(targets)
    if not nodes:
        raise EvidenceError(f"compiler call graph has no analyzed definitions {path}")
    return nodes


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
    for match in pattern.finditer(optimized):
        block = match.group(0)
        if "Type: function definition analyzed" not in block:
            continue
        symbol = match.group(1)
        targets: set[str] = set()
        calls = re.search(r"^  Calls:\s*(.*)$", block, re.MULTILINE)
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
        raise EvidenceError(
            f"compiler optimized call graph has no analyzed definitions {path}"
        )
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
        linked_by_key.setdefault((record.source.name, record.function), []).append(record)
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


def _attribute_linked_functions(
    evidence: BuildEvidence,
    policy: PresetPolicy,
    linked: list[StackUsage],
    cgraphs: dict[tuple[str, str], set[str]],
    roots: dict[tuple[str, str], set[str]],
    frame_limit: int,
    synchronous_cgraphs: dict[tuple[str, str], set[str]] | None = None,
) -> None:
    linked_by_key: dict[tuple[str, str], list[StackUsage]] = {}
    for record in linked:
        linked_by_key.setdefault((record.source.name, _canonical_function(record.function)), []).append(record)
    graph_nodes = set(cgraphs)
    by_function: dict[str, set[tuple[str, str]]] = {}
    for node in linked_by_key:
        by_function.setdefault(node[1], set()).add(node)
    for node in linked_by_key:
        if node not in cgraphs:
            evidence.issues.append(f"missing compiler call-graph node for {node[0]}:{node[1]}")

    owners: dict[tuple[str, str], set[str]] = {node: set() for node in graph_nodes}
    for root, root_owners in roots.items():
        if root not in graph_nodes:
            continue
        for owner in root_owners:
            # Root annotations are shared across exact role builds. A service
            # with zero configured stack capacity does not exist in that
            # preset and cannot authorize otherwise unowned linked code.
            if _owner_capacity(policy, owner) == 0:
                continue
            pending = [root]
            seen: set[tuple[str, str]] = set()
            while pending:
                node = pending.pop()
                if node in seen:
                    continue
                seen.add(node)
                owners[node].add(owner)
                for target in cgraphs.get(node, set()):
                    reference_edge = target.startswith(_CGRAPH_REFERENCE_PREFIX)
                    if reference_edge:
                        target = target[len(_CGRAPH_REFERENCE_PREFIX):]
                    # Prefer exact translation-unit definitions, including
                    # immutable callback/ops variables and non-linked compiler
                    # intermediates. Only an unresolved external name fans out
                    # to linked cross-object candidates. This stays
                    # conservative without joining unrelated static helpers or
                    # compiler constants which happen to share a name.
                    local = []
                    local_function = (node[0], target)
                    local_variable = (node[0], _CGRAPH_VARIABLE_PREFIX + target)
                    if local_function in graph_nodes:
                        local.append(local_function)
                    if local_variable in graph_nodes:
                        local.append(local_variable)
                    if local:
                        candidates = local
                    else:
                        candidates = list(by_function.get(target, ()))
                    if reference_edge:
                        candidates = [candidate for candidate in candidates
                                      if candidate not in roots]
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


def verify_build(build_dir: Path, policies: dict[str, PresetPolicy], frame_limit: int) -> BuildEvidence:
    build_dir = build_dir.resolve()
    evidence = BuildEvidence(build_dir)
    try:
        cache = parse_cmake_cache(build_dir / "CMakeCache.txt")
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
        _verify_config(evidence, policy)
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
        ninja, objects = _ninja_objects(build_dir / "build.ninja")
        evidence.app_object_count = len(objects)
        if "-fstack-usage" not in ninja:
            evidence.issues.append("build graph does not enable -fstack-usage")
        roots = load_thread_roots()
        records: list[StackUsage] = []
        cgraphs: dict[tuple[str, str], set[str]] = {}
        synchronous_cgraphs: dict[tuple[str, str], set[str]] = {}
        for object_path, source_path in objects:
            usage_path = (build_dir / object_path).with_suffix(".su")
            if not usage_path.is_file():
                evidence.issues.append(f"missing compiler stack evidence {usage_path.relative_to(build_dir)}")
                continue
            graph_path = _cgraph_path(build_dir, object_path)
            if not graph_path.is_file():
                evidence.issues.append(f"missing compiler call graph {graph_path.relative_to(build_dir)}")
                continue
            if source_path.is_file() and usage_path.stat().st_mtime + 0.001 < source_path.stat().st_mtime:
                evidence.issues.append(f"stale compiler stack evidence for {source_path}")
            if source_path.is_file() and graph_path.stat().st_mtime + 0.001 < source_path.stat().st_mtime:
                evidence.issues.append(f"stale compiler call graph for {source_path}")
            # GCC includes inline header helpers in a translation unit's .su
            # file.  The linked-app accounting domain is the object source
            # itself; Zephyr header helpers belong to their own platform
            # budgets and are not falsely treated as app symbols here.
            source_records = _parse_su(usage_path)
            try:
                source_resolved = source_path.resolve()
                records.extend(
                    record for record in source_records
                    if _resolve_compiler_source(record.source) == source_resolved
                )
            except OSError as exc:
                evidence.issues.append(f"cannot resolve application source {source_path}: {exc}")
            cgraphs.update(_parse_cgraph(graph_path, source_path.name))
            synchronous_cgraphs.update(
                _parse_synchronous_cgraph(graph_path, source_path.name)
            )
        linked = [
            record for record in records
            if record.function in symbols or
            _canonical_function(record.function) in symbols
        ]
        evidence.linked_usage_count = len(linked)
        if not linked:
            evidence.issues.append("no linked application functions matched compiler evidence")
        _attribute_linked_functions(
            evidence,
            policy,
            linked,
            cgraphs,
            roots,
            frame_limit,
            synchronous_cgraphs,
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
        "sysworkq": policy.system_workqueue_bytes,
        "mesh_route": policy.mesh_route_bytes,
    }
    clicker_action_bytes = _owner_capacity(policy, "clicker_action")
    if clicker_action_bytes:
        required["clicker_action"] = clicker_action_bytes
    anchor_uwb_scan_bytes = _owner_capacity(policy, "anchor_uwb_scan")
    if anchor_uwb_scan_bytes:
        required["anchor_uwb_scan"] = anchor_uwb_scan_bytes
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
                fields = _fields(line, "DBG_STACK_RUN_END", {"epoch", "run", "kind", "owner", "outcome", "queue", "custody", "credit", "retry", "drain", "src", "dst", "session", "seq", "type", "samples", "sequence", "previous", "uptime"})
                _u32(fields, "uptime")
                run_id = _u32(fields, "run")
                run = runs.pop(run_id, None)
                if run is None or open_sample is not None and open_sample["run"] == run_id:
                    raise EvidenceError("unmatched workload completion")
                identity = (_u64(fields, "src"), _u64(fields, "dst"), _u32(fields, "session"), _u32(fields, "seq"), _u32(fields, "type"))
                if _u64(fields, "epoch") != run["epoch"] or fields["kind"] != run["kind"] or fields["owner"] != run["owner"] or identity != run["identity"] or _u32(fields, "sequence") != run["sequence"] or _u32(fields, "previous") != run["previous"] or _u32(fields, "samples") != len(run["samples"]) or fields["outcome"] not in {"ack", "custody_drop", "direct_ack_failure", "preempted", "timeout_drop", "disconnect", "error"}:
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
            "-u", data["probe_id"], "--up-channel-id", "0",
        ]
        if provenance.get("rtt_command") != expected_command or provenance.get("tty_wrapper") != "script":
            raise EvidenceError("capture did not use the required pyOCD RTT pre-reset TTY workflow")
        started, ended = _parse_time(provenance.get("started_at_utc")), _parse_time(provenance.get("ended_at_utc"))
        now = datetime.now(timezone.utc)
        if ended > now + MAX_FUTURE_SKEW:
            raise EvidenceError("capture timestamp is in the future")
        if started > ended or ended - started > MAX_CAPTURE_DURATION:
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
