# High-Debug Hardware Bring-Up Firmware

Date: 2026-06-01

This document describes the staged high-debug firmware suite for real DWM3000 plus ANNA-B402/nRF52833 hardware. Hardware validation is pending until real bench logs are captured.

The existing production roles remain `clicker`, `anchor`, and `gateway`. In the high-debug presets, `tag` is only a bench-build alias for the existing `clicker` role.

The DWM3000 IRQ policy is unchanged: no DWM3000 IRQ GPIO is directly available to the MCU, no `irq-gpios` are added, and TX/RX completion is detected through bounded `SYS_STATUS` polling over SPI.

## Common High-Debug Policy

All stage presets enable:

- `CONFIG_IMEC_HIGH_DEBUG=y`
- `CONFIG_IMEC_BENCH_STAGE=<0|1|2|3>`
- one role selector: `CONFIG_IMEC_ROLE_TAG`, `CONFIG_IMEC_ROLE_ANCHOR`, or `CONFIG_IMEC_ROLE_GATEWAY`
- `CONFIG_IMEC_USB_BOOTLOADER=y`
- CDC logs and command input for tag/anchor presets
- RTT logs for all high-debug presets
- gateway binary CDC output kept separate from human logs in `gateway_stage3_highdebug`

The UWB protocol policy is unchanged:

- Channel 5 is used for wake, discovery, route contact, and ranging.
- Channel 9 is reserved for negotiated payload/mesh events after contact timing exists.
- PHY remains 850 kbps, 1024-symbol preamble, PAC8, STS disabled.
- `UWB_RANGE_REPLY_DELAY_UUS` remains fixed at 900.
- Normal click ranging still requires at least three eligible anchor replies.
- Up to six anchors may be scheduled in a normal-click burst.
- Three unique `RANGE_OK` anchors from the same click event and burst accept the click.

High-debug log lines use this stable prefix:

```text
[uptime_ms][role][device_id][stage][event] key=value key=value
```

The boot banner logs `BOOT_START`, `BOOT_CONFIG`, `USB_READY`, and `BOOTLOADER_READY`, including git/build identity, role, stage, board, device ID, network ID, UWB channels, SPI speed, SYS_STATUS polling, USB bootloader state, CDC log state, RTT log state, and gateway binary CDC state.

Counters are dumped periodically and through `dump_counters`. They include DWM3000 DEV_ID results, SYS_STATUS polling, RX/TX results, wake claims, discovery, schedules, DS-TWR, mesh, gateway packets, USB, bootloader requests, and command results. Boot count is currently volatile unless persistent storage is added later.

## Build Matrix

All presets are built from one source tree. Build output directories are named after the preset, and signed application images are generated at:

```text
build/<preset>/app/zephyr/zephyr.signed.bin
build/<preset>/app/zephyr/zephyr.signed.hex
```

| Preset | Role | Stage | Purpose |
| --- | --- | --- | --- |
| `tag_stage0_highdebug` | tag/clicker | 0 | Single tag diagnostics, no anchor required |
| `tag_stage1_highdebug` | tag/clicker | 1 | One tag plus one anchor bench ranging |
| `anchor_stage1_highdebug` | anchor | 1 | One-anchor low-duty wake scan and DS-TWR responder |
| `tag_stage2_highdebug` | tag/clicker | 2 | Multi-anchor production-threshold burst debug |
| `anchor_stage2_highdebug` | anchor | 2 | Multi-anchor scheduled responder debug |
| `tag_stage3_highdebug` | tag/clicker | 3 | Multi-anchor click path with gateway correlation fields |
| `anchor_stage3_highdebug` | anchor | 3 | Report queue and UWB mesh delivery debug |
| `gateway_stage3_highdebug` | gateway | 3 | Gateway mesh, ACK, command, and binary USB output debug |

## Build Commands

Run commands from the repository root:

```sh
cd /home/tommie/Projects/IMEC2
```

The sysbuild MCUboot path in this workspace needs the nRF module path and the local `imgtool` shim on `PATH`. The high-debug commands below include that environment prefix inline.

Clean native test build:

```sh
cmake -S firmware -B firmware/build
cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure
```

Clean high-debug tag/clicker builds:

