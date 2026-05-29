# IMEC Clicker Firmware

This directory contains the new firmware implementation for the ANNA-B402 + DWM3000 IMEC Clicker/Anchor/Gateway system.

The first layer is a native C library for protocol and state-machine behavior. It is intentionally buildable without Zephyr so message formats, UWB wake/discovery/schedule frames, UWB ranging frames, UWB mesh frames, routing policy, end-to-end gateway acknowledgements, click/self-test reports, self-test gestures, and survey command behavior can be tested before hardware integration.

## Build Native Tests

```sh
cmake -S firmware -B firmware/build
cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure
```

## Firmware Roles

- `clicker`: button-driven user device.
- `anchor`: fixed UWB-gated responder and mesh relay.
- `gateway`: mesh root connected over USB serial.

## Zephyr App

The Zephyr app lives in `firmware/app` and reuses the native core sources. The role builds require the DWM3000 IRQ pin to be present in devicetree. The source overlay maps the DWM3000 IRQ to `P0.02` with `irq-gpios = <&gpio0 2 GPIO_ACTIVE_HIGH>`. Build from a valid Zephyr workspace with the board target for the ANNA-B402/nRF52833 hardware:

```sh
UV_CACHE_DIR=$PWD/.uv-cache uv venv --clear .venv
UV_CACHE_DIR=$PWD/.uv-cache uv pip install --python .venv/bin/python -r zephyr/scripts/requirements.txt -r nrf/scripts/requirements.txt

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-clicker --pristine -- -DFIRMWARE_ROLE=clicker -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-anchor --pristine -- -DFIRMWARE_ROLE=anchor -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-gateway --pristine -- -DFIRMWARE_ROLE=gateway -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
```

`firmware/app/app.overlay` maps the documented SPI, button, battery, RGB LED, and DWM3000 IRQ pins. DWM3000 is attached to nRF52833 SPIM3 so runtime SPI can reach 32 MHz; reset/init paths stay at 2 MHz as required by the DW3000 reset clock limits. The DWM3000 IRQ pin is required by the firmware; there is no bounded SPI-polling runtime mode.

Use a temporary `EXTRA_DTC_OVERLAY_FILE` only when deliberately testing an alternate board pinout. The committed IMEC board path uses `P0.02`.

The current app constants use a 400 ms anchor wake-scan interval, 1 ms UWB wake receive window, 15 ms anchor claim collection window, 6 s periodic anchor UWB mesh RX cadence, 12 s gateway command-result timeout, 430 ms clicker wake train, and 850 kbps DWM3000 data rate for wake, discovery, ranging, and UWB mesh frames. Clickers use sampled UWB politeness, listening for 2 ms every 25 ms until two quiet samples are observed or the 500 ms maximum wait expires; after UWB activity, the busy politeness sample period stretches to 75 ms. Normal clicks also run a clicker-only BLE courtesy advertisement/scan on channel 37 for at least 75 ms during politeness; hearing a higher-precedence peer defers the same attempt briefly. They then apply randomized contention before the wake train: attempt 1 uses 16 slots of 12 ms, attempt 2 uses 32 slots, and attempt 3+ uses 64 slots. Retries add a 150 ms base delay plus a new politeness/courtesy gate and randomized window for the next attempt. Repeated `WAKE_CLAIM` frames use only 0-400 us jitter so exact simultaneous transmitters can drift apart without adding millisecond-scale holes to the long-preamble train.

The 400 ms anchor scan interval deliberately deviates from the 300 ms planning baseline because the Zephyr path currently includes a 2.5 ms wake/settle allowance before RX; the longer interval keeps normal idle wake scanning near the intended 1% DWM3000 awake-time target while preserving wake-train overlap. The command timeout is longer than the periodic anchor mesh RX cadence so a gateway-to-anchor command does not expire before a normally sleeping anchor has a receive opportunity. After a selected `WAKE_CLAIM`, the anchor starts the `DISCOVER` receive window one guard interval before the advertised discovery instant and listens through one guard interval after it so wake-claim TX airtime and host scheduling latency do not make the anchor wake late.
The app has build-time guards for that budget: configured anchor scan plus periodic anchor mesh RX awake time must stay at or below 1% of the periodic idle budget, the clicker wake train must cover at least one complete anchor scan period, and the maximum advertised click epoch must stay within the bounded `WAKE_CLAIM` duration. Runtime scan-duty commands are bounded by the same duty and wake-overlap limits, so they cannot stretch the scan interval beyond the clicker wake train.
Active route/report mesh TX/RX and scheduled ranging add separate awake-time windows; the theoretical budget in `Documentation/UWB+BLE Architecture 0.5.34.md` calculates those under the normalized workload and includes the BLE courtesy detection simulation, and diagnostics expose them for later hardware calibration.

