#!/usr/bin/env python3
"""Tests for issue-ledger preflight routing."""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPT_DIR))

import agent_preflight  # noqa: E402


class AgentPreflightTests(unittest.TestCase):
    @staticmethod
    def _valid_index(
        ledger: str,
        issues: list[dict[str, object]],
    ) -> dict[str, object]:
        return {
            "schema": 1,
            "ledger_guard": {
                "prefix_lines": len(ledger.splitlines()),
                "sha256": hashlib.sha256(ledger.encode("utf-8")).hexdigest(),
            },
            "history_policy": "History is non-authoritative.",
            "global_rules": [
                {
                    "id": rule_id,
                    "severity": "critical",
                    "current_rule": "Fail closed.",
                }
                for rule_id in sorted(agent_preflight.REQUIRED_RULE_IDS)
            ],
            "issues": issues,
        }

    def test_infers_overlapping_topics_from_paths(self) -> None:
        topics = agent_preflight.infer_topics(
            [
                "firmware/app/src/app_gateway_survey_round.c",
                "firmware/tests/mesh_integration/test_gateway_ble.c",
            ]
        )
        self.assertIn("survey", topics)
        self.assertIn("routing", topics)
        self.assertIn("ble", topics)
        self.assertIn("testing", topics)

    def test_selects_only_relevant_ledger_entries(self) -> None:
        ledger = "\n".join(
            [
                "# Known issues",
                "- Survey pair reset recovery remains unqualified.",
                "- pyOCD needs a TTY for RTT.",
                "- A wiki link was stale.",
            ]
        )
        entries = agent_preflight.select_entries(ledger, ["survey"])
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0][1], 2)

    def test_prioritizes_unresolved_entries(self) -> None:
        ledger = "\n".join(
            [
                "- Fixed the survey deadline.",
                "- Survey reset recovery remains unqualified.",
            ]
        )
        entries = agent_preflight.select_entries(ledger, ["survey"])
        self.assertEqual(entries[0][1], 2)

    def test_rejects_no_implicit_catch_all_topic(self) -> None:
        self.assertEqual(agent_preflight.infer_topics(["LICENSE"]), set())

    def test_generic_project_code_gets_safe_contract_topics(self) -> None:
        for path in (
            "firmware/src/protocol.c",
            "firmware/include/report.h",
            "firmware/app/src/app_clicker.c",
            "firmware/app/include/app_watchdog.h",
        ):
            with self.subTest(path=path):
                self.assertEqual(
                    agent_preflight.infer_topics([path]),
                    {"routing", "testing"},
                )

    def test_operations_infer_required_topics(self) -> None:
        self.assertEqual(
            agent_preflight.infer_operation_topics({"flash"}),
            {"deployment"},
        )
        self.assertEqual(
            agent_preflight.infer_operation_topics({"refactor"}),
            {"routing", "testing"},
        )
        self.assertEqual(agent_preflight.infer_operation_topics({"edit"}), set())

    def test_fixed_history_is_hidden_by_default(self) -> None:
        ledger = "\n".join(
            [
                "- Fixed a survey pair deadline.",
                "- Survey reset recovery remains unqualified.",
            ]
        )
        entries = agent_preflight.select_entries(ledger, ["survey"])
        self.assertEqual([entry[1] for entry in entries], [2])
        entries = agent_preflight.select_entries(
            ledger,
            ["survey"],
            include_fixed=True,
        )
        self.assertEqual({entry[1] for entry in entries}, {1, 2})

    def test_current_issue_index_requires_unique_fingerprint(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "current.json"
            ledger = "- unique fingerprint\n"
            path.write_text(
                json.dumps(
                    self._valid_index(
                        ledger,
                        [
                            {
                                "id": "KI-1",
                                "state": "active",
                                "severity": "high",
                                "topics": ["survey"],
                                "path_globs": ["firmware/*"],
                                "operations": ["survey"],
                                "current_rule": "Keep the owner live.",
                                "source_fingerprint": "unique fingerprint",
                            }
                        ],
                    )
                ),
                encoding="utf-8",
            )
            _, issues, errors = agent_preflight.load_current_issues(
                path,
                ledger,
            )
            self.assertEqual(errors, [])
            self.assertEqual(len(issues), 1)

    def test_current_issue_index_rejects_ambiguous_fingerprint(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "current.json"
            ledger = "duplicate\nduplicate\n"
            path.write_text(
                json.dumps(
                    self._valid_index(
                        ledger,
                        [
                            {
                                "id": "KI-1",
                                "state": "active",
                                "severity": "high",
                                "topics": ["survey"],
                                "current_rule": "Keep the owner live.",
                                "source_fingerprint": "duplicate",
                            }
                        ],
                    )
                ),
                encoding="utf-8",
            )
            _, _, errors = agent_preflight.load_current_issues(
                path,
                ledger,
            )
            self.assertTrue(any("matches 2" in error for error in errors), errors)

    def test_current_issue_index_rejects_malformed_lists_and_states(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "current.json"
            ledger = "- malformed fingerprint\n"
            index = self._valid_index(
                ledger,
                [
                    {
                        "id": "KI-1",
                        "state": ["active"],
                        "severity": {"high": True},
                        "topics": [["survey"]],
                        "path_globs": 7,
                        "operations": [7],
                        "current_rule": "Keep the owner live.",
                        "source_fingerprint": "malformed fingerprint",
                    }
                ],
            )
            path.write_text(json.dumps(index), encoding="utf-8")
            _, _, errors = agent_preflight.load_current_issues(path, ledger)
            self.assertTrue(any(".state is invalid" in error for error in errors))
            self.assertTrue(any(".severity is invalid" in error for error in errors))
            self.assertTrue(any(".topics must" in error for error in errors))
            self.assertTrue(any(".path_globs must" in error for error in errors))
            self.assertTrue(any(".operations must" in error for error in errors))

    def test_current_issue_index_rejects_stale_environment_claim(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "current.json"
            ledger = "- environment fingerprint\n"
            index = self._valid_index(
                ledger,
                [
                    {
                        "id": "KI-ENV",
                        "state": "environment",
                        "severity": "medium",
                        "topics": ["testing"],
                        "path_globs": [],
                        "operations": ["test"],
                        "match_topics": False,
                        "applies_to": "Test host",
                        "verified_at": "2000-01-01",
                        "recheck": "Run the fixture.",
                        "current_rule": "Use the qualified path.",
                        "source_fingerprint": "environment fingerprint",
                    }
                ],
            )
            path.write_text(json.dumps(index), encoding="utf-8")
            _, _, errors = agent_preflight.load_current_issues(path, ledger)
            self.assertTrue(any("verification is stale" in error for error in errors))

    def test_current_issue_index_rejects_ledger_prefix_rewrite(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "current.json"
            ledger = "- immutable history\n"
            path.write_text(
                json.dumps(self._valid_index(ledger, [])),
                encoding="utf-8",
            )
            _, _, errors = agent_preflight.load_current_issues(
                path,
                "- rewritten history\n",
            )
            self.assertTrue(
                any("append-only issue-ledger prefix changed" in error for error in errors)
            )

    def test_new_unresolved_history_requires_curated_overlay(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "current.json"
            guarded = "- immutable history\n"
            path.write_text(
                json.dumps(self._valid_index(guarded, [])),
                encoding="utf-8",
            )
            _, _, errors = agent_preflight.load_current_issues(
                path,
                guarded + "- Reset recovery remains unqualified.\n",
            )
            self.assertTrue(
                any("needs a curated overlay fingerprint" in error for error in errors)
            )
            _, _, fixed_errors = agent_preflight.load_current_issues(
                path,
                guarded + "- Fixed reset recovery that must preserve identity.\n",
            )
            self.assertEqual(fixed_errors, [])

    def test_selects_current_issue_by_operation(self) -> None:
        issue = {
            "id": "KI-1",
            "severity": "high",
            "topics": ["deployment"],
            "path_globs": [],
            "operations": ["rtt"],
        }
        selected = agent_preflight.select_current_issues(
            [issue],
            set(),
            [],
            {"rtt"},
            False,
        )
        self.assertEqual(selected, [issue])


if __name__ == "__main__":
    unittest.main()
