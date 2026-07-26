# IMEC Clicker Firmware — Technical Reference

This document is the detailed technical reference for the firmware implementation. For high-level project overview, navigation, and agent guidelines, start with:

- [../README.md](../README.md) (root)
- [../CODEMAP.md](../CODEMAP.md)
- [../AGENTS.md](../AGENTS.md) (mandatory reading)

All agents must also read [../AGENT_KNOWN_ISSUES.md](../AGENT_KNOWN_ISSUES.md) before making changes.

---

## Native Core Library

The core of the firmware is a platform-independent C library under `firmware/src/` and `firmware/include/`. It is deliberately buildable and testable without Zephyr.

This layer implements:
- Packet formats, TLVs, and COBS framing (`protocol.c`)
- UWB wake, discovery, schedule, and DS-TWR sessions (`uwb_session.c`, `uwb.c`)
- Mesh routing, relay, timing, preemption, and runtime coordination (`mesh*.c`, `route.c`)
- Reports, status, survey, and gateway commands

**Native tests** (recommended before any hardware work):

```sh
cmake -S firmware -B firmware/build
cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure
```

See `firmware/tests/` and the `mesh_integration` suite for higher-fidelity simulator tests.
For connected-routing work, the `protocol_matrix` CTest label runs the focused
Here-I-Am-through-survey, connection-control, and result-custody lifecycle gate.
The deterministic seed sweeps,
sanitizer commands, exact replays, and flash-once hardware workflow are in
[`tests/mesh_integration/README.md`](tests/mesh_integration/README.md).

---

## Zephyr Application

The Zephyr app lives in `firmware/app/`. It reuses the native core and adds:
- Board support, devicetree overlay, and DWM3000 driver/port
- Role-specific orchestration (`app_clicker.c`, `app_anchor.c`, etc.)
- Radio coordination policy, BLE, power management, and high-debug features

**Important hardware assumptions** (status-polled DWM3000, no direct IRQ):
- DWM3000 is on SPIM3 (32 MHz runtime SPI, 2 MHz for reset/init).
- TX/RX completion is detected via bounded `SYS_STATUS` polling.
- Retained sleep is heavily used for power.

The pin mapping is in `firmware/app/app.overlay`.

### Production Build Presets (Preferred)

Use the connected-routing mesh presets. These are the current production-candidate line.

```sh
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 \
  --build-dir build/mesh-clicker -- -DIMEC_BUILD_PRESET=mesh_clicker

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 \
  --build-dir build/mesh-anchor -- -DIMEC_BUILD_PRESET=mesh_anchor

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 \
  --build-dir build/mesh-gateway -- -DIMEC_BUILD_PRESET=mesh_gateway
```

**Deployment flashing** for `mesh_clicker`, `mesh_anchor`, and `mesh_gateway`
uses the verified wrapper only. Do not use
direct `west flash` for these deployable presets.

`mesh_anchor` is one exact artifact for every production anchor. At boot it
maps the nRF FICR `DEVICEID[1:0]` into the IMEC anchor identity domain without
discarding bits. Discovery/ranging order is not compiled into the image; the
gateway assigns it with `CMD_ASSIGN_DISCOVERY_SLOTS`, and an unassigned anchor
does not answer normal click discovery until that assignment is persisted.

```sh
.venv/bin/python firmware/scripts/flash_verified_mesh.py \
  --build-dir build/mesh-clicker \
  --probe-id <probe-id> \
  --stage-only

.venv/bin/python firmware/scripts/capture_stack_evidence.py \
  --build-dir build/mesh-clicker \
  --probe-id <probe-id> \
  --output-dir logs/stack-evidence \
  --duration-seconds 300

.venv/bin/python firmware/scripts/flash_verified_mesh.py \
  --build-dir build/mesh-clicker \
  --hardware-manifest logs/stack-evidence/mesh-clicker-<capture-id>.json \
  --probe-id <probe-id>
```

The first invocation snapshots complete internal flash, stages without reset
at the fixed 4 MHz rate, verifies the sector-erase result by full readback, and
leaves a durable `awaiting_qualification` journal. Run the required real
workload while `capture_stack_evidence.py` observes that exact staged artifact;
additional bounded captures and regressions do not require another flash. The
final invocation rejects a missing, mismatched, stale, or previously consumed
manifest, verifies that the target's code sectors still match the staged
artifact while allowing normal NVS drift, and consumes the capture without
programming the target again. A failed qualification preserves the staged
image and journal for diagnosis and retry. This is not cryptographic probe
attestation; the qualification and provisioning scripts are the executable
source of truth for their accepted arguments and local checks.

**See AGENTS.md for the full list of presets**, including traffic generators, ML collection builds, and legacy regression roles. Always state and verify the exact preset before flashing.

### Other Important Presets

