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
- `gateway`: mesh root connected to the PC over a connected Bluetooth GATT link.

## Zephyr App

The Zephyr app lives in `firmware/app` and reuses the native core sources. The role builds assume no DWM3000 IRQ pin is directly available to the MCU. The source overlay maps SPI, reset, and wakeup only; TX/RX completion is detected by bounded `SYS_STATUS` polling over SPI. Build from a valid Zephyr workspace with the board target for the ANNA-B402/nRF52833 hardware:

```sh
UV_CACHE_DIR=$PWD/.uv-cache uv venv --clear .venv
UV_CACHE_DIR=$PWD/.uv-cache uv pip install --python .venv/bin/python -r zephyr/scripts/requirements.txt -r nrf/scripts/requirements.txt

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-clicker --pristine -- -DFIRMWARE_ROLE=clicker -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-anchor --pristine -- -DFIRMWARE_ROLE=anchor -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-gateway --pristine -- -DFIRMWARE_ROLE=gateway -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/gateway_ble_connectivity_test --pristine -- -DIMEC_BUILD_PRESET=gateway_ble_connectivity_test -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
```

The `gateway_ble_connectivity_test` preset is a stripped gateway image for PC-link bring-up only. It initializes the connected BLE gateway service, advertises as `IMEC BLE Gateway Test`, stops primary-channel advertising on channels 37-39 once the PC connects, emits heartbeat/log notifications, and echoes complete COBS packet frames written to the packet RX characteristic back on the packet notification characteristic. It does not initialize DWM3000, UWB mesh, command routing, buttons, LEDs, ADC, USB CDC, or high-debug staged flows.

## ML Data-Collection Builds

The ML data-collection images are explicit presets and do not change the normal `clicker`, `anchor`, or `gateway` role builds. The clicker image exposes the connected gateway BLE packet service from the clicker role, accepts `CMD_ML_START_COLLECTION`, runs fresh UWB discovery for each command, performs production-like DS-TWR against discovered anchors, and streams one diagnostic `MSG_CLICK_REPORT` packet per attempted range sample over BLE. The laptop GUI contract is documented in `Documentation/ML BLE GUI Integration.md`.

```sh
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/ml-clicker --pristine -- -DIMEC_BUILD_PRESET=ml_clicker -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/ml-anchor-1 --pristine -- -DIMEC_BUILD_PRESET=ml_anchor_1 -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/ml-anchor-2 --pristine -- -DIMEC_BUILD_PRESET=ml_anchor_2 -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
```

`ml_anchor_1` through `ml_anchor_8` assign deterministic device IDs and discovery reply slots so fresh discovery can run with multiple anchors without reply-slot collisions. The clicker defaults are 8 samples per selected anchor, up to 4 scheduled anchors, and 8 discovery slots; override those at build time with `CONFIG_IMEC_ML_DEFAULT_SAMPLES_PER_ANCHOR`, `CONFIG_IMEC_ML_MAX_ANCHORS`, or `CONFIG_IMEC_ML_DISCOVERY_SLOT_COUNT`, or override sample count and discovery slot count per BLE command with `TLV_SAMPLE_COUNT` and `TLV_DISCOVERY_SLOT_COUNT`.

ML anchors use the same discovery/ranging responder path as normal anchors, but the explicit ML image uses a 20 ms UWB receive window followed by a 180 ms idle gap, for about a 20/200 ms cycle. LED0 blinks once per second with Li-ion battery status: red means charge immediately, blue means roughly half charge, and green means above 50%. The firmware enables the battery ADC divider MOSFET, waits more than 5 ms for the divided voltage to settle, samples the ADC, and doubles the measured voltage to compensate for the PCB's 2:1 resistor divider.

`firmware/app/app.overlay` maps the documented SPI, button, battery, RGB LED, DWM3000 reset, and DWM3000 wakeup pins. DWM3000 is attached to nRF52833 SPIM3 so runtime SPI can reach 32 MHz; reset/init paths stay at 2 MHz as required by the DW3000 reset clock limits. The DWM3000 IRQ pin is intentionally not configured. The firmware polls `SYS_STATUS` every 50 us while waiting for bounded TX/RX completion events.

Use a temporary `EXTRA_DTC_OVERLAY_FILE` only when deliberately testing an alternate board pinout for the configured SPI, reset, wakeup, button, battery, or LED signals.

