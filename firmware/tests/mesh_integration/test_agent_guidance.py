#!/usr/bin/env python3
"""Adversarial tests for the executable repository guidance contract."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPT_DIR))

import check_agent_guidance  # noqa: E402


class AgentGuidanceTests(unittest.TestCase):
    def _complete_guidance(self) -> str:
        return "\n".join(
            (
                *check_agent_guidance.REQUIRED_GUIDANCE_SECTIONS,
                *check_agent_guidance.REQUIRED_GUIDANCE_TEXT,
                check_agent_guidance.check_architecture_boundaries
                .IMMUTABLE_BASELINE_COMMIT,
            )
        )

    def test_accepts_complete_contract(self) -> None:
        guidance = self._complete_guidance()
        self.assertEqual(
            check_agent_guidance.guidance_contract_errors(guidance),
            [],
        )

    def test_rejects_each_missing_baseline_retention_rule(self) -> None:
        complete = self._complete_guidance()
        for required in check_agent_guidance.REQUIRED_GUIDANCE_TEXT:
            with self.subTest(required=required):
                guidance = complete.replace(required, "")
                errors = check_agent_guidance.guidance_contract_errors(guidance)
                self.assertTrue(
                    any(required in error for error in errors),
                    errors,
                )

    def test_rejects_guidance_with_stale_checker_pin(self) -> None:
        stale = self._complete_guidance()
        replacement = "f" * 40
        errors = check_agent_guidance.guidance_contract_errors(
            stale,
            expected_baseline=replacement,
        )
        self.assertTrue(
            any(replacement in error and "AGENTS.md" in error for error in errors),
            errors,
        )

    def test_rejects_reset_plan_with_stale_checker_pin(self) -> None:
        current = (
            check_agent_guidance.check_architecture_boundaries
            .IMMUTABLE_BASELINE_COMMIT
        )
        replacement = "e" * 40
        errors = check_agent_guidance.baseline_pin_errors(
            {
                check_agent_guidance.BASELINE_GUIDANCE_PATH: (
                    f"Immutable policy object: {current}"
                )
            },
            expected_baseline=replacement,
        )
        self.assertTrue(
            any(
                replacement in error
                and check_agent_guidance.BASELINE_GUIDANCE_PATH in error
                for error in errors
            ),
            errors,
        )


if __name__ == "__main__":
    unittest.main()
