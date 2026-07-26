# Development and Deployment Guide

This file owns volatile build, verification, flashing, and RTT procedures.
`AGENTS.md` owns the safety rules; `Documentation/CURRENT.json` owns the current
document set; each script's `--help` is the executable truth for its arguments.

## Firmware lines

The production-candidate line has exactly three deployable presets:

- `mesh_clicker` is the battery clicker. It sleeps normally, wakes for a
  physical click, ranges anchors, and delivers the report through the connected
  mesh.
- `mesh_anchor` is the common production anchor image. Hardware identity comes
  from FICR; gateway assignment supplies and persists logical order. It ranges
  local clicks, relays traffic, and keeps local click work ahead of transit.
- `mesh_gateway` is the connected-mesh root and BLE host edge.

`mesh_transmitter` and `mesh_transmitter_forcedhop` are powered traffic
generators. The forced-hop image is the only valid source for an anchor-relay
qualification because the generic transmitter may select the gateway directly.
`ml_clicker` and `ml_anchor_1` through `ml_anchor_8` are data-collection images.
Every `ml_anchor_<1-8>` preset has a distinct deterministic device identity and
discovery slot; never substitute one slot's artifact for another board. The ML
clicker owns the BLE PC link, while ML anchors exchange collection traffic over
UWB and do not carry the removed BLE debug-log service.
Generic `FIRMWARE_ROLE=clicker|anchor|gateway`, staged high-debug, and other
bring-up presets are compatibility or diagnostic images, not production truth.

## Environment and repository verification

Create the local Python environment when it is absent:

```sh
UV_CACHE_DIR=$PWD/.uv-cache uv venv --clear .venv
UV_CACHE_DIR=$PWD/.uv-cache uv pip install --python .venv/bin/python \
  -r zephyr/scripts/requirements.txt -r nrf/scripts/requirements.txt \
  -r firmware/tests/requirements-native.txt
```

The default verification command starts from a fresh temporary native build,
runs all repository checks and CTest tests, then executes the deterministic
500-seed busy-line merge gate:

```sh
.venv/bin/python firmware/scripts/verify_changes.py
```

Run the same gate with AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
.venv/bin/python firmware/scripts/verify_changes.py --sanitizers
```

For a Zephyr-facing change, build and statically verify all three exact
production roles as well:

```sh
.venv/bin/python firmware/scripts/verify_changes.py \
  --exact-roles --compatibility-builds
```

The compatibility gate compiles generic clicker/anchor/gateway roles, both mesh
traffic generators, `ml_clicker`, and the first and last deterministic ML
anchor slots. These builds remain regression and collection safeguards; passing
them does not make them production deployment candidates.

`--checks-only` runs the fast source-of-truth, architecture, guidance, and
deployment-policy checks. Use it for documentation-only work, but it does not
qualify firmware behavior.

## Exact role builds

The verification entrypoint above is preferred. These are the underlying
production-candidate builds when one role needs direct iteration:

```sh
.venv/bin/west build --pristine=always --no-sysbuild -s firmware/app \
  -b nrf52833dk/nrf52833 --build-dir build/mesh-clicker \
  -- -DIMEC_BUILD_PRESET=mesh_clicker
.venv/bin/west build --pristine=always --no-sysbuild -s firmware/app \
  -b nrf52833dk/nrf52833 --build-dir build/mesh-anchor \
  -- -DIMEC_BUILD_PRESET=mesh_anchor
.venv/bin/west build --pristine=always --no-sysbuild -s firmware/app \
  -b nrf52833dk/nrf52833 --build-dir build/mesh-gateway \
  -- -DIMEC_BUILD_PRESET=mesh_gateway
```

Bench traffic sources use `IMEC_BUILD_PRESET=mesh_transmitter` or
`IMEC_BUILD_PRESET=mesh_transmitter_forcedhop`. ML slots use
`IMEC_BUILD_PRESET=ml_clicker` or `IMEC_BUILD_PRESET=ml_anchor_<1-8>`.
Generic legacy roles use `-DFIRMWARE_ROLE=clicker|anchor|gateway`.

## Transactional production deployment

Direct `west flash` is never a production deployment path for `mesh_clicker`,
`mesh_anchor`, or `mesh_gateway`. A deployment is one transaction with three
steps, all bound to the exact build directory and probe.

First stage the candidate. This is the transaction's only target programming
step and leaves the candidate running while the repository records
`awaiting_qualification`:

```sh
.venv/bin/python firmware/scripts/flash_verified_mesh.py \
  --build-dir build/mesh-anchor \
  --probe-id E46070D247233537 \
  --stage-only
```

Then capture typed real-operation stack and workload evidence from that staged
artifact. The capture tool runs the fixed TTY-backed RTT workflow and never
flashes:

```sh
.venv/bin/python firmware/scripts/capture_stack_evidence.py \
  --build-dir build/mesh-anchor \
  --probe-id E46070D247233537 \
  --output-dir logs/stack-evidence
```

Finally promote the already-running candidate with the exact trusted capture.
Promotion verifies identity and evidence and does not program the target again:

```sh
.venv/bin/python firmware/scripts/flash_verified_mesh.py \
  --build-dir build/mesh-anchor \
  --hardware-manifest logs/stack-evidence/mesh-anchor-<capture-id>.json \
  --probe-id E46070D247233537
```

The gate fixes pyOCD at 4 MHz and binds the probe, target-reported preset and
build identity, ELF and HEX hashes, transcript hash, capture command, capture
window, typed workload runs, queue/custody completion, and stack policy. A
rejected or interrupted qualification deliberately leaves the staged candidate
available for investigation. This is strong local provenance, not remote or
cryptographic attestation; a malicious host owner can replace local tools and
artifacts. ISR/configuration markers are not runtime stack watermarks, and
marker-only captures or user-authored manifests are rejected as evidence.

## Bench, legacy, and ML flashing

Only nonproduction images may use direct west flashing. For example:

```sh
.venv/bin/west flash --runner pyocd \
  --build-dir build/mesh-transmitter-forcedhop \
  -- --dev-id <probe-id> --frequency 4000000
```

The same form is allowed for an explicitly selected `firmware-*`, `ml-*`, or
staged high-debug build. West receives `--dev-id <probe-id>` after the runner
separator. Direct pyOCD commands use `-u <probe-id>` instead. Keep the frequency
at 4 MHz; a failure at that proven rate is a cabling or access problem, not a
reason to weaken the deployment setting.

## RTT and hardware evidence

State the exact preset, artifact, probe ID, and probe-to-board mapping before
every hardware run, then recheck the mapping if boards or cables move. Startup
capture uses `pyocd rtt -t nrf52833 -M pre-reset -u <probe-id>`, but
`pre-reset` selects the connection sequence and does not itself guarantee that
an already-running target was reset. Perform an explicit verified reset when a
fresh boot identity is required.

`pyocd rtt` needs a TTY. Run it interactively or under `script`; redirecting it
directly can fail with `Inappropriate ioctl for device`. Hardware commands need
direct USB access. If `pyocd list` sees a probe in a sandbox but RTT waits
forever for that same full ID, rerun with host USB access instead of declaring
the probe disconnected. This host's pyOCD has no `list --json`; use its plain
table.

Simulation and native tests supplement this evidence. They never replace
exact-preset builds, probe-role verification, RTT capture, BLE observation, or
a requested multi-board smoke test.
