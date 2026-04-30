# IMEC Clicker Firmware

This directory contains the new firmware implementation for the ANNA-B402 + DWM3000 IMEC Clicker/Anchor/Gateway system.

The first layer is a native C library for protocol and state-machine behavior. It is intentionally buildable without Zephyr so message formats, BLE discovery payloads, UWB ranging frames, routing policy, mesh acknowledgements, click/self-test reports, self-test gestures, and survey command behavior can be tested before hardware integration.

## Build Native Tests

```sh
cmake -S firmware -B firmware/build
cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure
```

## Planned Firmware Roles

- `clicker`: button-driven user device.
- `anchor`: fixed BLE-gated UWB responder and mesh relay.
- `gateway`: mesh root connected over USB serial.

## Zephyr App Skeleton

The Zephyr app lives in `firmware/app` and reuses the native core sources. Build it from a valid Zephyr workspace with the board target for the ANNA-B402/nRF52833 hardware:

```sh
UV_CACHE_DIR=$PWD/.uv-cache uv venv --clear .venv
UV_CACHE_DIR=$PWD/.uv-cache uv pip install --python .venv/bin/python -r zephyr/scripts/requirements.txt -r nrf/scripts/requirements.txt

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-clicker --pristine -- -DFIRMWARE_ROLE=clicker -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-anchor --pristine -- -DFIRMWARE_ROLE=anchor -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-gateway --pristine -- -DFIRMWARE_ROLE=gateway -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
```

`firmware/app/app.overlay` maps the documented SPI, button, battery, and RGB LED pins. DWM3000 is attached to nRF52833 SPIM3 so runtime SPI can reach 32 MHz; reset/init paths stay at 2 MHz as required by the DW3000 reset clock limits. The DWM3000 IRQ pin is not connected in the current architecture pinout, so v1 uses bounded SPI polling during BLE-scheduled UWB windows.

## Current Implementation Status

Implemented:
- Native packet, TLV, COBS, BLE discovery payload, UWB frame, mesh ACK, report, route, status, button, and survey helpers with unit tests.
- Zephyr app skeleton with clicker/anchor/gateway role selection.
- ANNA-B402/DWM3000 devicetree overlay, including DWM3000 on nRF52833 SPIM3 with 2 MHz reset/init SPI and 32 MHz runtime SPI.
- Zephyr DWM3000 port layer for SPI, reset, wake, DEV_ID validation, and SDK-compatible SPI/sleep/tick/reset hooks.
- Qorvo DWM3000 decadriver source build integration with a DEV_ID probe through `dwt_readdevid()`.
- SPI-polled DWM3000 STS-SDC DS-TWR initiator/responder path using the native UWB frames.
- Multi-anchor MVP click path: clicker advertises discovery, collects up to eight READY anchors, sorts them by reciprocal RSSI score, ranges them sequentially, and builds one click report per successful range.
- Clicker self-test gesture, diagnostic BLE advertisement, READY scan, DWM3000 wake/reset/DEV_ID probe, and diagnostic UWB dud range.
- Anchor BLE scan gate: anchors listen with low-duty BLE, advertise READY after a valid discovery request, keep UWB awake for the scheduled responder window, and return the DWM3000 to standby afterward.
- USB CDC serial console with debug-level prototype logs for bring-up over the USB-C port.

Still pending:
- Hardware timing/calibration validation for the SPI-polled DWM3000 ranging runtime.
- Partial-failure reporting for anchors discovered over BLE but not successfully ranged.
- Mesh relay runtime, route advertisements, and gateway ACK processing in live firmware.
- Gateway command dispatch handlers and USB COBS serial packet I/O.
- Hardware smoke testing on the actual board.
