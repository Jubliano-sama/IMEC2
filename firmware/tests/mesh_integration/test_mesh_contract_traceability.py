#!/usr/bin/env python3
"""Validate the stable mesh-contract taxonomy and its traceability manifest."""

from __future__ import annotations

import hashlib
import re
import subprocess
import unittest
from pathlib import Path

import yaml


REPO_ROOT = Path(__file__).resolve().parents[3]
CONTRACT_PATH = REPO_ROOT / "Documentation/Mesh Connected Routing Contract.md"
MANIFEST_PATH = REPO_ROOT / "Documentation/mesh_contract_traceability.yaml"

EXPECTED_CATEGORIES = {
    "GOV": ("governance", 2),
    "HOST": ("host", 5),
    "ROLE": ("roles", 3),
    "RAD": ("radio", 9),
    "PWR": ("power", 4),
    "ROUTE": ("route", 9),
    "EVT": ("event", 3),
    "COMM": ("communication", 10),
    "ARCH": ("architecture", 1),
    "ACK": ("ack", 7),
    "GCTL": ("gateway_control", 6),
    "ASN": ("assignment", 4),
    "SUR": ("survey", 13),
    "FAIL": ("failure", 5),
    "END": ("teardown", 2),
    "SEC": ("security", 1),
    "OPT": ("optimization", 1),
    "VER": ("verification", 2),
}

HEADING_RE = re.compile(r"^### ([A-Z]+-\d{2}) — .+$", re.MULTILINE)
ANY_ID_RE = re.compile(r"\b[A-Z]+-\d{2}\b")
LINE_RANGE_RE = re.compile(r"^([1-9]\d*)-([1-9]\d*)$")
def expected_ids() -> list[str]:
    return [
        f"{prefix}-{index:02d}"
        for prefix, (_, count) in EXPECTED_CATEGORIES.items()
        for index in range(1, count + 1)
    ]


def checked_repo_path(value: object, label: str) -> Path:
    if not isinstance(value, str) or not value:
        raise AssertionError(f"{label} must be a nonempty repository-relative path")
    candidate = Path(value)
    if candidate.is_absolute() or ".." in candidate.parts:
        raise AssertionError(f"{label} escapes the repository: {value!r}")
    resolved = REPO_ROOT / candidate
    if not resolved.is_file():
        raise AssertionError(f"{label} does not name a file: {value!r}")
    return resolved


class MeshContractTraceabilityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.contract_text = CONTRACT_PATH.read_text(encoding="utf-8")
        cls.manifest_text = MANIFEST_PATH.read_text(encoding="utf-8")
        cls.manifest = yaml.safe_load(cls.manifest_text)

    def test_contract_has_exact_immutable_taxonomy(self) -> None:
        headings = HEADING_RE.findall(self.contract_text)
        self.assertEqual(headings, expected_ids())
        self.assertEqual(len(headings), len(set(headings)), "duplicate contract ID")

        all_tokens = set(ANY_ID_RE.findall(self.contract_text))
        self.assertEqual(all_tokens, set(headings), "contract contains an orphan ID")

    def test_manifest_categories_cannot_self_authorize_drift(self) -> None:
        categories = self.manifest.get("categories")
        self.assertIsInstance(categories, dict)
        actual = {
            prefix: (entry.get("name"), entry.get("count"))
            for prefix, entry in categories.items()
        }
        self.assertEqual(actual, EXPECTED_CATEGORIES)

    def test_manifest_has_one_entry_per_contract_id(self) -> None:
        requirements = self.manifest.get("requirements")
        self.assertIsInstance(requirements, list)
        ids = [entry.get("id") for entry in requirements]
        self.assertEqual(ids, expected_ids())
        self.assertEqual(len(ids), len(set(ids)), "duplicate manifest ID")
        self.assertEqual(set(ids), set(HEADING_RE.findall(self.contract_text)))
        self.assertEqual(set(ANY_ID_RE.findall(self.manifest_text)), set(ids))
        self.assertEqual(
            checked_repo_path(self.manifest.get("contract"), "contract"),
            CONTRACT_PATH,
        )

    def test_legacy_snapshot_and_ranges_are_resolvable(self) -> None:
        legacy = self.manifest.get("legacy_source")
        self.assertIsInstance(legacy, dict)
        legacy_path = legacy.get("path")
        checked_repo_path(legacy_path, "legacy_source.path")

        blob = legacy.get("git_blob")
        snapshot_commit = legacy.get("snapshot_commit")
        self.assertRegex(blob or "", r"^[0-9a-f]{40}$")
        self.assertRegex(snapshot_commit or "", r"^[0-9a-f]{40}$")

        committed_blob = subprocess.run(
            ["git", "rev-parse", f"{snapshot_commit}:{legacy_path}"],
            cwd=REPO_ROOT,
            check=True,
            text=True,
            capture_output=True,
        ).stdout.strip()
        self.assertEqual(committed_blob, blob)

        blob_bytes = subprocess.run(
            ["git", "cat-file", "blob", blob],
            cwd=REPO_ROOT,
            check=True,
            capture_output=True,
        ).stdout
        self.assertEqual(hashlib.sha256(blob_bytes).hexdigest(), legacy.get("sha256"))
        legacy_line_count = len(blob_bytes.decode("utf-8").splitlines())
        self.assertEqual(legacy_line_count, legacy.get("line_count"))

        for entry in self.manifest["requirements"]:
            ranges = entry.get("legacy_lines")
            self.assertIsInstance(ranges, list, entry["id"])
            self.assertTrue(ranges, entry["id"])
            for line_range in ranges:
                match = LINE_RANGE_RE.fullmatch(line_range or "")
                self.assertIsNotNone(match, f"{entry['id']}: invalid {line_range!r}")
                start, end = map(int, match.groups())
                self.assertLessEqual(start, end, entry["id"])
                self.assertLessEqual(end, legacy_line_count, entry["id"])

    def test_roles_categories_paths_and_evidence_are_explicit(self) -> None:
        expected_by_id = {
            stable_id: EXPECTED_CATEGORIES[stable_id.split("-", 1)[0]][0]
            for stable_id in expected_ids()
        }
        for entry in self.manifest["requirements"]:
            stable_id = entry["id"]
            self.assertEqual(entry.get("category"), expected_by_id[stable_id])

            roles = entry.get("roles")
            self.assertIsInstance(roles, list, stable_id)
            self.assertTrue(roles, stable_id)
            self.assertTrue(all(isinstance(role, str) and role for role in roles))

            for field in ("authoritative", "owners"):
                paths = entry.get(field)
                self.assertIsInstance(paths, list, f"{stable_id}.{field}")
                self.assertTrue(paths, f"{stable_id}.{field}")
                for index, path in enumerate(paths):
                    checked_repo_path(path, f"{stable_id}.{field}[{index}]")

            verification = entry.get("verification")
            self.assertIsInstance(verification, dict, stable_id)
            status = verification.get("status")
            self.assertIn(status, {"partial", "deferred", "verified"}, stable_id)
            evidence = verification.get("evidence")
            self.assertIsInstance(evidence, list, stable_id)
            if status in {"partial", "verified"}:
                self.assertTrue(evidence, f"{stable_id} needs concrete evidence")
            for index, path in enumerate(evidence):
                checked_repo_path(path, f"{stable_id}.evidence[{index}]")

            gap = verification.get("gap")
            if status in {"partial", "deferred"}:
                self.assertIsInstance(gap, str, stable_id)
                self.assertTrue(gap.strip(), f"{stable_id} needs an explicit gap")
            else:
                self.assertFalse(gap, f"{stable_id}: verified cannot retain a gap")

    def test_every_legacy_body_line_is_mapped(self) -> None:
        covered_lines: set[int] = set()
        for entry in self.manifest["requirements"]:
            for line_range in entry["legacy_lines"]:
                match = LINE_RANGE_RE.fullmatch(line_range)
                assert match is not None
                start, end = map(int, match.groups())
                covered_lines.update(range(start, end + 1))

        blob = self.manifest["legacy_source"]["git_blob"]
        legacy_lines = subprocess.run(
            ["git", "cat-file", "blob", blob],
            cwd=REPO_ROOT,
            check=True,
            text=True,
            capture_output=True,
        ).stdout.splitlines()
        uncovered = [
            f"{line_number}: {line.strip()}"
            for line_number, line in enumerate(legacy_lines, start=1)
            if line.strip()
            and not line.lstrip().startswith("#")
            and line_number not in covered_lines
        ]
        self.assertFalse(
            uncovered,
            "legacy non-heading body lines lack traceability:\n"
            + "\n".join(uncovered[:20]),
        )


if __name__ == "__main__":
    unittest.main()
