#!/usr/bin/env python3
"""Tests for the repository source-of-truth gate."""

from __future__ import annotations

import copy
import json
import posixpath
import re
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
        (self.root / "docs/wiki/_context").mkdir(parents=True)
        (self.root / "docs/wiki/_reports").mkdir(parents=True)
        self.architecture = Path("Documentation/UWB+BLE Architecture 1.2.1.md")
        self.protocol = Path("Documentation/UWB+BLE Protocols and Strategies 2.4.md")
        self.contract = Path("Documentation/Contract.md")
        self.runtime_flow = Path("Documentation/Runtime.md")
        self.development = Path("Documentation/Development.md")
        self.architecture_reset = Path("Documentation/Reset.md")
        self.wiki_page = Path("docs/wiki/README.md")
        self.wiki_page_id = "test-start"
        self.wiki_section_id = "test-start-overview"
        self.repo_base_url = check_repository_truth.CANONICAL_WIKI_REPO_BASE_URL
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
        self._write_wiki_bundle(self.snapshot_ref)
        subprocess.run(["git", "add", "-A"], cwd=self.root, check=True)
        subprocess.run(
            ["git", "commit", "-q", "-m", "Generate bound wiki evidence"],
            cwd=self.root,
            check=True,
        )
        self.wiki_commit = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=self.root,
            check=True,
            text=True,
            stdout=subprocess.PIPE,
        ).stdout.strip()

    def _citation(
        self,
        source: Path,
        start: int,
        end: int,
        *,
        ref: str | None = None,
    ) -> str:
        commit = self.snapshot_ref if ref is None else ref
        encoded = quote(source.as_posix(), safe="/")
        return (
            f"[{source.name}:{start}-{end}]"
            f"({self.repo_base_url}/{commit}/{encoded}#L{start}-L{end})"
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

    def _write_wiki_bundle(self, ref: str) -> None:
        toc = "\n".join(
            (
                "project:",
                '  name: "IMEC2 test"',
                f'  repo_base_url: "{self.repo_base_url}"',
                f'  ref_commit_hash: "{ref}"',
                '  updated_at: "2026-07-26"',
                "pages:",
                f"  - id: {self.wiki_page_id}",
                '    title: "Start"',
                '    filename: "README.md"',
                "    source_files:",
                f'      - "{self.architecture}"',
                f'      - "{self.protocol}"',
                "    sections:",
                f"      - id: {self.wiki_section_id}",
                '        title: "Overview"',
                "        autogen: true",
                "        diagrams_needed: true",
                '        diagram_types: ["flowchart"]',
                "",
            )
        )
        (self.root / "docs/wiki/toc.yaml").write_text(toc, encoding="utf-8")
        page = "\n".join(
            (
                f"<!-- PAGE_ID: {self.wiki_page_id} -->",
                "",
                "<details>",
                "<summary>Historical source inventory</summary>",
                "",
                (
                    f"[old:{999}-{1000}]"
                    f"({self.repo_base_url}/{'1' * 40}/old.c#L999-L1000)"
                ),
                "",
                "</details>",
                "",
                f"<!-- BEGIN:AUTOGEN {self.wiki_section_id} -->",
                "## Overview",
                "",
                f"Current evidence: {self._citation(self.architecture, 1, 4, ref=ref)}.",
                "",
                "```mermaid",
                "flowchart LR",
                '    A["Source"] --> B["Wiki"]',
                "```",
                f"<!-- END:AUTOGEN {self.wiki_section_id} -->",
                "",
            )
        )
        (self.root / self.wiki_page).write_text(page, encoding="utf-8")
        self._write_wiki_artifacts(ref)

    def _wiki_state(self) -> str:
        toc_path = Path("docs/wiki/toc.yaml")
        pages, issues = check_repository_truth._parse_wiki_toc(
            toc_path,
            (self.root / toc_path).read_text(encoding="utf-8"),
        )
        self.assertEqual(issues, [])
        return check_repository_truth._wiki_state_sha256(
            self.root,
            toc_path,
            pages,
        )

    def _write_wiki_artifacts(self, ref: str) -> None:
        wiki_state = self._wiki_state()
        binding = {
            "ref_commit_hash": ref,
            "wiki_state_sha256": wiki_state,
        }
        (self.root / "docs/wiki/_context/context_pack.json").write_text(
            json.dumps(
                {
                    "metadata": {
                        **binding,
                        "repo_path": str(self.root),
                    },
                    "structure": "test",
                }
            ),
            encoding="utf-8",
        )
        (self.root / "docs/wiki/_context/sync_context.json").write_text(
            json.dumps(
                {
                    "new_pages": [],
                    "pages_to_update": {},
                    "unchanged_pages": [self.wiki_page_id],
                    "toc_project": {
                        "ref_commit_hash": ref,
                        "updated_at": "2026-07-26",
                    },
                    "metadata": {
                        **binding,
                        "total_toc_pages": 1,
                        "total_existing_docs": 1,
                        "total_new_pages": 0,
                        "total_pages_to_update": 0,
                        "total_unchanged_pages": 1,
                        "total_new_sections": 0,
                        "total_deleted_sections": 0,
                        "total_empty_sections_detected": 0,
                    },
                }
            ),
            encoding="utf-8",
        )
        (self.root / "docs/wiki/_context/update_context.json").write_text(
            json.dumps(
                {
                    "base_commit": ref,
                    "target_commit": ref,
                    "toc_updated_at": "2026-07-26",
                    "sections_to_update": [],
                    "metadata": {
                        **binding,
                        "total_changed_files": 0,
                    },
                }
            ),
            encoding="utf-8",
        )
        (self.root / "docs/wiki/_reports/structure_validation.json").write_text(
            json.dumps(
                {
                    "metadata": binding,
                    "summary": {
                        "pages_validated": 1,
                        "pages_missing": 0,
                        "sections_validated": 1,
                        "sections_missing": 0,
                        "total_errors": 0,
                        "total_warnings": 0,
                        "is_valid": True,
                    },
                    "errors": [],
                    "warnings": [],
                }
            ),
            encoding="utf-8",
        )
        (self.root / "docs/wiki/_reports/mermaid_invalid.json").write_text(
            json.dumps(
                {
                    "metadata": binding,
                    "invalid_blocks": [],
                    "total_invalid": 0,
                    "total_scanned": 1,
                    "files_affected": 0,
                }
            ),
            encoding="utf-8",
        )
        (self.root / "docs/wiki/_reports/SUMMARY.md").write_text(
            "\n".join(
                (
                    "# Wiki Documentation Summary",
                    "",
                    f"Commit: `{ref}`",
                    f"Wiki state SHA-256: `{wiki_state}`",
                    "",
                    "## Generation Status",
                    "",
                    "**Overall Status**: ✅ Complete",
                    "",
                )
            ),
            encoding="utf-8",
        )

    def _repin_wiki(self, ref: str) -> None:
        toc_path = self.root / "docs/wiki/toc.yaml"
        toc_text = toc_path.read_text(encoding="utf-8")
        toc_text = re.sub(
            r'(ref_commit_hash:\s*")[0-9a-f]{40}(")',
            rf"\g<1>{ref}\g<2>",
            toc_text,
        )
        toc_path.write_text(toc_text, encoding="utf-8")
        page_path = self.root / self.wiki_page
        page_text = page_path.read_text(encoding="utf-8")
        page_text = page_text.replace(
            f"{self.repo_base_url}/{self.snapshot_ref}/",
            f"{self.repo_base_url}/{ref}/",
        )
        page_path.write_text(page_text, encoding="utf-8")
        self._write_wiki_artifacts(ref)

    def _refresh_wiki_bindings(self, ref: str | None = None) -> None:
        self._write_wiki_artifacts(self.snapshot_ref if ref is None else ref)

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

    def test_accepts_later_commit_when_mapped_sources_are_unchanged(self) -> None:
        (self.root / "unrelated.txt").write_text("later output\n", encoding="utf-8")
        subprocess.run(
            ["git", "add", "unrelated.txt"],
            cwd=self.root,
            check=True,
        )
        subprocess.run(
            ["git", "commit", "-q", "-m", "Add unrelated output"],
            cwd=self.root,
            check=True,
        )
        self.assertEqual(check_repository_truth.check_repository(self.root), [])

    def test_accepts_report_only_commit_without_changing_wiki_digest(self) -> None:
        summary_path = self.root / "docs/wiki/_reports/SUMMARY.md"
        summary_path.write_text(
            summary_path.read_text(encoding="utf-8") + "Report note.\n",
            encoding="utf-8",
        )
        subprocess.run(
            ["git", "add", "docs/wiki/_reports/SUMMARY.md"],
            cwd=self.root,
            check=True,
        )
        subprocess.run(
            ["git", "commit", "-q", "-m", "Update wiki report"],
            cwd=self.root,
            check=True,
        )
        self.assertEqual(check_repository_truth.check_repository(self.root), [])

    def test_rejects_wiki_pin_that_is_not_an_ancestor(self) -> None:
        tree = subprocess.run(
            ["git", "rev-parse", "HEAD^{tree}"],
            cwd=self.root,
            check=True,
            text=True,
            stdout=subprocess.PIPE,
        ).stdout.strip()
        unrelated = subprocess.run(
            ["git", "commit-tree", tree],
            cwd=self.root,
            check=True,
            input="Unrelated source snapshot\n",
            text=True,
            stdout=subprocess.PIPE,
        ).stdout.strip()
        self._repin_wiki(unrelated)
        issues = check_repository_truth.check_repository(self.root)
        self.assertTrue(
            any("is not an ancestor of HEAD" in issue for issue in issues),
            issues,
        )

    def test_rejects_mapped_source_drift_after_wiki_generation(self) -> None:
        architecture_path = self.root / self.architecture
        architecture_path.write_text(
            architecture_path.read_text(encoding="utf-8") + "new source truth\n",
            encoding="utf-8",
        )
        subprocess.run(
            ["git", "add", str(self.architecture)],
            cwd=self.root,
            check=True,
        )
        subprocess.run(
            ["git", "commit", "-q", "-m", "Change mapped source"],
            cwd=self.root,
            check=True,
        )
        issues = check_repository_truth.check_repository(self.root)
        self.assertTrue(
            any(
                "mapped wiki source changed since pinned ref" in issue
                and str(self.architecture) in issue
                for issue in issues
            ),
            issues,
        )

    def test_rejects_wrong_autogen_citation_commit(self) -> None:
        page_path = self.root / self.wiki_page
        page_text = page_path.read_text(encoding="utf-8").replace(
            f"{self.repo_base_url}/{self.snapshot_ref}/",
            f"{self.repo_base_url}/{self.wiki_commit}/",
        )
        page_path.write_text(page_text, encoding="utf-8")
        self._refresh_wiki_bindings()
        issues = check_repository_truth.check_repository(self.root)
        self.assertTrue(
            any(
                "AUTOGEN section" in issue
                and f"pins {self.wiki_commit}, expected {self.snapshot_ref}" in issue
                for issue in issues
            ),
            issues,
        )

    def test_rejects_foreign_wiki_repository_base(self) -> None:
        foreign_base = "https://github.com/attacker/imec2/blob"
        for relative in (Path("docs/wiki/toc.yaml"), self.wiki_page):
            path = self.root / relative
            path.write_text(
                path.read_text(encoding="utf-8").replace(
                    self.repo_base_url,
                    foreign_base,
                ),
                encoding="utf-8",
            )
        self._refresh_wiki_bindings()
        issues = check_repository_truth.check_repository(self.root)
        self.assertTrue(
            any(
                "project.repo_base_url must be canonical" in issue
                and check_repository_truth.CANONICAL_WIKI_REPO_BASE_URL in issue
                for issue in issues
            ),
            issues,
        )

    def test_rejects_autogen_citation_range_beyond_pinned_file(self) -> None:
        page_path = self.root / self.wiki_page
        page_text = page_path.read_text(encoding="utf-8")
        page_text = page_text.replace(
            f"{self.architecture.name}:1-4",
            f"{self.architecture.name}:1-400",
        ).replace("#L1-L4", "#L1-L400")
        page_path.write_text(page_text, encoding="utf-8")
        self._refresh_wiki_bindings()
        issues = check_repository_truth.check_repository(self.root)
        self.assertTrue(
            any(
                "AUTOGEN section" in issue
                and "citation range 1-400 exceeds" in issue
                for issue in issues
            ),
            issues,
        )

    def test_allows_historical_inventory_citations_outside_autogen(self) -> None:
        page_path = self.root / self.wiki_page
        page_text = page_path.read_text(encoding="utf-8")
        page_text = page_text.replace(
            f"{self.repo_base_url}/{'1' * 40}/old.c#L999-L1000",
            f"{self.repo_base_url}/{'2' * 40}/missing.c#L9999-L10000",
        )
        page_path.write_text(page_text, encoding="utf-8")
        self._refresh_wiki_bindings()
        self.assertEqual(check_repository_truth.check_repository(self.root), [])

    def test_rejects_tracked_top_level_page_absent_from_toc(self) -> None:
        obsolete = self.root / "docs/wiki/obsolete.md"
        obsolete.write_text("# Obsolete generated page\n", encoding="utf-8")
        subprocess.run(
            ["git", "add", "docs/wiki/obsolete.md"],
            cwd=self.root,
            check=True,
        )
        subprocess.run(
            ["git", "commit", "-q", "-m", "Retain obsolete wiki page"],
            cwd=self.root,
            check=True,
        )
        issues = check_repository_truth.check_repository(self.root)
        self.assertTrue(
            any(
                "tracked top-level wiki page is absent from" in issue
                and "docs/wiki/obsolete.md" in issue
                for issue in issues
            ),
            issues,
        )

    def test_rejects_each_stale_report_and_context_binding(self) -> None:
        artifacts = (
            *check_repository_truth.WIKI_CONTEXT_ARTIFACTS,
            Path("docs/wiki/_reports/structure_validation.json"),
            Path("docs/wiki/_reports/mermaid_invalid.json"),
        )
        for relative in artifacts:
            with self.subTest(artifact=str(relative)):
                path = self.root / relative
                original = path.read_text(encoding="utf-8")
                artifact = json.loads(original)
                artifact["metadata"]["ref_commit_hash"] = "0" * 40
                artifact["metadata"]["wiki_state_sha256"] = "0" * 64
                path.write_text(json.dumps(artifact), encoding="utf-8")
                issues = check_repository_truth.check_repository(self.root)
                self.assertTrue(
                    any(
                        str(relative) in issue
                        and "metadata.ref_commit_hash" in issue
                        for issue in issues
                    ),
                    issues,
                )
                self.assertTrue(
                    any(
                        str(relative) in issue
                        and "metadata.wiki_state_sha256" in issue
                        for issue in issues
                    ),
                    issues,
                )
                path.write_text(original, encoding="utf-8")

    def test_rejects_stale_sync_and_update_source_pins(self) -> None:
        sync_path = self.root / "docs/wiki/_context/sync_context.json"
        sync = json.loads(sync_path.read_text(encoding="utf-8"))
        sync["toc_project"]["ref_commit_hash"] = "0" * 40
        sync_path.write_text(json.dumps(sync), encoding="utf-8")
        update_path = self.root / "docs/wiki/_context/update_context.json"
        update = json.loads(update_path.read_text(encoding="utf-8"))
        update["target_commit"] = "0" * 40
        update_path.write_text(json.dumps(update), encoding="utf-8")
        issues = check_repository_truth.check_repository(self.root)
        self.assertTrue(
            any(
                "sync_context.json toc_project.ref_commit_hash" in issue
                for issue in issues
            ),
            issues,
        )
        self.assertTrue(
            any(
                "update_context.json target_commit" in issue
                for issue in issues
            ),
            issues,
        )

    def test_rejects_stale_summary_wiki_digest(self) -> None:
        summary_path = self.root / "docs/wiki/_reports/SUMMARY.md"
        summary_text = summary_path.read_text(encoding="utf-8")
        summary_text = re.sub(
            r"Wiki state SHA-256: `[0-9a-f]{64}`",
            f"Wiki state SHA-256: `{'0' * 64}`",
            summary_text,
        )
        summary_path.write_text(summary_text, encoding="utf-8")
        issues = check_repository_truth.check_repository(self.root)
        self.assertTrue(
            any(
                "SUMMARY.md Wiki state SHA-256" in issue
                for issue in issues
            ),
            issues,
        )

    def test_rejects_stale_wiki_digest_after_page_edit(self) -> None:
        page_path = self.root / self.wiki_page
        page_path.write_text(
            page_path.read_text(encoding="utf-8") + "\nnew generated prose\n",
            encoding="utf-8",
        )
        issues = check_repository_truth.check_repository(self.root)
        for relative in (
            *check_repository_truth.WIKI_CONTEXT_ARTIFACTS,
            Path("docs/wiki/_reports/SUMMARY.md"),
            Path("docs/wiki/_reports/structure_validation.json"),
            Path("docs/wiki/_reports/mermaid_invalid.json"),
        ):
            with self.subTest(artifact=str(relative)):
                self.assertTrue(
                    any(str(relative) in issue for issue in issues),
                    issues,
                )

    def test_rejects_stale_structure_and_mermaid_results(self) -> None:
        structure_path = self.root / "docs/wiki/_reports/structure_validation.json"
        structure = json.loads(structure_path.read_text(encoding="utf-8"))
        structure["summary"]["sections_validated"] = 0
        structure_path.write_text(json.dumps(structure), encoding="utf-8")
        mermaid_path = self.root / "docs/wiki/_reports/mermaid_invalid.json"
        mermaid = json.loads(mermaid_path.read_text(encoding="utf-8"))
        mermaid["total_scanned"] = 0
        mermaid_path.write_text(json.dumps(mermaid), encoding="utf-8")
        issues = check_repository_truth.check_repository(self.root)
        self.assertTrue(
            any(
                "structure_validation.json summary.sections_validated" in issue
                for issue in issues
            ),
            issues,
        )
        self.assertTrue(
            any(
                "mermaid_invalid.json total_scanned" in issue
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
