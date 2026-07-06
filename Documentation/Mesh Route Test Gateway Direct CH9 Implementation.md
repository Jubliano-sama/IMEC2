# Mesh Route Test Gateway Direct CH9 Implementation

Date: 2026-07-04

## Purpose

This note documents the mesh-route-test implementation changes for gateway-bound
channel-9 traffic. It is intended as an engineering review file for the current
firmware behavior and the assumptions behind it.

## Protocol Behavior

- The gateway route-test role no longer scans channel 5.
- The gateway route-test role does not use slotted channel-9 receive windows.
- The gateway arms continuous channel-9 RX in mesh-payload mode and stays there
  until a packet is received or the long host-side RX call times out.
- When the gateway receives a packet requiring a gateway ACK, RX has already
  returned, so the gateway waits a conservative 10 ms turnaround guard, sends
  the ACK immediately on channel 9, and then rearms continuous channel-9 RX.
- Non-gateway nodes repairing a gateway route first send a short direct
  `MSG_GATEWAY_ROUTE_REQ` on channel 9 and wait up to 120 ms for a matching
  `MSG_GATEWAY_ACK`.
- A successful direct gateway probe installs a direct route to the gateway only;
  it does not install a synthetic or slotted gateway channel-9 timing.
- If the direct probe fails, the node falls back to the existing channel-5
  wake-train plus AODV-style route-request flow.
- The mesh transmitter preset currently enables a relay-required route request
  debug mode. In that mode, gateway-bound route repair skips the direct
  channel-9 gateway probe and marks the channel-5 `MSG_ROUTE_REQ` with
  `TLV_ROUTE_REQUEST_FLAGS = MESH_ROUTE_REQ_FLAG_RELAY_REQUIRED`.
- In the mesh transmitter preset, `MSG_GATEWAY_ROUTE_ADV` frames are ignored so
  they cannot seed a direct `transmitter -> gateway` route during relay testing.
  Anchors still accept and forward gateway route advertisements for normal
  upstream route repair.
- A route-test gateway ignores direct, hop-count-0 relay-required route
  requests from the origin. The request is still eligible to be answered after
  an anchor has a valid upstream route, because the reply then describes a
  normal `origin -> anchor -> gateway` path.
- If an anchor hears a gateway-bound route request but has no selected upstream
  route after reset, it treats that as normal route repair: it forwards the
  route request on channel 5 and starts its own gateway route discovery. Once
  an upstream is learned, a repeated route-request frame can be answered by the
  anchor.
- Route requests carry `TLV_ROUTE_REPLY_RX_DELAY_MS`, a sender-provided
  countdown until the origin's route-reply receive window is open. The bounded
  channel-5 flood sender rewrites this countdown for each repeated copy, so a
  relay that receives an early copy waits longer while a relay that receives a
  later copy can answer sooner. Route replies use this only as a local
  transmit-not-before hint and do not echo the TLV.
- Route initiators own the channel-9 timing proposal for their next hop.
  Channel-5 route requests can carry proposed channel-9 timing relative to the
  request reference time. Receivers install that proposal as local-RX-first.
- Forwarded route requests clear the child timing before sending upstream, so a
  relay does not leak one hop's timing into another hop. Route replies sent back
  downstream reattach the stored downstream timing so the child can install the
  matching local-TX-first timing.

## Timing Constants

- Gateway immediate ACK guard: `MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS = 10 ms`.
- Direct gateway probe ACK guard: `MESH_GATEWAY_DIRECT_PROBE_ACK_GUARD_MS = 10 ms`.
- Direct gateway probe ACK listen: `MESH_GATEWAY_DIRECT_PROBE_ACK_RX_MS = 120 ms`.
- Gateway route advertisement period: `MESH_GATEWAY_ROUTE_ADV_PERIOD_MS = 600000 ms`.
- Route request reply-open countdown:
  `MESH_ROUTE_TEST_ROUTE_REPLY_RX_DELAY_MS = FLOOD_RELAY_BURST_MS + 20 ms`.
- Route-test channel-9 event guard: `MESH_EVENT_DEFAULT_GUARD_MS = 30 ms`.
- Route-test channel-5 scan slot: `CONFIG_IMEC_MESH_ROUTE_TEST_CH5_SCAN_INTERVAL_MS = 50 ms`.
- Route-test channel-5 scan RX: `CONFIG_IMEC_MESH_ROUTE_TEST_CH5_SCAN_RX_US = 50000 us`.
- Gateway continuous channel-9 RX host window:
  `UWB_MESH_GATEWAY_RX_WINDOW_MS = 30000 ms`.

The guards are intentionally conservative for current bring-up. They are not
optimized for final latency or power.

## Debug LEDs

- Mesh transmitter: LED0 blinks red as the power indicator.
- Mesh gateway: LED0 stays red as the power indicator.
- Mesh anchors: LED0 stays blue as the power indicator.
- LED1 is reserved for short raw UWB channel activity pulses in route-test
  builds.
- On mesh anchors, LED1 pulses red for channel-5 route-control activity and
  green for channel-9 activity.

## Implementation Touchpoints

- `firmware/include/protocol.h` and `firmware/src/protocol.c`
  add `MSG_GATEWAY_ROUTE_REQ`, the route-request flags TLV, and the
  route-reply receive-delay TLV.
- `firmware/src/mesh_relay.c`
  adds timed route-request preparation, direct gateway route installation, route
  reply timing propagation back downstream, route-reply timing installation at
  the origin, gateway suppression of direct relay-required route requests, and
  route-reply transmit-not-before scheduling from the request ETA.
- `firmware/app/src/app_mesh_report.c`
  adds direct gateway probing, immediate direct gateway TX, continuous gateway
  channel-9 RX, relay-required route request emission for the transmitter
  preset, route-reply ETA waiting, and gateway ACK turnaround guard logging.
- `firmware/app/src/app_mesh_flood.c`
  updates the route-reply ETA countdown on each bounded route-request flood
  repeat.
- `firmware/app/src/app_mesh_rx_policy.c`
  keeps the mesh transmitter preset from installing routes from broadcast
  gateway route advertisements while leaving anchor behavior unchanged.
- `firmware/app/src/app_board.c`
  owns the mesh-route-test LED0 power indicators and LED1 channel activity
  pulses.
- `firmware/app/src/app_mesh_gateway_ack_policy.c`
  changes route-test gateway channel-9 ACKs from batched ACKs to immediate
  current-channel ACKs.
- `firmware/app/src/app_config.h`,
  `firmware/app/CMakeLists.txt`, and
  `firmware/app/conf/mesh-route-test.conf`
  hold the updated conservative timing defaults.
- `firmware/tests/test_mesh_relay.c` and
  `firmware/tests/test_app_mesh_gateway_ack_policy.c`
  cover the route-timing and ACK-policy contracts.

## Review Checklist

- Verify the gateway route-test RX path returns before ACK TX and does not call
  `mesh_select_channel9_rx_event()` for gateway RX.
- Verify direct gateway traffic bypasses `mesh_relay_start_channel9_tx()` and
  uses immediate channel-9 TX when the selected next hop is the gateway, except
  in the relay-required transmitter preset where direct gateway route
  advertisements are intentionally ignored.
- Verify fallback CH5 route requests still carry only the initiating hop's
  proposed channel-9 timing.
- Verify relays clear timing on upstream route-request forwarding and reattach
  stored downstream timing on the route reply sent back to the child.
- Verify RTT logs show `DBG_GATEWAY_CH9_RX_CONT_*`, `DBG_DIRECT_GW_*`, and
  immediate gateway ACK markers during smoke testing.