The current app constants use a 380 ms anchor wake-scan interval, a 3 ms normal UWB wake receive window, a 5 ms Stage 1 rxproof debug receive window, a 15 ms anchor claim collection window, a 12 s gateway command-result timeout, a 400 ms clicker wake train, and 850 kbps DWM3000 data rate for wake, discovery, ranging, and UWB mesh frames. Clickers require two back-to-back 50 ms channel-5 receive windows with no valid, undecodable, or partial UWB activity before the wake train, bounded by the 500 ms maximum politeness wait. Activity resets the quiet streak and the DWM3000 is rearmed immediately. Exhausting the bound is logged explicitly before click priority proceeds. Normal clicks run the clicker-only BLE courtesy advertisement/scan on channel 37 in parallel with the external DWM3000 channel-5 receive gate for at least 100 ms; hearing a higher-precedence peer defers the same attempt briefly. They then apply randomized contention before the wake train: attempt 1 uses 16 slots of 12 ms, attempt 2 uses 32 slots, and attempt 3+ uses 64 slots. Retries add a 150 ms base delay plus a new politeness/courtesy gate and randomized window for the next attempt. Repeated `WAKE_CLAIM` frames use only 0-400 us jitter so exact simultaneous transmitters can drift apart without adding millisecond-scale holes to the long-preamble train.

Clicker idle has two selectable low-power paths. Normal clicker deployment builds and all high-debug tag/clicker stages default to retained System ON idle (`CONFIG_IMEC_CLICKER_SYSTEMON_RETAINED_IDLE`): the clicker stays booted, parks BLE, LEDs, the battery ADC divider, and DWM3000, puts the DWM3000 into retained sleep, floats DWM3000 SPI/CS/WAKE/RST pins to avoid leakage, disables USB command polling, and wakes from the live button GPIO interrupt. The click button on P0.26 is included in the GPIO `sense-edge-mask`, so Zephyr uses the low-power GPIO SENSE/PORT path for the idle edge interrupt instead of a GPIOTE IN event channel. The first high-debug visible wake marker is LED0 cyan plus LED1 blue at `systemon_button_press`. The explicit fallback/power-profile path is system-off idle (`CONFIG_IMEC_CLICKER_SYSTEMOFF_IDLE`), which requests RAM retention with `CONFIG_IMEC_CLICKER_SYSTEMOFF_RAM_RETENTION` but still wakes through a reset-style boot path. Terminal clicker actions return to the selected idle path. Anchors and gateways do not use this clicker idle path because they must keep duty-cycled UWB receive opportunities available.

The 380 ms anchor scan interval is tuned around wake-train overlap with the 400 ms advertised clicker train. The normal 3 ms RX window is the validated low-duty setting: it is about 0.78% RX-window duty over the full wake/sleep cycle, or 0.79% against the configured 380 ms sleep interval, and about 1.47% conservative DWM3000 awake-time duty if startup and PLL time are charged at the same current. The Stage 1 rxproof preset deliberately retains a 5 ms debug RX slice unless an explicit over-budget debug override is enabled for bench experiments. After a selected `WAKE_CLAIM`, the anchor starts the `DISCOVER` receive window one guard interval before the advertised discovery instant and listens through one guard interval after it so wake-claim TX airtime and host scheduling latency do not make the anchor wake late.
The app has build-time guards for that budget: configured anchor scan RX duty must stay inside the calibrated 13,000 us/s RX budget unless an explicit Stage 1 over-budget debug override is enabled, the clicker wake train must cover the anchor RX-off gap, and the maximum advertised click epoch must stay within the bounded `WAKE_CLAIM` duration. Runtime scan-duty commands are bounded by the same duty and wake-overlap limits, so they cannot stretch the scan interval beyond the clicker wake train.
Active route/report mesh TX/RX and scheduled ranging add separate awake-time windows; the theoretical budget in `Documentation/UWB+BLE Architecture 0.6.5.md` calculates those under the normalized workload and includes packet-age survey timing, the BLE courtesy detection simulation, and diagnostics for later hardware calibration.

Status polling does not change the configured DWM3000 awake windows, but it keeps the MCU and SPI active during bounded UWB waits. The extra daily cost is approximately `MCU_active_current_mA * status_polled_seconds_per_day / 3600`; on the normal 3 ms periodic anchor scan baseline, the conservative awake-time model is about 1270 polled seconds/day, so a 4-6 mA MCU active-current delta adds roughly 1.41-2.12 mAh/day before margin. Any over-budget Stage 1 debug scan is dominated by longer RX windows and must not be extrapolated into production power.

## Hardware Bring-Up Smoke Checklist

For staged high-debug firmware builds with MCUboot/J-Link/USB-C commands and per-stage behavior, see `Documentation/HARDWARE_BRINGUP_DEBUG.md`.

Use this checklist on the board with no DWM3000 IRQ routed directly to the MCU. The minimum bench topology is one gateway, one anchor, and one clicker; full normal-click acceptance needs three reachable anchors because the server-side solver accepts three anchor distances and the clicker requires three unique successful ranges.