```sh
env ZEPHYR_NRF_MODULE_DIR=$PWD/nrf PATH=$PWD/.venv/bin:$PWD/firmware/app/scripts:$PATH .venv/bin/west build --sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/tag_stage0_highdebug --pristine -- -DIMEC_BUILD_PRESET=tag_stage0_highdebug -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
env ZEPHYR_NRF_MODULE_DIR=$PWD/nrf PATH=$PWD/.venv/bin:$PWD/firmware/app/scripts:$PATH .venv/bin/west build --sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/tag_stage1_highdebug --pristine -- -DIMEC_BUILD_PRESET=tag_stage1_highdebug -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
env ZEPHYR_NRF_MODULE_DIR=$PWD/nrf PATH=$PWD/.venv/bin:$PWD/firmware/app/scripts:$PATH .venv/bin/west build --sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/tag_stage2_highdebug --pristine -- -DIMEC_BUILD_PRESET=tag_stage2_highdebug -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
env ZEPHYR_NRF_MODULE_DIR=$PWD/nrf PATH=$PWD/.venv/bin:$PWD/firmware/app/scripts:$PATH .venv/bin/west build --sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/tag_stage3_highdebug --pristine -- -DIMEC_BUILD_PRESET=tag_stage3_highdebug -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
```

Clean high-debug anchor builds:

```sh
env ZEPHYR_NRF_MODULE_DIR=$PWD/nrf PATH=$PWD/.venv/bin:$PWD/firmware/app/scripts:$PATH .venv/bin/west build --sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/anchor_stage1_highdebug --pristine -- -DIMEC_BUILD_PRESET=anchor_stage1_highdebug -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
env ZEPHYR_NRF_MODULE_DIR=$PWD/nrf PATH=$PWD/.venv/bin:$PWD/firmware/app/scripts:$PATH .venv/bin/west build --sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/anchor_stage2_highdebug --pristine -- -DIMEC_BUILD_PRESET=anchor_stage2_highdebug -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
env ZEPHYR_NRF_MODULE_DIR=$PWD/nrf PATH=$PWD/.venv/bin:$PWD/firmware/app/scripts:$PATH .venv/bin/west build --sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/anchor_stage3_highdebug --pristine -- -DIMEC_BUILD_PRESET=anchor_stage3_highdebug -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
```

Clean high-debug gateway build:

```sh
env ZEPHYR_NRF_MODULE_DIR=$PWD/nrf PATH=$PWD/.venv/bin:$PWD/firmware/app/scripts:$PATH .venv/bin/west build --sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/gateway_stage3_highdebug --pristine -- -DIMEC_BUILD_PRESET=gateway_stage3_highdebug -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
```

Production role build check:

```sh
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-clicker --pristine -- -DFIRMWARE_ROLE=clicker -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-anchor --pristine -- -DFIRMWARE_ROLE=anchor -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-gateway --pristine -- -DFIRMWARE_ROLE=gateway -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
```

## Anchor ID Builds

Build anchors with unique device IDs and slots using CMake cache overrides. Keep the same source tree:

```sh
env ZEPHYR_NRF_MODULE_DIR=$PWD/nrf PATH=$PWD/.venv/bin:$PWD/firmware/app/scripts:$PATH .venv/bin/west build --sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/anchor_stage2_highdebug_a1 --pristine -- -DIMEC_BUILD_PRESET=anchor_stage2_highdebug -DIMEC_DEVICE_ID=0x2222000000000001ull -DIMEC_ANCHOR_SLOT=1 -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
env ZEPHYR_NRF_MODULE_DIR=$PWD/nrf PATH=$PWD/.venv/bin:$PWD/firmware/app/scripts:$PATH .venv/bin/west build --sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/anchor_stage2_highdebug_a2 --pristine -- -DIMEC_BUILD_PRESET=anchor_stage2_highdebug -DIMEC_DEVICE_ID=0x2222000000000002ull -DIMEC_ANCHOR_SLOT=2 -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
env ZEPHYR_NRF_MODULE_DIR=$PWD/nrf PATH=$PWD/.venv/bin:$PWD/firmware/app/scripts:$PATH .venv/bin/west build --sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/anchor_stage2_highdebug_a3 --pristine -- -DIMEC_BUILD_PRESET=anchor_stage2_highdebug -DIMEC_DEVICE_ID=0x2222000000000003ull -DIMEC_ANCHOR_SLOT=3 -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
```