## Hardware Bring-Up Smoke Checklist

Use this checklist on the board with the DWM3000 IRQ wired to `P0.02`. The minimum bench topology is one gateway, one anchor, and one clicker; full normal-click acceptance needs three reachable anchors because the server-side solver accepts three anchor distances and the clicker requires three unique successful ranges.

1. Confirm `firmware/app/app.overlay` contains `irq-gpios = <&gpio0 2 GPIO_ACTIVE_HIGH>`.
2. Build all three roles and verify devicetree accepts the real IRQ GPIO:
   ```sh
   .venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-clicker --pristine -- -DFIRMWARE_ROLE=clicker -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
   .venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-anchor --pristine -- -DFIRMWARE_ROLE=anchor -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
   .venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-gateway --pristine -- -DFIRMWARE_ROLE=gateway -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
   ```
3. Flash each role from the matching build directory, adding board-specific runner arguments when required:
   ```sh
   .venv/bin/west flash --build-dir build/firmware-clicker
   .venv/bin/west flash --build-dir build/firmware-anchor
   .venv/bin/west flash --build-dir build/firmware-gateway
   ```
4. Capture serial logs or gateway output showing the DWM3000 DEV_ID probe, wake/reset path, IRQ-backed TX/RX completion, and return to retained sleep after each active window.
5. Run the clicker self-test gesture. Pass evidence is a diagnostic wake/discovery attempt, a scheduled dud range attempt, and a host-visible self-test result.
6. Run a one-anchor ranging smoke. Pass evidence is a CRC-valid `WAKE_CLAIM`, anchor discovery reply, accepted `RANGE_SCHEDULE`, one selected-clicker DS-TWR exchange, and a gateway-bound UWB mesh report or explicit gateway ACK path.
7. Run a full normal click with three or more anchors when available. Pass evidence is three unique successful DS-TWR results from one burst identity, no success counted from discovery-only anchors, queued reports sent through UWB mesh, and COBS-framed gateway output.
8. Measure the clicker wake train on the real firmware and verify the maximum no-preamble gap between repeated long-preamble `WAKE_CLAIM` frames stays within the protocol target.
9. Measure DS-TWR IRQ timing on the real board: DWM3000 IRQ assertion, firmware RX-good handler entry, and the last point before delayed TX programming. Use that evidence to sweep the fixed equal `UWB_RANGE_REPLY_DELAY_UUS` downward from 900 DWM/DW3000 delayed-TX units and choose the smallest value with zero delayed-TX misses plus margin at 850 kbps.
10. During anchor idle, record wake-scan diagnostics: scans attempted, preambles detected, SFD timeouts, CRC failures, valid claims, false-wake cooldowns, and measured `awake_us` split across startup, PLL, RX, scheduled ranging, and mesh RX/TX where available.
11. Accept the power budget only after measuring both the compile-time periodic idle estimate and the real combined scan plus mesh duty cycle. The periodic scan plus mesh-RX baseline should remain near the intended 1% DWM3000 awake-time target; active mesh traffic and scheduled ranging are empirical tuning results for the final IRQ pin, scan cadence, and mesh cadence.

Record the acceptance evidence in this form before closing hardware validation:

| Gate | Required Evidence | Result |
| --- | --- | --- |
| Final IRQ pin | Real `irq-gpios` in `firmware/app/app.overlay`; clicker, anchor, and gateway builds pass without `IRQ_OVERLAY_ARGS` | Completed 2026-05-17: `P0.02`; `build/firmware-clicker-p002`, `build/firmware-anchor-p002`, and `build/firmware-gateway-p002` |
| DWM3000 bring-up | DEV_ID, wake/reset, IRQ-backed TX/RX completion, and retained sleep return shown in logs | Pending |
| Self-test | Long-press plus short-press gesture emits diagnostic wake/discovery, scheduled dud range, and host-visible self-test result | Pending |
| One-anchor smoke | CRC-valid `WAKE_CLAIM`, discovery reply, accepted `RANGE_SCHEDULE`, one selected-clicker DS-TWR, and gateway-bound UWB mesh report or ACK | Pending |
| Three-anchor click | Three unique completed DS-TWR ranges from one burst identity, no discovery-only success count, queued reports over UWB mesh, and COBS gateway output | Pending |
| Wake train | Measured maximum no-preamble gap between repeated long-preamble `WAKE_CLAIM` frames stays within the protocol target | Pending |
| DS-TWR reply-delay calibration | IRQ-to-handler-to-delayed-TX timing captured; fixed equal reply delay swept downward from 900 DWM/DW3000 delayed-TX units and selected with zero delayed-TX misses plus margin at 850 kbps | Pending |
| Idle diagnostics | Scan, preamble, SFD timeout, CRC failure, claim, false-wake cooldown, mesh packet, and `awake_us` counters captured during idle | Pending |
| Power budget | Measured scan plus periodic mesh RX duty remains near the intended 1% DWM3000 awake-time target on final hardware | Pending |

