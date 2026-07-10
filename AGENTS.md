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

### Firmware Lines and Role Meaning

The connected-routing mesh line is the production successor and will become
the main firmware after the migration is complete. Treat its role presets as
the default target for new product behavior even while their names still carry
the `mesh_` prefix:

- `mesh_clicker`: normal battery clicker behavior. It sleeps normally, wakes
  for a physical click/range sequence, and uses the connected-routing mesh path
  for delivery. It is not a continuously active test transmitter.
- `mesh_anchor_1` through `mesh_anchor_5`: anchor identities for the same
  connected-routing firmware. An anchor ranges local clicks, relays mesh work,
  and prioritizes its own click reports over transit traffic.
- `mesh_gateway`: gateway role for the same connected-routing firmware,
  including gateway BLE ingress/egress and highest-priority gateway commands.

The remaining build lines are not alternative production architectures:

- `mesh_transmitter` and `mesh_transmitter_forcedhop` are synthetic traffic
  generators used to load and regression-test the production-successor mesh
  path. They must not be treated as deployable anchor firmware.
- `ml_clicker` and `ml_anchor_1` through `ml_anchor_8` are demo/data-collection
  images for gathering training and validation data for a distance-offset
  compensation model. They are not production clicker or anchor builds.
- Direct `FIRMWARE_ROLE=clicker|anchor|gateway` builds and staged high-debug
  presets are legacy or bring-up compatibility images. Maintain them for
  bounded regression coverage, but do not add new mainline behavior there
  unless the task explicitly targets a legacy build.

Until the production-successor presets are renamed, always state and verify the
exact preset and probe-to-board mapping before flashing; do not infer behavior
from the generic role name alone.

Production-candidate mesh role builds:

```sh
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/mesh-clicker -- -DIMEC_BUILD_PRESET=mesh_clicker
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/mesh-anchor-1 -- -DIMEC_BUILD_PRESET=mesh_anchor_1
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/mesh-gateway -- -DIMEC_BUILD_PRESET=mesh_gateway
```

Legacy regression role builds:

```sh
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-clicker -- -DFIRMWARE_ROLE=clicker
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-anchor -- -DFIRMWARE_ROLE=anchor
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-gateway -- -DFIRMWARE_ROLE=gateway
```

### Firmware role meaning

- `mesh_clicker`, `mesh_anchor_<slot>`, and `mesh_gateway` are the active
  production-candidate firmware line and the source of truth for behavior that
  will become the main firmware.
- Plain `clicker`, `anchor`, and `gateway` role builds are legacy compatibility
  and regression images. Keep them building, but do not use them to infer the
  current mesh runtime contract.
- `mesh_transmitter` and `mesh_transmitter_forcedhop` are powered bench traffic
  sources for route, retry, and preemption regression tests; they are not a
  deployed product role.
- `ml_clicker` and `ml_anchor_<slot>` are demo/data-collection images for a
  distance-offset compensation model. ML-specific behavior is not the main
  product contract.

ML collection builds:

```sh
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/ml-clicker -- -DIMEC_BUILD_PRESET=ml_clicker
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/ml-anchor-1 -- -DIMEC_BUILD_PRESET=ml_anchor_1
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/ml-anchor-2 -- -DIMEC_BUILD_PRESET=ml_anchor_2
```

For deterministic ML anchors, replace `build/ml-anchor-2` and `ml_anchor_2` with the anchor slot being programmed, such as `build/ml-anchor-3` / `ml_anchor_3` through `build/ml-anchor-8` / `ml_anchor_8`. Each preset assigns a unique `DEVICE_ID` and deterministic discovery slot.

Flash the exact build directory for the image you intend to program; do not rely on west's previous build context:

```sh
.venv/bin/west flash --runner pyocd --build-dir build/mesh-clicker -- --frequency 4000000
.venv/bin/west flash --runner pyocd --build-dir build/mesh-anchor-1 -- --frequency 4000000
.venv/bin/west flash --runner pyocd --build-dir build/mesh-gateway -- --frequency 4000000
```

The following flash commands are only for legacy regression and ML collection
images; do not use them for connected-routing hardware tests:

```sh
.venv/bin/west flash --runner pyocd --build-dir build/firmware-clicker -- --frequency 4000000
.venv/bin/west flash --runner pyocd --build-dir build/firmware-anchor -- --frequency 4000000
.venv/bin/west flash --runner pyocd --build-dir build/firmware-gateway -- --frequency 4000000
.venv/bin/west flash --runner pyocd --build-dir build/ml-clicker -- --frequency 4000000
.venv/bin/west flash --runner pyocd --build-dir build/ml-anchor-1 -- --frequency 4000000
```

With more than one probe attached, pass the probe ID to west after the runner
separator: `-- --dev-id <probe-id> --frequency 4000000`. The shorter
`-u <probe-id>` form belongs to direct `pyocd` commands such as RTT and must not
be used as a west-flash argument.

For deterministic ML anchors, replace `build/ml-anchor-1` with the anchor image being programmed, such as `build/ml-anchor-2` through `build/ml-anchor-8`.