Use the same pattern for Stage 3 anchors by replacing `anchor_stage2_highdebug` with `anchor_stage3_highdebug`.

## First Flash With J-Link

The first upload should use J-Link/nrfjprog so MCUboot and the application are both installed. J-Link remains the recovery path.

```sh
.venv/bin/west flash --runner nrfjprog --build-dir build/tag_stage0_highdebug
.venv/bin/west flash --runner nrfjprog --build-dir build/tag_stage1_highdebug
.venv/bin/west flash --runner nrfjprog --build-dir build/anchor_stage1_highdebug
.venv/bin/west flash --runner nrfjprog --build-dir build/tag_stage2_highdebug
.venv/bin/west flash --runner nrfjprog --build-dir build/anchor_stage2_highdebug
.venv/bin/west flash --runner nrfjprog --build-dir build/tag_stage3_highdebug
.venv/bin/west flash --runner nrfjprog --build-dir build/anchor_stage3_highdebug
.venv/bin/west flash --runner nrfjprog --build-dir build/gateway_stage3_highdebug
```

For multiple identical boards attached at once, add the runner serial number supported by your local Zephyr runner, for example `--dev-id <JLINK_SERIAL>` after the `flash` command.

## USB-C MCUboot Update

The high-debug sysbuild uses MCUboot serial recovery with a single application slot. After first J-Link flash, use USB-C for later updates:

1. Put the board in MCUboot serial recovery.
   - Tag and anchor high-debug images: open the CDC console and send `bootloader`.
   - Gateway Stage 3: CDC is reserved for binary COBS gateway output, so use reset into the MCUboot wait-for-DFU window or J-Link recovery. Human gateway logs use RTT by default.
2. Upload the signed application image.
3. Reset the board.

Example for a tag image:

```sh
mcumgr --conntype serial --connstring "dev=/dev/ttyACM0,baud=115200" image upload build/tag_stage0_highdebug/app/zephyr/zephyr.signed.bin
mcumgr --conntype serial --connstring "dev=/dev/ttyACM0,baud=115200" reset
```

Example for an anchor image:

```sh
mcumgr --conntype serial --connstring "dev=/dev/ttyACM0,baud=115200" image upload build/anchor_stage2_highdebug/app/zephyr/zephyr.signed.bin
mcumgr --conntype serial --connstring "dev=/dev/ttyACM0,baud=115200" reset
```

Example for the gateway image:

```sh
mcumgr --conntype serial --connstring "dev=/dev/ttyACM0,baud=115200" image upload build/gateway_stage3_highdebug/app/zephyr/zephyr.signed.bin
mcumgr --conntype serial --connstring "dev=/dev/ttyACM0,baud=115200" reset
```

If serial recovery cannot be entered or the image will not boot, recover with J-Link:

```sh
.venv/bin/west flash --runner nrfjprog --build-dir build/<preset>
```

## USB CDC And RTT Logs

Tag and anchor high-debug presets use CDC for human logs and the simple command parser:

```sh
python3 -m serial.tools.miniterm /dev/ttyACM0 115200
```

Supported tag/anchor commands:

```text
status
dump_counters
uwb_probe
uwb_sleep
uwb_wake
send_wake_claim_once
send_wake_train
reboot
bootloader
```

`gateway_stage3_highdebug` uses CDC for binary COBS gateway packets and RTT for human logs. Do not mix human text with the gateway binary CDC endpoint.

Open RTT logs with your local J-Link RTT tool, for example:

```sh
JLinkRTTViewer
```

## Stage 0: Single Tag Only

Build output: `tag_stage0_highdebug`

Purpose: verify one tag/clicker board without an anchor.

Behavior:

- Boots MCUboot plus the tag application.
- Initializes USB, logs, GPIO, and DWM3000 reset/wake/SPI.
- Reads and logs DWM3000 DEV_ID, switches to runtime SPI speed, then parks the radio in a safe state.
- Does not require anchor discovery for the board to be considered alive.
- Short button press emits a local simulated click, blinks LEDs, and logs `RANGE_OK` with `BENCH_ONLY simulated=1`.
- Optional wake-claim train on short press is controlled by `CONFIG_IMEC_STAGE0_SEND_WAKE_CLAIM_ON_CLICK`.
- Long-press plus confirm runs reset/wake/probe/sleep/wake diagnostics.
- CDC commands can probe, sleep/wake, send wake-claim frames, reboot, and request MCUboot.

