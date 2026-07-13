# Gateway Here-I-Am Stress Coverage

`CMD_FORCE_REDISCOVERY` (`0x000C`) is the host request for a gateway route
refresh. The gateway accepts it through the priority command ingress, completes
the bounded command lifecycle, and creates `MSG_GATEWAY_ROUTE_ADV`; anchors
validate that advertisement, install only current-epoch gateway candidates,
and forward the same flood identity on channel 5 through the production flood
timing path.

The `mesh_gateway_control_stress_scenarios` native target composes production
helpers and keeps host completion separate from radio reachability. The static
`gateway_route_host_packet()` wrapper is not directly invoked by the native
fixture, so this is helper-composition evidence rather than host-to-radio
end-to-end evidence. It covers:

- host serial decode, priority admission, lifecycle dispatch, independent
  route-advertisement construction, `COMMAND_OK`, and retained terminal
  observability during host backpressure;
- direct fanout at 2, 6, 16, 32, and 50 anchors, plus an eight-hop propagation
  chain bounded by the production flood TTL;
- duplicate suppression, stale epoch rejection, wrong gateway identity
  rejection, and terminal TTL behavior without a seeded-route fallback;
- four-opportunity channel-5 flood behavior under click deferral and RF-busy
  politeness, followed by an explicit retry;
- exact mesh-control PHY airtime, complete receive-window containment,
  simultaneous-forwarder collision, partial-window failure, and a separated
  successful retry;
- explicit simulator failure on `MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV` until
  the simulator can model the production four-opportunity quiet-check flood
  state machine. The PHY fixture separately uses the production outbound due
  time, repeat interval, and bounded busy-backoff helper.

Here-I-Am is an announcement flood and has no per-anchor ACK/result collection
contract. `COMMAND_OK` means the gateway accepted and scheduled the refresh; it
does not claim that every anchor received it. Reachability evidence comes from
the modeled radio receptions and installed route candidates, not from the host
terminal result.
