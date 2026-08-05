#!/usr/bin/env python3
"""Reject repository-supported production mesh deployment bypasses.

This is a repository and CI gate, not host access control.  A local host owner
can change or bypass this script and invoke a probe directly.  Its scope is to
make release/deployment eligibility mechanically fail for repository-owned
automation, scripts, workflows, and supported documentation that bypasses the
verified flasher.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SUPPORTED_PATHS = (
    Path("AGENTS.md"),
    Path("README.md"),
    Path("CODEMAP.md"),
    Path("firmware/README.md"),
    Path("Documentation"),
    Path("firmware/scripts"),
    Path(".github/workflows"),
)
TEXT_SUFFIXES = frozenset({".md", ".py", ".sh", ".yml", ".yaml"})
DIRECT_FLASH = re.compile(
    r"^\s*(?:[$#]\s*)?(?:(?:\.?/?[A-Za-z0-9_.-]+/)+)?(?:west\s+flash|pyocd\s+(?:flash|erase))\b",
    re.IGNORECASE,
)
ALLOWED_DIRECT_PRESETS = re.compile(
    r"\b(?:mesh[-_]transmitter(?:[-_]forcedhop)?|mesh[-_]anchor[-_]forcedhop|ml[-_](?:clicker|anchor[-_][1-8])|"
    r"firmware[-_](?:clicker|anchor|gateway)|power-clicker-sleep|legacy-or-bench-preset)\b",
    re.IGNORECASE,
)


def _supported_files(repo_root: Path) -> list[Path]:
    files: list[Path] = []
    for relative in SUPPORTED_PATHS:
        path = repo_root / relative
        if path.is_file():
            files.append(path)
        elif path.is_dir():
            files.extend(
                candidate for candidate in path.rglob("*")
                if candidate.is_file() and candidate.suffix in TEXT_SUFFIXES
            )
    return sorted(files)


def _command_lines(text: str) -> list[str]:
    # A shell continuation is still one deploy command for policy purposes.
    normalized = re.sub(r"\\\s*\n", " ", text)
    return normalized.splitlines()


def _verify_flasher_source(repo_root: Path) -> list[str]:
    path = repo_root / "firmware/scripts/flash_verified_mesh.py"
    if not path.is_file():
        return ["verified deployment flasher is missing"]
    text = path.read_text(encoding="utf-8", errors="replace")
    required = (
        "FLASH_FREQUENCY_HZ = 4_000_000",
        "WEST_EXECUTABLE = REPO_ROOT / \".venv\" / \"bin\" / \"west\"",
        "--frequency",
        "--stage-only",
        "--hardware-manifest",
        "awaiting_qualification",
        "_code_sectors_match",
        "_record_consumed_capture",
        "verify_flash(",
    )
    return [f"verified flasher lacks required fixed-policy token {token!r}" for token in required if token not in text]


def check_repository(repo_root: Path = REPO_ROOT) -> list[str]:
    issues = _verify_flasher_source(repo_root)
    for path in _supported_files(repo_root):
        relative = path.relative_to(repo_root)
        for line_number, line in enumerate(_command_lines(path.read_text(encoding="utf-8", errors="replace")), start=1):
            if not DIRECT_FLASH.search(line):
                continue
            if ALLOWED_DIRECT_PRESETS.search(line):
                continue
            issues.append(
                f"{relative}:{line_number}: direct deployment command is not an explicit bench/legacy exception"
            )
    return issues


def main() -> int:
    issues = check_repository()
    if issues:
        print("mesh deployment policy failed:", file=sys.stderr)
        for issue in issues:
            print(f"  {issue}", file=sys.stderr)
        return 1
    print("mesh deployment policy passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
