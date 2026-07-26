#!/usr/bin/env python3
"""Regression tests for immutable verification input handling."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPT_DIR))

import verification_inputs  # noqa: E402


class VerificationInputTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name)
        self.sdk = self.base / "dwm-source"
        self.repo = self.base / "repository"
        self._init_repo(self.sdk)
        (self.sdk / "sdk.c").write_text("int sdk;\n", encoding="utf-8")
        self._git(self.sdk, "add", ".")
        self._git(self.sdk, "commit", "-q", "-m", "sdk")
        self.sdk_sha = self._git(self.sdk, "rev-parse", "HEAD").strip()

        self._init_repo(self.repo)
        (self.repo / ".gitignore").write_text("build/\n", encoding="utf-8")
        (self.repo / "tracked.txt").write_text("base\n", encoding="utf-8")
        self._git(
            self.repo,
            "-c",
            "protocol.file.allow=always",
            "submodule",
            "add",
            "-q",
            os.fspath(self.sdk),
            os.fspath(verification_inputs.DWM_SUBMODULE),
        )
        self._git(self.repo, "add", ".")
        self._git(self.repo, "commit", "-q", "-m", "baseline")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _init_repo(self, path: Path) -> None:
        path.mkdir()
        subprocess.run(["git", "init", "-q"], cwd=path, check=True)
        self._git(path, "config", "user.email", "verify@example.invalid")
        self._git(path, "config", "user.name", "Verifier")

    def _git(self, path: Path, *arguments: str) -> str:
        result = subprocess.run(
            ["git", *arguments],
            cwd=path,
            env=verification_inputs.verification_environment(),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=True,
        )
        return result.stdout

    def _write_pinned_west_config(self, workspace: Path) -> Path:
        config = workspace / ".west/config"
        config.parent.mkdir(parents=True)
        config.write_text(
            "[manifest]\n"
            "path = manifest\n"
            "file = west.yml\n"
            "\n"
            "[zephyr]\n"
            "base = zephyr\n",
            encoding="utf-8",
        )
        return config

    def test_snapshot_captures_dirty_and_untracked_content_with_local_sdk(
        self,
    ) -> None:
        (self.repo / "tracked.txt").write_text(
            "captured dirty value\n",
            encoding="utf-8",
        )
        (self.repo / "untracked.txt").write_text(
            "captured untracked value\n",
            encoding="utf-8",
        )
        expected = verification_inputs.repository_fingerprint(self.repo)

        with verification_inputs.frozen_source_snapshot(
            self.repo,
            expected_fingerprint=expected,
        ) as snapshot:
            snapshot_root = snapshot.root
            self.assertEqual(snapshot.fingerprint, expected)
            self.assertEqual(
                (snapshot.root / "tracked.txt").read_text(encoding="utf-8"),
                "captured dirty value\n",
            )
            self.assertEqual(
                (snapshot.root / "untracked.txt").read_text(encoding="utf-8"),
                "captured untracked value\n",
            )
            snapshot_sdk_sha = self._git(
                snapshot.root / verification_inputs.DWM_SUBMODULE,
                "rev-parse",
                "HEAD",
            ).strip()
            self.assertEqual(snapshot_sdk_sha, self.sdk_sha)
            self.assertEqual(
                verification_inputs.repository_fingerprint(snapshot.root),
                expected,
            )
        self.assertFalse(snapshot_root.exists())

    def test_transient_original_edit_restore_cannot_reach_snapshot_process(
        self,
    ) -> None:
        expected = verification_inputs.repository_fingerprint(self.repo)
        with verification_inputs.frozen_source_snapshot(
            self.repo,
            expected_fingerprint=expected,
        ) as snapshot:
            original = self.repo / "tracked.txt"
            original.write_text("transient mutation\n", encoding="utf-8")
            result = subprocess.run(
                [
                    sys.executable,
                    "-c",
                    "from pathlib import Path; print(Path('tracked.txt').read_text(), end='')",
                ],
                cwd=snapshot.root,
                env=verification_inputs.verification_environment(),
                stdout=subprocess.PIPE,
                text=True,
                check=True,
            )
            original.write_text("base\n", encoding="utf-8")
            self.assertEqual(result.stdout, "base\n")
            self.assertEqual(
                verification_inputs.repository_fingerprint(self.repo),
                expected,
            )
            self.assertEqual(
                verification_inputs.repository_fingerprint(snapshot.root),
                expected,
            )

    def test_transient_snapshot_edit_restore_is_rejected(self) -> None:
        expected = verification_inputs.repository_fingerprint(self.repo)
        with self.assertRaisesRegex(
            RuntimeError,
            "immutable source snapshot changed",
        ):
            with verification_inputs.frozen_source_snapshot(
                self.repo,
                expected_fingerprint=expected,
            ) as snapshot:
                tracked = snapshot.root / "tracked.txt"
                tracked.write_text("transient mutation\n", encoding="utf-8")
                tracked.write_text("base\n", encoding="utf-8")
                self.assertEqual(
                    verification_inputs.repository_fingerprint(snapshot.root),
                    expected,
                )

    def test_ignored_snapshot_write_is_rejected(self) -> None:
        expected = verification_inputs.repository_fingerprint(self.repo)
        with self.assertRaisesRegex(
            RuntimeError,
            "immutable source snapshot changed",
        ):
            with verification_inputs.frozen_source_snapshot(
                self.repo,
                expected_fingerprint=expected,
            ) as snapshot:
                generated = snapshot.root / "build/generated.h"
                generated.parent.mkdir()
                generated.write_text("#define INJECTED 1\n", encoding="utf-8")
                self.assertEqual(
                    verification_inputs.repository_fingerprint(snapshot.root),
                    expected,
                )

    def test_snapshot_rejects_symlink_outside_frozen_source(self) -> None:
        outside = self.base / "outside.txt"
        outside.write_text("mutable external input\n", encoding="utf-8")
        (self.repo / "external-link").symlink_to("../outside.txt")
        expected = verification_inputs.repository_fingerprint(self.repo)
        with self.assertRaisesRegex(RuntimeError, "symlink escapes"):
            with verification_inputs.frozen_source_snapshot(
                self.repo,
                expected_fingerprint=expected,
            ):
                pass

    def test_dirty_west_project_is_rejected(self) -> None:
        expected = self._git(self.sdk, "rev-parse", "HEAD").strip()
        self._git(
            self.sdk,
            "update-ref",
            "refs/heads/manifest-rev",
            expected,
        )
        project = verification_inputs.WestProject(
            "sdk",
            self.sdk,
            "v1",
            expected,
        )
        (self.sdk / "sdk.c").write_text("int dirty_sdk;\n", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "dirty paths"):
            verification_inputs.validate_west_projects(
                [project],
                phase="test",
            )

    def test_wrong_manifest_sha_west_project_is_rejected(self) -> None:
        expected = self._git(self.sdk, "rev-parse", "HEAD").strip()
        self._git(
            self.sdk,
            "update-ref",
            "refs/heads/manifest-rev",
            expected,
        )
        (self.sdk / "sdk.c").write_text("int sdk_v2;\n", encoding="utf-8")
        self._git(self.sdk, "add", "sdk.c")
        self._git(self.sdk, "commit", "-q", "-m", "sdk v2")
        project = verification_inputs.WestProject(
            "sdk",
            self.sdk,
            "v1",
            expected,
        )
        with self.assertRaisesRegex(RuntimeError, "HEAD .* != manifest SHA"):
            verification_inputs.validate_west_projects(
                [project],
                phase="test",
            )

    def test_skip_worktree_west_change_is_rejected(self) -> None:
        expected = self._git(self.sdk, "rev-parse", "HEAD").strip()
        self._git(
            self.sdk,
            "update-ref",
            "refs/heads/manifest-rev",
            expected,
        )
        self._git(self.sdk, "update-index", "--skip-worktree", "sdk.c")
        (self.sdk / "sdk.c").write_text(
            "int concealed_sdk;\n",
            encoding="utf-8",
        )
        project = verification_inputs.WestProject(
            "sdk",
            self.sdk,
            "v1",
            expected,
        )
        with self.assertRaisesRegex(RuntimeError, "skip-worktree"):
            verification_inputs.validate_west_projects(
                [project],
                phase="test",
            )

    def test_assume_unchanged_west_change_is_rejected(self) -> None:
        expected = self._git(self.sdk, "rev-parse", "HEAD").strip()
        self._git(
            self.sdk,
            "update-ref",
            "refs/heads/manifest-rev",
            expected,
        )
        self._git(self.sdk, "update-index", "--assume-unchanged", "sdk.c")
        (self.sdk / "sdk.c").write_text(
            "int concealed_sdk;\n",
            encoding="utf-8",
        )
        project = verification_inputs.WestProject(
            "sdk",
            self.sdk,
            "v1",
            expected,
        )
        with self.assertRaisesRegex(RuntimeError, "assume-unchanged"):
            verification_inputs.validate_west_projects(
                [project],
                phase="test",
            )

    def test_moving_head_and_manifest_ref_cannot_bypass_repository_lock(
        self,
    ) -> None:
        expected = self._git(self.sdk, "rev-parse", "HEAD").strip()
        self._git(
            self.sdk,
            "update-ref",
            "refs/heads/manifest-rev",
            expected,
        )
        (self.sdk / "sdk.c").write_text("int sdk_v2;\n", encoding="utf-8")
        self._git(self.sdk, "add", "sdk.c")
        self._git(self.sdk, "commit", "-q", "-m", "sdk v2")
        moved = self._git(self.sdk, "rev-parse", "HEAD").strip()
        self._git(
            self.sdk,
            "update-ref",
            "refs/heads/manifest-rev",
            moved,
        )
        project = verification_inputs.WestProject(
            "sdk",
            self.sdk,
            "v1",
            expected,
        )
        with self.assertRaisesRegex(RuntimeError, "manifest-rev moved"):
            verification_inputs.validate_west_projects(
                [project],
                phase="test",
            )

    def test_ignored_generated_west_input_is_rejected(self) -> None:
        (self.sdk / ".gitignore").write_text("generated/\n", encoding="utf-8")
        self._git(self.sdk, "add", ".gitignore")
        self._git(self.sdk, "commit", "-q", "-m", "ignore generated files")
        expected = self._git(self.sdk, "rev-parse", "HEAD").strip()
        self._git(
            self.sdk,
            "update-ref",
            "refs/heads/manifest-rev",
            expected,
        )
        generated = self.sdk / "generated/autoconf.h"
        generated.parent.mkdir()
        generated.write_text("#define LOCAL_OVERRIDE 1\n", encoding="utf-8")
        project = verification_inputs.WestProject(
            "sdk",
            self.sdk,
            "v1",
            expected,
        )
        with self.assertRaisesRegex(
            RuntimeError,
            "ignored paths can affect unpinned build inputs.*generated/autoconf.h",
        ):
            verification_inputs.validate_west_projects(
                [project],
                phase="test",
            )

    def test_west_project_rejects_symlink_outside_guarded_projects(self) -> None:
        outside = self.base / "outside-sdk-input"
        outside.mkdir()
        (self.sdk / "external-link").symlink_to("../outside-sdk-input")
        self._git(self.sdk, "add", "external-link")
        self._git(self.sdk, "commit", "-q", "-m", "external symlink")
        expected = self._git(self.sdk, "rev-parse", "HEAD").strip()
        self._git(
            self.sdk,
            "update-ref",
            "refs/heads/manifest-rev",
            expected,
        )
        project = verification_inputs.WestProject(
            "sdk",
            self.sdk,
            "v1",
            expected,
        )
        with self.assertRaisesRegex(RuntimeError, "outside guarded west projects"):
            verification_inputs.validate_west_projects(
                [project],
                phase="test",
            )

    def test_inotify_catches_modify_and_restore(self) -> None:
        guarded = self.sdk / "sdk.c"
        original = guarded.read_text(encoding="utf-8")
        guard = verification_inputs.LinuxInotifyWriteGuard([self.sdk])
        try:
            guarded.write_text("temporary\n", encoding="utf-8")
            guarded.write_text(original, encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "worktree changed"):
                guard.assert_stable("test")
        finally:
            guard.close()

    def test_west_config_mutate_and_restore_is_rejected(self) -> None:
        workspace = self.base / "workspace"
        config = self._write_pinned_west_config(workspace)
        original = config.read_text(encoding="utf-8")
        expected = self._git(self.sdk, "rev-parse", "HEAD").strip()
        self._git(
            self.sdk,
            "update-ref",
            "refs/heads/manifest-rev",
            expected,
        )
        project = verification_inputs.WestProject(
            "sdk",
            self.sdk,
            "v1",
            expected,
        )
        with self.assertRaisesRegex(
            RuntimeError,
            r"\.west configuration changed",
        ):
            with verification_inputs.frozen_west_dependencies(
                [project],
                workspace_root=workspace,
            ):
                config.write_text(
                    original.replace("base = zephyr", "base = injected"),
                    encoding="utf-8",
                )
                config.write_text(original, encoding="utf-8")

    def test_west_config_values_are_pinned(self) -> None:
        workspace = self.base / "workspace"
        config = self._write_pinned_west_config(workspace)
        config.write_text(
            config.read_text(encoding="utf-8").replace(
                "base = zephyr",
                "base = injected",
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(
            RuntimeError,
            "differs from the pinned local configuration",
        ):
            verification_inputs.validate_west_workspace_config(
                workspace,
                phase="test",
            )

    def test_environment_disables_python_and_git_writes(self) -> None:
        environment = verification_inputs.verification_environment({})
        self.assertEqual(environment["PYTHONDONTWRITEBYTECODE"], "1")
        self.assertEqual(environment["GIT_OPTIONAL_LOCKS"], "0")


if __name__ == "__main__":
    unittest.main()
