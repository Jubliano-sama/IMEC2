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
- `mesh_event_owner.c` binds UPDATE and END to the PROPOSE operation that owns
  a connection, including independent local and remote sequence domains.
- `discovery_assignment.c`, `survey.c`, and the survey lease/transaction
  modules apply assignment, discovery, pair-control, and result identities.
- `gateway_collection.c` applies durable bundle and collection-EACK
  idempotence after the gateway relay requests semantic delivery.
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

## Deterministic protocol stress runner

`mesh_stress` is the short feedback loop for protocol work. `one-hop` drives
real encoded mesh data and ACKs between an origin and gateway, while
`busy-line` gives every node in a three-node adjacent-only line local traffic at
the same time. `queue-full` fills the production-sized origin queue and requires
the next admission to fail explicitly. The runner executes the production
parser, relay, route, retry, duplicate, ACK, and runtime code. The simulator
owns the clock, radio/connection scheduling, and a queue-admission adapter that
uses the production capacities and ACK-coalescing identities.

The relay line has recurring Channel-9 connections only between adjacent
non-gateway nodes. Its final anchor-to-gateway hop uses the production direct
gateway radio turn, and the runner requires the gateway ACK to complete within
the sender's same 250 ms receive window after the fixed 10 ms guard.

Receiver-side loss, ACK-only loss, duplication, and bounded extra delay are
chosen from a fault RNG separate from the protocol scheduling RNG. This means
turning a fault on does not silently change the production-policy random
sequence. Rates are parts per 10000. Extra delay can produce reordering, and a
busy-line run can reset its middle node at an exact simulator step.

Each runner radio or connection-action boundary checks queue bounds and entries,
relay/runtime ownership, operation-generation work, semantic delivery counts,
and duplicate delivery. Terminal checks require bounded retries, one terminal
ACK per surviving origin packet, released queues and pending work, and a valid
settled world. A JSONL trace records the configuration, retained ordered
transitions with fault decision ordinals, gateway deliveries, timing, and
terminal fault counts. A run fails if any retained telemetry ring truncates, so
a passing trace is complete for every bound it proves.

The simulator scheduler also bounds total event dispatch and consecutive
dispatches at one timestamp. A callback chain that continually reschedules
itself, including a zero-time worker loop, therefore returns
`MESH_SIM_ERR_LIVENESS` with the blocked event identity instead of consuming a
host thread indefinitely.

`mesh_protocol_lifecycle_scenarios` is the continuous forced-relay slice for
protocol changes. It sends real encoded frames through gateway route
advertisement, assignment CLAIM/TABLE/ACK, survey discovery reports, pair planning,
PREPARE/result reconciliation, and a pair sample. It injects a lost hop, a
stale prior assignment table, a stale prior survey result, and an exact sample
duplicate, then requires bounded retries, direct-gateway ACK custody, and a
fully settled network:

```sh
ctest --test-dir firmware/build-stress \
  -R '^mesh_protocol_lifecycle_scenarios$' --output-on-failure
```

The real Here-I-Am route request/reply/ACK and reverse-route formation, deeper
discovery announce/listen, assignment publisher/persistence,
START/GO/ABORT lease cleanup, survey journal/reset, and maximum-depth topology
cases remain in their focused assignment, gateway-control, survey PHY/topology,
and pair-lease suites. They are part of the required labels below rather than
being approximated inside one oversized scenario.

`mesh_survey_phy_scenarios` carries one gateway-planned pair across the
remaining radio boundary. Both endpoint leases cross PREPARE, START custody,
and matching GO before real production POLL, RESP, FINAL, and REPORT codecs run
over complete channel-5 airtime windows. The first FINAL has no receive window,
so the same sample retries within the public initiator timeout; all three
samples then complete exactly once, a duplicate sample is idempotent, stale
operation-N input cannot mutate operation N+1, and the gateway round plus both
radios must release all ownership within an exact 15-transmission bound. Pair
results carry the synchronized round generation; the runtime regression queues
the identical pair for rerun and proves a delayed result from batch N cannot
fill batch N+1's freshly cleared sample mask.