Expected snippets:

```text
[00000012][tag][0x1111111111111111][0][BOOT_START] preset=tag_stage0_highdebug ...
[00000013][tag][0x1111111111111111][0][BOOT_CONFIG] ... sys_status_polling=1 usb_bootloader=1 usb_cdc_logs=1
[00000020][tag][0x1111111111111111][0][DWM_DEV_ID_READ] spi_hz=2000000
[00000023][tag][0x1111111111111111][0][DWM_DEV_ID_OK] dev_id=0x...
[00001000][tag][0x1111111111111111][0][RANGE_OK] BENCH_ONLY simulated=1 event_seq=...
```

Acceptance remains hardware validation pending until a board enumerates over USB-C, logs the DEV_ID result, responds to button input, completes sleep/wake diagnostics, and enters MCUboot from command or documented reset recovery.

## Stage 1: One Tag Plus One Anchor

Build outputs:

- `tag_stage1_highdebug`
- `anchor_stage1_highdebug`

Purpose: verify UWB wake, discovery, and single-anchor DS-TWR on the bench.

Behavior:

- Anchor runs the low-duty UWB wake scan and logs scan aperture start/end, startup/PLL, RX enable, preamble/frame/CRC outcome, and false-wake cooldown.
- A valid `WAKE_CLAIM` logs network, channel, epoch, clicker, event, attempt, priority, nonce, and accept/reject reason.
- Stage 1 enables `CONFIG_IMEC_STAGE1_ALLOW_SINGLE_ANCHOR_RANGE=y`.
- The one-anchor path is logged as `BENCH_ONLY`; it does not change production normal-click acceptance.
- Tag short press sends the wake train, discovery, accepts one discovery reply in bench mode, sends one schedule, runs DS-TWR, and logs distance/status/quality.
- Identity, nonce, event, anchor ID, reply-delay, and schedule validation remain active.

Expected snippets:

```text
[00000420][anchor][0x2222222222222222][1][UWB_RX_START] mode=wake_scan ...
[00000750][anchor][0x2222222222222222][1][WAKE_CLAIM_ACCEPT] clicker=0x... event_seq=... nonce=0x...
[00000800][tag][0x1111111111111111][1][DISCOVERY_REPLY_RX] anchor=0x... BENCH_ONLY allow_single_anchor=1
[00000900][tag][0x1111111111111111][1][DS_TWR_POLL_TX] anchor=0x...
[00000950][tag][0x1111111111111111][1][RANGE_OK] anchor=0x... distance_mm=... quality=...
```

Failure reasons should name the failed stage, such as `NO_DISCOVERY_REPLY`, `SCHEDULE_TX_FAIL`, `POLL_TX_FAIL`, `RESP_TIMEOUT`, `FINAL_TX_FAIL`, `REPORT_TIMEOUT`, `IDENTITY_REJECT`, `TIMING_REJECT`, or `RADIO_ERROR`.

## Stage 2: One Tag Plus Multiple Anchors

Build outputs:

- `tag_stage2_highdebug`
- `anchor_stage2_highdebug`

Purpose: verify the production-style discovery threshold and serialized scheduled burst ranging.

Behavior:

- Tag requires at least three eligible discovery replies before normal-click burst ranging starts.
- Tag schedules up to six anchors.
- Scheduled anchors remain in the same continuous responder burst window.
- DS-TWR remains addressed and serialized, not parallel.
- Anchor logs discovery slot ID, anchor ID, reply decision, schedule received/rejected, poll matched/wrong-target/ignored, range status, and report queued.
- Tag prints discovery, schedule, and range tables in machine-greppable logs.
- Final click decision logs accepted/retry/fail, unique `RANGE_OK` count, attempt, and burst ID.
- Fewer than three anchors does not start ranging. Zero replies retry/fail without release; one or two replies send release, then retry/fail.
- Wrong-target polls are ignored by anchors without ending the continuous burst.

Expected snippets:

