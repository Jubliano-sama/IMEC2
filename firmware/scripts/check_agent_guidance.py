#!/usr/bin/env python3
"""Validate the curated current-issue overlay against the append-only ledger."""

from __future__ import annotations

import sys

import agent_preflight


def main() -> int:
    try:
        ledger = agent_preflight.DEFAULT_LEDGER.read_text(
            encoding="utf-8",
            errors="replace",
        )
    except OSError as exc:
        print(f"cannot read issue ledger: {exc}", file=sys.stderr)
        return 1
    _, _, errors = agent_preflight.load_current_issues(
        agent_preflight.DEFAULT_CURRENT_ISSUES,
        ledger,
    )
    if errors:
        print("agent guidance check failed:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("agent guidance check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
