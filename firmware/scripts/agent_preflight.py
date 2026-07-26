#!/usr/bin/env python3
"""Route planned work to relevant entries in AGENT_KNOWN_ISSUES.md."""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import re
import subprocess
import sys
from dataclasses import dataclass
from datetime import date
from pathlib import Path
from typing import Iterable


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LEDGER = REPO_ROOT / "AGENT_KNOWN_ISSUES.md"
DEFAULT_CURRENT_ISSUES = REPO_ROOT / "AGENT_CURRENT_ISSUES.json"


@dataclass(frozen=True)
class Topic:
    path_terms: tuple[str, ...]
    keywords: tuple[str, ...]
    references: tuple[str, ...]


TOPICS: dict[str, Topic] = {
    "survey": Topic(
        ("survey", "assignment", "discovery"),
        (
            "survey",
            "pair",
            "prepare",
            "survey start",
            "survey go",
            "assignment",
            "discovery",
        ),
        (
            "Documentation/Mesh Connected Routing Contract.md",
            "firmware/tests/mesh_integration/README.md",
        ),
    ),
    "routing": Topic(
        ("mesh", "route", "node_comm", "relay", "radio"),
        (
            "route",
            "relay",
            "custody",
            "channel-5",
            "channel 5",
            "channel-9",
            "channel 9",
            "gateway ack",
            "radio owner",
            "workqueue",
        ),
        (
            "Documentation/Mesh Connected Routing Contract.md",
            "Documentation/Mesh Connected Routing Walkthrough.md",
        ),
    ),
    "dwm3000": Topic(
        ("dwm3000", "uwb", "ranging", "spi"),
        (
            "dwm3000",
            "dwt",
            "ds-twr",
            "spi",
            "phr",
            "delayed final",
            "range_ok",
        ),
        (
            "Documentation/HARDWARE_BRINGUP_DEBUG.md",
            "Documentation/Mesh Connected Routing Contract.md",
        ),
    ),
    "ble": Topic(
        ("ble", "bluetooth", "gatt"),
        (
            "ble",
            "bluetooth",
            "gatt",
            "packet_tx_uuid",
            "packet_rx_uuid",
            "backpressure",
            "notification",
        ),
        (
            "Documentation/Gateway BLE Streaming.md",
            "Documentation/Mesh Connected Routing Contract.md",
        ),
    ),
    "persistence": Topic(
        ("persist", "journal", "nvs", "outbox", "reset"),
        (
            "nvs",
            "journal",
            "persistent",
            "persistence",
            "outbox",
            "reset loss",
            "torn",
        ),
        (
            "Documentation/Mesh Connected Routing Contract.md",
            "firmware/tests/mesh_integration/README.md",
        ),
    ),
    "deployment": Topic(
        ("flash", "stack", "deploy", "capture", "rtt", "pyocd", "preset"),
        (
            "flash",
            "stack evidence",
            "stack verification",
            "pyocd",
            "rtt",
            "probe",
            "hardware manifest",
            "exact role",
        ),
        (
            "Documentation/Development and Deployment Guide.md",
            "Documentation/HARDWARE_BRINGUP_DEBUG.md",
        ),
    ),
    "testing": Topic(
        ("test", "sim", "cmake", "workflow", ".github"),
        (
            "fixture",
            "simulator",
            "ctest",
            "cmake",
            "asan",
            "tsan",
            "sanitizer",
            "source guard",
            "native test",
            "test binary",
        ),
        (
            "firmware/tests/mesh_integration/README.md",
            "Documentation/Mesh Integration Coverage Matrix 2026-07-12.md",
        ),
    ),
    "documentation": Topic(
        ("documentation", "readme", "codemap", "wiki", ".md"),
        (
            "documentation",
            "versioned",
            "version bump",
            "wiki",
            "mermaid",
            "source of truth",
        ),
        (
            "Documentation/CURRENT.json",
            "Documentation/INDEX.md",
        ),
    ),
    "tooling": Topic(
        ("agent", "git", ".github"),
        (
            "git add",
            "git worktree",
            "ignored",
            "sandbox",
            "context-mode",
            "shell",
            "python unittest",
            "tool",
        ),
        (
            "AGENTS.md",
            "Documentation/Development and Deployment Guide.md",
        ),
    ),
}

PROJECT_CODE_PREFIXES = (
    "firmware/src/",
    "firmware/include/",
    "firmware/app/src/",
    "firmware/app/include/",
)
SCRIPT_PREFIX = "firmware/scripts/"

