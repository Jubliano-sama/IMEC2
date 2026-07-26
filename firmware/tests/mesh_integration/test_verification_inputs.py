#!/usr/bin/env python3
"""Regression tests for immutable verification input handling."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT_DIR = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPT_DIR))

import verification_inputs  # noqa: E402


WEST_MANIFEST = (
    "manifest:\n"
    "  projects:\n"
    "    - name: sdk\n"
    "      path: sdk\n"
    "      revision: v1\n"
)


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
        self._write_west_manifest(self.repo)
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
        config.parent.mkdir(parents=True, exist_ok=True)
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

    def _write_west_manifest(
        self,
        root: Path,
        payload: str = WEST_MANIFEST,
    ) -> Path:
        manifest = root / verification_inputs.WEST_MANIFEST_RELATIVE
        manifest.parent.mkdir(parents=True, exist_ok=True)
        manifest.write_text(payload, encoding="utf-8")
        return manifest

    def _write_west_lock(
        self,
        path: Path,
        *,
        sha: str = "0" * 40,
        revision: str = "v1",
    ) -> Path:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps(
                {
                    "schema": verification_inputs.WEST_LOCK_SCHEMA,
                    "projects": [
                        {
                            "name": "sdk",
                            "path": "sdk",
                            "revision": revision,
                            "sha": sha,
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )
        return path

    def _prepare_west_metadata(
        self,
        workspace: Path,
    ) -> tuple[Path, Path, Path]:
        self._write_pinned_west_config(workspace)
        live_manifest = self._write_west_manifest(workspace)
        frozen_manifest = self._write_west_manifest(
            self.base / "frozen-source",
        )
        lock = self._write_west_lock(self.base / "west-projects.lock.json")
        return live_manifest, frozen_manifest, lock

    def _clean_sdk_project(self) -> verification_inputs.WestProject:
        expected = self._git(self.sdk, "rev-parse", "HEAD").strip()
        self._git(
            self.sdk,
            "update-ref",
            "refs/heads/manifest-rev",
            expected,
        )
        return verification_inputs.WestProject(
            "sdk",
            self.sdk,
            "v1",
            expected,
        )

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
        live_manifest, frozen_manifest, lock = self._prepare_west_metadata(
            workspace,
        )
        self.assertTrue(live_manifest.is_file())
        config = workspace / ".west/config"
        original = config.read_text(encoding="utf-8")
        project = self._clean_sdk_project()
        with self.assertRaisesRegex(
            RuntimeError,
            r"\.west configuration.*changed",
        ):
            with mock.patch.object(
                verification_inputs,
                "active_west_projects",
                return_value=(project,),
            ):
                with verification_inputs.frozen_west_dependencies(
                    [project],
                    workspace=verification_inputs.WestWorkspace(
                        "west",
                        workspace,
                    ),
                    lock_path=lock,
                    frozen_manifest_path=frozen_manifest,
                ):
                    config.write_text(
                        original.replace("base = zephyr", "base = injected"),
                        encoding="utf-8",
                    )
                    config.write_text(original, encoding="utf-8")

    def test_live_manifest_identity_mismatch_blocks_west_list(self) -> None:
        workspace = self.base / "workspace"
        live_manifest, frozen_manifest, lock = self._prepare_west_metadata(
            workspace,
        )
        live_manifest.write_text(
            WEST_MANIFEST.replace("revision: v1", "revision: injected"),
            encoding="utf-8",
        )
        workspace_record = verification_inputs.WestWorkspace("west", workspace)
        with mock.patch.object(
            verification_inputs,
            "_run_capture",
        ) as run_capture:
            with self.assertRaisesRegex(
                RuntimeError,
                "live west manifest differs.*sha256",
            ):
                verification_inputs.active_west_projects(
                    workspace_record,
                    lock,
                    frozen_manifest,
                )
        run_capture.assert_not_called()

    def test_manifest_reads_cannot_block_on_raced_fifo_replacement(self) -> None:
        workspace = self.base / "workspace"
        _live_manifest, frozen_manifest, _lock = self._prepare_west_metadata(
            workspace,
        )
        real_open = os.open
        observed_flags: list[int] = []

        def inspect_open(
            path: os.PathLike[str] | str,
            flags: int,
            mode: int = 0o777,
        ) -> int:
            observed_flags.append(flags)
            return real_open(path, flags, mode)

        with mock.patch.object(
            verification_inputs.os,
            "open",
            side_effect=inspect_open,
        ):
            verification_inputs.validate_west_manifest_identity(
                workspace,
                frozen_manifest,
                phase="test",
            )
        self.assertEqual(len(observed_flags), 2)
        self.assertTrue(
            all(flags & os.O_NONBLOCK for flags in observed_flags)
        )

    def test_safe_reader_rejects_real_fifo_replacement(self) -> None:
        target = self.base / "raced-manifest.yml"
        target.write_bytes(b"manifest: {}\n")
        real_open = os.open

        def replace_with_fifo(
            path: os.PathLike[str] | str,
            flags: int,
            mode: int = 0o777,
        ) -> int:
            self.assertEqual(Path(path), target)
            self.assertTrue(flags & os.O_NONBLOCK)
            self.assertTrue(flags & os.O_NOFOLLOW)
            target.unlink()
            os.mkfifo(target)
            return real_open(path, flags, mode)

        with mock.patch.object(
            verification_inputs.os,
            "open",
            side_effect=replace_with_fifo,
        ):
            with self.assertRaisesRegex(
                RuntimeError,
                "became a non-regular file",
            ):
                verification_inputs._read_regular_file_bytes(
                    target,
                    subject="test manifest",
                    phase="during FIFO race",
                )

    def test_safe_reader_bounds_endless_mocked_reads(self) -> None:
        target = self.base / "endless-manifest.yml"
        target.write_bytes(b"x")
        read_count = 0

        def endless_read(_descriptor: int, _requested: int) -> bytes:
            nonlocal read_count
            read_count += 1
            if read_count > 9:
                raise AssertionError("safe reader exceeded its bounded call count")
            return b"x"

        with mock.patch.object(
            verification_inputs.os,
            "read",
            side_effect=endless_read,
        ) as read:
            with self.assertRaisesRegex(
                RuntimeError,
                "exceeded the 8-byte verification limit while reading",
            ):
                verification_inputs._read_regular_file_bytes(
                    target,
                    subject="test manifest",
                    phase="during endless read",
                    max_bytes=8,
                )
        self.assertEqual(read.call_count, 9)

    def test_safe_reader_rejects_oversize_mocked_read(self) -> None:
        target = self.base / "oversize-manifest.yml"
        target.write_bytes(b"x")

        def oversize_read(_descriptor: int, requested: int) -> bytes:
            return b"x" * (requested + 1)

        with mock.patch.object(
            verification_inputs.os,
            "read",
            side_effect=oversize_read,
        ):
            with self.assertRaisesRegex(
                RuntimeError,
                "returned more bytes than requested",
            ):
                verification_inputs._read_regular_file_bytes(
                    target,
                    subject="test manifest",
                    phase="during oversize read",
                    max_bytes=8,
                )

    def test_frozen_manifest_is_bounded_before_live_manifest(self) -> None:
        workspace = self.base / "workspace"
        live_manifest, frozen_manifest, _lock = self._prepare_west_metadata(
            workspace,
        )
        reads: list[Path] = []

        def record_read(
            path: Path,
            *,
            subject: str,
            phase: str,
            max_bytes: int = verification_inputs.WEST_METADATA_MAX_BYTES,
        ) -> bytes:
            self.assertTrue(subject)
            self.assertEqual(phase, "test")
            self.assertEqual(
                max_bytes,
                verification_inputs.WEST_METADATA_MAX_BYTES,
            )
            reads.append(path)
            return b"manifest: {}\n"

        with mock.patch.object(
            verification_inputs,
            "_read_regular_file_bytes",
            side_effect=record_read,
        ):
            verification_inputs.validate_west_manifest_identity(
                workspace,
                frozen_manifest,
                phase="test",
            )
        self.assertEqual(reads, [frozen_manifest, live_manifest])

    def test_active_projects_accept_separate_clean_workspace(self) -> None:
        workspace = self.base / "alternate-workspace"
        _live_manifest, frozen_manifest, lock = self._prepare_west_metadata(
            workspace,
        )
        with mock.patch.object(
            verification_inputs,
            "_run_capture",
            return_value=b"sdk\tsdk\tv1\tcloned\n",
        ):
            projects = verification_inputs.active_west_projects(
                verification_inputs.WestWorkspace("west", workspace),
                lock,
                frozen_manifest,
            )
        self.assertEqual(
            projects,
            (
                verification_inputs.WestProject(
                    "sdk",
                    (workspace / "sdk").resolve(),
                    "v1",
                    "0" * 40,
                ),
            ),
        )

    def test_live_manifest_symlink_blocks_west_list(self) -> None:
        workspace = self.base / "workspace"
        live_manifest, frozen_manifest, lock = self._prepare_west_metadata(
            workspace,
        )
        live_manifest.unlink()
        live_manifest.symlink_to(frozen_manifest)
        workspace_record = verification_inputs.WestWorkspace("west", workspace)
        with mock.patch.object(
            verification_inputs,
            "_run_capture",
        ) as run_capture:
            with self.assertRaisesRegex(
                RuntimeError,
                "live west manifest must be a regular non-symlink file",
            ):
                verification_inputs.active_west_projects(
                    workspace_record,
                    lock,
                    frozen_manifest,
                )
        run_capture.assert_not_called()

    def test_active_projects_revalidate_config_after_guard_construction(
        self,
    ) -> None:
        workspace = self.base / "workspace"
        _live_manifest, frozen_manifest, lock = self._prepare_west_metadata(
            workspace,
        )
        config = workspace / ".west/config"
        original = config.read_text(encoding="utf-8")
        real_guard = verification_inputs.LinuxInotifyWriteGuard

        def construct_after_mutation(
            roots: list[Path],
            *,
            subject: str,
            exclude_git: bool = True,
        ) -> verification_inputs.LinuxInotifyWriteGuard:
            config.write_text(
                original.replace("base = zephyr", "base = injected"),
                encoding="utf-8",
            )
            return real_guard(
                roots,
                subject=subject,
                exclude_git=exclude_git,
            )

        try:
            with mock.patch.object(
                verification_inputs,
                "LinuxInotifyWriteGuard",
                side_effect=construct_after_mutation,
            ), mock.patch.object(
                verification_inputs,
                "_run_capture",
            ) as run_capture:
                with self.assertRaisesRegex(
                    RuntimeError,
                    "differs from the pinned local configuration",
                ):
                    verification_inputs.active_west_projects(
                        verification_inputs.WestWorkspace(
                            "west",
                            workspace,
                        ),
                        lock,
                        frozen_manifest,
                    )
            run_capture.assert_not_called()
        finally:
            config.write_text(original, encoding="utf-8")

    def test_workspace_discovery_revalidates_config_after_guard_construction(
        self,
    ) -> None:
        config = self._write_pinned_west_config(self.repo)
        original = config.read_text(encoding="utf-8")
        local_west = self.repo / ".venv/bin/west"
        local_west.parent.mkdir(parents=True)
        local_west.write_text("#!/bin/sh\n", encoding="utf-8")
        real_guard = verification_inputs.LinuxInotifyWriteGuard

        def construct_after_mutation(
            roots: list[Path],
            *,
            subject: str,
            exclude_git: bool = True,
        ) -> verification_inputs.LinuxInotifyWriteGuard:
            config.write_text(
                original.replace("base = zephyr", "base = injected"),
                encoding="utf-8",
            )
            return real_guard(
                roots,
                subject=subject,
                exclude_git=exclude_git,
            )

        try:
            with mock.patch.object(
                verification_inputs,
                "_git_common_checkout",
                return_value=None,
            ), mock.patch.object(
                verification_inputs.shutil,
                "which",
                return_value=None,
            ), mock.patch.object(
                verification_inputs,
                "LinuxInotifyWriteGuard",
                side_effect=construct_after_mutation,
            ), mock.patch.object(
                verification_inputs.subprocess,
                "run",
            ) as west_topdir:
                with self.assertRaisesRegex(
                    RuntimeError,
                    "differs from the pinned local configuration",
                ):
                    verification_inputs.discover_west_workspace(self.repo)
            west_topdir.assert_not_called()
        finally:
            config.write_text(original, encoding="utf-8")

    def test_west_list_manifest_mutate_and_restore_is_rejected(self) -> None:
        workspace = self.base / "workspace"
        live_manifest, frozen_manifest, lock = self._prepare_west_metadata(
            workspace,
        )
        original = live_manifest.read_bytes()

        def mutate_and_list(
            _command: list[str],
            *,
            cwd: Path,
            input_bytes: bytes | None = None,
            env: dict[str, str] | None = None,
        ) -> bytes:
            self.assertEqual(cwd, workspace)
            self.assertIsNone(input_bytes)
            self.assertIsNotNone(env)
            live_manifest.write_bytes(b"manifest:\n  projects: []\n")
            live_manifest.write_bytes(original)
            return b"sdk\tsdk\tv1\tcloned\n"

        with mock.patch.object(
            verification_inputs,
            "_run_capture",
            side_effect=mutate_and_list,
        ):
            with self.assertRaisesRegex(
                RuntimeError,
                "live west manifest changed",
            ):
                verification_inputs.active_west_projects(
                    verification_inputs.WestWorkspace("west", workspace),
                    lock,
                    frozen_manifest,
                )

    def test_matrix_manifest_mutate_and_restore_is_rejected(self) -> None:
        workspace = self.base / "workspace"
        live_manifest, frozen_manifest, lock = self._prepare_west_metadata(
            workspace,
        )
        original = live_manifest.read_bytes()
        project = self._clean_sdk_project()
        with mock.patch.object(
            verification_inputs,
            "active_west_projects",
            return_value=(project,),
        ):
            with self.assertRaisesRegex(
                RuntimeError,
                "live west manifest changed",
            ):
                with verification_inputs.frozen_west_dependencies(
                    [project],
                    workspace=verification_inputs.WestWorkspace(
                        "west",
                        workspace,
                    ),
                    lock_path=lock,
                    frozen_manifest_path=frozen_manifest,
                ):
                    live_manifest.write_bytes(
                        b"manifest:\n  projects: []\n"
                    )
                    live_manifest.write_bytes(original)

    def test_post_matrix_project_resolution_drift_is_rejected(self) -> None:
        workspace = self.base / "workspace"
        _live_manifest, frozen_manifest, lock = self._prepare_west_metadata(
            workspace,
        )
        project = self._clean_sdk_project()
        drifted = verification_inputs.WestProject(
            project.name,
            project.path,
            "injected-revision",
            project.expected_sha,
        )
        with mock.patch.object(
            verification_inputs,
            "active_west_projects",
            side_effect=((project,), (drifted,)),
        ):
            with self.assertRaisesRegex(
                RuntimeError,
                "active west project set changed",
            ):
                with verification_inputs.frozen_west_dependencies(
                    [project],
                    workspace=verification_inputs.WestWorkspace(
                        "west",
                        workspace,
                    ),
                    lock_path=lock,
                    frozen_manifest_path=frozen_manifest,
                ):
                    pass

    def test_matrix_preserves_body_failure_with_cleanup_drift(self) -> None:
        workspace = self.base / "workspace"
        live_manifest, frozen_manifest, lock = self._prepare_west_metadata(
            workspace,
        )
        original = live_manifest.read_bytes()
        project = self._clean_sdk_project()
        body_error = ValueError("matrix body failed")
        with mock.patch.object(
            verification_inputs,
            "active_west_projects",
            return_value=(project,),
        ):
            with self.assertRaisesRegex(
                verification_inputs.VerificationMatrixFailure,
                "matrix body failed.*cleanup also failed.*manifest changed",
            ) as captured:
                with verification_inputs.frozen_west_dependencies(
                    [project],
                    workspace=verification_inputs.WestWorkspace(
                        "west",
                        workspace,
                    ),
                    lock_path=lock,
                    frozen_manifest_path=frozen_manifest,
                ):
                    live_manifest.write_bytes(
                        b"manifest:\n  projects: []\n"
                    )
                    live_manifest.write_bytes(original)
                    raise body_error
        self.assertIs(captured.exception.primary_error, body_error)
        self.assertIs(captured.exception.__cause__, body_error)
        self.assertTrue(captured.exception.cleanup_errors)

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
