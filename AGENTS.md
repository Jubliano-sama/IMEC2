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

ML collection builds:

```sh
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/ml-clicker -- -DIMEC_BUILD_PRESET=ml_clicker
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/ml-anchor-1 -- -DIMEC_BUILD_PRESET=ml_anchor_1
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/ml-anchor-2 -- -DIMEC_BUILD_PRESET=ml_anchor_2
```

For deterministic ML anchors, replace `build/ml-anchor-2` and `ml_anchor_2` with the anchor slot being programmed, such as `build/ml-anchor-3` / `ml_anchor_3` through `build/ml-anchor-8` / `ml_anchor_8`. Each preset assigns a unique `DEVICE_ID` and deterministic discovery slot.

Flash the exact build directory for the image you intend to program; do not rely on west's previous build context:

```sh
.venv/bin/west flash --runner pyocd --build-dir build/firmware-clicker -- --frequency 4000000
.venv/bin/west flash --runner pyocd --build-dir build/firmware-anchor -- --frequency 4000000
.venv/bin/west flash --runner pyocd --build-dir build/firmware-gateway -- --frequency 4000000
.venv/bin/west flash --runner pyocd --build-dir build/ml-clicker -- --frequency 4000000
.venv/bin/west flash --runner pyocd --build-dir build/ml-anchor-1 -- --frequency 4000000
```

For deterministic ML anchors, replace `build/ml-anchor-1` with the anchor image being programmed, such as `build/ml-anchor-2` through `build/ml-anchor-8`.

Flashing at 4MHz has been proven to work, if a flash fails, assume the cabling is at fault and do not reduce flash speed.

## Coding Style & Naming Conventions

Code is C using Zephyr conventions: 4-space indentation, braces on the same line for functions/control blocks, and `snake_case` for functions and variables. Do not add project-wide prefixes such as `IMEC_` to new identifiers; use descriptive module-scoped names and the existing protocol/route/status naming style. Keep hardware-independent logic in `firmware/src`; keep Zephyr, GPIO, BLE, and SPI code in `firmware/app`.

## Testing Guidelines

Add native tests for protocol/state behavior before relying on hardware. Keep tests focused and named after the module under test, such as `test_protocol.c` or `test_survey.c`. Run `ctest --test-dir firmware/build --output-on-failure` before submitting changes. For Zephyr-facing changes, build all three roles.

## Commit & Pull Request Guidelines

Use clear imperative commit subjects, for example `Add BLE-gated anchor ranging MVP`. PRs should include a short summary, affected roles (`clicker`, `anchor`, `gateway`), test/build commands run, and any hardware assumptions or smoke-test gaps. After a bug is fixed, always commit your work, even if the bug is only partially fixed.

## Agent-Specific Instructions

Do not edit imported dependency trees unless the task explicitly targets them. Prefer documenting protocol changes in `Documentation/` alongside code changes.

For timing, radio state, routing, queues, packet capacity, or success/failure accounting, add a worst-case test or build-time guard before relying on the path. Keep hardware assumptions aligned across code, docs, and this file.

For difficult DWM3000 bring-up failures, explicitly audit SPI speed transitions and sleep/wake configuration retention before assuming the protocol or RF path is at fault. Both fast/slow SPI ordering and retained sleep configuration have caused hard-to-find behavior where a path works once after reset but fails after sleep or wake.

When modifying versioned documentation in `Documentation/`, increment only the patch component unless the user requests a larger version change. For example, the next edit to `Documentation/UWB+BLE Architecture 0.4.md` becomes `Documentation/UWB+BLE Architecture 0.4.1.md`; do not change `0.4` to `0.5` unless explicitly requested. Cross-reference-only edits do not need a version bump or changelog entry; update the existing document in place unless the content itself changes. Every documentation version bump must also add a short dated changelog entry inside the modified file. This rule applies to all versioned docs:

- `UWB+BLE Architecture X.Y.Z.md`
- `UWB+BLE Protocols and Strategies X.Y.md`
- `Firmware State Machines and Status Report X.Y.md`

For `Documentation/Firmware State Machines and Status Report X.Y.Z.md`, keep diagrams as a high-level reader overview. Mermaid flowchart and state labels should describe system behavior in plain language, not function names, enum/action constants, internal variable names, or low-level implementation shorthand. When a loop represents a bounded time window, state that it is the same continuous window and not a restarted window. Prefer labels such as "Run UWB range with next anchor", "Send result through mesh", and "Mark anchor idle and resume scan" over labels such as `dwm3000_driver_*`, `mesh_start_tracked_tx`, `RANGE_OK`, `success_count++`, or "poll once until deadline". Keep precise source references in the prose above each chart, not inside the chart labels.