OPERATION_TOPICS: dict[str, tuple[str, ...]] = {
    "architecture": ("documentation", "routing", "testing"),
    "build": ("testing",),
    "commit": ("tooling",),
    "deploy": ("deployment",),
    "documentation": ("documentation",),
    "edit": (),
    "flash": ("deployment",),
    "hil": ("deployment", "dwm3000", "testing"),
    "native_sim": ("persistence", "testing"),
    "preflight": ("tooling",),
    "probe": ("deployment",),
    "recover": ("survey",),
    "refactor": ("routing", "testing"),
    "reset": ("persistence", "testing"),
    "rtt": ("deployment", "dwm3000"),
    "sanitizer": ("testing",),
    "stress": ("routing", "testing"),
    "survey": ("survey",),
    "test": ("testing",),
}

UNRESOLVED_TERMS = (
    " still ",
    " remain ",
    " remains ",
    " unqualified",
    " do not ",
    " must ",
    " cannot ",
)
FIXED_HISTORY_PREFIXES = (
    "- fixed ",
    "- corrected ",
    "- resolved ",
    "- removed ",
    "- replaced ",
    "- restored ",
)
WORD = re.compile(r"[a-z0-9_]+")
ISSUE_STATES = frozenset({"active", "unqualified", "environment", "mitigated"})
SEVERITIES = frozenset({"critical", "high", "medium", "low"})
REQUIRED_RULE_IDS = frozenset(
    {
        "RULE-ROBUSTNESS-001",
        "RULE-DEPLOYMENT-001",
        "RULE-CONCURRENCY-001",
    }
)
ENVIRONMENT_MAX_AGE_DAYS = 120


def infer_topics(paths: Iterable[str]) -> set[str]:
    inferred: set[str] = set()
    for raw_path in paths:
        path = raw_path.lower().replace("\\", "/")
        tokens = set(WORD.findall(path))
        path_topics: set[str] = set()
        for name, topic in TOPICS.items():
            if any(term in path or term in tokens for term in topic.path_terms):
                path_topics.add(name)
        if not path_topics and path.startswith(PROJECT_CODE_PREFIXES):
            # Generic protocol/app files still participate in the connected
            # firmware contract even when their names lack a domain keyword.
            path_topics.update(("routing", "testing"))
        if not path_topics and path.startswith(SCRIPT_PREFIX):
            path_topics.add("tooling")
        inferred.update(path_topics)
    return inferred


def infer_operation_topics(operations: Iterable[str]) -> set[str]:
    return {
        topic
        for operation in operations
        for topic in OPERATION_TOPICS.get(operation, ())
    }