1. Confirm `firmware/app/app.overlay` does not define `irq-gpios` for the DWM3000 node.
2. Build all three roles and verify devicetree accepts the IRQ-free DWM3000 node:
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
4. Capture tag/anchor logs or gateway BLE log notifications showing the DWM3000 DEV_ID probe, wake/reset path, `SYS_STATUS`-polled TX/RX completion, and return to retained sleep after each active window.
5. Run the clicker self-test gesture. Pass evidence is a diagnostic wake/discovery attempt, a scheduled dud range attempt, and a host-visible self-test result.
6. Run a one-anchor ranging smoke. Pass evidence is a CRC-valid `WAKE_CLAIM`, anchor discovery reply, accepted `RANGE_SCHEDULE`, one selected-clicker DS-TWR exchange, and a gateway-bound UWB mesh report or explicit gateway ACK path.
7. Run a full normal click with three or four anchors when available. Pass evidence is three unique successful DS-TWR results from one burst identity, no success counted from discovery-only anchors, queued reports sent through UWB mesh, and COBS-framed gateway BLE output.
8. Measure the clicker wake train on the real firmware and verify the maximum no-preamble gap between repeated long-preamble `WAKE_CLAIM` frames stays within the protocol target.
9. Measure DS-TWR polling timing on the real board: RX-good status availability, firmware status-detect latency, and the last point before delayed TX programming. The main firmware now uses the long-range 850 kbps, 4096-symbol, PAC32 PHY with fixed equal `UWB_RANGE_REPLY_DELAY_LONG_RANGE_UUS = 8000` and a 30 ms exchange stride. `UWB_RANGE_REPLY_DELAY_SHORT_RANGE_UUS = 2750` remains a lower-delay candidate. Both presets need recalibration before either is treated as final.
10. During anchor idle, record wake-scan diagnostics: scans attempted, preambles detected, SFD timeouts, CRC failures, valid claims, false-wake cooldowns, and measured `awake_us` split across startup, PLL, RX, scheduled ranging, and mesh RX/TX where available.
11. Measure clicker system-off current before and after one full click. The current must return to the same low-current system-off state after the wake train, BLE courtesy window, ranging/report attempt, or self-test terminal action.
12. Accept the anchor/gateway power budget only after measuring both the compile-time periodic idle estimate and the real combined scan plus mesh duty cycle. The periodic scan plus mesh-RX baseline should remain near the calibrated RX-duty budget and measured awake-time estimate; active mesh traffic and scheduled ranging are empirical tuning results for the status-polling cadence, scan cadence, and mesh cadence.

Record the acceptance evidence in this form before closing hardware validation:

| Gate | Required Evidence | Result |
| --- | --- | --- |
| IRQ-free DWM3000 node | No `irq-gpios` in `firmware/app/app.overlay`; clicker, anchor, and gateway builds pass with status polling | Completed 2026-06-01: `build/firmware-clicker`, `build/firmware-anchor`, and `build/firmware-gateway` |
| DWM3000 bring-up | DEV_ID, wake/reset, `SYS_STATUS`-polled TX/RX completion, and retained sleep return shown in logs | Pending |
| Self-test | Long-press plus short-press gesture emits diagnostic wake/discovery, scheduled dud range, and host-visible self-test result | Pending |
| One-anchor smoke | CRC-valid `WAKE_CLAIM`, discovery reply, accepted `RANGE_SCHEDULE`, one selected-clicker DS-TWR, and gateway-bound UWB mesh report or ACK | Pending |
| Three-anchor click | Three unique completed DS-TWR ranges from one burst identity, no discovery-only success count, queued reports over UWB mesh, and COBS gateway BLE output | Pending |
| Wake train | Measured maximum no-preamble gap between repeated long-preamble `WAKE_CLAIM` frames stays within the protocol target | Pending |
| DS-TWR reply-delay calibration | Status-detect-to-delayed-TX timing captured; short-range and long-range fixed equal reply-delay presets recalibrated with zero delayed-TX misses plus margin on the final firmware timing path | Pending |
| Idle diagnostics | Scan, preamble, SFD timeout, CRC failure, claim, false-wake cooldown, mesh packet, and `awake_us` counters captured during idle | Pending |
| Clicker system-off current | Button-wake clicker returns to the same measured system-off current after a normal click and after self-test terminal actions | Pending |
| Power budget | Measured scan plus periodic mesh RX duty remains near the calibrated RX-duty budget and awake-time estimate on final hardware | Pending |

## Current Implementation Status