```text
[00001000][tag][0x1111111111111111][2][DISCOVER_TX] event_seq=... attempt=...
[00001020][tag][0x1111111111111111][2][DISCOVERY_REPLY_RX] anchor=0x... slot=1 accepted_for_schedule=1
[00001080][tag][0x1111111111111111][2][RANGE_SCHEDULE_TX] burst_id=... anchors=3
[00001120][anchor][0x2222000000000001][2][DS_TWR_POLL_RX] matched=1 round=0 sample=0
[00001200][tag][0x1111111111111111][2][RANGE_OK] anchor=0x2222000000000001 distance_mm=...
[00001300][tag][0x1111111111111111][2][RANGE_OK] final_decision=accepted unique_range_ok_count=3 burst_id=...
```

## Stage 3: One Tag Plus Multiple Anchors Plus Gateway

Build outputs:

- `tag_stage3_highdebug`
- `anchor_stage3_highdebug`
- `gateway_stage3_highdebug`

Purpose: verify report delivery, UWB mesh, gateway ACKs, USB gateway output, and command/debug path.

Behavior:

- Tag keeps the Stage 2 click/range behavior and logs clicker/tag ID, click event, attempt, burst ID, anchor ID, sample index, and round.
- Anchor queues reports after ranging, drains them through UWB mesh, waits for gateway ACK before considering a report delivered, and gives active click service priority over mesh traffic.
- Gateway boots as gateway, receives mesh reports, emits binary COBS packets on CDC, logs human-readable diagnostics over RTT, and routes commands.
- Gateway logs boot/config, route requests/replies, mesh RX/TX, gateway ACK TX, report decode, USB packet output, command RX, command routing, and command result/timeout.
- Gateway command hooks cover ping anchor, get anchor status, trigger LED pattern, start/stop heartbeat, dump route table, clear route, and dump counters through the existing command path.

Expected snippets:

```text
[00000020][gateway][0x3333333333333333][3][BOOT_CONFIG] ... usb_cdc_logs=0 rtt_logs=1 gateway_binary_cdc=1
[00002000][anchor][0x2222000000000001][3][ANCHOR_REPORT_QUEUE] clicker=0x... event_seq=... burst_id=...
[00002050][anchor][0x2222000000000001][3][MESH_TX] msg=report gateway=0x...
[00002100][gateway][0x3333333333333333][3][MESH_RX] msg=report anchor=0x...
[00002110][gateway][0x3333333333333333][3][GATEWAY_ACK_TX] seq=...
[00002120][gateway][0x3333333333333333][3][USB_GATEWAY_PACKET_TX] msg=0x...
[00003000][gateway][0x3333333333333333][3][COMMAND_RX] command=...
[00003050][anchor][0x2222000000000001][3][COMMAND_RESULT_TX] status=ok
```

## Failure Triage

| Symptom | Likely area | First checks |
| --- | --- | --- |
| No USB device | USB or boot state | J-Link recover, confirm MCUboot first flash, check `/dev/ttyACM*`, try reset into MCUboot wait window |
| No boot banner | Console route | Tag/anchor use CDC; gateway Stage 3 uses RTT for human logs |
| DEV_ID fail | DWM3000 SPI/reset/wake | Check reset/wake pins, SPI CS/SCK/MOSI/MISO, `DWM_DEV_ID_READ`, `DWM_DEV_ID_FAIL`, and SPI speed logs |
| TX/RX hang | Status polling path | Check `UWB_SYS_STATUS_POLL_START`, `DONE`, `TIMEOUT`, max poll duration, and poll timeout counters |
| Anchor never accepts wake | Wake scan or frame identity | Check channel 5, network ID, epoch, clicker ID, nonce, CRC, and accept/reject reason |
| Stage 1 does not range | Bench gate or schedule | Confirm `CONFIG_IMEC_STAGE1_ALLOW_SINGLE_ANCHOR_RANGE=y` and `BENCH_ONLY` log is present |
| Stage 2 ranges with fewer than three anchors | Production threshold regression | Stop and inspect tag logs; Stage 2 must release/retry/fail instead of ranging with one or two replies |
| Wrong anchor responds | Identity/schedule validation | Compare click event, nonce, anchor ID, schedule order, round, and sample index |
| Gateway CDC unreadable | Binary CDC expected | Use RTT for human logs; CDC carries COBS packets in `gateway_stage3_highdebug` |
| Gateway report never delivered | Mesh or ACK | Check route request/reply, mesh retry/drop, gateway ACK TX/RX, duplicate handling, and active-click preemption logs |

