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
| Completed | Implement BLE discovery request/READY payloads | Normal and diagnostic discovery payloads |
| In progress | Implement DWM3000 STS-SDC DS-TWR wrapper | SPI-polled initiator/responder runtime is wired; hardware timing/calibration smoke test pending |
| Completed | Implement multi-anchor MVP click flow | Clicker advertises discovery, collects up to 8 READY anchors, RSSI-sorts them, ranges sequentially, and builds one report per successful range |
| In progress | Implement full clicker normal-click flow | Explicit partial-failure reporting and gateway delivery pending |
| In progress | Implement clicker self-test flow | Gesture, diagnostic BLE advertisement, READY scan, DWM3000 wake/reset/DEV_ID probe, and diagnostic UWB dud range wired; hardware validation pending |
| Completed | Enforce BLE-gated anchor UWB wake | Anchor low-duty scans BLE, wakes UWB only after discovery, keeps a 400 ms responder window, then returns DWM3000 to standby |

## P3 - Mesh, Gateway, and Survey

| Status | Task | Output |
| --- | --- | --- |
| In progress | Implement mesh packet relay with hop ACK | Hop ACK packet builder complete; relay runtime pending |
| In progress | Implement gateway ACK handling | End-to-end gateway ACK packet builder complete; relay runtime pending |
| Pending | Implement route advertisements and route status | Self-organizing mesh root discovery |
| In progress | Implement gateway command dispatcher | Command and command-result packet builders complete; dispatch handlers pending |
| Pending | Implement anchor self-distance survey | Reachability graph, pair preparation, exactly `n` measurements |
| Pending | Implement COBS USB serial gateway output | Binary frames for off-site processor |

## P4 - Verification

| Status | Task | Output |
| --- | --- | --- |
| Completed | Native unit tests | Packet, TLV, COBS, discovery, UWB frames, status, route, mesh ACK, reports, survey |
| Completed | Zephyr build test | App compiles for `nrf52833dk/nrf52833` in the root west workspace |
| Pending | Hardware smoke test | Device ID, BLE discovery/READY, DWM3000 sleep/wake, one SPI-polled range |
| Pending | Integration test | Normal click, self-test, route retry, survey run |
