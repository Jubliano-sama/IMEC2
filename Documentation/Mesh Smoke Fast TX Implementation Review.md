# Mesh Smoke Fast TX Implementation Review

Date: 2026-07-03

This document is a single-file engineering review of the current mesh smoke
test implementation against the requested one-gateway, one-anchor fast TX
hardware smoke objective. It describes what is implemented now, where the code
lives, what behavior is proven by build-time structure or tests, and what is
still needs hardware evidence before the objective can be called complete.

## Requested Objective

Add a hardware smoke-test mode for one gateway and one default-behaving anchor
node, with no intermediate anchors. Both devices are assumed to be in range.
The anchor should transmit gateway-bound packets as fast as the normal mesh
protocol safely allows.

The test must not add a shortcut path. It must use:

- Channel-5 contact when channel-9 timing is missing or stale.
- `MSG_MESH_EVENT_PROPOSE` / `MSG_MESH_EVENT_ACCEPT` for channel-9 timing.
- Existing scheduled finite channel-9 windows, guards, alternating direction,
  supervision, retries, gateway ACKs, hop/custody ACKs, route retry, and
  preemption behavior.
- Existing click priority and required channel-5 scan behavior.
- Persistent delivery state and gateway ACK return paths.

## Current Implementation Status

Status: implemented in firmware, pending a long two-board hardware soak.

The repository has an isolated mesh route-test profile and an anchor-role
synthetic transmitter. The fast smoke option queues normal `MSG_MESH_DATA`
packets through the normal anchor report / mesh relay path as soon as the
normal protocol state allows more traffic.

Implemented pieces:

- Isolated test profile: `CONFIG_IMEC_MESH_ROUTE_TEST`.
- Anchor-role transmitter: `CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER`.
- Fast smoke mode: `CONFIG_MESH_SMOKE_FAST_TX`.
- Presets:
  - `mesh_gateway`
  - `mesh_transmitter`
  - `mesh_anchor_1` through `mesh_anchor_5`
- Synthetic packets are `MSG_MESH_DATA` with `FLAG_GATEWAY_ACK_REQUIRED` and
  `FLAG_DIAGNOSTIC`.
- Transmitter uses `queue_anchor_report()`, so the packet enters the same queue
  and mesh delivery path as ordinary anchor reports.
- The transmitter waits while relay TX, route-waiting TX, channel-9 ACK wait,
  or report queue pressure is active.
- In fast mode, the transmitter uses zero delay after a successful queue
  decision, so traffic is limited by normal mesh state rather than a fixed
  periodic interval.
- Synthetic payload includes monotonic packet ID, local uptime, packet age,
  retry count, selected parent, channel-9 timing state, and payload CRC.
- Gateway firmware verifies synthetic payload CRC, monotonic delivery,
  duplicates, gaps, late missing delivery, retry totals, queue depth, and
  latency samples before BLE output.
- Gateway firmware periodically emits a `mesh-smoke summary` line.
- LED behavior for mesh smoke:
  - LED0 red blinks on the transmitter as a power indicator.
  - LED0 red stays solid on the gateway as a power indicator.
  - LED1 green pulses for raw channel-9 RX/TX activity.
  - LED1 blue pulses for raw channel-5 RX/TX activity.
- Build-time guards bound burst size and payload size.
- Existing mesh code contains detailed debug markers for channel-5 contact,
  channel-9 TX/RX, ACK batching, route waiting, preemption, timeout, and
  persistence paths.
- Host monitor decodes gateway BLE packet output and reports packet ID gaps,
  attempts, drop count, hop count, selected parent, channel-9 timing state,
  payload CRC, message age, source, destination, and final summary counters.

Remaining evidence gap:

- A long hardware run is still needed to prove indefinite operation and capture
  bench evidence that every missing sequence is either delivered later or
  attributed to an explicit protocol reason.

## Build-Time Entry Points

Source files:

- `firmware/app/Kconfig`
- `firmware/app/CMakeLists.txt`
- `firmware/app/conf/mesh-route-test.conf`
- `firmware/app/src/app_mesh_test.c`
- `firmware/app/src/app_mesh_smoke_fast.c`
- `firmware/app/src/app_board.c`
- `tools/mesh_ble_route_monitor.py`
- `Documentation/Mesh Routing Test Firmware.md`

Relevant Kconfig options:

