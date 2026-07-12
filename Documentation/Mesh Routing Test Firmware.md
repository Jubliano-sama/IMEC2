# Mesh Routing Test Firmware

This isolated firmware profile is selected with `IMEC_BUILD_PRESET` and leaves
the normal `clicker`, `anchor`, and `gateway` builds untouched.

## Presets

- `mesh_gateway`: gateway/root node, BLE name `IMEC Mesh Test Gateway`.
- `mesh_transmitter`: anchor-role transmitter node, BLE name `IMEC Mesh Tx`.
- `mesh_anchor`: the single production relay/ranging anchor image.

Every `mesh_anchor` uses the same HEX. It derives its network identity from the
nRF FICR hardware ID and starts unassigned when no matching persisted gateway
assignment exists. `CMD_ASSIGN_DISCOVERY_SLOTS` discovers each physical anchor,
assigns the logical discovery/ranging order, and persists the local slot. A
blank or migrated unassigned anchor can claim an assignment but does not answer
normal click discovery. The removed `mesh_anchor_1` through `mesh_anchor_5`
presets are not compatibility aliases: after upgrade, rerun the assignment
command so stale numbered IDs are replaced by hardware-derived identities.
The transmitter is also an anchor-role mesh node so it can use the normal
relay, route-discovery, gateway-ACK, and channel-9 timing machinery.

## Channel Behavior

Channel 5 remains the wake/contact and route-discovery channel. Channel 9 is
the mesh payload channel once channel-9 event timing is installed.

Generic channel-5 control dissemination is called a `flood_epoch`. Do not call
these control bursts clicks: click service is reserved for real accepted
clicker-originated wake, discovery, schedule, and ranging work. A `flood_epoch`
is a bounded channel-5 control event for route solicitation, route
advertisement, gateway command broadcast, or collection-status broadcast. It
uses one origin, one request ID, and one `flood_epoch_id`; relays forward the
same event within the configured `FLOOD_FORWARD_*` and `FLOOD_EPOCH_*_TTL`
bounds and never create child route discoveries for the same gateway target.

Radio priority stays:

1. Active channel-5 click service.
2. Required quick channel-5 wake scan.
3. Channel-5 contact for route/control refresh.
4. Negotiated channel-9 mesh event.
5. Retained sleep.

Route knowledge and channel-9 timing freshness remain separate. Stale
channel-9 timing triggers channel-5 contact refresh before payload transfer; it
does not delete the route by age alone.

In this test profile, any valid channel-5 mesh frame addressed to the local
anchor or to the global mesh broadcast ID immediately forces that anchor's
channel-5 scan interval to zero. That makes subsequent channel-5 wake/contact
scans reschedule continuously.

The firmware emits `MESH_CH5_PREEMPT_CH9` over the BLE log characteristic when
the scheduler clips, skips, or defers channel-9 work because a channel-5 scan
or contact window must take priority.

## Synthetic Packet Payload

The transmitter sends normal `MSG_MESH_DATA` packets to the gateway with
`FLAG_GATEWAY_ACK_REQUIRED` and `FLAG_DIAGNOSTIC` set. Payload TLVs include:

- `TLV_MESH_TEST_PACKET_ID` (`0x59`): incrementing synthetic packet ID.
- `TLV_MESH_TEST_ATTEMPT` (`0x5A`): transmitter attempt count for that packet.
- `TLV_MESH_TEST_DROP_COUNT` (`0x5B`): transmitter-side launch/build failures.
- `TLV_MESH_TEST_ORIGIN_ID` (`0x5C`): transmitter device ID.
- `TLV_MESH_TEST_TARGET_ID` (`0x5D`): gateway device ID.
- `TLV_MESH_TEST_FLAGS` (`0x5E`): synthetic test flags.
- `TLV_MESH_TEST_PACKET_AGE_MS` (`0x97`): transmitter-side packet age at
  payload build time.
- `TLV_MESH_TEST_SELECTED_PARENT_ID` (`0x98`): route-selected next hop toward
  the gateway, or zero when no route is currently selected.
- `TLV_MESH_TEST_CH9_TIMING_STATE` (`0x99`): channel-9 timing state for the
  selected parent: `0` none, `1` route only, `2` stale timing, `3` fresh timing.
