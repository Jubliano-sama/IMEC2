#!/usr/bin/env python3
"""Host portability regressions for the verified mesh flasher."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPTS = REPO_ROOT / "firmware" / "scripts"
sys.path.insert(0, str(SCRIPTS))

import artifact_cohort as cohort


def _load_flasher():
    path = SCRIPTS / "flash_verified_mesh.py"
    spec = importlib.util.spec_from_file_location("flash_verified_mesh_portability", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


flash = _load_flasher()


class VerifiedFlashPortabilityTests(unittest.TestCase):
    def test_venv_executable_uses_native_layout(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            windows_west = root / ".venv" / "Scripts" / "west.exe"
            posix_west = root / ".venv" / "bin" / "west"
            windows_west.parent.mkdir(parents=True)
            posix_west.parent.mkdir(parents=True)
            windows_west.touch()
            posix_west.touch()

            self.assertEqual(
                windows_west,
                flash._venv_executable("west", repo_root=root, platform_name="nt"),
            )
            self.assertEqual(
                posix_west,
                flash._venv_executable("west", repo_root=root, platform_name="posix"),
            )

    def test_cohort_west_discovery_uses_native_layout(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            windows_west = root / ".venv" / "Scripts" / "west.exe"
            posix_west = root / ".venv" / "bin" / "west"
            windows_west.parent.mkdir(parents=True)
            posix_west.parent.mkdir(parents=True)
            windows_west.touch()
            posix_west.touch()

            self.assertEqual(
                str(windows_west),
                cohort._repository_venv_executable(
                    root, "west", platform_name="nt",
                ),
            )
            self.assertEqual(
                str(posix_west),
                cohort._repository_venv_executable(
                    root, "west", platform_name="posix",
                ),
            )

    def test_ledger_lock_is_usable_on_the_current_host(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            ledger = Path(temporary) / "ledger.jsonl"
            with flash._ledger_lock(ledger):
                ledger.write_text("locked\n", encoding="utf-8")

            self.assertEqual("locked\n", ledger.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