- `CONFIG_IMEC_MESH_ROUTE_TEST`: enables the isolated mesh route-test profile.
- `CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER`: enables the anchor-role synthetic
  transmitter worker.
- `CONFIG_MESH_SMOKE_FAST_TX`: makes the transmitter queue again immediately
  after normal mesh state allows a successful burst.
- `CONFIG_IMEC_MESH_ROUTE_TEST_TX_INTERVAL_MS`: interval between synthetic TX
  bursts when fast mode is disabled. Current generated transmitter value:
  `1000`.
- `CONFIG_IMEC_MESH_ROUTE_TEST_TX_BURST_COUNT`: number of packets queued per
  interval. Current generated transmitter value: `8`.
- `CONFIG_IMEC_MESH_ROUTE_TEST_PAYLOAD_BYTES`: target payload size. Current
  generated transmitter value: `900`.
- `CONFIG_IMEC_MESH_ROUTE_TEST_CH5_SCAN_INTERVAL_MS`: channel-5 scan idle
  interval in the test profile.
- `CONFIG_IMEC_MESH_ROUTE_TEST_CH5_SCAN_RX_US`: channel-5 receive window.
- `CONFIG_IMEC_MESH_ROUTE_TEST_STARTUP_GRACE_MS`: startup delay before UWB
  activity.

The CMake preset path forces these settings for `mesh_transmitter`:

```text
CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER=y
CONFIG_MESH_SMOKE_FAST_TX=y
CONFIG_IMEC_MESH_ROUTE_TEST_TX_INTERVAL_MS=1000
CONFIG_IMEC_MESH_ROUTE_TEST_TX_BURST_COUNT=8
CONFIG_IMEC_MESH_ROUTE_TEST_PAYLOAD_BYTES=900
CONFIG_IMEC_MESH_ROUTE_TEST_STARTUP_GRACE_MS=3000
CONFIG_IMEC_MESH_ROUTE_TEST_CH5_SCAN_INTERVAL_MS=100
CONFIG_IMEC_MESH_ROUTE_TEST_CH5_SCAN_RX_US=20000
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=16384
CONFIG_LOG=n
```

## Runtime TX Path

The transmitter starts from `app_mesh_test_start()` when all of these are true:

- `DEVICE_ROLE == ROLE_ANCHOR`
- `CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER=y`
- The build selected the mesh route-test profile.

`app_mesh_test_start()` creates a Zephyr thread named `mesh_test`. The thread
waits for startup grace, then repeatedly calls `mesh_test_tx_once()`.

`mesh_test_tx_once()` does not directly send a UWB frame. It first checks the
existing mesh delivery state:

- `mesh_relay_tx_active(&mesh_runtime)`
- `mesh_route_waiting_tx_active()`
- `mesh_report_ch9_ack_wait_active()`
- `report_tx_queue_used()`

If any of those are busy, the transmitter logs wait markers and backs off.
If there is queue headroom, it builds up to
`CONFIG_IMEC_MESH_ROUTE_TEST_TX_BURST_COUNT` synthetic `MSG_MESH_DATA` packets
and calls `queue_anchor_report()` for each one.

When `CONFIG_MESH_SMOKE_FAST_TX=y`, a successful queue decision returns zero
delay to the thread loop. Busy states still use a bounded retry delay. This
keeps the stress source constrained by the existing relay, route-waiting, ACK,
queue, channel-5, and channel-9 machinery.

That means delivery continues through the normal mesh report queue, route
selection, route waiting, channel-5 contact, channel-9 event negotiation,
channel-9 payload transfer, ACK, retry, and persistence paths. There is no
special direct gateway shortcut in `app_mesh_test.c`.

## Synthetic Payload

Current synthetic payload TLVs:

| TLV | Name | Purpose |
| --- | --- | --- |
| `0x59` | `TLV_MESH_TEST_PACKET_ID` | Monotonic synthetic packet ID |
| `0x5A` | `TLV_MESH_TEST_ATTEMPT` | Current transmitter attempt count |
| `0x5B` | `TLV_MESH_TEST_DROP_COUNT` | Transmitter build/launch drop count |
| `0x5C` | `TLV_MESH_TEST_ORIGIN_ID` | Transmitter device ID |
| `0x5D` | `TLV_MESH_TEST_TARGET_ID` | Gateway device ID |
| `0x5E` | `TLV_MESH_TEST_FLAGS` | Synthetic test flags |
| `0x06` | `TLV_EVENT_SEQ` | Mirrors packet ID |
| `0x1B` | `TLV_RETRY_COUNT` | Attempt count capped to `uint8_t` |
| `0x1D` | `TLV_UPTIME_MS` | Transmitter uptime at packet build |
| `0x01` | `TLV_DEVICE_ROLE` | Role of the transmitter |
| `0x2A` | `TLV_MESH_CHANNEL` | Expected mesh payload channel |
| `0x61` | `TLV_MESH_TEST_PADDING` | Optional padding up to target payload size |
| `0x97` | `TLV_MESH_TEST_PACKET_AGE_MS` | Packet age at payload build |
| `0x98` | `TLV_MESH_TEST_SELECTED_PARENT_ID` | Selected next hop toward gateway |
| `0x99` | `TLV_MESH_TEST_CH9_TIMING_STATE` | Selected-parent channel-9 timing state |
| `0x9A` | `TLV_MESH_TEST_PAYLOAD_CRC` | CRC over all preceding synthetic payload TLVs |

The normal protocol packet still has its standard protocol CRC. The synthetic
payload also carries a payload-local CRC so the gateway verifier can validate
the stress payload independently.

## Gateway and Host Observability

Gateway-side delivery currently reaches the host through the gateway BLE packet
notify path. The host monitor is `tools/mesh_ble_route_monitor.py`.

The monitor:

- Reassembles and COBS-decodes BLE packet frames.
- Verifies the normal protocol packet CRC.
- Parses normal TLVs and mesh-test TLVs.
- Identifies synthetic mesh-test packets by the synthetic TLV set.
- Computes packet ID gaps and out-of-order observations.
- Tracks maximum hop count and maximum attempt.
- Prints per-packet fields:
  - packet ID
  - gap state
  - attempt
  - drop count
  - hop count
  - selected parent
  - channel-9 timing state
  - synthetic payload CRC
  - message type
  - protocol sequence
  - TTL
  - message age
  - source ID
  - destination ID
- Prints a final summary:
  - packets seen
  - synthetic packets seen
  - gap events
  - missing count
  - last ID
  - last drop count
  - max hop count
  - max attempt
  - decode errors

Gateway firmware also runs an on-device verifier for synthetic `MSG_MESH_DATA`
before BLE output. BLE loss can hide packets from the host monitor, but it does
not affect the gateway-side delivery counters.

The gateway summary includes delivered count, duplicate count, gap count,
missing count, late missing delivery count, attributed missing count, retry
total/max, missed channel-9 events, channel-5 refreshes, queue-depth maximum,
and gateway-observed latency p50/p95/max.

## Debug Logging Coverage

The current mesh implementation already has many debug markers relevant to the
requested uncertain paths:

- Channel-5 contact, scan, preemption, and gap scan markers.
- Channel-9 event grant, TX batch start/stop, fit failure, config failure,
  send failure, ACK wait, ACK timeout, ACK requeue, RX arm, RX frame, RX miss,
  and RX timeout markers.
- Gateway ACK and hop ACK path logs.
- Route-waiting and retry logs.
- Persistent outbox save, restore, clear, and failure logs.
- BLE streaming diagnostics from the gateway BLE stream module when enabled.

The requested logging categories are broadly covered, but not yet normalized
into one smoke-test report stream. An engineer reviewing hardware output should
expect to correlate `DBG_*`, high-debug events, Zephyr logs, BLE packet output,
and host monitor output.

## Acceptance Matrix

