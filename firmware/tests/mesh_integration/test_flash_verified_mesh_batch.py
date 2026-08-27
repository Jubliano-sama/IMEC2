#!/usr/bin/env python3
"""Regression tests for concurrent verified production flashing."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from contextlib import nullcontext
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPTS = REPO_ROOT / "firmware" / "scripts"
sys.path.insert(0, str(SCRIPTS))


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


batch = _load(
    "flash_verified_mesh_batch_test",
    SCRIPTS / "flash_verified_mesh_batch.py",
)


class BatchFlashTests(unittest.TestCase):
    def test_role_builds_are_only_production_clicker_and_anchor(self) -> None:
        self.assertEqual(
            {"clicker": "mesh-clicker", "anchor": "mesh-anchor"},
            batch.ROLE_BUILDS,
        )

    def test_factory_new_batch_archives_unlocks_and_stages_every_probe(self) -> None:
        probes = ["P1", "P2", "P3", "P4"]
        build = object()
        binding = {"cohort_id": "cohort"}
        calls: list[tuple[str, str]] = []
        journals = {
            probe: {"probe_id": probe, "state": "awaiting_qualification"}
            for probe in probes
        }

        def unlock(probe: str) -> bool:
            calls.append(("unlock", probe))
            return True

        def stage(*args: object, **kwargs: object) -> None:
            probe = str(args[2])
            self.assertTrue(kwargs["initialize_storage"])
            self.assertFalse(kwargs["bench_only"])
            calls.append(("stage", probe))

        with (
            mock.patch.object(batch, "_build_once"),
            mock.patch.object(batch.flash, "_ledger_lock", return_value=nullcontext()),
            mock.patch.object(batch, "_verify_batch", return_value=(build, binding)),
            mock.patch.object(batch.flash, "_load_journal", side_effect=journals.get),
            mock.patch.object(
                batch.flash, "_abandon_staged_candidate",
                side_effect=lambda data: calls.append(("archive", str(data["probe_id"]))),
            ),
            mock.patch.object(batch, "_mass_unlock", side_effect=unlock),
            mock.patch.object(batch.flash, "_stage_for_qualification", side_effect=stage),
        ):
            self.assertEqual(
                0,
                batch.run_batch(
                    "clicker", probes, factory_new=True, jobs=len(probes),
                ),
            )

        for operation in ("archive", "unlock", "stage"):
            self.assertEqual(
                set(probes),
                {probe for name, probe in calls if name == operation},
            )

    def test_failed_unlock_is_not_staged_but_other_probes_continue(self) -> None:
        probes = ["GOOD", "BAD"]
        staged: list[str] = []

        def unlock(probe: str) -> bool:
            if probe == "BAD":
                raise batch.flash.TransactionError("protected target failed")
            return True

        with (
            mock.patch.object(batch, "_build_once"),
            mock.patch.object(batch.flash, "_ledger_lock", return_value=nullcontext()),
            mock.patch.object(batch, "_verify_batch", return_value=(object(), {})),
            mock.patch.object(batch.flash, "_load_journal", return_value=None),
            mock.patch.object(batch, "_mass_unlock", side_effect=unlock),
            mock.patch.object(
                batch.flash, "_stage_for_qualification",
                side_effect=lambda _build, _dir, probe, _binding, **_kwargs: staged.append(probe),
            ),
        ):
            self.assertEqual(
                1,
                batch.run_batch("anchor", probes, factory_new=True, jobs=2),
            )
        self.assertEqual(["GOOD"], staged)

    def test_duplicate_probe_ids_are_rejected(self) -> None:
        with self.assertRaises(SystemExit):
            batch.parse_args([
                "--role", "clicker", "--probe-id", "P1", "--probe-id", "P1",
            ])


if __name__ == "__main__":
    unittest.main()