## Local Verification

The following local checks were run after adding the high-debug suite:

```sh
cmake -S firmware -B firmware/build
cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure

env ZEPHYR_NRF_MODULE_DIR=$PWD/nrf PATH=$PWD/.venv/bin:$PWD/firmware/app/scripts:$PATH .venv/bin/west build --build-dir build/tag_stage0_highdebug
env ZEPHYR_NRF_MODULE_DIR=$PWD/nrf PATH=$PWD/.venv/bin:$PWD/firmware/app/scripts:$PATH .venv/bin/west build --build-dir build/tag_stage1_highdebug
env ZEPHYR_NRF_MODULE_DIR=$PWD/nrf PATH=$PWD/.venv/bin:$PWD/firmware/app/scripts:$PATH .venv/bin/west build --build-dir build/anchor_stage1_highdebug
env ZEPHYR_NRF_MODULE_DIR=$PWD/nrf PATH=$PWD/.venv/bin:$PWD/firmware/app/scripts:$PATH .venv/bin/west build --build-dir build/tag_stage2_highdebug
env ZEPHYR_NRF_MODULE_DIR=$PWD/nrf PATH=$PWD/.venv/bin:$PWD/firmware/app/scripts:$PATH .venv/bin/west build --build-dir build/anchor_stage2_highdebug
env ZEPHYR_NRF_MODULE_DIR=$PWD/nrf PATH=$PWD/.venv/bin:$PWD/firmware/app/scripts:$PATH .venv/bin/west build --build-dir build/tag_stage3_highdebug
env ZEPHYR_NRF_MODULE_DIR=$PWD/nrf PATH=$PWD/.venv/bin:$PWD/firmware/app/scripts:$PATH .venv/bin/west build --build-dir build/anchor_stage3_highdebug
env ZEPHYR_NRF_MODULE_DIR=$PWD/nrf PATH=$PWD/.venv/bin:$PWD/firmware/app/scripts:$PATH .venv/bin/west build --build-dir build/gateway_stage3_highdebug

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-clicker --pristine -- -DFIRMWARE_ROLE=clicker -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-anchor --pristine -- -DFIRMWARE_ROLE=anchor -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-gateway --pristine -- -DFIRMWARE_ROLE=gateway -DPYTHON_EXECUTABLE=$PWD/.venv/bin/python -DPython3_EXECUTABLE=$PWD/.venv/bin/python
```

Known non-blocking build warnings:

- Zephyr USB reports the default test VID. Assign a production VID/PID before field release.
- Sysbuild emits inactive app-child Kconfig warnings for `UPDATEABLE_IMAGE_NUMBER` and `MCUBOOT_UPDATE_FOOTER_SIZE` in the single-app MCUboot configuration. The signed images are still generated.
- MCUboot warns to enable `CONFIG_DISABLE_FLASH_PATCH` for production secure boot integrity.

## Next Bench Test Order

Hardware validation is pending. Run the bench in this order:

1. J-Link flash `tag_stage0_highdebug` to one tag. Verify USB enumeration, boot banner, DWM3000 DEV_ID, button simulated click, self-test sleep/wake, `dump_counters`, and `bootloader`.
2. J-Link flash `tag_stage1_highdebug` and `anchor_stage1_highdebug`. Verify anchor low-duty scan, valid `WAKE_CLAIM_ACCEPT`, discovery reply, one-anchor `BENCH_ONLY` schedule, DS-TWR, and one `RANGE_OK`.
3. Build and flash at least three `anchor_stage2_highdebug` images with distinct `IMEC_DEVICE_ID` and `IMEC_ANCHOR_SLOT` values. Flash `tag_stage2_highdebug`. Verify three discovery replies, schedule table, same continuous responder burst, three unique `RANGE_OK`, and correct release/retry behavior with fewer than three anchors.
4. Flash `gateway_stage3_highdebug`, keep gateway human logs on RTT, and keep CDC for COBS binary packets. Verify anchor report queueing, mesh route, gateway ACK, USB packet output, and a gateway command returning `COMMAND_RESULT`.
5. Capture logs from all boards and only then mark hardware validation complete for the stage that passed.