| Requirement | Current status | Evidence / notes |
| --- | --- | --- |
| Gateway, anchor, clicker builds pass | Verified | Normal role builds passed after this change |
| One gateway plus one anchor, no intermediate anchors | Supported | Use `mesh_gateway` plus `mesh_transmitter`; relay-anchor presets remain optional for older route tests |
| Anchor behaves like a normal anchor | Supported | Transmitter is `ROLE_ANCHOR` and queues through `queue_anchor_report()` |
| No fast/reliable shortcut | Supported by current transmitter path | No direct UWB send in `app_mesh_test.c`; synthetic packets enter report queue |
| Use CH5 before stale/missing CH9 timing | Supported by shared mesh path | Implemented outside smoke module in mesh route/event machinery |
| Use propose/accept for CH9 timing | Supported by shared mesh path | Existing `MSG_MESH_EVENT_PROPOSE` / `ACCEPT` path |
| Preserve scheduled finite CH9 windows and guards | Supported by shared mesh path | Existing channel-9 planning path remains used |
| Preserve gateway ACK, hop/custody ACK, route retry, persistence | Supported by shared mesh path | Packets request gateway ACK and enter normal relay path |
| Preserve click priority and CH5 scan behavior | Supported by shared scheduler, but hardware smoke evidence is still needed | No protocol timing constants changed |
| Continuously queue synthetic packets as soon as allowed | Supported | Fast mode returns zero delay after a safe successful queue decision |
| Payload includes all requested fields | Supported | Payload carries uptime, age, retry count, selected parent, CH9 timing state, and CRC |
| Gateway verifies monotonic sequence, duplicates, gaps, CRC, ACK path, latency | Mostly supported | Gateway verifies sequence/gap/duplicate/CRC before BLE and tracks latency samples; ACK-path latency is gateway-observed delivery latency |
| Gateway periodic throughput/latency/debug summaries | Supported | Gateway emits `mesh-smoke summary` periodically |
| BLE streaming drops logged if enabled, without affecting UWB correctness | Mostly supported | Gateway BLE stream has bounded queue diagnostics; UWB delivery does not depend on BLE |
| Can run indefinitely without RAM growth | Plausible but not fully proven for this objective | Fixed queues and static thread stack are used; needs long hardware run evidence |
| No protocol timing constants changed | Verified by code review | Existing route-test config changes test scheduling knobs, not protocol constants |
| Every missing sequence is later delivered or explicitly attributed | Partially supported | Gateway tracks missing, late delivery, and attribution counters; long hardware evidence still needed |
| No CH9 timing entry closed merely because one payload/ACK completed | Supported by existing mesh behavior | Existing channel-9 timing reuse/expiry behavior remains outside the smoke module |

## Review Build Commands

Run normal native and role builds:

```sh
cmake -S firmware -B firmware/build
cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-clicker -- -DFIRMWARE_ROLE=clicker
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-anchor -- -DFIRMWARE_ROLE=anchor
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-gateway -- -DFIRMWARE_ROLE=gateway
```

Run smoke-related mesh route-test builds:

```sh
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/mesh-gateway -- -DIMEC_BUILD_PRESET=mesh_gateway
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/mesh-transmitter -- -DIMEC_BUILD_PRESET=mesh_transmitter
```

Optional relay-anchor build from the older route-test profile:

```sh
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/mesh-anchor-1 -- -DIMEC_BUILD_PRESET=mesh_anchor_1
```

For the new one-hop smoke objective, the primary hardware setup should use only
`mesh_gateway` and `mesh_transmitter`.

## Hardware Run Command

With both devices flashed and in range:

```sh
./tools/mesh_ble_route_monitor.py \
  --gateway "IMEC Mesh Test Gateway" \
  --gateway-logs \
  --duration-s 120 \
  --include-all-mesh-data \
  --verbose
```

Expected current output:

- Increasing synthetic packet IDs.
- No packet ID gaps during stable in-range operation.
- Attempt normally at `1`.
- Drop count normally `0`.
- Hop count usually `1`.
- `parent=...`, `ch9_state=...`, and `payload_crc=...` fields on synthetic
  packets.
- Gateway logs containing route/channel/ACK state when enabled.
- Gateway `mesh-smoke summary` lines.
- LED0 red blinking on the transmitter and solid red on the gateway.
- LED1 green pulses for channel-9 RX/TX, blue pulses for channel-5 RX/TX.

This is the current smoke observation flow. The remaining completion evidence
is a long two-board run that proves stable runtime and accounts for any missing
sequence.

## Remaining Next Steps

1. Flash `build/mesh-gateway` and `build/mesh-transmitter`.
2. Run a long two-board hardware smoke and keep the log artifact.
3. Confirm gateway `mesh-smoke summary` plus host JSONL output show every
   missing sequence either delivered later or attributed to an explicit
   protocol reason.

## Conclusion

The current codebase now contains the requested fast smoke mode, synthetic
payload fields, gateway-side verifier, host monitor decoding, and LED activity
signals. The remaining gap is hardware evidence for indefinite runtime and
missing-sequence attribution under real RF conditions.
