#!/usr/bin/env python3
"""Tests for the repository source-of-truth gate."""

from __future__ import annotations

import copy
import json
import posixpath
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from urllib.parse import quote


SCRIPT_DIR = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPT_DIR))

import check_repository_truth  # noqa: E402


class RepositoryTruthTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)
        self.snapshot_ref = "0" * 40
        (self.root / "Documentation").mkdir()
        (self.root / "firmware").mkdir()
        (self.root / "docs/wiki").mkdir(parents=True)
        self.architecture = Path("Documentation/UWB+BLE Architecture 1.2.1.md")
        self.protocol = Path("Documentation/UWB+BLE Protocols and Strategies 2.4.md")
        self.contract = Path("Documentation/Contract.md")
        self.runtime_flow = Path("Documentation/Runtime.md")
        self.development = Path("Documentation/Development.md")
        self.architecture_reset = Path("Documentation/Reset.md")
        (self.root / self.architecture).write_text(
            "\n".join(
                (
                    "Version: 1.2.1",
                    "[[UWB+BLE Protocols and Strategies 2.4]]",
                    "[[Contract]]",
                    "[[Runtime]]",
                    "",
                )
            ),
            encoding="utf-8",
        )
        (self.root / self.protocol).write_text(
            "\n".join(
                (
                    "Version: 2.4",
                    "[architecture](UWB%2BBLE%20Architecture%201.2.1.md)",
                    "[contract](Contract.md)",
                    "[runtime](Runtime.md)",
                    "",
                )
            ),
            encoding="utf-8",
        )
        (self.root / self.contract).write_text("contract\n", encoding="utf-8")
        (self.root / self.runtime_flow).write_text("runtime\n", encoding="utf-8")
        (self.root / self.development).write_text("guide\n", encoding="utf-8")
        (self.root / self.architecture_reset).write_text("plan\n", encoding="utf-8")
        self.surfaces = (
            Path("README.md"),
            Path("CODEMAP.md"),
            Path("Documentation/INDEX.md"),
            Path("firmware/README.md"),
            Path("docs/wiki/toc.yaml"),
        )
        for surface in self.surfaces:
            self._write_surface(surface, self.architecture, self.protocol)
        self.manifest = {
            "schema": 1,
            "documents": {
                "contract": str(self.contract),
                "architecture": str(self.architecture),
                "protocol": str(self.protocol),
                "runtime_flow": str(self.runtime_flow),
                "development": str(self.development),
                "architecture_reset": str(self.architecture_reset),
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
            "retired_series": {
                "state_machines": {
                    "last_version": "0.2.4",
                    "deleted_in": "9925dcd",
                    "replacement": "runtime_flow",
                    "forbidden_current_references": [
                        "Firmware State Machines 0.2.4",
                        "Firmware State Machines and Status Report",
                    ],
                }
            },
            "required_cross_references": {
                "architecture": ["protocol", "contract", "runtime_flow"],
                "protocol": ["architecture", "contract", "runtime_flow"],
            },
            "reference_surfaces": {
                str(surface): ["architecture", "protocol"]
                for surface in self.surfaces
            },
        }
        self._write_manifest(self.manifest)
        subprocess.run(["git", "init", "-q"], cwd=self.root, check=True)
        subprocess.run(
            ["git", "config", "user.name", "Repository Truth Test"],
            cwd=self.root,
            check=True,
        )
        subprocess.run(
            ["git", "config", "user.email", "truth-test@example.invalid"],
            cwd=self.root,
            check=True,
        )
        subprocess.run(["git", "add", "-A"], cwd=self.root, check=True)
        subprocess.run(
            ["git", "commit", "-q", "-m", "Create source snapshot"],
            cwd=self.root,
            check=True,
        )
        self.snapshot_ref = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=self.root,
            check=True,
            text=True,
            stdout=subprocess.PIPE,
        ).stdout.strip()
        self._write_surface(
            Path("docs/wiki/toc.yaml"),
            self.architecture,
            self.protocol,
        )

    def _write_manifest(self, manifest: dict[str, object]) -> None:
        (self.root / "Documentation/CURRENT.json").write_text(
            json.dumps(manifest),
            encoding="utf-8",
        )

    def _surface_target(self, surface: Path, target: Path) -> str:
        base = str(surface.parent)
        relative = posixpath.relpath(str(target), "." if base == "." else base)
        return quote(relative)

    def _write_surface(self, surface: Path, *targets: Path) -> None:
        if surface.suffix == ".yaml":
            body = (
                "project:\n"
                f'  ref_commit_hash: "{self.snapshot_ref}"\n'
                '  updated_at: "2026-07-26"\n'
                "source_files:\n"
            ) + "".join(
                f'  - "{target}"\n' for target in targets
            )
        else:
            body = "".join(
                f"[{target.name}]({self._surface_target(surface, target)})\n"
                for target in targets
            )
        (self.root / surface).write_text(body, encoding="utf-8")

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
        self._write_surface(
            Path("README.md"),
            old.relative_to(self.root),
            self.protocol,
        )
        issues = check_repository_truth.check_repository(self.root)
        self.assertTrue(any("README.md does not link" in issue for issue in issues), issues)
        self.assertTrue(any("README.md still references" in issue for issue in issues), issues)

    def test_rejects_filename_header_mismatch(self) -> None:
        (self.root / self.protocol).write_text("Version: 2.3\n", encoding="utf-8")
        issues = check_repository_truth.check_repository(self.root)
        self.assertTrue(any("declares Version 2.3" in issue for issue in issues), issues)

    def test_rejects_each_omitted_schema_section(self) -> None:
        for field in (
            "documents",
            "versioned_series",
            "retired_series",
            "required_cross_references",
            "reference_surfaces",
        ):
            with self.subTest(field=field):
                manifest = copy.deepcopy(self.manifest)
                manifest.pop(field)
                self._write_manifest(manifest)
                issues = check_repository_truth.check_repository(self.root)
                self.assertTrue(
                    any(f"{field} section is required" in issue for issue in issues),
                    issues,
                )

    def test_rejects_missing_canonical_document_key(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        manifest["documents"].pop("runtime_flow")
        self._write_manifest(manifest)
        issues = check_repository_truth.check_repository(self.root)
        self.assertTrue(
            any(
                "documents must use the fixed schema keys" in issue
                and "runtime_flow" in issue
                for issue in issues
            ),
            issues,
        )

    def test_rejects_missing_required_cross_reference(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        manifest["required_cross_references"]["architecture"].remove("contract")
        self._write_manifest(manifest)
        issues = check_repository_truth.check_repository(self.root)
        self.assertTrue(
            any(
                "required_cross_references.architecture must contain exactly" in issue
                for issue in issues
            ),
            issues,
        )

    def test_rejects_missing_retired_series_metadata(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        manifest["retired_series"]["state_machines"].pop("deleted_in")
        self._write_manifest(manifest)
        issues = check_repository_truth.check_repository(self.root)
        self.assertTrue(
            any(
                "retired_series.state_machines.deleted_in must be non-empty" in issue
                for issue in issues
            ),
            issues,
        )

    def test_rejects_empty_reference_surfaces(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        manifest["reference_surfaces"] = {}
        self._write_manifest(manifest)
        issues = check_repository_truth.check_repository(self.root)
        self.assertTrue(
            any("reference_surfaces must be a non-empty object" in issue for issue in issues),
            issues,
        )

    def test_rejects_omitted_required_reference_surface(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        manifest["reference_surfaces"].pop("firmware/README.md")
        self._write_manifest(manifest)
        issues = check_repository_truth.check_repository(self.root)
        self.assertTrue(
            any(
                "reference_surfaces must use the fixed schema keys" in issue
                and "firmware/README.md" in issue
                for issue in issues
            ),
            issues,
        )

    def test_rejects_filename_label_with_broken_markdown_target(self) -> None:
        (self.root / "README.md").write_text(
            "\n".join(
                (
                    f"[{self.architecture.name}](Documentation/Missing.md)",
                    f"[{self.protocol.name}](Documentation/Also%20Missing.md)",
                    "",
                )
            ),
            encoding="utf-8",
        )
        issues = check_repository_truth.check_repository(self.root)
        self.assertTrue(
            any(
                f"README.md does not link to canonical current architecture: "
                f"{self.architecture}" in issue
                for issue in issues
            ),
            issues,
        )
        self.assertTrue(
            any(
                f"README.md does not link to canonical current protocol: "
                f"{self.protocol}" in issue
                for issue in issues
            ),
            issues,
        )

    def test_rejects_broken_wiki_source_target(self) -> None:
        (self.root / "docs/wiki/toc.yaml").write_text(
            "\n".join(
                (
                    "project:",
                    f'  ref_commit_hash: "{self.snapshot_ref}"',
                    '  updated_at: "2026-07-26"',
                    "source_files:",
                    '  - "Documentation/UWB+BLE Architecture missing.md"',
                    f'  - "{self.protocol}"',
                    "",
                )
            ),
            encoding="utf-8",
        )
        issues = check_repository_truth.check_repository(self.root)
        self.assertTrue(
            any(
                f"docs/wiki/toc.yaml does not link to canonical current architecture: "
                f"{self.architecture}" in issue
                for issue in issues
            ),
            issues,
        )

    def test_rejects_source_missing_from_pinned_wiki_snapshot(self) -> None:
        new_source = self.root / "Documentation/New Runtime Source.md"
        new_source.write_text("new\n", encoding="utf-8")
        (self.root / "docs/wiki/toc.yaml").write_text(
            "\n".join(
                (
                    "project:",
                    f'  ref_commit_hash: "{self.snapshot_ref}"',
                    '  updated_at: "2026-07-26"',
                    "source_files:",
                    f'  - "{self.architecture}"',
                    f'  - "{self.protocol}"',
                    '  - "Documentation/New Runtime Source.md"',
                    "",
                )
            ),
            encoding="utf-8",
        )
        issues = check_repository_truth.check_repository(self.root)
        self.assertTrue(
            any(
                "New Runtime Source.md" in issue
                and "does not exist at pinned ref" in issue
                for issue in issues
            ),
            issues,
        )

    def test_rejects_cross_reference_label_with_broken_target(self) -> None:
        (self.root / self.architecture).write_text(
            "\n".join(
                (
                    "Version: 1.2.1",
                    f"[[Missing|{self.protocol.name}]]",
                    "[[Contract]]",
                    "[[Runtime]]",
                    "",
                )
            ),
            encoding="utf-8",
        )
        issues = check_repository_truth.check_repository(self.root)
        self.assertTrue(
            any(
                f"{self.architecture} does not link to canonical protocol: "
                f"{self.protocol}" in issue
                for issue in issues
            ),
            issues,
        )


if __name__ == "__main__":
    unittest.main()
