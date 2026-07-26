#!/usr/bin/env python3
"""Validate the curated current-issue overlay against the append-only ledger."""

from __future__ import annotations

import subprocess
import sys

import agent_preflight


REQUIRED_GUIDANCE_SECTIONS = (
    "## Mandatory preflight",
    "## Protected scope and code placement",
    "## Firmware roles and hardware safety",
    "## Verification contract",
    "## Mesh and state-ownership changes",
    "## Architecture and file health",
    "## Documentation and delivery discipline",
)
REQUIRED_REFERENCES = (
    "Documentation/CURRENT.json",
    "Documentation/Mesh Connected Routing Contract.md",
    "Documentation/Development and Deployment Guide.md",
    "Documentation/Architecture Reset Plan.md",
    "firmware/architecture_boundaries.json",
)


def _tracked_project_sources() -> tuple[list[str], list[str]]:
    result = subprocess.run(
        ["git", "ls-files", "--", "firmware"],
        cwd=agent_preflight.REPO_ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        return [], [f"git ls-files failed: {result.stderr.strip()}"]
    sources = [
        path
        for path in result.stdout.splitlines()
        if path.endswith((".c", ".h"))
        and path.startswith(agent_preflight.PROJECT_CODE_PREFIXES)
    ]
    errors = [
        f"preflight cannot infer a topic for tracked project source {path}"
        for path in sources
        if not agent_preflight.infer_topics([path])
    ]
    return sources, errors


def main() -> int:
    errors: list[str] = []
    try:
        ledger = agent_preflight.DEFAULT_LEDGER.read_text(
            encoding="utf-8",
            errors="replace",
        )
    except OSError as exc:
        print(f"cannot read issue ledger: {exc}", file=sys.stderr)
        return 1
    _, _, index_errors = agent_preflight.load_current_issues(
        agent_preflight.DEFAULT_CURRENT_ISSUES,
        ledger,
    )
    errors.extend(index_errors)
    guidance_path = agent_preflight.REPO_ROOT / "AGENTS.md"
    try:
        guidance = guidance_path.read_text(encoding="utf-8")
    except OSError as exc:
        errors.append(f"cannot read {guidance_path}: {exc}")
        guidance = ""
    for section in REQUIRED_GUIDANCE_SECTIONS:
        if section not in guidance:
            errors.append(f"AGENTS.md is missing required section {section!r}")
    for rule_id in sorted(agent_preflight.REQUIRED_RULE_IDS):
        if rule_id not in guidance:
            errors.append(f"AGENTS.md is missing direct rule id {rule_id}")
    for relative in REQUIRED_REFERENCES:
        if relative not in guidance:
            errors.append(f"AGENTS.md does not point to {relative}")
        if not (agent_preflight.REPO_ROOT / relative).is_file():
            errors.append(f"required guidance reference is missing: {relative}")
    sources, coverage_errors = _tracked_project_sources()
    errors.extend(coverage_errors)
    if not sources:
        errors.append("no tracked project C/H sources were found for routing coverage")
    if not agent_preflight.infer_operation_topics({"flash"}):
        errors.append("flash operation does not infer deployment guidance")
    if not agent_preflight.infer_operation_topics({"refactor"}):
        errors.append("refactor operation does not infer architecture guidance")
    if errors:
        print("agent guidance check failed:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("agent guidance check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