def _changed_paths(repo_root: Path) -> list[str]:
    paths: set[str] = set()
    for command in (
        ["git", "diff", "--name-only", "HEAD"],
        ["git", "ls-files", "--others", "--exclude-standard"],
    ):
        result = subprocess.run(
            command,
            cwd=repo_root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if result.returncode == 0:
            paths.update(line for line in result.stdout.splitlines() if line)
    return sorted(paths)


def load_current_issues(
    path: Path = DEFAULT_CURRENT_ISSUES,
    ledger_text: str | None = None,
) -> tuple[list[dict[str, object]], list[dict[str, object]], list[str]]:
    errors: list[str] = []
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [], [], [f"cannot read current-issue index {path}: {exc}"]
    if not isinstance(value, dict) or value.get("schema") != 1:
        return [], [], [f"{path} must contain a schema-1 object"]
    guard = value.get("ledger_guard")
    history_policy = value.get("history_policy")
    raw_rules = value.get("global_rules")
    raw_issues = value.get("issues")
    if not isinstance(raw_rules, list) or not isinstance(raw_issues, list):
        return [], [], [f"{path} global_rules and issues must be lists"]
    if not isinstance(history_policy, str) or not history_policy:
        errors.append(f"{path} history_policy must be non-empty")
    if not isinstance(guard, dict):
        errors.append(f"{path} ledger_guard must be an object")
    elif ledger_text is not None:
        prefix_lines = guard.get("prefix_lines")
        expected_hash = guard.get("sha256")
        ledger_lines = ledger_text.splitlines(keepends=True)
        if (
            not isinstance(prefix_lines, int)
            or isinstance(prefix_lines, bool)
            or prefix_lines <= 0
            or prefix_lines > len(ledger_lines)
        ):
            errors.append(f"{path} ledger_guard.prefix_lines is invalid")
        elif (
            not isinstance(expected_hash, str)
            or not re.fullmatch(r"[0-9a-f]{64}", expected_hash)
        ):
            errors.append(f"{path} ledger_guard.sha256 is invalid")
        else:
            guarded = "".join(ledger_lines[:prefix_lines]).encode("utf-8")
            actual_hash = hashlib.sha256(guarded).hexdigest()
            if actual_hash != expected_hash:
                errors.append(
                    "append-only issue-ledger prefix changed: "
                    f"expected {expected_hash}, got {actual_hash}"
                )

    ids: set[str] = set()
    validated_rules: list[dict[str, object]] = []
    validated_issues: list[dict[str, object]] = []
    for group_name, raw_group, destination in (
        ("global_rules", raw_rules, validated_rules),
        ("issues", raw_issues, validated_issues),
    ):
        for index, raw in enumerate(raw_group):
            field = f"{group_name}[{index}]"
            if not isinstance(raw, dict):
                errors.append(f"{field} must be an object")
                continue
            issue_id = raw.get("id")
            severity = raw.get("severity")
            current_rule = raw.get("current_rule")
            if not isinstance(issue_id, str) or not issue_id:
                errors.append(f"{field}.id must be non-empty")
                continue
            if issue_id in ids:
                errors.append(f"duplicate current-issue id {issue_id}")
            ids.add(issue_id)
            if not isinstance(severity, str) or severity not in SEVERITIES:
                errors.append(f"{field}.severity is invalid: {severity!r}")
            if not isinstance(current_rule, str) or not current_rule:
                errors.append(f"{field}.current_rule must be non-empty")
            if group_name == "issues":
                state = raw.get("state")
                topics = raw.get("topics")
                fingerprint = raw.get("source_fingerprint")
                operations = raw.get("operations", [])
                path_globs = raw.get("path_globs", [])
                if not isinstance(state, str) or state not in ISSUE_STATES:
                    errors.append(f"{field}.state is invalid: {state!r}")
                if (
                    not isinstance(topics, list)
                    or not topics
                    or any(
                        not isinstance(topic, str) or topic not in TOPICS
                        for topic in topics
                    )
                ):
                    errors.append(f"{field}.topics must name known topics")
                if not isinstance(fingerprint, str) or not fingerprint:
                    errors.append(f"{field}.source_fingerprint must be non-empty")
                elif ledger_text is not None:
                    matches = ledger_text.count(fingerprint)
                    if matches != 1:
                        errors.append(
                            f"{issue_id} fingerprint matches {matches} ledger entries"
                        )
                if (
                    not isinstance(operations, list)
                    or any(
                        not isinstance(operation, str)
                        or operation not in OPERATION_TOPICS
                        for operation in operations
                    )
                ):
                    errors.append(f"{field}.operations must name known operations")
                if (
                    not isinstance(path_globs, list)
                    or any(
                        not isinstance(pattern, str) or not pattern
                        for pattern in path_globs
                    )
                ):
                    errors.append(f"{field}.path_globs must contain non-empty strings")
                match_topics = raw.get("match_topics", True)
                if not isinstance(match_topics, bool):
                    errors.append(f"{field}.match_topics must be boolean")
                if state == "environment":
                    applies_to = raw.get("applies_to")
                    verified_at = raw.get("verified_at")
                    recheck = raw.get("recheck")
                    if not isinstance(applies_to, str) or not applies_to:
                        errors.append(
                            f"{field}.applies_to must describe the environment"
                        )
                    if not isinstance(recheck, str) or not recheck:
                        errors.append(f"{field}.recheck must be non-empty")
                    if not isinstance(verified_at, str):
                        errors.append(
                            f"{field}.verified_at must be an ISO date string"
                        )
                    else:
                        try:
                            verified_date = date.fromisoformat(verified_at)
                        except ValueError:
                            errors.append(
                                f"{field}.verified_at must be an ISO date string"
                            )
                        else:
                            age = (date.today() - verified_date).days
                            if age < 0 or age > ENVIRONMENT_MAX_AGE_DAYS:
                                errors.append(
                                    f"{field} environment verification is stale "
                                    f"({verified_at}, age {age} days)"
                                )
            destination.append(raw)
    rule_ids = {str(rule.get("id")) for rule in validated_rules}
    missing_rules = sorted(REQUIRED_RULE_IDS - rule_ids)
    if missing_rules:
        errors.append(f"missing required global rules: {', '.join(missing_rules)}")

    if isinstance(guard, dict) and ledger_text is not None:
        prefix_lines = guard.get("prefix_lines")
        if isinstance(prefix_lines, int) and not isinstance(prefix_lines, bool):
            fingerprints = {
                str(issue.get("source_fingerprint"))
                for issue in validated_issues
                if isinstance(issue.get("source_fingerprint"), str)
            }
            extra_lines = ledger_text.splitlines()[prefix_lines:]
            for offset, line in enumerate(extra_lines, start=prefix_lines + 1):
                lowered = f" {line.lower()} "
                looks_active = any(term in lowered for term in UNRESOLVED_TERMS)
                if (
                    line.strip().startswith("- ")
                    and looks_active
                    and history_entry_state(line) != "fixed"
                    and not any(fingerprint in line for fingerprint in fingerprints)
                ):
                    errors.append(
                        f"new unresolved ledger entry at line {offset} "
                        "needs a curated overlay fingerprint"
                    )
    return validated_rules, validated_issues, errors


def select_current_issues(
    issues: Iterable[dict[str, object]],
    topic_names: set[str],
    paths: Iterable[str],
    operations: set[str],
    include_all: bool,
) -> list[dict[str, object]]:
    selected: list[dict[str, object]] = []
    normalized_paths = [path.replace("\\", "/") for path in paths]
    for issue in issues:
        issue_topics = set(issue.get("topics", []))
        issue_operations = set(issue.get("operations", []))
        globs = issue.get("path_globs", [])
        path_match = any(
            fnmatch.fnmatch(path, pattern)
            for path in normalized_paths
            for pattern in globs
        )
        topic_match = bool(topic_names & issue_topics) and bool(
            issue.get("match_topics", True)
        )
        if include_all or topic_match or bool(operations & issue_operations) or path_match:
            selected.append(issue)
    severity_rank = {"critical": 0, "high": 1, "medium": 2, "low": 3}
    selected.sort(
        key=lambda issue: (
            severity_rank.get(str(issue.get("severity")), 99),
            str(issue.get("id")),
        )
    )
    return selected


def current_issue_match_reasons(
    issue: dict[str, object],
    topic_names: set[str],
    paths: Iterable[str],
    operations: set[str],
    include_all: bool,
) -> list[str]:
    if include_all:
        return ["all"]
    reasons: list[str] = []
    issue_topics = set(issue.get("topics", []))
    if issue.get("match_topics", True):
        matched_topics = sorted(topic_names & issue_topics)
        if matched_topics:
            reasons.append(f"topic={','.join(matched_topics)}")
    matched_operations = sorted(operations & set(issue.get("operations", [])))
    if matched_operations:
        reasons.append(f"operation={','.join(matched_operations)}")
    globs = issue.get("path_globs", [])
    matched_paths = sorted(
        {
            path.replace("\\", "/")
            for path in paths
            if any(
                fnmatch.fnmatch(path.replace("\\", "/"), pattern)
                for pattern in globs
            )
        }
    )
    if matched_paths:
        reasons.append(f"path={','.join(matched_paths)}")
    return reasons


def history_entry_state(line: str) -> str:
    lowered = line.strip().lower()
    if lowered.startswith(FIXED_HISTORY_PREFIXES) or " superseded " in lowered:
        return "fixed"
    return "historical"


def select_entries(
    ledger_text: str,
    topic_names: Iterable[str],
    paths: Iterable[str] = (),
    operations: Iterable[str] = (),
    include_fixed: bool = False,
) -> list[tuple[int, int, str, tuple[str, ...]]]:
    selected: list[tuple[int, int, str, tuple[str, ...]]] = []
    names = tuple(sorted(set(topic_names)))
    scope_terms = set(operation.lower() for operation in operations)
    for path in paths:
        normalized = path.lower().replace("\\", "/")
        scope_terms.update(
            token
            for token in WORD.findall(Path(normalized).stem)
            if len(token) >= 4 and token not in {"firmware", "script", "test"}
        )
    for line_number, raw_line in enumerate(ledger_text.splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if history_entry_state(line) == "fixed" and not include_fixed:
            continue
        lowered = f" {line.lower()} "
        matched_topics: list[str] = []
        score = 0
        for name in names:
            matches = sum(keyword in lowered for keyword in TOPICS[name].keywords)
            if matches:
                matched_topics.append(name)
                score += matches
        if not matched_topics:
            continue
        score += 2 * sum(term in lowered for term in UNRESOLVED_TERMS)
        score += 6 * sum(term in lowered for term in scope_terms)
        if not line.startswith("- "):
            score += 1
        selected.append((score, line_number, line, tuple(matched_topics)))
    selected.sort(key=lambda item: (item[0], item[1]), reverse=True)
    return selected


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Print issue-ledger entries relevant to planned paths and topics."
    )
    parser.add_argument(
        "--paths",
        nargs="+",
        default=[],
        help="Repository paths expected to change.",
    )
    parser.add_argument(
        "--topics",
        nargs="+",
        default=[],
        help=f"Explicit topics: {', '.join(sorted(TOPICS))}",
    )
    parser.add_argument(
        "--operations",
        nargs="+",
        default=[],
        help="Planned operations such as flash, rtt, reset, stress, refactor, or commit.",
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="Use every topic for architecture-wide work.",
    )
    parser.add_argument(
        "--max-entries",
        type=int,
        default=40,
        help="Maximum matches to print; zero prints all (default: 40).",
    )
    parser.add_argument(
        "--include-fixed-history",
        action="store_true",
        help="Also print fixed historical entries; they remain non-authoritative.",
    )
    parser.add_argument(
        "--ledger",
        type=Path,
        default=DEFAULT_LEDGER,
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--current-issues",
        type=Path,
        default=DEFAULT_CURRENT_ISSUES,
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--list-topics",
        action="store_true",
        help="List topic routing and exit.",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    if args.list_topics:
        for name, topic in sorted(TOPICS.items()):
            print(f"{name}: paths={', '.join(topic.path_terms)}")
        return 0
    if args.max_entries < 0:
        parser.error("--max-entries must be zero or positive")

    unknown = sorted(set(args.topics) - set(TOPICS))
    if unknown:
        parser.error(f"unknown topics: {', '.join(unknown)}")

    paths = list(args.paths)
    if not paths and not args.topics and not args.operations and not args.all:
        paths = _changed_paths(REPO_ROOT)
    operations = {operation.lower() for operation in args.operations}
    unknown_operations = sorted(operations - set(OPERATION_TOPICS))
    if unknown_operations:
        parser.error(
            "unknown operations: "
            f"{', '.join(unknown_operations)}; known operations: "
            f"{', '.join(sorted(OPERATION_TOPICS))}"
        )
    topics = (
        set(TOPICS)
        if args.all
        else infer_topics(paths)
        | infer_operation_topics(operations)
        | set(args.topics)
    )
    if not topics:
        parser.error(
            "no topic inferred from this scope; pass an explicit --topics value "
            "or --all"
        )

    try:
        ledger_text = args.ledger.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        print(f"cannot read issue ledger {args.ledger}: {exc}", file=sys.stderr)
        return 1

    global_rules, current_issues, index_errors = load_current_issues(
        args.current_issues,
        ledger_text,
    )
    if index_errors:
        print("current-issue index is invalid:", file=sys.stderr)
        for error in index_errors:
            print(f"  {error}", file=sys.stderr)
        return 1

    print("global critical rules:")
    for rule in global_rules:
        print(f"  {rule['id']} [{rule['severity']}] {rule['current_rule']}")
    print(f"preflight topics: {', '.join(sorted(topics))}")
    if operations:
        print(f"preflight operations: {', '.join(sorted(operations))}")
    references = sorted(
        {
            reference
            for name in topics
            for reference in TOPICS[name].references
        }
    )
    print("required references:")
    for reference in references:
        print(f"  {reference}")

    selected_current = select_current_issues(
        current_issues,
        topics,
        paths,
        operations,
        args.all,
    )
    print(f"current issue and environment rules: {len(selected_current)}")
    for issue in selected_current:
        reasons = current_issue_match_reasons(
            issue,
            topics,
            paths,
            operations,
            args.all,
        )
        print(
            f"  {issue['id']} [{issue['state']}/{issue['severity']}] "
            f"({'; '.join(reasons)}) {issue['current_rule']}"
        )

    entries = select_entries(
        ledger_text,
        topics,
        paths,
        operations,
        args.include_fixed_history,
    )
    limit = len(entries) if args.max_entries == 0 else args.max_entries
    shown = entries[:limit]
    history_kind = (
        "historical matches including fixed entries"
        if args.include_fixed_history
        else "non-fixed historical context matches"
    )
    print(f"{history_kind} (non-authoritative): {len(entries)}")
    for _, line_number, line, matched_topics in shown:
        print(
            f"  {args.ledger.name}:{line_number} "
            f"[{history_entry_state(line)}/{','.join(matched_topics)}] {line}"
        )
    if len(shown) < len(entries):
        print(
            f"  ... {len(entries) - len(shown)} more; rerun with --max-entries 0 "
            "or narrower topics"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