The forced RF lifecycle tests decode a real frame before handing it to a small
application adapter that invokes the corresponding production state owner.
They do not instantiate the Zephyr workqueue, BLE host, or full application
coordinator; the source-invariant and application-policy tests in the same
matrix cover those seams separately.

`mesh_survey_round_adversarial_lifecycle` extends the continuous path through
all four discovery announce rounds and PREPARE/START/GO/ABORT. It forces a
gateway-to-relay-to-leaf topology and injects lost, duplicated, corrupted,
reordered, stale, and expired controls plus a reset and route repair between
operations N and N+1. It bounds total RF traffic, identical control traffic,
elapsed simulated time, semantic completion counts, lease ownership, and final
settled state. The seed controls the production forwarding jitter and is
printed with an exact replay command on failure:

```sh
firmware/build-stress/test_mesh_survey_round_adversarial_lifecycle \
  --seed 0x5eed5307
```

Sweep forwarding-jitter seeds without rebuilding; the first failure prints its
exact one-command replay:

```sh
for seed in $(seq 0 255); do
  firmware/build-stress/test_mesh_survey_round_adversarial_lifecycle \
    --seed "$seed" || exit 1
done
```

`mesh_event_control_rf_scenarios` negotiates PROPOSE/ACCEPT over RF on two
adjacent links in a forced four-node route, then injects duplicate, reordered,
malformed, stale, lost, reset-crossing, and expired UPDATE/END controls. It
requires complementary TX/RX parity, rejects operation N controls after N+1
starts, bounds the lost-END asymmetry by supervision expiry, and returns both
links to sleeping, ownership-free state.

`mesh_result_custody_rf_scenarios` drives RESULT_OFFER/GRANT, child result
custody, bundle construction, semantic gateway acceptance, gateway ACK, and
collection EACK. It covers queue pressure, persisted reset recovery, a
conflicting bundle, lost gateway ACK, exact durable duplicate classification,
stale/malformed/lost EACKs, route changes, and bounded terminal cleanup. A
bundle is retried only by its production owner; the simulator does not invent
unsupported intermediate bundle custody.

The active survey flow uses `MSG_SURVEY_DISCOVERY_REPORT` (0x55). Legacy
`MSG_SURVEY_REACH_REQ`/`MSG_SURVEY_REACH_REPORT` (0x50/0x51) remain codec and
relay compatibility types covered by the `survey` unit target; they are not
presented as the active mesh orchestrator path.

Run the complete focused lifecycle matrix while iterating on protocol timing or
custody. It combines click wake/discovery/ranging ownership and real
`MSG_CLICK_REPORT` multihop delivery, direct gateway probing and Here-I-Am route
formation, PROPOSE/ACCEPT/UPDATE/END, assignment publisher/persistence, survey
discovery and pair-ranging timing, result/EACK custody and persistence, generic
fault and reset models, post-operation liveness, and the known busy-line
ACK-loss regression:

```sh
ctest --test-dir firmware/build-stress -L protocol_matrix \
  --output-on-failure
```

Configure a fresh host build before using it as regression evidence; rebuilding
an old directory can otherwise leave stale test binaries:

```sh
cmake -S firmware -B firmware/build-stress -DCMAKE_BUILD_TYPE=Debug
cmake --build firmware/build-stress -j4
```

The fastest one-hop fault replay and a busy multi-node baseline are:

```sh
firmware/build-stress/mesh_stress \
  --scenario one-hop --seed 0x51a7e551 --fault-seed 0x12345678 \
  --packets 2 --loss 0 --ack-loss 3000 --duplicate 3000 \
  --delay 5000 --max-delay-us 4000 --max-steps 300 \
  --trace /tmp/mesh-one-hop.jsonl

firmware/build-stress/mesh_stress \
  --scenario busy-line --seed 0x10 --fault-seed 0x20 \
  --packets 2 --max-steps 300 --trace /tmp/mesh-busy-line.jsonl

firmware/build-stress/mesh_stress \
  --scenario queue-full --seed 1 --trace /tmp/mesh-queue-full.jsonl
```

Sweep independent deterministic cases in parallel with the repository runner:

