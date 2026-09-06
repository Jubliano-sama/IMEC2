# IMEC Clicker Firmware — Technical Reference

This document is the detailed technical reference for the firmware implementation. For high-level project overview, navigation, and agent guidelines, start with:

- [Documentation index](../Documentation/README.md)
- [../AGENTS.md](../AGENTS.md) (mandatory reading)

Read the entire [known-issues summary](../AGENT_KNOWN_ISSUES_SUMMARY.md) before work; search the append-only [bug log](../AGENT_KNOWN_ISSUES.md) for relevant history. These logs are evidence, not design requirements.

---

## Native Core Library

The core of the firmware is a platform-independent C library under `firmware/src/` and `firmware/include/`. It is deliberately buildable and testable without Zephyr.

This layer implements:
- Packet formats, TLVs, and COBS framing (`protocol.c`)
- UWB wake, discovery, schedule, and DS-TWR sessions (`uwb_session.c`, `uwb.c`)
- Mesh routing, relay, timing, preemption, and runtime coordination (`mesh*.c`, `route.c`)
- Reports, status, enumeration, and gateway commands

**Native tests** (recommended before any hardware work):

```sh
cmake -S firmware -B firmware/build
cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure
```

See `firmware/tests/` and the `mesh_integration` suite for higher-fidelity simulator tests.
For connected-routing work, the `protocol_matrix` CTest label runs the focused
Here-I-Am-through-enumeration and result-custody lifecycle gate, including retained legacy connection-control coverage. Legacy C9 tests do not define the current C5 production transport.
The deterministic seed sweeps,
sanitizer commands, exact replays, and flash-once hardware workflow are in
[`tests/mesh_integration/README.md`](tests/mesh_integration/README.md).

---

## Zephyr Application

The Zephyr app lives in `firmware/app/`. It reuses the native core and adds:
- Board support, devicetree overlay, and DWM3000 driver/port
- Role-specific orchestration (`app_clicker.c`, `app_anchor.c`, etc.)
- Radio coordination policy, BLE, and power management

**Important hardware assumptions** (status-polled DWM3000, no direct IRQ):
- DWM3000 is on SPIM3 (32 MHz effective runtime SPI, 2 MHz for reset/init and the wake handshake).
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

**Deployment flashing** uses `west flash` for every mesh role:

```sh
PATH="$PWD/.venv/bin:$PATH" .venv/bin/west flash --runner pyocd --build-dir build/mesh-clicker -- --dev-id <probe-id> --frequency 4000000
```

Enumerate live probes and record the selected board and intended role before programming. Authorized bench work may reassign roles; back up durable state and initialize incompatible role storage before migration. Normal sector erase preserves durable configuration. Check static RAM headroom in the build output (the prototype requires more than 4 KiB), then record RTT while exercising the changed behavior. Flashing needs no manifest, capture ledger, or promotion step.

`mesh_anchor` is one image for every production anchor. Its identity comes from the nRF FICR hardware identity; the gateway assigns discovery/ranging order. Verify enumeration, survey, and click delivery through their actual results on the host.

**See AGENTS.md for role semantics** and the application build configuration for available presets. Always state and verify the exact preset before flashing.

### Other Important Presets

- `gateway_ble_connectivity_test`: Stripped gateway for BLE link bring-up only (no DWM3000, no mesh).
- ML collection builds (`ml_clicker`, `ml_anchor_*`): For training data; outside production validation unless requested.
- Various `*_test/` directories contain standalone smoke, power-profile, and range-test applications.

---

## Timing, Power, and Low-Level Details

The [Mesh contract](<../Documentation/Mesh Connected Routing Contract.md>) owns current wake/scan values, survey exclusivity and radio state requirements. Shared numeric bounds live in `include/mesh_radio_timing.h`, `include/dwm3000_timing.h` and `app/src/app_config.h`; avoid copying a second timing table here.

The normal battery clicker uses retained System ON idle, with a configured System OFF alternative. Anchor acquisition is continuous inside each scan slice, with DW3000 parking between idle scans; hardware SNIFF and pulsed acquisition are excluded. An active survey keeps its participating anchors exclusively through all PLAN batches and drains.

The SPI port restores effective 32 MHz after the 2 MHz wake handshake. Shared configuration caching, bounded status polling and successful radio parking are part of that guarantee. CPU/clock/peripheral, GPIO/LED, regulator and DW3000 residency all matter to power. Scan duty is a model input, not measured current or battery life. The current bench has no power instrument.

---

## Hardware Bring-Up Smoke Checklist

Use this checklist with no DWM3000 IRQ routed directly to the MCU. Minimum useful topology: one gateway + one anchor + one clicker. Select evidence appropriate to the configured anchor cohort and changed behavior; the normal schedule supports up to four selected anchors.

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
11. Verify the configured clicker low-power state is restored after the full click cycle.
12. Compare measured timing/state residency with the power model. Current measurement requires an external instrument and remains unperformed when one is unavailable.

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

## Implementation and evidence

The production-candidate source contains the normal click path, C5 report bank, authoritative enumeration, compact survey response lane, BLE command lifecycle and host geometry/CIR processing. [The architecture map](<../Documentation/UWB+BLE Architecture.md>) points to their owners.

Presence in source is not a qualification claim. Run the focused native tests and the mandatory `mesh_integration` and `hardware_models` labels from AGENTS.md before a qualifying flash. Run those two labels sequentially because some fixtures share build directories. Validate changed hardware behavior using the resulting build and live cohort: programming/readback, startup role, assignment, actual survey samples and click host receipts are distinct checks. Dated reports under `Documentation/Reviews/` retain their original scope and do not qualify a later dirty worktree.

---

This document focuses on implementation details and hardware bring-up. For build commands, role semantics, testing gates, and agent rules, **always consult `../AGENTS.md` first**.
