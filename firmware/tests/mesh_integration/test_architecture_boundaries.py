#!/usr/bin/env python3
"""Tests for the architectural growth gate."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from copy import deepcopy
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
        self.baseline = deepcopy(self.manifest)
        self.baseline_sources = {
            path.relative_to(self.root): path.read_text(encoding="utf-8")
            for path in self.root.rglob("*")
            if path.is_file() and path.suffix in {".c", ".h"}
        }

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def _write_manifest(self) -> None:
        self.manifest_path.write_text(json.dumps(self.manifest), encoding="utf-8")

    def _check(self) -> list[str]:
        return check_architecture_boundaries.check_repository(
            self.root,
            Path("boundaries.json"),
            baseline_manifest=self.baseline,
            baseline_sources=self.baseline_sources,
        )

    def test_accepts_frozen_composition(self) -> None:
        self.assertEqual(self._check(), [])

    def test_missing_immutable_history_fails_with_rebaseline_procedure(self) -> None:
        with self.assertRaisesRegex(
            ValueError,
            "Do not squash, rebase, prune, or reconstruct.*two separately reviewed",
        ):
            check_architecture_boundaries._load_immutable_baseline(self.root)

    def test_rejects_present_baseline_outside_head_history(self) -> None:
        subprocess.run(["git", "init", "-q"], cwd=self.root, check=True)
        subprocess.run(
            ["git", "config", "user.name", "Architecture Test"],
            cwd=self.root,
            check=True,
        )
        subprocess.run(
            ["git", "config", "user.email", "architecture@example.invalid"],
            cwd=self.root,
            check=True,
        )
        subprocess.run(["git", "add", "-A"], cwd=self.root, check=True)
        subprocess.run(
            ["git", "commit", "-q", "-m", "Baseline"],
            cwd=self.root,
            check=True,
        )
        baseline = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=self.root,
            check=True,
            stdout=subprocess.PIPE,
            text=True,
        ).stdout.strip()
        subprocess.run(
            ["git", "checkout", "-q", "--orphan", "unrelated"],
            cwd=self.root,
            check=True,
        )
        subprocess.run(
            ["git", "commit", "-q", "--allow-empty", "-m", "Unrelated"],
            cwd=self.root,
            check=True,
        )
        original = check_architecture_boundaries.IMMUTABLE_BASELINE_COMMIT
        check_architecture_boundaries.IMMUTABLE_BASELINE_COMMIT = baseline
        try:
            for loader in (
                check_architecture_boundaries._load_immutable_baseline,
                check_architecture_boundaries._load_immutable_sources,
            ):
                with self.subTest(loader=loader.__name__):
                    with self.assertRaisesRegex(
                        ValueError,
                        "present but is not an ancestor of HEAD",
                    ):
                        loader(self.root)
        finally:
            check_architecture_boundaries.IMMUTABLE_BASELINE_COMMIT = original

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

    def test_rejects_same_change_default_ceiling_relaxation(self) -> None:
        self.manifest["default_c_max_lines"] = 4
        (self.root / "firmware/src/new.c").write_text("1\n2\n3\n4\n", encoding="utf-8")
        self._write_manifest()
        issues = self._check()
        self.assertTrue(
            any("default_c_max_lines raised" in issue for issue in issues),
            issues,
        )

    def test_rejects_same_change_fragment_ceiling_relaxation(self) -> None:
        self.manifest["approved_include_fragments"][
            "firmware/src/legacy.inc"
        ] = 3
        (self.root / "firmware/src/legacy.inc").write_text(
            "one\ntwo\nthree\n",
            encoding="utf-8",
        )
        self._write_manifest()
        issues = self._check()
        self.assertTrue(
            any("include-fragment ceiling raised" in issue for issue in issues),
            issues,
        )

    def test_rejects_same_change_composed_ceiling_relaxation(self) -> None:
        self.manifest["composed_translation_units"]["firmware/src/legacy.c"][
            "max_composed_lines"
        ] = 5
        (self.root / "firmware/src/legacy.c").write_text(
            '#include "legacy.inc"\nbody\nmore\n',
            encoding="utf-8",
        )
        self._write_manifest()
        issues = self._check()
        self.assertTrue(
            any("composed ceiling raised" in issue for issue in issues),
            issues,
        )

    def test_rejects_new_frozen_exception(self) -> None:
        self.manifest["frozen_oversize_sources"]["firmware/src/new.c"] = 4
        (self.root / "firmware/src/new.c").write_text("1\n2\n3\n4\n", encoding="utf-8")
        self._write_manifest()
        issues = self._check()
        self.assertTrue(
            any("new frozen source exception" in issue for issue in issues),
            issues,
        )

    def test_rejects_new_approved_fragment(self) -> None:
        self.manifest["approved_include_fragments"]["firmware/src/more.inc"] = 1
        (self.root / "firmware/src/more.inc").write_text("new\n", encoding="utf-8")
        self._write_manifest()
        issues = self._check()
        self.assertTrue(
            any("new approved include fragment" in issue for issue in issues),
            issues,
        )

    def test_rejects_new_composed_translation_unit(self) -> None:
        self.manifest["composed_translation_units"]["firmware/src/plain.c"] = {
            "max_composed_lines": 1,
            "includes": [],
        }
        (self.root / "firmware/src/plain.c").write_text("body\n", encoding="utf-8")
        self._write_manifest()
        issues = self._check()
        self.assertTrue(
            any("new composed translation unit" in issue for issue in issues),
            issues,
        )

    def test_rejects_weakened_source_root(self) -> None:
        (self.root / "firmware/src/narrow").mkdir()
        self.manifest["source_roots"] = ["firmware/src/narrow"]
        self._write_manifest()
        issues = self._check()
        self.assertTrue(
            any("no longer cover baseline root" in issue for issue in issues),
            issues,
        )

    def test_rejects_removed_composed_ceiling_while_fragments_remain(self) -> None:
        self.manifest["composed_translation_units"] = {}
        self._write_manifest()
        issues = self._check()
        self.assertTrue(
            any("removed its composed ceiling" in issue for issue in issues),
            issues,
        )

    def test_rejects_removed_composed_ceiling_with_spaced_include(self) -> None:
        (self.root / "firmware/src/legacy.c").write_text(
            '  # include "legacy.inc"\nbody\n',
            encoding="utf-8",
        )
        self.manifest["composed_translation_units"] = {}
        self._write_manifest()
        issues = self._check()
        self.assertTrue(
            any("removed its composed ceiling" in issue for issue in issues),
            issues,
        )

    def test_rejects_renamed_composed_shell_without_ceiling(self) -> None:
        (self.root / "firmware/src/renamed.c").write_text(
            '#include "legacy.inc"\nbody\n',
            encoding="utf-8",
        )
        (self.root / "firmware/src/legacy.c").unlink()
        self.manifest["composed_translation_units"] = {}
        self._write_manifest()
        issues = self._check()
        self.assertTrue(
            any("without a declared composed" in issue for issue in issues),
            issues,
        )

    def test_rejects_fragment_with_multiple_c_owners(self) -> None:
        (self.root / "firmware/src/second.c").write_text(
            '#include "legacy.inc"\n',
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(
            any("exactly one declared C owner" in issue for issue in issues),
            issues,
        )

    def test_rejects_nested_include_fragments(self) -> None:
        (self.root / "firmware/src/nested.inc").write_text(
            "nested\n",
            encoding="utf-8",
        )
        (self.root / "firmware/src/legacy.inc").write_text(
            '#include "nested.inc"\n',
            encoding="utf-8",
        )
        for manifest in (self.baseline, self.manifest):
            manifest["approved_include_fragments"][
                "firmware/src/nested.inc"
            ] = 1
        self._write_manifest()
        issues = self._check()
        self.assertTrue(
            any("includes nested fragments" in issue for issue in issues),
            issues,
        )

    def test_rejects_fragment_debt_renamed_to_header(self) -> None:
        (self.root / "firmware/src/legacy.inc").unlink()
        (self.root / "firmware/src/legacy.h").write_text(
            "implementation\n"
            * (check_architecture_boundaries.MAX_HEADER_LINES + 1),
            encoding="utf-8",
        )
        (self.root / "firmware/src/legacy.c").write_text(
            '#include "legacy.h"\nbody\n',
            encoding="utf-8",
        )
        self.manifest["approved_include_fragments"] = {}
        self.manifest["composed_translation_units"] = {}
        self._write_manifest()
        issues = self._check()
        self.assertTrue(any("header ceiling" in issue for issue in issues), issues)

    def test_rejects_nonliteral_fragment_include(self) -> None:
        (self.root / "firmware/src/second.c").write_text(
            '#define FRAGMENT "legacy.inc"\n#include FRAGMENT\n',
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(any("non-literal include" in issue for issue in issues), issues)

    def test_detects_line_spliced_fragment_include(self) -> None:
        (self.root / "firmware/src/second.c").write_text(
            "#inc" + "\\" + '\nlude "legacy.inc"\n',
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(
            any("exactly one declared C owner" in issue for issue in issues),
            issues,
        )

    def test_detects_digraph_fragment_include(self) -> None:
        (self.root / "firmware/src/second.c").write_text(
            '%:include "legacy.inc"\n',
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(
            any("exactly one declared C owner" in issue for issue in issues),
            issues,
        )

    def test_rejects_absolute_and_parent_quoted_headers(self) -> None:
        source = self.root / "firmware/src/second.c"
        for target in ("/tmp/hidden.h", "../../hidden.h"):
            with self.subTest(target=target):
                source.write_text(
                    f'#include "{target}"\n',
                    encoding="utf-8",
                )
                issues = self._check()
                self.assertTrue(
                    any(
                        "includes header outside its approved source roots" in issue
                        for issue in issues
                    ),
                    issues,
                )

    def test_detects_comment_obscured_fragment_include(self) -> None:
        (self.root / "firmware/src/second.c").write_text(
            '#/**/include/**/"legacy.inc"\n',
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(
            any("exactly one declared C owner" in issue for issue in issues),
            issues,
        )

    def test_rejects_fragment_owned_through_header(self) -> None:
        (self.root / "firmware/src/wrapper.h").write_text(
            '#include "legacy.inc"\n',
            encoding="utf-8",
        )
        (self.root / "firmware/src/second.c").write_text(
            '#include "wrapper.h"\n',
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(
            any("must be owned directly" in issue for issue in issues),
            issues,
        )

    def test_rejects_large_local_header_closure_growth(self) -> None:
        includes: list[str] = []
        for index in range(10):
            name = f"implementation_{index}.h"
            includes.append(f'#include "{name}"')
            (self.root / "firmware/src" / name).write_text(
                ("implementation\n" * 100),
                encoding="utf-8",
            )
        (self.root / "firmware/src/wrapper.h").write_text(
            "\n".join(includes) + "\n",
            encoding="utf-8",
        )
        (self.root / "firmware/src/legacy.c").write_text(
            '#include "wrapper.h"\n#include "legacy.inc"\n',
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(
            any("local-header closure grew" in issue for issue in issues),
            issues,
        )

    def test_rejects_included_c_source(self) -> None:
        (self.root / "firmware/src/helper.c").write_text(
            "helper\n",
            encoding="utf-8",
        )
        (self.root / "firmware/src/legacy.c").write_text(
            '#include "legacy.inc"\n#include "helper.c"\n',
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(
            any("includes source-like file" in issue for issue in issues),
            issues,
        )

    def test_rejects_oversized_header_composition(self) -> None:
        (self.root / "firmware/src/implementation.h").write_text(
            "implementation\n"
            * (check_architecture_boundaries.MAX_HEADER_LINES + 1),
            encoding="utf-8",
        )
        (self.root / "firmware/src/legacy.c").write_text(
            '#include "implementation.h"\n#include "legacy.inc"\n',
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(any("header ceiling" in issue for issue in issues), issues)

    def test_rejects_production_source_outside_declared_roots(self) -> None:
        (self.root / "firmware/escape.c").write_text(
            "escaped\n",
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(
            any("outside declared production roots" in issue for issue in issues),
            issues,
        )

    def test_rejects_cpp_production_translation_unit(self) -> None:
        (self.root / "firmware/src/hidden.cpp").write_text(
            "int hidden;\n",
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(
            any("unsupported production translation unit" in issue for issue in issues),
            issues,
        )

    def test_rejects_preprocessed_c_target_source(self) -> None:
        (self.root / "firmware/src/hidden.i").write_text(
            "int hidden;\n",
            encoding="utf-8",
        )
        app = self.root / "firmware/app"
        app.mkdir()
        (app / "CMakeLists.txt").write_text(
            "target_sources(app PRIVATE ../src/hidden.i)\n",
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(
            any("target_sources permits only" in issue for issue in issues),
            issues,
        )

    def test_rejects_assembly_production_translation_unit(self) -> None:
        (self.root / "firmware/src/hidden.S").write_text(
            "hidden:\n",
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(
            any("unsupported production translation unit" in issue for issue in issues),
            issues,
        )

    def test_rejects_header_reclassified_as_translation_unit(self) -> None:
        app = self.root / "firmware/app"
        app.mkdir()
        tests = self.root / "firmware/tests"
        tests.mkdir()
        (tests / "hidden.h").write_text("int hidden;\n", encoding="utf-8")
        (app / "CMakeLists.txt").write_text(
            "set_source_files_properties(../tests/hidden.h PROPERTIES LANGUAGE C)\n"
            "target_sources(app PRIVATE ../tests/hidden.h)\n",
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(
            any("overrides a source language" in issue for issue in issues),
            issues,
        )

    def test_rejects_same_change_source_root_expansion(self) -> None:
        outside = self.root / "generated"
        outside.mkdir()
        (outside / "hidden.c").write_text("hidden\n", encoding="utf-8")
        self.manifest["source_roots"].append("generated")
        self._write_manifest()
        issues = self._check()
        self.assertTrue(
            any("adds unapproved root generated" in issue for issue in issues),
            issues,
        )

    def test_rejects_app_source_hidden_in_test_root(self) -> None:
        app = self.root / "firmware/app"
        app.mkdir()
        tests = self.root / "firmware/tests"
        tests.mkdir()
        (tests / "hidden.c").write_text("hidden\n", encoding="utf-8")
        (app / "CMakeLists.txt").write_text(
            "target_sources(app PRIVATE ../tests/hidden.c)\n",
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(
            any("CMakeLists.txt sources" in issue for issue in issues),
            issues,
        )

    def test_rejects_aux_source_directory_indirection(self) -> None:
        app = self.root / "firmware/app"
        app.mkdir()
        tests = self.root / "firmware/tests"
        tests.mkdir()
        (tests / "hidden.c").write_text("hidden\n", encoding="utf-8")
        (app / "CMakeLists.txt").write_text(
            "aux_source_directory(../tests HIDDEN)\n"
            "target_sources(app PRIVATE ${HIDDEN})\n",
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(
            any("uses aux_source_directory" in issue for issue in issues),
            issues,
        )
        self.assertTrue(
            any("variable-driven sources" in issue for issue in issues),
            issues,
        )

    def test_rejects_redirected_dwm_sdk_source(self) -> None:
        app = self.root / "firmware/app"
        app.mkdir()
        (app / "CMakeLists.txt").write_text(
            'set(DWM3000_SDK_DIR "/tmp/injected-sdk")\n'
            'target_sources(app PRIVATE '
            '"${DWM3000_SDK_DIR}/decadriver/deca_device.c")\n',
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(
            any("changes DWM3000_SDK_DIR" in issue for issue in issues),
            issues,
        )

    def test_rejects_external_target_include_directory(self) -> None:
        app = self.root / "firmware/app"
        app.mkdir()
        (app / "CMakeLists.txt").write_text(
            "target_include_directories(app PRIVATE /tmp/injected)\n"
            "target_sources(app PRIVATE ../src/legacy.c)\n",
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(
            any("changes production include directories" in issue for issue in issues),
            issues,
        )

    def test_rejects_variable_composed_source_path(self) -> None:
        app = self.root / "firmware/app"
        app.mkdir()
        (app / "CMakeLists.txt").write_text(
            "set(HIDDEN ../tests/hidden)\n"
            "set(SOURCE_SUFFIX c)\n"
            'target_sources(app PRIVATE "${HIDDEN}.${SOURCE_SUFFIX}")\n',
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(
            any("variable-driven sources" in issue for issue in issues),
            issues,
        )

    def test_rejects_dynamic_entries_in_approved_source_list(self) -> None:
        app = self.root / "firmware/app"
        app.mkdir()
        (app / "CMakeLists.txt").write_text(
            "set(IMEC_APP_LOCAL_SOURCES ../tests/hidden)\n"
            "target_sources(app PRIVATE ${IMEC_APP_LOCAL_SOURCES})\n",
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(
            any("dynamically or with non-C entries" in issue for issue in issues),
            issues,
        )

    def test_rejects_generated_or_out_of_policy_source(self) -> None:
        app = self.root / "firmware/app"
        app.mkdir()
        (app / "CMakeLists.txt").write_text(
            "target_sources(app PRIVATE ../src/generated.c)\n",
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(
            any("generated or out-of-policy translation unit" in issue for issue in issues),
            issues,
        )

    def test_allows_literal_repository_policy_source(self) -> None:
        app = self.root / "firmware/app"
        app.mkdir()
        (app / "CMakeLists.txt").write_text(
            "target_sources(app PRIVATE ../src/legacy.c)\n",
            encoding="utf-8",
        )
        self.assertEqual(self._check(), [])

    def test_rejects_app_source_that_escapes_repository(self) -> None:
        app = self.root / "firmware/app"
        app.mkdir()
        (app / "CMakeLists.txt").write_text(
            "target_sources(app PRIVATE ../../../firmware/src/escaped.c)\n",
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(
            any("CMakeLists.txt sources" in issue for issue in issues),
            issues,
        )

    def test_rejects_transitive_cmake_source_escape(self) -> None:
        app = self.root / "firmware/app"
        app.mkdir()
        tests = self.root / "firmware/tests"
        tests.mkdir()
        (tests / "hidden.c").write_text("hidden\n", encoding="utf-8")
        (app / "CMakeLists.txt").write_text(
            "include(sources.cmake)\n",
            encoding="utf-8",
        )
        (app / "sources.cmake").write_text(
            "target_sources(app PRIVATE ../tests/hidden.c)\n",
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(
            any("sources.cmake sources" in issue for issue in issues),
            issues,
        )

    def test_rejects_bracket_quoted_cmake_include_escape(self) -> None:
        app = self.root / "firmware/app"
        app.mkdir()
        tests = self.root / "firmware/tests"
        tests.mkdir()
        (tests / "hidden.c").write_text("hidden\n", encoding="utf-8")
        (app / "CMakeLists.txt").write_text(
            "include([[sources.cmake]])\n",
            encoding="utf-8",
        )
        (app / "sources.cmake").write_text(
            "target_sources(app PRIVATE ../tests/hidden.c)\n",
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(
            any("sources.cmake sources" in issue for issue in issues),
            issues,
        )

    def test_rejects_bracket_quoted_add_subdirectory_escape(self) -> None:
        app = self.root / "firmware/app"
        app.mkdir()
        tests = self.root / "firmware/tests"
        tests.mkdir()
        (tests / "CMakeLists.txt").write_text(
            "target_sources(app PRIVATE hidden.c)\n",
            encoding="utf-8",
        )
        (tests / "hidden.c").write_text("hidden\n", encoding="utf-8")
        (app / "CMakeLists.txt").write_text(
            "add_subdirectory([[../tests]])\n",
            encoding="utf-8",
        )
        issues = self._check()
        self.assertTrue(
            any("reaches CMake input" in issue for issue in issues),
            issues,
        )

    def test_allows_c_source_path_in_cmake_comment(self) -> None:
        app = self.root / "firmware/app"
        app.mkdir()
        (app / "CMakeLists.txt").write_text(
            "# Do not add ../tests/hidden.c here.\n",
            encoding="utf-8",
        )
        self.assertEqual(self._check(), [])

    def test_rejects_directory_symlink_inside_source_root(self) -> None:
        tests = self.root / "firmware/tests"
        tests.mkdir()
        (tests / "hidden.c").write_text("hidden\n", encoding="utf-8")
        (self.root / "firmware/src/link").symlink_to("../tests")
        issues = self._check()
        self.assertTrue(
            any("symlinked source input" in issue for issue in issues),
            issues,
        )

    def test_allows_reduced_ceilings(self) -> None:
        (self.root / "firmware/src/legacy.inc").write_text("one\n", encoding="utf-8")
        self.manifest["default_c_max_lines"] = 2
        self.manifest["approved_include_fragments"][
            "firmware/src/legacy.inc"
        ] = 1
        self.manifest["composed_translation_units"]["firmware/src/legacy.c"][
            "max_composed_lines"
        ] = 3
        self._write_manifest()
        self.assertEqual(self._check(), [])

    def test_allows_removed_debt_entries_after_extraction(self) -> None:
        (self.root / "firmware/src/legacy.inc").unlink()
        (self.root / "firmware/src/legacy.c").write_text("body\n", encoding="utf-8")
        self.manifest["approved_include_fragments"] = {}
        self.manifest["composed_translation_units"] = {}
        self._write_manifest()
        self.assertEqual(self._check(), [])

    def test_allows_removed_frozen_exception_after_reduction(self) -> None:
        oversized = self.root / "firmware/src/oversized.c"
        oversized.write_text("1\n2\n3\n4\n", encoding="utf-8")
        self.baseline["frozen_oversize_sources"]["firmware/src/oversized.c"] = 4
        self.manifest["frozen_oversize_sources"]["firmware/src/oversized.c"] = 4
        self._write_manifest()
        self.assertEqual(self._check(), [])

        oversized.write_text("1\n2\n3\n", encoding="utf-8")
        self.manifest["frozen_oversize_sources"] = {}
        self._write_manifest()
        self.assertEqual(self._check(), [])

    def test_allows_include_composition_reduction(self) -> None:
        self.baseline["approved_include_fragments"][
            "firmware/src/removed.inc"
        ] = 1
        self.baseline["composed_translation_units"]["firmware/src/legacy.c"][
            "includes"
        ] = ["legacy.inc", "removed.inc"]
        self.assertEqual(self._check(), [])


if __name__ == "__main__":
    unittest.main()