```sh
.venv/bin/python firmware/scripts/run_mesh_stress.py \
  --build-dir firmware/build-stress --scenario busy-line \
  --seed-start 0x1000 --count 1000 --jobs 8 --packets 2 \
  --loss 50 --ack-loss 300 --duplicate 300 \
  --delay 500 --max-delay-us 4000
```

The sweep copies and hashes `mesh_stress` once before starting workers, then
runs every seed from that immutable campaign snapshot. It exits nonzero if any
case fails and reruns retained failures once with tracing under
`logs/mesh-stress-failures/`, where `case.json`, the first and replay outputs,
`trace.jsonl`, and executable `replay.sh` preserve the exact protocol seed,
fault seed, arguments, binary hash, sanitizer environment, and wall timeout.
Per-case executables are hard links to one shared content-addressed snapshot,
so many failures do not duplicate a sanitizer binary. By default at most 25
full replay directories are retained; `--max-failure-artifacts N` changes that
cap, while the compact campaign manifest still lists every failing seed and a
bounded replay command. The same retained case can also be run using the
`replay:` command printed on stderr. Use
`--fault-seed-start` when the protocol and fault sequences need independent
linear sweeps, `--jobs 0` for all host CPUs, and `--reset-step N` only with
`busy-line`.

The passing profile above is the merge gate. Raise loss, ACK loss, duplication,
and delay for exploratory campaigns; those campaigns may intentionally expose
the operation-liveness bound, and every such seed still produces the same
snapshotted executable and replay artifacts instead of hanging indefinitely.

For ASan and UBSan, create a separate build so sanitizer flags cannot leak into
ordinary evidence:

```sh
cmake -S firmware -B firmware/build-stress-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build firmware/build-stress-asan -j4
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir firmware/build-stress-asan -L protocol_matrix \
    --output-on-failure
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  .venv/bin/python firmware/scripts/run_mesh_stress.py \
    --build-dir firmware/build-stress-asan --scenario busy-line \
    --seed-start 1 --count 500 --jobs 4 --packets 2 \
    --loss 50 --ack-loss 300 --duplicate 300 \
    --delay 500 --max-delay-us 4000
```

LeakSanitizer is disabled because it cannot run reliably under this host's
agent/ptrace environment; AddressSanitizer and UndefinedBehaviorSanitizer remain
active. Do not reuse this build for performance measurements.

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

Run the combined hardware-boundary gate, which includes both focused peripheral
models and simulator-backed radio-boundary scenarios, using:

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

## Validation ladder

Most protocol edits should finish at the first or second tier. Moving upward is
driven by the boundary changed and the evidence needed, so a parser, retry, or
state-machine edit does not require reflashing every board for every iteration.

1. **Focused host regression.** Build fresh, replay the relevant one-hop seed,
   and run the focused CTest target while iterating on parsing, ACKs, queues,
   retries, duplicate handling, or state ownership.
2. **Host stress gate.** Run a busy-line seed sweep, the sanitizer sweep, and
   both required labels before merging production-candidate mesh behavior.
3. **Exact-preset build and static stack gate.** Build the affected nRF52833
   role and run the stack verifier when a change alters Zephyr integration,
   call chains, locals, logging, workqueue ownership, or RAM. This still needs
   no hardware.
4. **Candidate hardware gate.** Flash one candidate per affected role, then run
   many workload cases against that same image. Repeat the full multi-board RF
   regression when the change touches the DWM3000 driver, SPI/IRQ/sleep-wake,
   airtime/RX windows, Zephyr scheduling, BLE controller coexistence, or a
   release boundary.

Build the complete fresh tree, then run the two mandatory host labels:

```sh
cmake --build firmware/build-stress
ctest --test-dir firmware/build-stress -L protocol_matrix --output-on-failure
ctest --test-dir firmware/build-stress -L mesh_integration --output-on-failure
ctest --test-dir firmware/build-stress -L hardware_models --output-on-failure
```

An opt-in nRF52833 stack-stress artifact adds the MPU stack guard, compiler
canaries, and the existing caller-driven per-thread watermark snapshot at typed
workload sample boundaries. It adds no periodic analyzer thread and does not
increase any stack size:

