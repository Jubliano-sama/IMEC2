# Repository Guidelines

## Project Structure & Module Organization

This workspace is a Zephyr/west firmware tree for the IMEC clicker, anchor, and gateway system. Project-specific code lives under `firmware/`; imported SDKs and platform dependencies live in `zephyr/`, `nrf/`, `modules/`, `nrfxlib/`, `bootloader/`, and `dwm3000 examples and sdk/`.

- `firmware/include/`: shared protocol, report, route, status, survey, and UWB headers.
- `firmware/src/`: platform-independent C modules with native unit tests.
- `firmware/app/`: Zephyr application, board overlay, DWM3000 port, role-specific runtime.
- `firmware/tests/`: native C tests.
- `Documentation/`: architecture, protocols, implementation task list.
- `old code/`: reference implementation only; do not modify unless explicitly requested.

## Build, Test, and Development Commands

Use the local uv-managed Python environment:

```sh
UV_CACHE_DIR=$PWD/.uv-cache uv venv --clear .venv
UV_CACHE_DIR=$PWD/.uv-cache uv pip install --python .venv/bin/python -r zephyr/scripts/requirements.txt -r nrf/scripts/requirements.txt
```

Native library tests:

```sh
cmake -S firmware -B firmware/build
cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure
```

Zephyr role builds:

```sh
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-clicker -- -DFIRMWARE_ROLE=clicker
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-anchor -- -DFIRMWARE_ROLE=anchor
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-gateway -- -DFIRMWARE_ROLE=gateway
```

## Coding Style & Naming Conventions

Code is C using Zephyr conventions: 4-space indentation, braces on the same line for functions/control blocks, `snake_case` for functions and variables, and `IMEC_*` for constants, flags, and enums. Keep hardware-independent logic in `firmware/src`; keep Zephyr, GPIO, BLE, and SPI code in `firmware/app`.

## Testing Guidelines

Add native tests for protocol/state behavior before relying on hardware. Keep tests focused and named after the module under test, such as `test_protocol.c` or `test_survey.c`. Run `ctest --test-dir firmware/build --output-on-failure` before submitting changes. For Zephyr-facing changes, build all three roles.

## Commit & Pull Request Guidelines

This workspace has no repo-level Git history available, so use clear imperative commit subjects, for example `Add BLE-gated anchor ranging MVP`. PRs should include a short summary, affected roles (`clicker`, `anchor`, `gateway`), test/build commands run, and any hardware assumptions or smoke-test gaps.

## Agent-Specific Instructions

Do not edit imported dependency trees unless the task explicitly targets them. Prefer documenting protocol changes in `Documentation/` alongside code changes. Treat the DWM3000 IRQ as unavailable for v1; use bounded SPI polling during BLE-scheduled UWB windows.

When modifying versioned documentation in `Documentation/`, increment only the patch component unless the user requests a larger version change. For example, the next edit to `Documentation/UWB+BLE Architecture 0.4.md` becomes `Documentation/UWB+BLE Architecture 0.4.1.md`; do not change `0.4` to `0.5` unless explicitly requested. Every documentation version bump must also add a short dated changelog entry inside the modified file.