- `TLV_MESH_TEST_PAYLOAD_CRC` (`0x9A`): CRC-16/CCITT-FALSE over all preceding
  synthetic payload TLVs, including padding.

The gateway emits delivered packets through the existing connected BLE packet
notify characteristic. The host monitor computes observed hop count from the
received packet TTL.

## Host Monitor

Run the BLE monitor from the repository root:

```sh
./tools/mesh_ble_route_monitor.py --gateway "IMEC Mesh Test Gateway"
```

Add relay/transmitter BLE logs when debugging channel scheduling:

```sh
./tools/mesh_ble_route_monitor.py \
  --gateway "IMEC Mesh Test Gateway" \
  --log-device "IMEC Mesh Tx"
```

## Test Plan

Build the native logic first, then build the isolated mesh presets:

```sh
cmake -S firmware -B firmware/build
cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/mesh-gateway -- -DIMEC_BUILD_PRESET=mesh_gateway
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/mesh-transmitter -- -DIMEC_BUILD_PRESET=mesh_transmitter
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/mesh-anchor -- -DIMEC_BUILD_PRESET=mesh_anchor
```

Expected build results:

- Native tests pass, including `MSG_MESH_HOP_ACK` encode validity.
- Mesh gateway, transmitter, and anchor images build without Kconfig warnings.
- Boot logs show `mesh-route test` behavior through the preset name and the
  expected BLE names.
- Anchor boot logs show a stable FICR-derived node ID and either a persisted
  gateway assignment or explicit `UNPROVISIONED` state.

For a close-range smoke test, place the gateway, transmitter, and one anchor
near each other and run:

```sh
./tools/mesh_ble_route_monitor.py \
  --gateway "IMEC Mesh Test Gateway" \
  --log-device "IMEC Mesh Tx" \
  --duration-s 120
```

Expected close-range output:

- Synthetic `packet_id` values increase monotonically.
- `origin_id` is the transmitter device ID.
- `attempt` is normally `1`.
- `drop_count` remains `0` unless the transmitter cannot launch a frame.
- `hop_count` is usually `1` when the gateway can receive directly.
- No packet ID gaps appear during stable close-range operation.

For a multi-hop test, move the transmitter out of direct gateway range and
place relay anchors so at least one path exists:

```sh
./tools/mesh_ble_route_monitor.py \
  --gateway "IMEC Mesh Test Gateway" \
  --log-device "IMEC Mesh Tx" \
  --log-device "IMEC Mesh Anchor 1" \
  --log-device "IMEC Mesh Anchor 2" \
  --jsonl mesh_route_run.jsonl
```

Expected multi-hop output:

- The gateway still receives increasing synthetic packet IDs.
- `hop_count` rises above `1` when relays are used.
- Missing packet IDs identify dropped synthetic packets.
- `attempt` rises during weak connectivity and settles when the route is
  stable.
- Relay-anchor BLE logs show channel-9 relay participation.

To verify channel-5 preemption, keep channel-9 mesh traffic active and then
introduce a valid channel-5 wake/contact mesh frame addressed globally or to a
specific anchor. Expected BLE debug output:

- The matching anchor emits `MESH_TEST_WAKE`.
- The first valid global/local channel-5 mesh wake switches that anchor to
  continuous channel-5 scan.
- `MESH_CH5_PREEMPT_CH9` appears when scheduled channel-5 work clips, defers,
  skips, or refreshes channel-9 work.
- The log context identifies transmit, receive, gateway ACK, or retransmit
  scheduling.
- Non-targeted anchors do not emit `MESH_TEST_WAKE` for another anchor's
  targeted wake event.

Pass criteria:

- Every isolated mesh preset needed for the setup builds.
- Every role exposes BLE packet or log output for debugging.
- The gateway receives the synthetic mesh packet stream over BLE.
- The host monitor reports packet ID, sequence gaps, attempts, drop count, and
  hop count.
- Channel-5 wake/contact events pre-empt channel-9 work on schedule.
- Valid global/local channel-5 mesh wake events force anchors into continuous
  channel-5 scan.
