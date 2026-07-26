#!/usr/bin/env python3
"""Tests for the single verification entrypoint."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT_DIR = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPT_DIR))

import verify_changes  # noqa: E402


class VerificationEntrypointTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)
        subprocess.run(["git", "init", "-q"], cwd=self.root, check=True)
        subprocess.run(
            ["git", "config", "user.email", "verify@example.invalid"],
            cwd=self.root,
            check=True,
        )
        subprocess.run(
            ["git", "config", "user.name", "Verifier"],
            cwd=self.root,
            check=True,
        )
        (self.root / ".gitignore").write_text("build/\n", encoding="utf-8")
        (self.root / "tracked.txt").write_text("base\n", encoding="utf-8")
        app_input = self.root / "firmware/app/tracked.c"
        app_input.parent.mkdir(parents=True)
        app_input.write_text("int tracked;\n", encoding="utf-8")
        manifest = self.root / "manifest/west.yml"
        manifest.parent.mkdir(parents=True)
        manifest.write_text(
            "manifest:\n  projects: []\n",
            encoding="utf-8",
        )
        subprocess.run(["git", "add", "."], cwd=self.root, check=True)
        subprocess.run(
            ["git", "commit", "-q", "-m", "baseline"],
            cwd=self.root,
            check=True,
        )
        self.original_root = verify_changes.REPO_ROOT
        self.original_fingerprint = verify_changes._EXPECTED_REPOSITORY_FINGERPRINT
        self.original_snapshot_fingerprint = (
            verify_changes._EXPECTED_SNAPSHOT_FINGERPRINT
        )
        self.original_verification_root = verify_changes._VERIFICATION_ROOT
        self.original_source_guard = verify_changes._ACTIVE_SOURCE_GUARD
        self.original_dependency_guard = verify_changes._ACTIVE_DEPENDENCY_GUARD

    def tearDown(self) -> None:
        verify_changes.REPO_ROOT = self.original_root
        verify_changes._EXPECTED_REPOSITORY_FINGERPRINT = self.original_fingerprint
        verify_changes._EXPECTED_SNAPSHOT_FINGERPRINT = (
            self.original_snapshot_fingerprint
        )
        verify_changes._VERIFICATION_ROOT = self.original_verification_root
        verify_changes._ACTIVE_SOURCE_GUARD = self.original_source_guard
        verify_changes._ACTIVE_DEPENDENCY_GUARD = self.original_dependency_guard
        self.tempdir.cleanup()

    def test_fingerprint_tracks_dirty_and_untracked_content(self) -> None:
        (self.root / "tracked.txt").write_text("first dirty value\n", encoding="utf-8")
        first = verify_changes._repository_fingerprint(self.root)
        (self.root / "tracked.txt").write_text("second dirty value\n", encoding="utf-8")
        second = verify_changes._repository_fingerprint(self.root)
        self.assertNotEqual(first, second)

        (self.root / "untracked.txt").write_text("one\n", encoding="utf-8")
        third = verify_changes._repository_fingerprint(self.root)
        (self.root / "untracked.txt").write_text("two\n", encoding="utf-8")
        fourth = verify_changes._repository_fingerprint(self.root)
        self.assertNotEqual(third, fourth)

    def test_run_rejects_source_mutation(self) -> None:
        verify_changes.REPO_ROOT = self.root
        verify_changes._VERIFICATION_ROOT = self.root
        verify_changes._EXPECTED_REPOSITORY_FINGERPRINT = (
            verify_changes._repository_fingerprint()
        )
        command = [
            sys.executable,
            "-c",
            (
                "from pathlib import Path; "
                "Path('tracked.txt').write_text('changed during command\\n')"
            ),
        ]
        with self.assertRaisesRegex(RuntimeError, "repository changed"):
            verify_changes._run("mutating command", command)

    def test_run_rejects_transient_west_config_mutation(self) -> None:
        config = self.root / ".west/config"
        config.parent.mkdir(parents=True)
        original = (
            "[manifest]\n"
            "path = manifest\n"
            "file = west.yml\n"
            "\n"
            "[zephyr]\n"
            "base = zephyr\n"
        )
        config.write_text(original, encoding="utf-8")
        guard = verify_changes.LinuxInotifyWriteGuard(
            [config.parent],
            subject=".west configuration",
        )
        verify_changes._ACTIVE_DEPENDENCY_GUARD = guard
        command = [
            sys.executable,
            "-c",
            (
                "from pathlib import Path; "
                "path = Path('.west/config'); "
                "path.write_text(path.read_text().replace("
                "'base = zephyr', 'base = injected')); "
                f"path.write_text({original!r})"
            ),
        ]
        try:
            with self.assertRaisesRegex(
                RuntimeError,
                r"\.west configuration changed",
            ):
                verify_changes._run(
                    "simulated west build",
                    command,
                    cwd=self.root,
                )
        finally:
            verify_changes._ACTIVE_DEPENDENCY_GUARD = None
            guard.close()

    def test_exact_artifacts_require_clean_git_identity(self) -> None:
        verify_changes.REPO_ROOT = self.root
        verify_changes._require_clean_repository()
        (self.root / "unrelated.log").write_text("local evidence\n", encoding="utf-8")
        verify_changes._require_clean_repository()
        (self.root / "firmware/app/tracked.c").write_text(
            "int dirty;\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(RuntimeError, "embedded Git identity"):
            verify_changes._require_clean_repository()

    def test_exact_artifacts_reject_untracked_build_inputs(self) -> None:
        verify_changes.REPO_ROOT = self.root
        build_input = self.root / "firmware/app/untracked.c"
        build_input.parent.mkdir(parents=True, exist_ok=True)
        build_input.write_text("input\n", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "untracked build inputs"):
            verify_changes._require_clean_repository()

    def test_exact_artifacts_reject_dirty_west_manifest(self) -> None:
        verify_changes.REPO_ROOT = self.root
        (self.root / "manifest/west.yml").write_text(
            "manifest:\n  projects:\n    - name: injected\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(RuntimeError, "embedded Git identity"):
            verify_changes._require_clean_repository()

    def test_checks_only_source_set_runs_agent_guidance_self_test(self) -> None:
        verify_changes._VERIFICATION_ROOT = self.root
        with mock.patch.object(verify_changes, "_run") as run:
            verify_changes._source_checks()
        calls = {
            call.args[0]: call.args[1]
            for call in run.call_args_list
        }
        self.assertIn("agent guidance self-test", calls)
        self.assertEqual(
            calls["agent guidance self-test"],
            [
                str(verify_changes.PYTHON),
                str(
                    self.root
                    / "firmware/tests/mesh_integration/test_agent_guidance.py"
                ),
            ],
        )

    def test_zephyr_builds_disable_ccache_and_isolate_flash_state(self) -> None:
        verify_changes._VERIFICATION_ROOT = self.root
        build_root = self.root / "zephyr"
        persistence = build_root / "mesh-persistence-native"
        (persistence / "zephyr").mkdir(parents=True)
        (persistence / "zephyr/zephyr.exe").write_text("", encoding="utf-8")
        calls: list[tuple[str, list[str], dict[str, str] | None, Path | None]] = []

        def record(
            label: str,
            command: list[str],
            *,
            env: dict[str, str] | None = None,
            cwd: Path | None = None,
        ) -> None:
            calls.append((label, command, env, cwd))

        with mock.patch.object(verify_changes, "_run", side_effect=record):
            workspace = verify_changes.WestWorkspace("west", self.root)
            verify_changes._run_exact_roles(build_root, 3, workspace)
            verify_changes._run_compatibility_builds(
                build_root / "compatibility",
                3,
                workspace,
            )

        west_calls = [call for call in calls if call[1][0] == "west"]
        self.assertEqual(len(west_calls), 12)
        for label, _command, env, _cwd in west_calls:
            with self.subTest(label=label):
                self.assertIsNotNone(env)
                self.assertEqual(env["CCACHE_DISABLE"], "1")
                self.assertEqual(env["CMAKE_BUILD_PARALLEL_LEVEL"], "3")
                self.assertEqual(
                    env["ZEPHYR_BASE"],
                    str(self.root / "zephyr"),
                )
                self.assertEqual(
                    env["WEST_CONFIG_LOCAL"],
                    str(self.root / ".west/config"),
                )
                self.assertEqual(env["WEST_CONFIG_GLOBAL"], "/dev/null")
                self.assertEqual(env["WEST_CONFIG_SYSTEM"], "/dev/null")
                self.assertIn(
                    f"-DUSER_CACHE_DIR={build_root / 'user-cache'}",
                    _command,
                )
                source_index = _command.index("-s") + 1
                self.assertEqual(
                    _command[source_index],
                    str(self.root / "firmware/app")
                    if "persistence" not in label
                    else str(self.root / "firmware/app/tests/mesh_persistence"),
                )
                self.assertEqual(_cwd, self.root)

        persistence_runs = [
            call for call in calls if call[0] == "run Zephyr NVS persistence test"
        ]
        self.assertEqual(len(persistence_runs), 1)
        self.assertEqual(persistence_runs[0][3], persistence)

    def test_ambient_build_input_overrides_fail_closed(self) -> None:
        workspace = verify_changes.WestWorkspace("west", self.root)
        overrides = (
            "ZEPHYR_BASE",
            "WEST_CONFIG",
            "ZEPHYR_MODULES",
            "EXTRA_ZEPHYR_MODULES",
            "BOARD_ROOT",
            "APPLICATION_ROOT",
            "Zephyr_ROOT",
            "ZEPHYR_TOOLCHAIN_VARIANT",
            "GNUARMEMB_TOOLCHAIN_PATH",
            "CMAKE_PROJECT_INCLUDE",
            "CC",
            "CPATH",
            "C_INCLUDE_PATH",
            "COMPILER_PATH",
            "LIBRARY_PATH",
        )
        for variable in overrides:
            with self.subTest(variable=variable):
                with mock.patch.dict(
                    verify_changes.os.environ,
                    {variable: "/tmp/injected"},
                    clear=False,
                ):
                    with self.assertRaisesRegex(
                        RuntimeError,
                        f"ambient build configuration.*{variable}",
                    ):
                        verify_changes._zephyr_environment(2, workspace)
                    with self.assertRaisesRegex(
                        RuntimeError,
                        f"ambient build configuration.*{variable}",
                    ):
                        verify_changes._native_environment(False)

    def test_default_zephyr_roots_are_unique_and_explicit_root_is_locked(self) -> None:
        verify_changes.REPO_ROOT = self.root
        with verify_changes._locked_zephyr_build_root(None) as first:
            first_path = first
            self.assertTrue(first.exists())
            with verify_changes._locked_zephyr_build_root(None) as second:
                second_path = second
                self.assertTrue(second.exists())
                self.assertNotEqual(first, second)
            self.assertFalse(second_path.exists())
        self.assertFalse(first_path.exists())

        explicit = self.root / "build/explicit"
        with verify_changes._locked_zephyr_build_root(explicit):
            with self.assertRaisesRegex(RuntimeError, "already in use"):
                with verify_changes._locked_zephyr_build_root(explicit):
                    pass

        symlink_root = self.root / "build/symlink-lock"
        symlink_root.mkdir()
        protected = self.root / "protected.txt"
        protected.write_text("keep me\n", encoding="utf-8")
        (symlink_root / ".verify.lock").symlink_to(protected)
        with self.assertRaises(OSError):
            with verify_changes._locked_zephyr_build_root(symlink_root):
                pass
        self.assertEqual(
            protected.read_text(encoding="utf-8"),
            "keep me\n",
        )

    def test_west_workspace_failure_is_explicit(self) -> None:
        verify_changes.REPO_ROOT = self.root
        with mock.patch.object(
            verify_changes,
            "discover_west_workspace",
            side_effect=RuntimeError("initialized west workspace is missing"),
        ):
            with self.assertRaisesRegex(RuntimeError, "initialized west workspace"):
                verify_changes._west_executable()

    def test_ci_checkouts_fetch_pinned_history(self) -> None:
        workflows = (
            ".github/workflows/firmware-verification.yml",
            ".github/workflows/mesh-deployment-policy.yml",
        )
        for relative in workflows:
            lines = (self.original_root / relative).read_text(encoding="utf-8").splitlines()
            checkout_indexes = [
                index
                for index, line in enumerate(lines)
                if "uses: actions/checkout@" in line
            ]
            self.assertTrue(checkout_indexes, relative)
            for index in checkout_indexes:
                indent = len(lines[index]) - len(lines[index].lstrip())
                block: list[str] = []
                for line in lines[index + 1:]:
                    line_indent = len(line) - len(line.lstrip())
                    if line.strip().startswith("- ") and line_indent <= indent:
                        break
                    block.append(line.strip())
                with self.subTest(workflow=relative, line=index + 1):
                    self.assertIn("fetch-depth: 0", block)


if __name__ == "__main__":
    unittest.main()
