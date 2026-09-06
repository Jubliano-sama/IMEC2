> Historical review/evidence: findings, timings, probe mappings and test totals below belong to the recorded checkout and experiment. Revalidate against the current implementation; use the [documentation index](../../../README.md) for current contracts. Recommendations here are not user-approved requirements.

# Reproducing the audit

Run from the repository root. These probes compile local code and use fake hardware boundaries; they do not access or flash boards. CMake's application-seam extracts must be regenerated from the same source revision as the archive. The recorded revision is `474ef8600ef25b50da5c3dbc1dc30241178d166a`.

```sh
CCACHE_DISABLE=1 cmake -S firmware -B /tmp/imec-recovery-scale-audit-c859cd2e -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DPython3_EXECUTABLE=/home/tommie/Projects/IMEC2/.venv/bin/python
CCACHE_DISABLE=1 cmake --build /tmp/imec-recovery-scale-audit-c859cd2e -j8
PATH=/home/tommie/Projects/IMEC2/.venv/bin:$PATH ctest --test-dir /tmp/imec-recovery-scale-audit-c859cd2e --output-on-failure
/home/tommie/Projects/IMEC2/.venv/bin/python -m unittest discover -s tools/gateway_gui/tests
```

Use this workspace's Zephyr-capable virtualenv if the recorded shared virtualenv path is unavailable. A global `west` caused three seam build-launch failures before PATH was corrected; the final complete test run passed.

Run probes separately so expected contract failures do not prevent later probes:

```sh
python3 Documentation/Reviews/recovery-scale-power-audit-2026-09-05/probes/survey_lane_runner.py --library /tmp/imec-recovery-scale-audit-c859cd2e/libcore.a --output Documentation/Reviews/recovery-scale-power-audit-2026-09-05/probes/survey_lane_results.json
python3 Documentation/Reviews/recovery-scale-power-audit-2026-09-05/probes/run_application_probes.py --build /tmp/imec-recovery-scale-audit-c859cd2e
python3 Documentation/Reviews/recovery-scale-power-audit-2026-09-05/probes/ble_survey_classification_runner.py
python3 Documentation/Reviews/recovery-scale-power-audit-2026-09-05/probes/ble_receipt_runner.py
```

- `survey_lane_runner.py` returns **1** for the current hang, merge-atomicity and stale-ACK failures. Each process has a one-second deadline. It records passing control cases as well as failures in `survey_lane_results.json`; see `survey_lane_notes.md`.
- `run_application_probes.py` returns **0 when the recorded problematic behavior is reproduced**, because its assertions describe current observations. It records the anchor restart/roster conflict, C5 bank expiry/starvation and 30-node plan limits in `application_results.json`. These are diagnostic probes, not already-passing recovery regressions.
- `ble_survey_classification_runner.py` returns **0 after recording classification**. The current observed survey value 0 is the defect; command-result value 1 is the control. It extracts the exact current helper and records hashes in `ble_survey_classification_results.json`.
- `ble_receipt_runner.py` returns **1** because the command-before-receipt case fails the expected progress contract. Four other cases pass. See `ble_receipt_notes.md` for the production reachability analysis and the modeled callback boundaries.

The JSON files record source/archive hashes and observed output. Re-running overwrites those evidence files and extracted helper files with the new source's evidence. The source hashes and build-directory requirement matter: a probe linked to an old archive is not evidence about changed firmware.

`../validation.json` records suite totals and scope; `../native-analyzer.json` preserves analyzer diagnostics. Static analysis was driven by the native `compile_commands.json`, retaining each source's compile definitions/include paths, removing `-c`, `-o` and `-Werror`, and adding `--analyze -Xanalyzer -analyzer-output=text`. It does not cover all preprocessor branches of the production Zephyr roles.