When more than one probe is connected, select the probe through west's pyOCD
runner with `--dev-id`, for example:

```sh
.venv/bin/west flash --runner pyocd --build-dir build/mesh-anchor-1 -- --dev-id E46070D247233537 --frequency 4000000
```

Do not pass `-u <probe-id>` to `west flash`; `-u` is used by direct `pyocd rtt`
commands and the west pyOCD runner rejects it. Keep the probe selector after
west's `--` separator as `--dev-id <probe-id>`.

Flashing at 4MHz has been proven to work, if a flash fails, assume the cabling is at fault and do not reduce flash speed.

When capturing RTT logs for startup or boot behavior, use pyOCD's `pre-reset` connect mode so the capture includes reset-time output, for example `pyocd rtt -t nrf52833 -M pre-reset -u <probe-id>`. `pyocd rtt` needs a TTY; run it interactively or under `script`, and do not redirect its stdout directly to a file because that can fail with `Inappropriate ioctl for device`.

Hardware RTT and flash commands must run with direct USB device access. A
sandboxed command can misleadingly show both probes in `pyocd list` yet leave
`pyocd rtt` waiting forever for the same probe ID because the RTT subprocess
cannot access the USB device. If that exact mismatch occurs, rerun the hardware
command with full host/USB permissions; do not treat it as a disconnected probe
or as firmware evidence.

## Coding Style & Naming Conventions

Code is C using Zephyr conventions: 4-space indentation, braces on the same line for functions/control blocks, and `snake_case` for functions and variables. Do not add project-wide prefixes such as `IMEC_` to new identifiers; use descriptive module-scoped names and the existing protocol/route/status naming style. Keep hardware-independent logic in `firmware/src`; keep Zephyr, GPIO, BLE, and SPI code in `firmware/app`.

## Testing Guidelines

Add native tests for protocol/state behavior before relying on hardware. Keep tests focused and named after the module under test, such as `test_protocol.c` or `test_survey.c`. Run `ctest --test-dir firmware/build --output-on-failure` before submitting changes. For Zephyr-facing changes, build all three roles.

## Commit & Pull Request Guidelines

Use clear imperative commit subjects, for example `Add BLE-gated anchor ranging MVP`. PRs should include a short summary, affected roles (`clicker`, `anchor`, `gateway`), test/build commands run, and any hardware assumptions or smoke-test gaps. After a bug is fixed, always commit your work, even if the bug is only partially fixed.

## Agent-Specific Instructions

Do not edit imported dependency trees unless the task explicitly targets them. Prefer documenting protocol changes in `Documentation/` alongside code changes.

The user grants standing permission to refactor when needed to prevent recurring bugs. Prefer explicit state ownership and timing-phase boundaries over accumulating local patches, while preserving contract behavior, role behavior, LEDs, tests, and required hardware checks.

For timing, radio state, routing, queues, packet capacity, or success/failure accounting, add a worst-case test or build-time guard before relying on the path. Keep hardware assumptions aligned across code, docs, and this file.

Before changing mesh routing, channel 5/channel 9 scheduling, DWM3000 sleep/idle behavior in mesh roles, ACK retry, route discovery, wake-train semantics, blind flooding, or click preemption, read `Documentation/Mesh Connected Routing Contract.md`. Treat it as the high-level design contract, state which invariants the change preserves, and update it alongside any intentional design change. Do not push through a change that contradicts the contract without explicit user permission; first generate a clear list of the new behavior, affected roles, changed or removed invariants, and required tests or hardware checks.

For difficult DWM3000 bring-up failures, explicitly audit SPI speed transitions and sleep/wake configuration retention before assuming the protocol or RF path is at fault. Both fast/slow SPI ordering and retained sleep configuration have caused hard-to-find behavior where a path works once after reset but fails after sleep or wake.

When modifying versioned documentation in `Documentation/`, increment only the patch component unless the user requests a larger version change. For example, the next edit to `Documentation/UWB+BLE Architecture 0.4.md` becomes `Documentation/UWB+BLE Architecture 0.4.1.md`; do not change `0.4` to `0.5` unless explicitly requested. Cross-reference-only edits do not need a version bump or changelog entry; update the existing document in place unless the content itself changes. Every documentation version bump must also add a short dated changelog entry inside the modified file. This rule applies to all versioned docs:

- `UWB+BLE Architecture X.Y.Z.md`
- `UWB+BLE Protocols and Strategies X.Y.md`
- `Firmware State Machines and Status Report X.Y.md`

For `Documentation/Firmware State Machines and Status Report X.Y.Z.md`, keep diagrams as a high-level reader overview. Mermaid flowchart and state labels should describe system behavior in plain language, not function names, enum/action constants, internal variable names, or low-level implementation shorthand. When a loop represents a bounded time window, state that it is the same continuous window and not a restarted window. Prefer labels such as "Run UWB range with next anchor", "Send result through mesh", and "Mark anchor idle and resume scan" over labels such as `dwm3000_driver_*`, `mesh_start_tracked_tx`, `RANGE_OK`, `success_count++`, or "poll once until deadline". Keep precise source references in the prose above each chart, not inside the chart labels.