Implemented:
- Native packet, TLV, COBS, UWB wake/discovery/schedule/ranging frames, UWB mesh frames, UWB clicker/anchor session helpers, mesh ACK, report, route, status, button, and survey helpers with unit tests. Retired discovery IDs, legacy route beacon IDs, and compact-only UWB frame types are rejected by the shared packet envelope. UWB POLL/RESP/FINAL/REPORT headers carry full initiator/responder IDs, `network_id`, event/session ID, and session nonce for wrong-epoch and wrong-device rejection.
- Zephyr app runtime with clicker/anchor/gateway role selection. Clicker idle defaults to nRF system-off with physical-low button wake; anchor/gateway roles stay in duty-cycled UWB receive loops.
- ANNA-B402/DWM3000 devicetree overlay, including DWM3000 on nRF52833 SPIM3 with 2 MHz reset/init SPI, 32 MHz runtime SPI, reset on `P0.31`, and wakeup on `P0.30`.
- Zephyr DWM3000 port layer for SPI, reset, wake, DEV_ID validation, and SDK-compatible SPI/sleep/tick/reset hooks without any MCU-visible DWM3000 IRQ GPIO.
- Qorvo DWM3000 decadriver source build integration with a DEV_ID probe through `dwt_readdevid()`.
- Status-polled DWM3000 no-STS DS-TWR initiator/responder path using native UWB frames, retained sleep/restore between windows, long-preamble wake/discovery mode, fixed equal reply-delay validation with `RANGE_TIMING_INVALID` rejection, and responder windows that ignore unrelated pre-POLL frames without restarting or extending the scheduled slot.
- UWB-gated multi-anchor click path: clicker uses sampled UWB politeness, clicker-only BLE courtesy collision hints on channel 37, randomized contention/retry backoff, sub-millisecond wake-claim jitter, sends `WAKE_CLAIM`, transitions into the wake state, enters discovery by sending `DISCOVER`, accepts `DISCOVERY_REPLY` frames only during that discovery phase, transmits a `RANGE_SCHEDULE`, ranges scheduled anchors sequentially, and requires three unique successful anchors from one burst identity.
- Clicker self-test gesture, DWM3000 wake/reset/DEV_ID probe, diagnostic UWB wake/discovery/scheduled dud range, and best-effort direct UWB mesh `SELF_TEST_REPORT` emission to the gateway.
- Anchor UWB scan gate: anchors run low-duty long-preamble UWB wake scans, treat no-preamble timeouts as idle scans, classify DWM3000 SFD/frame/CRC wake failures for diagnostics, admit only CRC-valid local claims that pass network/channel/epoch checks, arbitrate competing clickers, wake with a guard before the advertised discovery instant, answer in static discovery slots, accept the selected range schedule only after the discovery-reply phase, sleep until scheduled polls, continue listening through unrelated pre-POLL noise inside the same scheduled slot, and return the DWM3000 to retained sleep afterward.
- Mesh relay core with reactive route discovery, gateway ACKs, retry handling, duplicate suppression, UWB mesh frame encode/decode, UWB outbound transport, and periodic UWB mesh RX windows for anchors and the gateway. The legacy BLE mesh frame path and legacy `ROUTE_ADV`/`ROUTE_STATUS` beacon path are removed from the app and native core.
- Operational wake, discovery, ranging, and mesh forwarding are UWB-owned. Clickers still use a clicker-only BLE courtesy side channel for collision hints, while gateways use a separate connected BLE GATT service for the PC edge.
- Gateway connected BLE GATT service with COBS-framed packet RX/TX characteristics and a separate debug-log notification characteristic. App-side USB CDC gateway communication is removed.
- Gateway command routing for COBS-wrapped `MSG_COMMAND` packets received over the BLE packet stream, including basic anchor ping/status responses, heartbeat start/stop, LED pattern, manual route set/clear, scan-duty updates guarded by duty-cycle and wake-overlap limits, reboot-after-result handling, compile-time role-change policy, and host-visible command failure results.
- Stripped `gateway_ble_connectivity_test` preset for PC BLE link validation without DWM3000, UWB mesh, command routing, GPIO, ADC, USB CDC, or high-debug staged runtime.
- Anchor heartbeat reporting with `MSG_ANCHOR_HEARTBEAT` over UWB mesh, gateway ACK, UWB health status bits and route TLVs, optional command interval, and deferral behind active click epochs or tracked mesh TX.
- Survey reachability, gateway reach-report collection, pair planning, automatic prepare/start dispatch, pair prepare, start, abort, initiator/responder DS-TWR loops, and per-sample survey result reports over UWB mesh are wired for software build validation.

Still pending:
- Hardware timing/calibration validation for the status-polled DWM3000 ranging runtime.
- Hardware validation for anchor-to-anchor survey runs.
- Hardware smoke testing on the actual board.
