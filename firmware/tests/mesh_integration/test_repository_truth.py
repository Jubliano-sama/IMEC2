#!/usr/bin/env python3
"""Tests for the repository source-of-truth gate."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPT_DIR))

import check_repository_truth  # noqa: E402


class RepositoryTruthTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)
        (self.root / "Documentation").mkdir()
        (self.root / "firmware").mkdir()
        (self.root / "docs/wiki").mkdir(parents=True)
        self.architecture = Path("Documentation/UWB+BLE Architecture 1.2.1.md")
        self.protocol = Path("Documentation/UWB+BLE Protocols and Strategies 2.4.md")
        (self.root / self.architecture).write_text("Version: 1.2.1\n", encoding="utf-8")
        (self.root / self.protocol).write_text("Version: 2.4\n", encoding="utf-8")
        (self.root / "Documentation/Contract.md").write_text("contract\n", encoding="utf-8")
        (self.root / "Documentation/Development.md").write_text("guide\n", encoding="utf-8")
        (self.root / "Documentation/Reset.md").write_text("plan\n", encoding="utf-8")
        self.surfaces = (
            Path("README.md"),
            Path("CODEMAP.md"),
            Path("Documentation/INDEX.md"),
            Path("firmware/README.md"),
            Path("docs/wiki/toc.yaml"),
        )
        for surface in self.surfaces:
            (self.root / surface).write_text(
                f"{self.architecture.name}\n{self.protocol.name}\n",
                encoding="utf-8",
            )
        manifest = {
            "schema": 1,
            "documents": {
                "mesh_contract": "Documentation/Contract.md",
                "architecture": str(self.architecture),
                "protocol": str(self.protocol),
                "development": "Documentation/Development.md",
                "architecture_reset": "Documentation/Reset.md",
            },
            "versioned_series": {
                "architecture": {
                    "prefix": "UWB+BLE Architecture ",
                    "document": "architecture",
                },
                "protocol": {
                    "prefix": "UWB+BLE Protocols and Strategies ",
                    "document": "protocol",
                },
            },
            "reference_surfaces": {
                str(surface): ["architecture", "protocol"]
                for surface in self.surfaces
            },
        }
        (self.root / "Documentation/CURRENT.json").write_text(
            json.dumps(manifest),
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def test_accepts_consistent_current_documents(self) -> None:
        self.assertEqual(check_repository_truth.check_repository(self.root), [])

    def test_rejects_manifest_when_newer_version_exists(self) -> None:
        newer = self.root / "Documentation/UWB+BLE Architecture 1.2.2.md"
        newer.write_text("Version: 1.2.2\n", encoding="utf-8")
        issues = check_repository_truth.check_repository(self.root)
        self.assertTrue(any("latest architecture" in issue for issue in issues), issues)

    def test_rejects_stale_reference_surface(self) -> None:
        old = self.root / "Documentation/UWB+BLE Architecture 1.2.md"
        old.write_text("Version: 1.2\n", encoding="utf-8")
        (self.root / "README.md").write_text(
            f"{old.name}\n{self.protocol.name}\n",
            encoding="utf-8",
        )
        issues = check_repository_truth.check_repository(self.root)
        self.assertTrue(any("README.md does not reference" in issue for issue in issues), issues)
        self.assertTrue(any("README.md still references" in issue for issue in issues), issues)

    def test_rejects_filename_header_mismatch(self) -> None:
        (self.root / self.protocol).write_text("Version: 2.3\n", encoding="utf-8")
        issues = check_repository_truth.check_repository(self.root)
        self.assertTrue(any("declares Version 2.3" in issue for issue in issues), issues)


if __name__ == "__main__":
    unittest.main()
