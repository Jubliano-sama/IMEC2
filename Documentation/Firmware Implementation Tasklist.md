#internship #imec #firmware #tasklist

# Firmware Implementation Tasklist

This tasklist prioritizes the DWM3000 IMEC Clicker/Anchor/Gateway firmware work. The first milestone is a native, testable protocol/state library. Hardware integration follows after the message formats and state behavior are stable.

## P0 - Protocol and State Foundation

| Status | Task | Output |
| --- | --- | --- |
| Completed | Create a new `firmware/` tree | Isolated implementation separate from `old code` and imported SDK |
| Completed | Implement shared IMEC packet envelope, TLV helpers, CRC, and COBS serial framing | Native C library with tests |
| Completed | Implement message/type/flag/status enums | Single source of truth for firmware protocol constants |
| Completed | Implement clicker button/self-test state logic | Testable long-press then short-press gesture handling |
| Completed | Implement status indication pattern selection | Testable priority and failure-code behavior |
| Completed | Implement route candidate and ACK retry policy helpers | Testable route choice and failure handling |
| Completed | Implement anchor survey command/result data structures | Testable survey command validation |

## P1 - Zephyr Application Skeleton

| Status | Task | Output |
| --- | --- | --- |
| Completed | Add Zephyr app CMake/prj.conf | Builds for target board when Zephyr workspace is available |
| Completed | Add ANNA-B402/DWM3000 devicetree overlay | MCU pins from architecture document mapped to Zephyr aliases/nodes |
| Completed | Port DWM3000 SDK platform layer to custom pinout | Zephyr SPI/reset/wake hooks, SDK-compatible SPI/sleep/tick/reset ABI, decadriver source build, DEV_ID probe, and 32 MHz SPIM3 runtime path |
| Completed | Add role configuration | Clicker, anchor, and gateway builds from one codebase |

## P2 - Radio Behavior

| Status | Task | Output |
| --- | --- | --- |
| Completed | Implement BLE wake/READY payloads | Normal and diagnostic wake requests, addressed READY payloads, attempt index, priority, READY timing, and minimum anchor count |
| Completed | Implement DWM3000 STS-SDC DS-TWR wrapper | SPI-polled initiator/responder runtime with compact UWB frames, equal reply delays, per-anchor retry with backoff, and 500 ms responder window; hardware timing/calibration smoke test pending (P4) |
| Completed | Implement multi-anchor MVP click flow | Clicker runs UWB politeness sniff, advertises wake, collects up to 8 addressed READY anchors per attempt, RSSI-sorts them, ranges sequentially, and requires 4 unique successful anchors |
| Completed | Implement full clicker normal-click flow | Software path complete: up to 6 wake attempts, 50 ms per-anchor retry windows, successful anchor reports route through mesh from a post-UWB report queue, and post-poll responder failures are reported; hardware validation remains in P4 |
| Completed | Implement clicker self-test flow | Gesture, diagnostic BLE advertisement, READY scan, DWM3000 wake/reset/DEV_ID probe, diagnostic UWB dud range, and result reporting all wired; hardware validation pending (P4) |
| Completed | Enforce BLE-gated anchor UWB wake | Anchor parks the DWM3000 wake pin inactive at boot, low-duty scans BLE, wakes UWB only after a wake request, sends addressed READY after deterministic arbitration, keeps a 500 ms responder window, then puts DWM3000 into deep sleep |
| Completed | Give clicker discovery priority over mesh BLE traffic | Mesh route discovery advertisements reduced from 250 ms to 150 ms; anchors immediately follow each mesh ad with a 100 ms full-duty BLE scan so clicker wake requests arriving during the transmit window are received before resuming low-duty scanning |

## P3 - Mesh, Gateway, and Survey

| Status | Task | Output |
| --- | --- | --- |
| Completed | Implement mesh packet relay over BLE connections | Relay runtime over connected mesh transport, gateway ACK retries, duplicate re-forward repair, 60 s duplicate expiry, and burst RX queue |
| Completed | Implement gateway ACK handling | End-to-end gateway ACK runtime for gateway-bound reports/status/results |
| Completed | Implement reactive route discovery | `ROUTE_REQ`/`ROUTE_REPLY` advertisements discover upstream and downlink paths on demand; operational mesh packets use BLE connections while role scanning stays active at the configured duty cycle; anchors run a 100 ms full-duty BLE scan after each mesh advertisement to catch clicker wake requests that arrived during the transmit window |
| In progress | Implement gateway command dispatcher | USB command routing, anchor ping/status with route telemetry, unsupported-command responses, route timeout reporting, and 5 s command-result timeout tracking; broader command set pending |
| Pending | Implement anchor self-distance survey | Reachability graph, pair preparation, exactly `n` measurements |
| Pending | Implement anchor heartbeat reporting | Protocol already defined (`CMD_START_HEARTBEAT`, `CMD_STOP_HEARTBEAT`, `MSG_ANCHOR_HEARTBEAT`); needs periodic timer, report builder, and TX path |
| Completed | Implement COBS USB serial gateway output | Binary gateway packets and command failures emitted over USB CDC |

## P4 - Verification

| Status | Task | Output |
| --- | --- | --- |
| Completed | Native unit tests | Packet, TLV, COBS, discovery, UWB frames, status, route, mesh relay, BLE mesh frames, gateway command policy, reports, survey |
| Completed | Zephyr build test | App compiles for `nrf52833dk/nrf52833` in the root west workspace |
| Pending | Hardware smoke test | Device ID, BLE discovery/READY, DWM3000 sleep/wake, one SPI-polled range |
| Pending | Integration test | Normal click, self-test, route retry, survey run |