```sh
.venv/bin/west build --no-sysbuild -s firmware/app \
  -b nrf52833dk/nrf52833 --build-dir build/mesh-anchor-stack-stress -- \
  -DIMEC_BUILD_PRESET=mesh_anchor -DIMEC_STACK_STRESS_BUILD=ON

.venv/bin/python firmware/scripts/verify_stack_evidence.py \
  --build-dir build/mesh-anchor-stack-stress
```

Replace `mesh-anchor` and `mesh_anchor` together with `mesh-clicker` /
`mesh_clicker` or `mesh-gateway` / `mesh_gateway` for the other exact role.
Host stack consumption and native sanitizer success are never evidence that an
nRF52833 stack is safe; only the exact cross-compiled call-frame gate plus
on-target guards, canaries, thread high-water output, and typed workload
captures cover the embedded stacks.

## Flash once, run many hardware cases

Stage the exact stack-stress artifact once through the verified wrapper. This
is the only programming operation in the transaction; it performs a full
readback and leaves a durable `awaiting_qualification` journal:

```sh
.venv/bin/python firmware/scripts/flash_verified_mesh.py \
  --build-dir build/mesh-anchor-stack-stress \
  --probe-id <ANCHOR_PROBE_ID> --stage-only
```

Then run the capture and a role-specific real workload concurrently against
that staged artifact; an anchor example is:

```sh
.venv/bin/python firmware/scripts/capture_stack_evidence.py \
  --build-dir build/mesh-anchor-stack-stress \
  --probe-id <ANCHOR_PROBE_ID> --output-dir logs/stack-evidence \
  --duration-seconds 600

.venv/bin/python firmware/scripts/provision_mesh_anchor.py \
  --gateway <GATEWAY_BLE_ADDRESS> --command survey \
  --require-survey-success --expected-anchors 3 --expected-pairs 3 \
  --duration 300
```

The required successful typed workloads are `click_activity` for a clicker,
`anchor_survey_report` for an anchor, and all of `gateway_report_ingress`,
`gateway_priority_control`, and `ble_backpressure` for a gateway. Promote the
qualified, already-running artifact through the same wrapper, never through
direct `west flash` for a deployable mesh role:

```sh
.venv/bin/python firmware/scripts/flash_verified_mesh.py \
  --build-dir build/mesh-anchor-stack-stress \
  --hardware-manifest \
    logs/stack-evidence/mesh-anchor-<capture-id>.json \
  --probe-id <ANCHOR_PROBE_ID>
```

Promotion verifies only the staged artifact's code sectors, so ordinary NVS
changes from qualification are allowed; it consumes the manifest and writes no
firmware. Before or after promotion, `capture_stack_evidence.py` can attach to
the already-loaded image and record each new bounded scenario without
programming it. Start it in one terminal and inject a workload from another,
for example:

```sh
.venv/bin/python firmware/scripts/capture_stack_evidence.py \
  --build-dir build/mesh-anchor-stack-stress \
  --probe-id <ANCHOR_PROBE_ID> --output-dir logs/stack-evidence \
  --duration-seconds 600

.venv/bin/python firmware/scripts/provision_mesh_anchor.py \
  --gateway <GATEWAY_BLE_ADDRESS> --command here-i-am \
  --repeat 100 --interval 0.05 --duration 60 \
  --notification-hold-s 5
```

Use repeated `assign-slots` or `survey` commands with fresh identities to
exercise re-entrant admission and BUSY handling, and use `qualify-reachability`
when terminal direct/multihop evidence is required. The CLI's `--repeat`,
`--interval`, `--duration`, and `--notification-hold-s` parameters change the
host workload, so these cases do not require a firmware rebuild or reflash.
Capture gateway traffic independently when diagnosing loss or latency:

```sh
.venv/bin/python tools/mesh_ble_route_monitor.py \
  --gateway <GATEWAY_BLE_ADDRESS> --duration-s 600 \
  --include-all-mesh-data --jsonl-file logs/mesh-stress-gateway.jsonl \
  --jsonl-fsync --verbose
```

The native simulator remains the main iteration loop. This flash-once workflow
checks embedded stack and real scheduling/RF boundaries for a candidate; it is
not a reason to repeat a full physical regression for every host-proven
protocol edit.
