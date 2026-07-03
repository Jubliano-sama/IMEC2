# Mesh Smoke Fast TX Implementation Review

Date: 2026-07-03

This document is a single-file engineering review of the current mesh smoke
test implementation against the requested one-gateway, one-anchor fast TX
hardware smoke objective. It describes what is implemented now, where the code
lives, what behavior is proven by build-time structure or tests, and what is
still missing before the objective can be called complete.

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

Status: partially implemented.

The repository already has an isolated mesh route-test profile and an
anchor-role synthetic transmitter. That path uses normal `MSG_MESH_DATA`
packets and queues them through the normal anchor report / mesh relay path.
It is useful for stressing the real mesh machinery, but it does not yet fully
match the stricter fast smoke-test objective.

Implemented pieces:

- Isolated test profile: `CONFIG_IMEC_MESH_ROUTE_TEST`.
- Anchor-role transmitter: `CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER`.
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
- Build-time guards bound burst size and payload size.
- Existing mesh code contains detailed debug markers for channel-5 contact,
  channel-9 TX/RX, ACK batching, route waiting, preemption, timeout, and
  persistence paths.
- Host monitor decodes gateway BLE packet output and reports packet ID gaps,
  attempts, drop count, hop count, message age, source, destination, and final
  summary counters.

Missing or incomplete pieces:

- No `CONFIG_MESH_SMOKE_FAST_TX` alias exists yet. The current option is
  `CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER`.
- The transmitter default is not "as fast as safely allowed"; the current
  generated transmitter config sets `CONFIG_IMEC_MESH_ROUTE_TEST_TX_INTERVAL_MS=1000`.
- Payload does not yet include all requested fields:
  - packet age inside the synthetic payload
  - selected parent
  - channel-9 timing state
  - payload-local CRC
- Gateway firmware does not yet verify monotonic sequence, duplicates, gaps,
  payload CRC, ACK path, or latency as an on-device smoke-test verifier.
  The host monitor performs some packet/gap reporting after BLE delivery.
- Gateway firmware does not yet periodically print the full requested summary:
  throughput, delivered count, duplicate count, gap count, retries, missed
  channel-9 events, channel-5 refreshes, gateway ACK latency p50/p95/max,
  queue depth, and explicit drop/defer reason.
- Missing sequence explanations are not yet guaranteed on-device. The host
  monitor can detect gaps, and firmware logs many protocol reasons, but there
  is no single gateway-side correlation layer proving every missing sequence is
  later delivered or attributed.

## Build-Time Entry Points

Source files:

- `firmware/app/Kconfig`
- `firmware/app/CMakeLists.txt`
- `firmware/app/conf/mesh-route-test.conf`
- `firmware/app/src/app_mesh_test.c`
- `tools/mesh_ble_route_monitor.py`
- `Documentation/Mesh Routing Test Firmware.md`

Relevant Kconfig options:

- `CONFIG_IMEC_MESH_ROUTE_TEST`: enables the isolated mesh route-test profile.
- `CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER`: enables the anchor-role synthetic
  transmitter worker.
- `CONFIG_IMEC_MESH_ROUTE_TEST_TX_INTERVAL_MS`: interval between synthetic TX
  bursts. Current generated transmitter value: `1000`.
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

Requested but not yet present:

- Packet age as a synthetic TLV.
- Selected parent as a synthetic TLV.
- Channel-9 timing state as a synthetic TLV.
- Synthetic payload-local CRC TLV.

The normal protocol packet still has its standard protocol CRC. The missing
piece is a smoke-test-specific payload CRC that the gateway verifier can check
and report independently.

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

