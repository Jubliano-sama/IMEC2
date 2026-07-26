#!/usr/bin/env python3
"""Tests for issue-ledger preflight routing."""

from __future__ import annotations

import sys
import json
import tempfile
import unittest
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPT_DIR))

import agent_preflight  # noqa: E402


class AgentPreflightTests(unittest.TestCase):
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
                "- Fixed a survey pair deadline.",
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

    def test_current_issue_index_requires_unique_fingerprint(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "current.json"
            path.write_text(
                json.dumps(
                    {
                        "schema": 1,
                        "global_rules": [
                            {
                                "id": "RULE-1",
                                "severity": "critical",
                                "current_rule": "Fail closed.",
                            }
                        ],
                        "issues": [
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
                    }
                ),
                encoding="utf-8",
            )
            _, issues, errors = agent_preflight.load_current_issues(
                path,
                "- unique fingerprint\n",
            )
            self.assertEqual(errors, [])
            self.assertEqual(len(issues), 1)

    def test_current_issue_index_rejects_ambiguous_fingerprint(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "current.json"
            path.write_text(
                json.dumps(
                    {
                        "schema": 1,
                        "global_rules": [],
                        "issues": [
                            {
                                "id": "KI-1",
                                "state": "active",
                                "severity": "high",
                                "topics": ["survey"],
                                "current_rule": "Keep the owner live.",
                                "source_fingerprint": "duplicate",
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            _, _, errors = agent_preflight.load_current_issues(
                path,
                "duplicate\nduplicate\n",
            )
            self.assertTrue(any("matches 2" in error for error in errors), errors)

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