- `gateway_ble_connectivity_test`: Stripped gateway for BLE link bring-up only (no DWM3000, no mesh).
- ML collection builds (`ml_clicker`, `ml_anchor_*`): For training data. See details in the original long-form notes below if needed.
- Various `*_test/` directories contain standalone smoke, power-profile, and range-test applications.

---

## Timing, Power, and Low-Level Details

Current key constants (as of the latest implementation):
- Anchor low-duty scan: 380 ms interval, 3 ms normal RX window (≈0.78–0.79% duty).
- Clicker wake train: 400 ms.
- UWB PHY: 850 kbps, long preamble for wake/range, 1024-symbol for mesh control.
- Clicker politeness: requires ≥100 ms quiet channel-5 time before wake train.
- Status polling overhead is modeled separately from radio duty.

Clicker idle paths:
- Default: retained System ON (`CONFIG_IMEC_CLICKER_SYSTEMON_RETAINED_IDLE`).
- Fallback: System OFF with RAM retention.

Detailed power budget calculations, scan duty guards, and awake-time accounting are in `Documentation/UWB+BLE Architecture 0.6.6.1.md`; current wire behavior is in `Documentation/UWB+BLE Protocols and Strategies 0.3.12.3.md`.

The app enforces build-time and runtime guards so that anchor scan duty stays inside the calibrated budget (currently 13,000 µs/s) unless an explicit debug override is enabled.

---

## Hardware Bring-Up Smoke Checklist

For staged high-debug work, see `Documentation/HARDWARE_BRINGUP_DEBUG.md`.

Use this checklist with no DWM3000 IRQ routed directly to the MCU. Minimum useful topology: one gateway + one anchor + one clicker. Full acceptance requires three unique successful ranges.

1. Confirm `firmware/app/app.overlay` does **not** define `irq-gpios` for the DWM3000 node.
2. Build the production mesh presets and verify the IRQ-free node is accepted.
3. Flash from the matching build directory (see AGENTS.md for exact commands and probe handling).
4. Capture logs showing DWM3000 DEV_ID probe, wake/reset, `SYS_STATUS` polling, and return to retained sleep.
5. Exercise clicker self-test gesture (diagnostic wake + scheduled dud range + visible report).
6. Run one-anchor ranging smoke (valid `WAKE_CLAIM` → discovery → schedule → DS-TWR → mesh delivery or ACK).
7. Run full three-anchor click (three unique successful ranges from one burst, reports delivered over mesh).
8. Measure clicker wake train: max no-preamble gap must stay within protocol target.
9. Measure DS-TWR timing (status-detect to delayed TX) for the long-range preset (`UWB_RANGE_REPLY_DELAY_LONG_RANGE_UUS = 8000`).
10. Record anchor idle diagnostics (scans, preambles, SFD/CRC failures, claims, `awake_us` breakdown).
11. Verify clicker system-off current returns to baseline after full click cycle.
12. Validate power budget against the calibrated RX-duty model.

Acceptance table (update as you complete gates):

| Gate                              | Required Evidence                                      | Result     |
|-----------------------------------|--------------------------------------------------------|------------|
| IRQ-free DWM3000 node             | No `irq-gpios`; mesh presets build cleanly             |            |
| DWM3000 bring-up                  | DEV_ID + wake/reset + polling + retained sleep         |            |
| Self-test                         | Diagnostic sequence + host-visible result              |            |
| One-anchor smoke                  | Full path to mesh report or ACK                        |            |
| Three-anchor click                | Three unique ranges, mesh delivery                     |            |
| Wake train & timing               | Measured gaps and DS-TWR latency within spec           |            |
| Power & idle                      | Duty cycle and current measurements match model        |            |

---

## Current Implementation Status

**Implemented** (core functionality is present and exercised in simulation + unit tests):
- Native protocol, UWB sessions, mesh relay/routing/preemption, reports, survey, and gateway command handling.
- Full click path (wake politeness → claim → discovery → schedule → multi-anchor DS-TWR → mesh report).
- Anchor low-duty scanning, claim arbitration, and retained-sleep behavior.
- Gateway connected BLE GATT (COBS packet service) + command routing.
- Status-polled DWM3000 driver with proper SPI ordering and sleep/wake contracts.
- Comprehensive native + mesh-integration test coverage.

**Still pending / hardware validation required**:
- Full calibration of status-polled DS-TWR reply delays on real hardware.
- Anchor-to-anchor survey runs.
- End-to-end multi-board smoke testing and power measurements on target hardware.
- Any remaining discrepancies between the simulator model and actual radio/SPI timing.

For the most up-to-date status on specific features, cross-reference the latest entries in `Documentation/` (especially the Architecture and Mesh Contract documents) and run the relevant test labels.

---

This document focuses on implementation details and hardware bring-up. For build commands, role semantics, testing gates, and agent rules, **always consult `../AGENTS.md` first**.
