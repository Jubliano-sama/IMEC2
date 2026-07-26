#!/usr/bin/env python3
"""Tests for the architectural growth gate."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPT_DIR))

import check_architecture_boundaries  # noqa: E402


class ArchitectureBoundaryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)
        source = self.root / "firmware/src"
        source.mkdir(parents=True)
        (source / "legacy.inc").write_text("one\ntwo\n", encoding="utf-8")
        (source / "legacy.c").write_text(
            '#include "legacy.inc"\nbody\n',
            encoding="utf-8",
        )
        self.manifest = {
            "schema": 1,
            "source_roots": ["firmware/src"],
            "default_c_max_lines": 3,
            "frozen_oversize_sources": {},
            "approved_include_fragments": {
                "firmware/src/legacy.inc": 2,
            },
            "composed_translation_units": {
                "firmware/src/legacy.c": {
                    "max_composed_lines": 4,
                    "includes": ["legacy.inc"],
                },
            },
        }
        self.manifest_path = self.root / "boundaries.json"
        self._write_manifest()

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def _write_manifest(self) -> None:
        self.manifest_path.write_text(json.dumps(self.manifest), encoding="utf-8")

    def _check(self) -> list[str]:
        return check_architecture_boundaries.check_repository(
            self.root,
            Path("boundaries.json"),
        )

    def test_accepts_frozen_composition(self) -> None:
        self.assertEqual(self._check(), [])

    def test_rejects_growth_in_existing_fragment(self) -> None:
        (self.root / "firmware/src/legacy.inc").write_text(
            "one\ntwo\nthree\n",
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(any("frozen 2-line ceiling" in issue for issue in issues), issues)

    def test_rejects_new_include_fragment(self) -> None:
        (self.root / "firmware/src/more.inc").write_text("new\n", encoding="utf-8")
        issues = self._check()
        self.assertTrue(any("new include fragment" in issue for issue in issues), issues)

    def test_rejects_new_oversize_c_file(self) -> None:
        (self.root / "firmware/src/new.c").write_text("1\n2\n3\n4\n", encoding="utf-8")
        issues = self._check()
        self.assertTrue(any("new source ceiling" in issue for issue in issues), issues)

    def test_rejects_composition_change(self) -> None:
        (self.root / "firmware/src/legacy.c").write_text("body\n", encoding="utf-8")
        issues = self._check()
        self.assertTrue(
            any("include-fragment composition changed" in issue for issue in issues),
            issues,
        )


if __name__ == "__main__":
    unittest.main()
