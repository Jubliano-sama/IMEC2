# Native Mesh Integration Simulator

This test target runs the contemporary platform-independent mesh code against a
deterministic discrete-event radio model. It is intended to catch timing,
ownership, retry, and forwarding regressions before a multi-board flash cycle.

## What is real firmware code

The scenarios call the production-candidate native modules directly:

- `protocol.c` encodes every mesh transmission and decodes it after reception.
- `mesh.c` negotiates channel-9 timing, chooses TX/RX slot ownership, advances
  event counters, and accounts for missed events.
- `route.c` selects established upstream and downlink paths.
- `mesh_relay.c` performs forwarding, duplicate handling, gateway/hop ACK
  processing, timeout/backoff, retransmission, and delivery confirmation.
- `uwb_session.c` drives the clicker through wake, discovery, schedule, and
  successful range-result states and receives low-duty scan diagnostics.
- `report.c` creates the local anchor click report used by the preemption test.

Roles are explicit simulator instances: clicker, synthetic transmitter,
anchors, and gateway. Load and priority scenarios may seed established routes
as explicit preconditions. The route-formation scenarios start without a route
and pass requests, replies, and reply ACKs through the same dispatcher and core
handlers used by every other simulated radio exchange.

## Radio model

Discrete-event scheduling uses integer microseconds and stable insertion
ordering. Radio containment and collision decisions use fixed-point DW3000
RCTU intervals. Links are deterministic and symmetric; configured propagation
delay is applied to each arrival interval.

The airtime profiles mirror the current firmware configuration:

- channel 5 wake/range: 4096-symbol preamble, 16-symbol SFD, 850 kbps;
- channel 9 mesh: 1024-symbol preamble, 8-symbol SFD, 850 kbps.

`dwm3000_timing.c` calculates SHR, PHR, FCS, and Reed-Solomon coded data time in
RCTU and rounds outward only when exposing integer microseconds. Golden vectors
cover representative control/ACK lengths, 81 bytes, 965/974 bytes, and both
valid PSDU limits. Delayed TX applies the production 512-RCTU quantization and
TX antenna delay before propagation. A packet decodes only when its complete
arrival interval is inside one RX window with the same channel and PHY.
Partial preamble/SFD/frame and collision outcomes do not fall back to delivery.

Every capacity limit, radio conflict, malformed wire frame, unsupported relay
action, or unexpected route-discovery request fails the simulation. There is
no direct-delivery fallback.

## Current scenarios

- transmitter through one anchor to gateway, including gateway ACK return;
- transmitter through two anchors to gateway, including multi-hop ACK return;
- a normal click taking an anchor radio slot while transit is attempted,
  followed by origin retry on the still-live negotiated connection without
  route reacquisition;
- a 3 ms low-duty channel-5 scan observing preamble activity but timing out;
- a fully contained frame decoding successfully;
- two overlapping transmissions colliding instead of decoding;
- a truncated first route request, TTL expansion, two-relay request/reply
  formation, exact discovery identity and nonce preservation, and usable routes;
- three and eight eligible responders occupying distinct bounded randomized
  reply slots, including complete reply/ACK exchange inside the origin budget.

`mesh_production_scenarios` adds deterministic production-line regressions:

- one through six adjacent-only relay lines, three sequential payloads per
  line, gateway ACK return, and an explicit no-route-discovery assertion;
- a safe-boundary runtime ordering check: gateway command, local click, event
  repair, then transit; plus a DS-TWR-owned receiver causing a dropped transit
  opportunity and origin retry on the same negotiated route;
- a 400 ms channel-5 wake train over four scan phases. The real 3 ms / 380 ms
  low-duty schedule must see enough preamble to extend that same RX operation
  and accept a valid first-attempt wake claim;
- 50 deterministic anchor claims, one extended-packet assignment table,
  one-to-eight-hop response staggering, forced decoded-claim/ACK collisions,
  four bounded retry rounds, table retransmission, and cumulative ACK
  completion. The test records only modelled decoded outcomes; a scheduled
  response is never treated as delivery by itself.

Assertions cover slot counters, missed-event state, route freshness, TTL at the
gateway, packet order, retry timing, radio transitions, and final delivery.
Additional targets cover gateway-command/click/transit priority, repeated click
spam, route-parent hold-down and alternate selection, six-hop load, 50-anchor
assignment, wake-train politeness, watchdog expiry/reset, BLE fragmentation and
credits, CIR-sized records, and stack-budget bounds.

## Focused hardware-independent models

- `dwm3000_runtime.c` enforces the modeled slow/fast SPI order and deterministic
  reset, wake, retained restore, configure, PLL, RX/TX, status, frame, and CIR
  costs. Illegal SPI/radio overlap fails instead of completing instantly.
- `gateway_ble_transport.c` uses negotiated ATT MTU minus three, preserves a TX
  cursor across rejected completions, decodes arbitrarily fragmented/coalesced
  serial frames, and models bounded notification credits at deterministic
  connection events. A stalled central withholds credits; disconnect drops
  controller in-flight work and reconnect starts a new credit generation.
- `stack_budget.c` records the current five role baselines and evaluates explicit
  measured frames plus an indirect-call reserve. The combined scenario uses the
  largest nested workqueue branch; five sequential clicks do not incorrectly
  multiply one thread's peak stack.

Run these models without the mesh simulator using:

```sh
ctest --test-dir firmware/build -L hardware_models --output-on-failure
```

## Boundary and next seams

This is not Zephyr, SoftDevice Controller, or silicon emulation. The SPI model
uses production/configured delays and conservative bench-derived transaction
costs, but does not execute the vendor driver, interrupts, or DMA. Retained
configuration is an explicit state contract, not a simulation of DWM3000
register contents.

The BLE model stops at the application/ATT/controller-credit boundary. It does
not model BLE analog RF, channel selection, retransmissions, encryption, or the
nRF controller scheduler. UWB and BLE RF concurrency and Zephyr workqueue CPU
contention remain outside these focused models. Simulator watchdogs use the
production timeout policy and explicit progress feeds, but do not emulate the
nRF watchdog peripheral.

Stack calculations are deterministic budget checks, not whole-program proof.
Compiler `.su` call-chain parsing, indirect Zephyr/Bluetooth call resolution,
and hardware high-water measurements remain separate work. Analog UWB path
loss/capture and hardware DS-TWR timestamp/range calculation are also outside
the simulator.

Remaining hardware boundaries are the vendor driver and actual SPI/IRQ/DMA
ordering, Zephyr thread scheduling, SoftDevice Controller coexistence, NVS
power-loss behavior, analog UWB path loss/capture, and hardware DS-TWR
timestamps. Connection-slot orchestration is a simulator adapter; relay, route,
protocol, report, runtime priority, and UWB-session decisions use the
production-independent implementations listed above.

Run only this target with:

```sh
ctest --test-dir firmware/build -L mesh_integration --output-on-failure
```