## Current Implementation Status

Implemented:
- Native packet, TLV, COBS, UWB wake/discovery/schedule/ranging frames, UWB mesh frames, UWB clicker/anchor session helpers, mesh ACK, report, route, status, button, and survey helpers with unit tests. Retired discovery IDs, legacy route beacon IDs, and compact-only UWB frame types are rejected by the shared packet envelope. UWB POLL/RESP/FINAL/REPORT headers carry full initiator/responder IDs, `network_id`, event/session ID, and session nonce for wrong-epoch and wrong-device rejection.
- Zephyr app runtime with clicker/anchor/gateway role selection.
- ANNA-B402/DWM3000 devicetree overlay, including DWM3000 on nRF52833 SPIM3 with 2 MHz reset/init SPI, 32 MHz runtime SPI, and IRQ on `P0.02`.
- Zephyr DWM3000 port layer for SPI, reset, wake, DEV_ID validation, and SDK-compatible SPI/sleep/tick/reset/IRQ hooks. The IRQ GPIO handler only signals waiters and defers any registered SDK callback to workqueue context so the callback may safely use SPI.
- Qorvo DWM3000 decadriver source build integration with a DEV_ID probe through `dwt_readdevid()`.
- IRQ-driven DWM3000 STS-SDC DS-TWR initiator/responder path using native UWB frames, retained sleep/restore between windows, long-preamble wake/discovery mode, fixed equal reply-delay validation with `RANGE_TIMING_INVALID` rejection, and responder windows that ignore unrelated pre-POLL frames without restarting or extending the scheduled slot.
- UWB-gated multi-anchor click path: clicker uses sampled UWB politeness, clicker-only BLE courtesy collision hints on channel 37, randomized contention/retry backoff, sub-millisecond wake-claim jitter, sends `WAKE_CLAIM`, transitions into the wake state, enters discovery by sending `DISCOVER`, accepts `DISCOVERY_REPLY` frames only during that discovery phase, transmits a `RANGE_SCHEDULE`, ranges scheduled anchors sequentially, and requires three unique successful anchors from one burst identity.
- Clicker self-test gesture, DWM3000 wake/reset/DEV_ID probe, diagnostic UWB wake/discovery/scheduled dud range, and best-effort direct UWB mesh `SELF_TEST_REPORT` emission to the gateway.
- Anchor UWB scan gate: anchors run low-duty long-preamble UWB wake scans, treat no-preamble timeouts as idle scans, classify DWM3000 SFD/frame/CRC wake failures for diagnostics, admit only CRC-valid local claims that pass network/channel/epoch checks, arbitrate competing clickers, wake with a guard before the advertised discovery instant, answer in static discovery slots, accept the selected range schedule only after the discovery-reply phase, sleep until scheduled polls, continue listening through unrelated pre-POLL noise inside the same scheduled slot, and return the DWM3000 to retained sleep afterward.
- Mesh relay core with reactive route discovery, gateway ACKs, retry handling, duplicate suppression, UWB mesh frame encode/decode, UWB outbound transport, and periodic UWB mesh RX windows for anchors and the gateway. The legacy BLE mesh frame path and legacy `ROUTE_ADV`/`ROUTE_STATUS` beacon path are removed from the app and native core.
- Zephyr app runtime without a Bluetooth stack dependency; operational wake, discovery, ranging, and mesh forwarding are UWB-only.
- USB CDC serial console with debug-level prototype logs plus COBS-framed gateway packet input/output over the USB-C port.
- Gateway command routing for COBS-wrapped `MSG_COMMAND` packets, including basic anchor ping/status responses, heartbeat start/stop, LED pattern, manual route set/clear, scan-duty updates guarded by duty-cycle and wake-overlap limits, reboot-after-result handling, compile-time role-change policy, and host-visible command failure results.
- Anchor heartbeat reporting with `MSG_ANCHOR_HEARTBEAT` over UWB mesh, gateway ACK, UWB health status bits and route TLVs, optional command interval, and deferral behind active click epochs or tracked mesh TX.
- Survey reachability, gateway reach-report collection, pair planning, automatic prepare/start dispatch, pair prepare, start, abort, initiator/responder DS-TWR loops, and per-sample survey result reports over UWB mesh are wired for software build validation.

Still pending:
- Hardware timing/calibration validation for the IRQ-capable DWM3000 ranging runtime.
- Hardware validation for anchor-to-anchor survey runs.
- Hardware smoke testing on the actual board.