This is useful, but it is not equivalent to the requested gateway firmware
verifier. BLE loss can hide packets from the host monitor, so on-device gateway
verification is still needed before the smoke objective is complete.

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
| Gateway, anchor, clicker builds pass | Previously verified for normal role builds after BLE streaming work; should be rerun after any smoke edits | Build commands are listed below |
| One gateway plus one anchor, no intermediate anchors | Partially supported | Use `mesh_gateway` plus `mesh_transmitter`; docs still also describe relay anchors |
| Anchor behaves like a normal anchor | Mostly supported | Transmitter is `ROLE_ANCHOR` and queues through `queue_anchor_report()` |
| No fast/reliable shortcut | Supported by current transmitter path | No direct UWB send in `app_mesh_test.c`; synthetic packets enter report queue |
| Use CH5 before stale/missing CH9 timing | Supported by shared mesh path | Implemented outside smoke module in mesh route/event machinery |
| Use propose/accept for CH9 timing | Supported by shared mesh path | Existing `MSG_MESH_EVENT_PROPOSE` / `ACCEPT` path |
| Preserve scheduled finite CH9 windows and guards | Supported by shared mesh path | Existing channel-9 planning path remains used |
| Preserve gateway ACK, hop/custody ACK, route retry, persistence | Supported by shared mesh path | Packets request gateway ACK and enter normal relay path |
| Preserve click priority and CH5 scan behavior | Supported by shared scheduler, but hardware smoke evidence is still needed | No timing constants changed by current test profile review |
| Continuously queue synthetic packets as soon as allowed | Partial | Current worker waits for safe state, but also uses a fixed 1000 ms interval after successful burst |
| Payload includes all requested fields | Partial | Missing packet age, selected parent, CH9 timing state, payload CRC |
| Gateway verifies monotonic sequence, duplicates, gaps, CRC, ACK path, latency | Partial | Host monitor checks some post-BLE observations; gateway firmware verifier missing |
| Gateway periodic throughput/latency/debug summaries | Missing | No on-device summary aggregator yet |
| BLE streaming drops logged if enabled, without affecting UWB correctness | Mostly supported | Gateway BLE stream has bounded queue diagnostics; UWB delivery does not depend on BLE |
| Can run indefinitely without RAM growth | Plausible but not fully proven for this objective | Fixed queues and static thread stack are used; needs long hardware run evidence |
| No protocol timing constants changed | Supported by current review | Existing route-test config changes test scheduling knobs, not protocol constants |
| Every missing sequence is later delivered or explicitly attributed | Missing | Needs gateway-side verifier/correlation |
| No CH9 timing entry closed merely because one payload/ACK completed | Supported by recent mesh changes, but should be regression-tested in smoke build | Existing code has channel-9 timing reuse/expiry behavior outside smoke module |

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
- Gateway logs containing route/channel/ACK state when enabled.

This is a current smoke observation flow, not a full acceptance proof for the
new objective.

## Recommended Next Implementation Steps

1. Add a named smoke alias such as `CONFIG_MESH_SMOKE_FAST_TX` or
   `CONFIG_IMEC_MESH_SMOKE_FAST_TX` that selects the existing mesh route-test
   transmitter path.
2. Change fast mode scheduling so the transmitter queues the next burst
   immediately after the normal protocol state allows it, while retaining the
   current busy-state guards.
3. Extend synthetic payload TLVs with:
   - packet build uptime / age basis
   - selected parent
   - channel-9 timing state
   - synthetic payload CRC
4. Add a gateway-side smoke verifier that tracks packet IDs before BLE output:
   delivered, duplicates, gaps, late arrivals, payload CRC failures, ACK path,
   latency samples, retries, missed channel-9 events, channel-5 refreshes,
   queue depth, and explicit drop/defer reasons.
5. Emit periodic gateway summaries from firmware, not only from the host
   monitor.
6. Add focused native tests for:
   - fast-mode "queue only when safe" decision logic
   - payload encode/decode and CRC
   - gateway verifier duplicate/gap/late-delivery accounting
   - bounded latency percentile calculation
   - missing-sequence reason attribution
7. Rebuild normal clicker, anchor, gateway images and the two smoke images.
8. Run a long two-board hardware smoke and keep the log artifact.

## Conclusion

The current codebase has a strong foundation for the requested smoke mode: a
normal anchor-role synthetic transmitter, normal mesh delivery path usage,
bounded queue guards, and useful host-side monitoring. The objective is not
complete yet because the requested fast scheduling semantics, smoke payload
fields, on-gateway verification, periodic metrics, and missing-sequence
attribution are not fully implemented.
