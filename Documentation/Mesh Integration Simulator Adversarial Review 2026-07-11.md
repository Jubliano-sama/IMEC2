# Mesh Integration Simulator Adversarial Review

Date: 2026-07-11

Status: Open. The integration-test work must not be called complete until every
finding below is either fixed and regression-tested or explicitly accepted as a
documented residual risk.

Two independent `gpt-5.6-terra` reviewers at `xhigh` reasoning performed
read-only adversarial reviews:

- fidelity review: agent `019f4e3f-f822-7402-95aa-e49fbf2d5164`;
- coverage and false-positive review: agent
  `019f4e3f-fbf3-7322-9617-f974f1422c10`.

Both reviewers read the production mesh contract, inspected the simulator and
production paths, and ran the focused integration and hardware-model suites.
No reviewer changed repository files.

## Completion gate

For each finding:

1. Confirm or reject the failure mechanism against current production code.
2. Add a regression that fails under the proposed mutation or equivalent fault.
3. Fix the simulator, test seam, or production code as appropriate.
4. Run the focused test, all native tests, and ASan/UBSan.
5. Where the finding crosses a vendor-driver, Zephyr, BLE-controller, or actual
   stack-watermark boundary, retain a named hardware acceptance gate.

The high-level contract remains authoritative. Simulator mechanics must not be
added to the contract unless they intentionally change product behavior.

## Fidelity review findings

### F1 - High - Channel 9 bypasses the DWM3000 runtime timeline

The simulated Channel 9 path creates an RX window and starts TX at a fixed
offset, then changes simulator state at TX start. It does not require the
DWM3000 wake, SPI, PHY configuration, frame-write, or TX-start operations to fit
inside the slot.

Evidence:

- `firmware/sim/mesh_sim.c:1931`
- `firmware/sim/mesh_sim.c:1959`
- `firmware/sim/mesh_sim.c:3089`
- production immediate-TX preparation in
  `firmware/app/src/dwm3000_driver.c:3196`
- retained-wake work in `firmware/src/dwm3000_runtime.c:327`

Risk: a first-slot TX or RX can pass in simulation even when real wake,
configuration, or SPI work would overrun the slot.

Required regression: make every simulated Channel 9 RX and TX consume the DWM
runtime operation timeline. A first-slot operation must fail when
prepare/write/start cannot complete before its radio boundary.

### F2 - High - Connection establishment and repair are direct state updates

`mesh_sim_add_connection()` negotiates both peers directly from shared
parameters. Repair also installs state directly. Neither path transmits and
receives the production `MSG_MESH_EVENT_PROPOSE` and
`MSG_MESH_EVENT_ACCEPT` exchange.

Evidence:

- `firmware/sim/mesh_sim.c:731`
- `firmware/sim/mesh_sim.c:1832`
- production propose path in `firmware/app/src/app_mesh_report.c:8445`
- production accept path in `firmware/app/src/app_mesh_report.c:8606`

Risk: collision, truncation, stale accept, reservation rejection, or Channel 5
preemption cannot prevent a simulated repair.

Required regression: encode and schedule propose/accept through the normal
radio path. Collision, partial-frame, stale-identity, and reservation-failure
mutations must not create a connection or report a successful repair.

### F3 - High - No-route originated dispatch is not end-to-end

`mesh_sim_queue_originated()` selects an existing next hop and returns a core
error when no route exists. Relay action handling treats route discovery as a
simulation failure. Production retains the pending packet, starts route
discovery, and resumes delivery after a route is installed. The route-formation
test currently prepares the route request manually.

Evidence:

- `firmware/sim/mesh_sim.c:1107`
- `firmware/sim/mesh_sim.c:2200`
- production route-wait path in `firmware/app/src/app_mesh_report.c:9567`
  and `firmware/app/src/app_mesh_report.c:9580`
- manual test request in
  `firmware/tests/mesh_integration/test_mesh_route_formation_scenarios.c:406`

Risk: the production route-waiting lifecycle, retry ownership, and resumed
delivery can regress while route-formation tests remain green.

Required regression: submit an originated packet with no route through a
production-compatible wait/dispatch seam, drive request/reply formation, then
deliver the retained packet and its acknowledgements without fixture-installed
routes.

### F4 - Medium - Link quality does not affect RF delivery

The simulator records link quality and uses it in route metrics, but every
reachable, contained, non-colliding frame decodes. A marginal parent remains a
perfect RF path until a test explicitly disconnects or collides it.

Evidence:

- `firmware/sim/mesh_sim.c:418`
- `firmware/sim/mesh_sim.c:469`
- unconditional reachable-frame acceptance around
  `firmware/sim/mesh_sim.c:2669`

Required regression: add deterministic directed loss/error schedules and use
them to prove three-failure hold-down, alternate-parent selection, and bounded
rediscovery under contract lines 604-625.

### F5 - Medium - Gateway BLE and UWB run on separate timelines

The gateway BLE scenario finishes mesh delivery, copies the recorded deliveries
into a stream, then starts an independent BLE clock at zero.

Evidence:

- `firmware/tests/mesh_integration/test_mesh_gateway_ble_scenarios.c:350`
- `firmware/tests/mesh_integration/test_mesh_gateway_ble_scenarios.c:639`
- `firmware/tests/mesh_integration/test_mesh_gateway_ble_scenarios.c:712`

Risk: BLE credits, disconnects, reconnects, and stream backpressure cannot delay
gateway mesh handling or contend with click and command work.

Required regression: schedule BLE completion and credit events on the same
timeline as gateway UWB events. Assert bounded mesh latency and queue behavior
during disconnect, reconnect, and backpressure.

### F6 - Medium - Missed radio events feed the simulated watchdog

The simulator records decoded versus missed connection events, but completion
feeds both system and radio watchdogs in either case. Production feeds only
while the required progress leases remain fresh.

Evidence:

- event result around `firmware/sim/mesh_sim.c:2042`
- unconditional feeds around `firmware/sim/mesh_sim.c:2053` and
  `firmware/sim/mesh_sim.c:2057`
- production lease policy in `firmware/app/src/app_watchdog.c:70`

Required regression: keep connection events scheduled while every RX misses.
The radio lease must expire and produce the same feed-stop/reset decision as
production instead of treating event churn as radio progress.

### F7 - Low - Stack evidence is arithmetic rather than measured execution

The worst-case native stack scenario uses literal inputs. Runtime diagnostics
query and print Zephyr stack availability, but the native gate cannot detect a
changed call chain, ISR nesting, or BLE/UWB concurrency peak.

Evidence:

- `firmware/tests/mesh_integration/test_stack_budget_model.c:164`
- `firmware/app/src/app_stack_diag.c:43`

Required regression: parse exact-preset stack configuration and compiler stack
usage where possible, then retain hardware watermark assertions for combined
click spam, relay retry, BLE backpressure, and CIR handling.

## Coverage and false-positive review findings

### C1 - High - Production priority and BLE command ingress are not gated

The priority scenarios exercise the standalone native `mesh_runtime.c`
scheduler. The Zephyr app does not link that module and implements command
priority through work submission. The native BLE stream target links the stream
encoder, not the production `app_gateway_ble.c` GATT/control path.

Evidence:

- `firmware/tests/mesh_integration/test_mesh_runtime_priority_scenarios.c:429`
- app source list in `firmware/app/CMakeLists.txt:395`
- production priority submission in
  `firmware/app/src/app_mesh_report.c:14468`
- native BLE stream target in `firmware/CMakeLists.txt:346`

Risk: contract priority at lines 68-78 and 464-474 can regress in Zephyr
workqueue or GATT completion behavior while native integration remains green.

Required regression: add a Zephyr or native-sim seam from BLE command ingress
through Channel 5 command flood, app radio abort/workqueue ordering, anchor
receipt, and result reporting while local click, transit, and retry work are
pending.

### C2 - High - Four-origin test serializes admission before relay contention

The test queues four reports at their origins, but opens only one downstream
connection at a time and force-closes it after that source drains. It proves
identity, exact delivery, one intentional retry, upstream stability, and no
fallback for controlled serial handoff. It does not place accepted reports from
multiple child anchors into relay custody together.

Evidence:

- simultaneous origin queues at
  `firmware/tests/mesh_integration/test_mesh_network_stress_scenarios.c:1496`
- sequential downstream admission at
  `firmware/tests/mesh_integration/test_mesh_network_stress_scenarios.c:1529`
- direct close after each drain at
  `firmware/tests/mesh_integration/test_mesh_network_stress_scenarios.c:1749`

Mutation that currently escapes: retain only one already-accepted child report
in the relay queue. The next source is not admitted until the previous path has
drained, so the defect is not exercised.

Required regression: make all four anchors ready together and let automated
one-downstream admission rotate sources while the one upstream connection
remains stable. Accumulate multiple accepted child reports in relay custody and
assert per-source order, exact-once delivery, custody/ACK identity, bounded
latency, no fallback, and at most one upstream plus one downstream reservation
at every event.

### C3 - High - Route recovery remains fixture-driven

Route formation stops after selected forward/reverse control hops and does not
send data over the resulting route. The partition scenario restores routes and
downlinks directly. The focused recovery test calls parent-failure accounting
directly.

Evidence:

- `firmware/tests/mesh_integration/test_mesh_route_formation_scenarios.c:816`
- `firmware/tests/mesh_integration/test_mesh_network_stress_scenarios.c:1100`
- `firmware/tests/mesh_integration/test_mesh_route_recovery_scenarios.c:278`

Required regression: after physical parent loss, inject no replacement route.
Drive three failed gateway-ACK cycles and prove hold-down, alternate selection
or discovery, downstream invalidation, packet identity preservation, bounded
recovery, and no wake train while a valid Channel 9 connection remains alive.

### C4 - Medium - ACK and custody behavior lacks an airtime-level scenario

Integration scenarios prove final delivery and some retry state, but do not
directly assert `MSG_MESH_HOP_ACK` identities, duplicate ACK stickiness, or a
hop ACK extending the gateway deadline. Those checks live mainly in a separate
core unit target.

Evidence:

- ACK unit target around `firmware/CMakeLists.txt:229`
- contract ownership requirements at lines 150-160 and 590-595

Required regression: drop the first hop ACK, retransmit the same
`(src_id, session_id, seq)`, and delay the gateway ACK. Prove one payload
delivery, duplicate suppression, correctly addressed hop ACK packet identities,
deadline extension on hop progress, and ownership release only on final gateway
ACK.

### C5 - Medium - DWM and stack models are disconnected from exact app builds

The app builds the vendor-facing DWM3000 driver rather than
`dwm3000_runtime.c`. `stack_budget.c` is also absent from the app target, and its
test validates hard-coded role values rather than generated preset
configuration, compiler call-chain use, or runtime watermarks.

Evidence:

- app driver source list around `firmware/app/CMakeLists.txt:400`
- hard-coded stack baseline in
  `firmware/tests/mesh_integration/test_stack_budget_model.c:60`

Required regression: make exact-preset builds validate generated Kconfig and
compiler stack evidence, and capture RTT high-water marks under combined stress.
This overlaps F1 and F7 but remains a separate build-integration concern.

### C6 - Medium - TTL coverage does not prove the full ladder

Route formation asserts TTL 1 followed by TTL 2. The six-hop data scenario only
asserts that final TTL is nonzero. It does not prove the required discovery
ladder 1, 2, 4, 6 or exact data TTL decrement at each hop.

Evidence:

- `firmware/tests/mesh_integration/test_mesh_route_formation_scenarios.c:672`
- `firmware/tests/mesh_integration/test_mesh_network_stress_scenarios.c:1376`

Required regression: force four discovery attempts through a multi-hop
topology, assert TTL 1/2/4/6, forwarded hop count and TTL at every relay, then
deliver data and acknowledgements over the route formed by attempt four.

## Confirmed strict controls

The reviewers confirmed these simulator properties are real and should be
preserved:

- a frame decodes only when its full arrival is contained in the RX window
  (`firmware/sim/mesh_sim.c:2703`);
- overlapping transmissions are detected as collisions
  (`firmware/sim/mesh_sim.c:2360`);
- only decoded protocol frames enter production relay dispatch
  (`firmware/sim/mesh_sim.c:2637`);
- the current shared-relay scenario proves identities, no duplicates, a stable
  upstream connection, and no fallback for its controlled serial-admission
  topology;
- the stale wake-test comment about a missing DS-TWR handoff is not a current
  functional failure; the handoff is implemented and the test passes.

## Reviewer verification

Both reviewers ran the applicable focused binaries. They also independently
reported:

- `ctest --test-dir firmware/build -L mesh_integration --output-on-failure`:
  9 of 9 passed;
- `ctest --test-dir firmware/build -L hardware_models --output-on-failure`:
  3 of 3 passed.

These green results are evidence for the controls above, not closure of the open
findings.

## Additional Production-Path Cross-Check

The following review points were supplied after the two adversarial reviews.
They are relevant, but they refine existing findings rather than create a
second, competing issue list. A focused simulator-only result must not close
any of the mapped findings below.

### A1 - High - Real app arbitration and preemption are outside the simulator

This is a stricter formulation of C1. Most production work enters through the
app coordinator and preemption path rather than directly through
`mesh_runtime`. The integration gate must invoke the real coordinator decision,
preemption-plan, paused-delivery, Channel 5 priority, and Channel 9 ACK policy
modules in one scenario timeline. It must prove that gateway commands win at a
safe boundary, click/ranging preempts relay and route-wait work, and retained
reports and loss-count attachment are neither lost nor duplicated.

### A2 - High - First Channel 9 slot after wake or retune remains an F1/C5 gate

F1 now models DWM runtime operations and rejects an impossible short deadline,
but it is not closed until the simulator derives the deadline from the same
wake, retained-configuration restore, slow-to-fast SPI ordering, and retune
sequence as the production driver. The regression must exercise the first
connection event after retained sleep and after a PHY change, with insufficient
lead time producing the same explicit failure class as the DWM runtime model.

### A3 - High - Over-the-air repair must use the ordinary Channel 5 contention path

This refines F2. A successful synthetic propose/accept exchange is not enough
if it runs inside an isolated preallocated window. The connection-establishment
scenario must admit normal Channel 5 contention, click preemption, stale
identity/session/sequence rejection, partial frames, and reservation failure;
only a completely decoded, accepted exchange may install or repair a
connection.

### A4 - High - No-route retention must prove the complete wait/resume lifecycle

This refines F3. The retained production outbound must survive route request
ownership, formation retries, route epoch changes, and resume. The test must
prove one final delivery on the selected hop, no double-send, and no fixture
route or direct-delivery shortcut.

### A5 - Medium - Link quality needs a deterministic delivery-failure model

This refines F4. Directed failures are useful fault injection, but the
integration suite also needs deterministic marginal and burst profiles that
exercise the production three-failure hold-down and alternate-parent behavior.
The profile must affect delivery, not only route cost, and must remain
reproducible without random fallbacks.

### A6 - Medium - Watchdog tests must conform to production lease semantics

This refines F6. The simulator must not assume that every missed connection
event is either progress or failure. It must model exactly which recoverable
radio-worker outcomes renew the production lease and which outcomes stop feeds.
Sustained event churn without the required progress must reach the same
feed-stop/reset action as production; a bounded recoverable miss may continue
only when production permits it.

### A7 - High - Long-running multi-origin pressure and simulator capacity are separate risks

This refines C2 and C6. The suite must inject simultaneous accepted reports
from multiple children through the same relay while repair, failure, and
priority churn are active. It must distinguish an intentional simulator
capacity guard from a protocol failure, include a bounded long-run mode that
does not simply terminate at a fixed transition/event/reception array limit,
and resolve the existing sustained six-hop failure before this finding can
close.

## Post-Refactor Terra Review - 2026-07-11

Terra xhigh review `019f4eaf-971f-7420-9e69-499a9b29dad4` reviewed the first
coordinator and gateway-command extraction. Its verdict was high risk still
present: the native helper tests did not yet prove the production boundary.

- **R1 / High / C1-A1 remains open:** the production state capture in
  `app_mesh_report` and the BLE ingress path in `app_anchor` must both invoke a
  shared production-owned arbitration seam. A `mesh_integration` scenario must
  drive BLE command ingress while click, transit, route-wait, and ACK work are
  pending, then prove abort-before-command-work, safe-boundary command flood,
  click resume, and preserved report identity/loss accounting.
- **R2 / Medium:** coordinator runtime capture must derive decisions from each
  live source rather than carry unused queue and connection metadata. Native
  coverage must exercise RX queue, report queue, relay, route wait, ACK
  wait/send, click, survey, and gateway state independently.
- **R3 / Low:** missing capture input must fail explicitly and leave retained
  coordinator state untouched.
- **R4 / Low:** move production snapshot capture and Zephyr DWM/workqueue
  adapters out of the oversized `app_mesh_report.c` into
  `app_mesh_arbitration_zephyr.[ch]`; verify mesh-anchor and mesh-gateway role
  builds after the move.

Remediation owner: Terra worker `019f4eb6-daaf-7db1-a97c-37ba459c8577`.
No item above is considered closed until a follow-up adversarial review finds
no high-risk gap in the production arbitration path.

## Capacity And Watchdog Terra Review - 2026-07-11

Terra xhigh review `019f4ebc-040b-7d20-a977-89416e1255a9` reviewed the first
trace-capacity and watchdog hardening pass. Its verdict was not ready for
closure.

- **R5 / High:** repeated watchdog feeds can accumulate stale future expiry
  events until the pending-event queue reaches capacity. Expiry scheduling must
  be generation-based or otherwise guarantee one pending expiry per role; a
  regression must sustain at least 4,097 feeds at 30 ms without capacity loss.
- **R6 / High:** connection events, RX windows, transmissions, and receptions
  are still append-only lifetime arrays. Replace them with explicit bounded
  telemetry tails and exact counters/stable snapshots so long runs cannot end
  in a simulator capacity error. Prove a six-hop run beyond 512 TX/RX records
  with exact-once delivery.
- **R7 / Medium:** trace detail searches must expose when older evidence was
  truncated, and unknown node IDs must not change count semantics after trace
  eviction.
- **R8 / Medium:** model radio-lease expiry, feed-stop, and reset distinctly
  enough to test repeated recoverable misses and started-but-never-completed
  workers against production lease behavior.
- **R9 / Medium:** keep the six-hop trace-overflow scenario narrowly labelled
  as a forwarding/capacity test until it is combined with real discovery and
  concurrent-origin contention.
- **R10 / Low:** add an ordering regression across repeated event compaction
  for timestamp, priority, sequence, and object identity.

The reviewer also required an immediate split of `mesh_sim.c` into private
trace and scheduler modules. Remediation must preserve complete-airtime RX,
collision, and explicit-capacity invariants while making trace retention
semantics observable to scenarios.

## Stack Evidence Terra Review - 2026-07-11

Terra xhigh review `019f4eba-acd2-7e83-826b-5891c971afd6` found the first
stack/RAM evidence pass insufficient for a production pre-flash gate.

- **R11 / High:** evidence verification is a manual CLI, not part of a
  maintained verified-flash path. Missing compiler evidence, invalid manifests,
  or inadequate RAM headroom must stop the flasher before it touches hardware.
- **R12 / High:** policy must cover the deployable clicker, all five anchors,
  and gateway separately from bench transmitters. The flashable clicker must
  be capable of validated runtime stack watermarks instead of requiring an
  impossible hardware manifest.
- **R13 / High:** runtime proof must require every configured thread owner and
  bind scenario markers, exact generated stack sizes, and a hash of the raw RTT
  capture. A free-form manifest stress label is not proof.
- **R14 / Medium:** compiler evidence needs owner-rooted call-chain accounting
  or an explicit conservative unsupported-owner failure; maximum individual
  frame size cannot prove thread stack use.
- **R15 / Low:** sequential-click stress must either model retained stack state
  or state and prove that repetition is stack-neutral.

Existing synthetic verifier tests and static RAM checks are useful controls,
but do not close R11-R15 until exact builds and captured runtime evidence pass.

## Arbitration Follow-Up Terra Review - 2026-07-11

Terra xhigh follow-up review `019f4ed0-0258-7ca0-9490-00410b6f9cf5` found
the initial C1/A1 remediation incomplete and still high risk.

- **R16 / High:** the arbitration integration scenario assembles helpers but
  does not execute the production BLE ingress, Zephyr priority adapter,
  cooperative DWM receive-abort boundary, command flood, anchor receipt, or
  result path. It must become a production-owned end-to-end host seam that
  drives that complete lifecycle.
- **R17 / Medium:** gateway command work is scheduled immediately after an
  abort request, without proof that the receive loop reached a safe boundary.
  Implement and test an explicit abort-observed/safe-boundary handoff.
- **R18 / Medium:** a failed priority submission can emit an error after the
  command has entered the host queue, leaving it eligible for later execution.
  Failed commands must be removed or tombstoned by identity before reporting
  failure; test that they cannot execute subsequently.
- **R19 / Medium:** click-preemption persistence, timeout-scheduling, and
  report-requeue callback failures are only recorded, not propagated to the
  production caller. Define explicit failure behavior that preserves custody
  and add injected-failure scenarios.

No C1/A1 item is closed until a further adversarial review sees the actual
ingress-to-result test path and no High finding.

## Stack Follow-Up Terra Review - 2026-07-11

Terra xhigh follow-up review `019f4ed3-7557-7aa0-bae3-1036957a4879` found
R11-R15 still incomplete and deployment evidence untrusted.

- **R20 / High:** verified flashing remains bypassable through documented
  direct `west flash` paths and mutable wrapper policy/frequency inputs.
  Production documentation and automation must use the verified path only.
- **R21 / High:** real workloads do not create correlated stack stress windows
  for click spam, relay retry, BLE backpressure, or CIR handling. Implement a
  typed controller with run/sample IDs and reject marker-only evidence.
- **R22 / High:** manifest probe and ELF identity are self-attested. Capture
  must come from a trusted probe/RTT invocation and include target boot image
  identity verified against the exact artifact; reject replay, future time, and
  fabricated evidence.
- **R23 / Medium:** a single multi-owner source such as `app_anchor.c` cannot
  be assigned to one generous thread root. Fail multi-owner attribution until
  it is split or explicitly owner-annotated and prove the lower owner bound.
- **R24 / Medium:** ISR evidence is configuration-only, not a watermark; do
  not present it as runtime high-water proof without a supported measurement.
- **R25 / Low:** prove sequential-click neutrality with repeated real sequences
  and retained queue state rather than a constant boolean.

Exact static evidence for clicker, anchor 1, and gateway is useful but does not
close R20-R25. Hardware proof remains blocked until all deployable preset
builds and trusted captures exist.

## Implementation checkpoint

Checkpoint date: 2026-07-11. This section records work in progress so a later
session must not confuse focused success with completed verification.

- F1 is implemented locally: Channel 9 uses the shared production 15 ms TX
  offset and consumes modeled DWM wake/configure, RX-arm, frame-write,
  TX-start, frame-read, and finish operations. The focused integration target
  passes, including a deliberately impossible 50 us offset that now fails with
  `MESH_SIM_ERR_RADIO_DEADLINE`.
- F2 is implemented locally: a production-like connection API sends decoded
  `MSG_MESH_EVENT_PROPOSE` and `MSG_MESH_EVENT_ACCEPT` frames over Channel 5
  before installing timing. Focused success, collision rejection, and truncated
  ACCEPT rejection tests pass. Direct `mesh_sim_add_connection()` remains an
  explicit established-connection fixture.
- F3 is in progress: no-route originated work is retained, production
  route-request attempt/TTL/backoff logic is invoked, retries are scheduled, and
  the retained packet resumes when a real route becomes selectable. The code
  compiles and the pre-existing focused integration test passes, but the new
  end-to-end no-route regression is not written yet.
- F4 is partially implemented: deterministic directed RX failure schedules are
  available and already prove a truncated ACCEPT cannot repair a connection.
  Alternate-parent/three-failure recovery coverage is still open.
- F5 was implemented by worker `019f4e4c-a0af-70d2-a427-c6f5c2cb3ba2` in
  `test_mesh_gateway_ble_scenarios.c`. Its focused normal and ASan/UBSan tests
  passed, but the parent agent has not yet reviewed or rerun it in the complete
  suite.
- F6 needs a documented disposition. The review's proposed failure mechanism
  conflicts with current production call sites, which credit bounded radio-loop
  iterations even after recoverable RX misses. Do not change watchdog semantics
  until those call sites and the separate system/radio lease model are reconciled.
- C2, C4, and C6 are being implemented by worker
  `019f4e4c-9d8f-7a93-9315-ec76f13ab429` in the stress and route-formation test
  files. Its result has not yet been integrated.
- F7/C5 exact-build stack evidence is being implemented by worker
  `019f4e4c-a380-7550-aee5-602c43f21cda`. Its result has not yet been integrated.
- C1 and the autonomous-recovery portion of C3 remain open.
- No complete native, sanitizer, role-build, or hardware verification has been
  run after the latest F1-F4/F5 edits. Earlier 46-of-46 and role-build results
  predate these remediation edits and must be rerun.
- No post-review firmware has been flashed. Live mapping before the pause was
  anchor probe `E46070D247233537` and synthetic transmitter probe
  `E46070D247394D36`. The required relay-regression transmitter artifact remains
  `build/mesh-transmitter-forcedhop`, not `build/mesh-transmitter`.

## Remediation tracker

| ID | Severity | State |
|---|---|---|
| F1 | High | Partial |
| F2 | High | Partial |
| F3 | High | Partial |
| F4 | Medium | Partial |
| F5 | Medium | Partial |
| F6 | Medium | Partial |
| F7 | Low | Open (see R20-R25) |
| C1 | High | Open (see R16-R19) |
| C2 | High | Partial |
| C3 | High | Open |
| C4 | Medium | Closed for original scope |
| C5 | Medium | Open |
| C6 | Medium | Closed for original scope |

## Capacity And Watchdog Follow-Up Terra Review - 2026-07-11

Terra xhigh follow-up review `019f4ed6-5468-73f3-8663-ac2c4f84bc5b` found
no remaining High finding in the first R5-R10 remediation, but did not close
the work.

- **R8 / Medium:** watchdog expiry sets a role to sleep while already scheduled
  RX/TX completion events still run. Those stale completions then fail event
  ordering instead of being cancelled, and the existing test never schedules
  the claimed in-progress receiver work. Scheduled role work needs a reset
  generation/epoch, stale completion discard, an aborted-worker record, and a
  regression proving no delivery or lease feed after reset followed by fresh
  SFD-timeout recovery.
- **R10 / Low:** compaction coverage checks one initial ordering and then only
  one marker per iteration. It needs repeated multi-event batches that assert
  timestamp, priority, sequence, and unique object identity together after each
  compaction cycle.

The reviewer verified bounded telemetry tails and global snapshot semantics
(R5-R7), fixture-routed capacity behavior without a direct-delivery fallback
(R9), and the existing scheduler tie-break implementation. The simulator split
is present, but `mesh_sim.c` remains above the repository size threshold and
event/reset ownership should be extracted after the functional remediation.

No R5-R10 item is closed until the reset-lifecycle and compaction-order
regressions pass a further adversarial review.

## Original Gap Reassessment Terra Review - 2026-07-11

Terra xhigh review `019f4edf-9277-7360-b5b8-52901dc5b552` inspected the
current implementation and tests for F1-F6 and C2-C6. It found C4 and C6 closed
for their original simulator/core scope, but found five High and four Medium
items still open or only partially implemented.

- **F1 / High / Partial:** Channel 9 uses the production timing model and the
  impossible 50 us deadline is rejected, but no first-slot case proves retained
  sleep recovery or a Channel 5-to-Channel 9 PHY transition against the real
  driver sequence.
- **F2 / High / Partial:** propose/accept is over the simulated radio, including
  collision and truncated-ACCEPT rejection, but ACCEPT installation lacks the
  production reservation guard. Stale ACCEPT, occupied reservation, and click
  preemption during negotiation are not covered.
- **F3 / High / Partial:** the simulator retains no-route work and schedules
  discovery, but the route-formation test queues data only after a route exists.
  It must originate before any route, complete real request/reply retries, and
  prove exactly-once resume without a fixture route.
- **C2 / High / Partial:** four reports can coexist in relay custody, but the
  test scripts each downstream admission and force-closes it. It must prove
  automatic contention/admission while the upstream connection remains live.
- **C3 / High / Open:** recovery still invokes the parent-failure helper
  directly. It must drive three physical Channel 9/gateway-ACK failures and
  prove hold-down, alternate/discovery selection, dependent invalidation, and
  preserved packet identity.
- **F4 / Medium / Partial:** directed failures exist, but link quality still
  does not affect decode. Deterministic marginal and burst profiles must drive
  the C3 recovery scenario.
- **F5 / Medium / Partial:** BLE and UWB share a test timestamp loop, but BLE
  work does not reserve the modeled gateway execution resource. Backpressure
  therefore cannot delay UWB work as the production runtime can.
- **F6 / Medium / Partial:** the original premise is wrong: production credits
  any bounded completed radio worker, including recoverable misses. The exact
  predicate must be documented and tested through repeated recoverable and
  terminal failures. The review also found the watchdog test source newer than
  its binary, so the observed result was stale.
- **C5 / Medium / Open:** model tests do not prove the exact Zephyr preset uses
  matching DWM timing, SPI, stack, and generated configuration assumptions.
  Add an exact-preset generated-config comparison gate.
- **C4 / Closed for original scope:** ACK loss, same-identity retry, duplicate
  suppression, deadline extension, ACK identity, and custody release are all
  asserted through airtime.
- **C6 / Closed for original scope:** the TTL ladder and forward/reverse hop
  decrements are asserted. Its fixture connections do not close F2.

This review observed 11 of 12 then-built mesh-integration binaries passing; the
excluded arbitration target failed and the stale watchdog binary is not valid
evidence. Every partial/open item above remains in the completion gate.

## Simulator Refactor Plan - 2026-07-11

Terra architecture review `019f4ee4-f50f-7aa1-bc0a-1d99d6ba0c9b` found
`mesh_sim.c` still above 4,000 lines and proposed a behavior-preserving split
after the active reset fix is frozen:

- `mesh_sim_relay.c`: relay queues, route-wait ownership, dispatch, and
  delivery/custody behavior.
- `mesh_sim_connection.c`: timing compatibility, over-air propose/accept,
  repair, and connection-event lifecycle.
- `mesh_sim_radio.c`: DWM timing, RX/TX reservations, low-duty scanning,
  complete-airtime containment, partial decode, and collision handling.
- `mesh_sim_events.c`: watchdog leases, work epochs, reset cancellation, and
  event dispatch.

The dependency direction must remain `events -> radio|connection|relay`,
`connection -> radio`, and `relay -> radio|connection`. The scheduler remains
the sole owner of deterministic `(time, priority, sequence)` ordering and reset
cancellation must stay atomic across pending radio, connection, and repair
work. No module may introduce implicit global world state or direct delivery.

## Reset And Scheduler Closure Review - 2026-07-11

Terra xhigh reviewer `019f5124-4d17-73b1-8409-eb4fccbb3a62` found no High
or Medium issue in the R8/R10 remediation and closed both findings.

- Reset advances a role work epoch and atomically cancels role-owned TX, RX,
  connection, repair, runtime, relay, and route events. TX/RX and connection
  completions also reject stale epochs.
- Active workers are aborted exactly once and recorded through the bounded
  trace. Cancelled work cannot feed a lease, deliver, mutate a fresh object, or
  produce an event-order failure.
- The watchdog regression starts overlapping Channel 9 TX and RX, expires
  during airtime, proves no stale effect, and then completes two fresh
  SFD-timeout workers.
- The scheduler regression runs 32 eight-event compaction cycles and checks
  timestamp, priority, sequence-derived ordering, and unique object identity.
- No direct-delivery or seeded-route fallback was introduced; airtime,
  collision, frame-size, and capacity checks remain enforced.

Fresh isolated verification passed 2/2 focused tests, 12/12 mesh-integration
tests, 5/5 hardware-model tests, and 2/2 focused ASan/UBSan tests. The remaining
Low item is the 4,021-line `mesh_sim.c`, addressed by the separate refactor plan.

## Arbitration Second Terra Review - 2026-07-11

Terra xhigh reviewer `019f5124-5045-7da2-9a1f-57ac321d65cb` found R16-R19
still open in commit `11a30ba`.

- **Buildability / High:** the commit snapshot registers uncommitted native
  sources/tests and requires a missing stack-diagnostics config. Fresh native,
  gateway, and anchor configuration all fail before compilation. Current-tree
  files cannot be used to claim that snapshot is buildable.
- **R16 / High:** production BLE decode does call the identity-aware ingress
  seam, but the integration test replaces flood, anchor receipt, and result
  delivery with `simulated_*` callbacks. It does not traverse the production
  command/flood/result lifecycle or explicitly delimit unmodeled GATT and DWM
  airtime.
- **R17 / High:** the first attempt waits for abort observation, but an
  `-EAGAIN` retry directly reschedules command work and can execute without a
  fresh arbitration submission or safe-boundary handoff.
- **R19 / High:** failed outbox persistence records an error but execution
  continues through timeout scheduling, requeue, cancellation, and clearing.
  The production caller is `void`, so custody failure cannot propagate.
- **R18 / Medium:** if post-admission cancellation/tombstoning fails, ingress
  returns before emitting a result while the command remains executable in the
  queue. Tests cover only successful cancellation and the production tombstone
  table is bounded.

R16-R19 remain open until a different implementer fixes these paths and a fresh
review proves every retry re-enters arbitration, failed persistence prevents
destructive custody transfer, cancellation is authoritative by identity, and
the integration scenario runs the production-owned command lifecycle.

## DWM Timing And Preset Design Review - 2026-07-11

Terra xhigh design review `019f512b-8d1c-7f81-b6f8-64760cc64c17` confirmed
F1 and C5 remain open and found four concrete model/production mismatches:

- The simulator can mark the radio asleep after Channel 9 work without calling
  `dwm3000_runtime_enter_retained_sleep()`, allowing the next slot to skip wake
  cost while appearing to start asleep.
- The model's retained cross-PHY path resets directly, while production first
  wakes/restores common state, validates the device, and only then performs the
  full reset/init/configure path.
- `DWM3000_RUNTIME_SOFT_RESET_US` is asserted by tests but not charged by a
  runtime operation.
- The pure timing model and vendor driver duplicate PHY values with no
  compile-time or generated-config comparison.

The remediation must add a pure lifecycle transition plan shared by the driver
adapter and simulator, plus an exact-preset `imec-radio-contract.json` generated
from `.config`, DTS, CMake preset identity, canonical PHY definitions, and
artifact hashes. It must build and verify clicker, anchors 1-5, and gateway.
Required timing cases include retained same-PHY, retained Channel 5-to-9,
awake cross-PHY, exact-ready success, one-microsecond-short deadline failure,
SPI-order failure, and missing/corrupt retained-state recovery. Channel 9 must
be modeled as immediate TX after host readiness, not DWM delayed-DX-TIME.

## Connection And Recovery Design Review - 2026-07-11

Terra xhigh design review `019f512b-8fe2-7e13-b1ae-970b09fd5d78` specified the
production seams required to close F2-F4 and C2-C3:

- Extract a shared connection-transaction policy validating full
  `(src,dst,session_id,seq)` identity before reservation and guarded install.
  Both production and simulation must use it; simulator connection code remains
  wire/timer orchestration.
- Test stale/truncated/colliding ACCEPT, occupied reservation, install failure,
  and click preemption with complete rollback and no timing installation.
- Originate a click report before any route, retain the exact identity through
  actual request/reply retries, resume once, and release only on gateway ACK.
- Replace scripted four-child relay admission with over-air contention while
  the upstream connection remains live. Preserve four distinct custody records
  and four matching gateway ACKs.
- Drive parent failure through physical ACK loss and a deterministic
  link-quality profile. Per contract, failures 1-3 retry the current parent;
  failure 4 starts hold-down and alternate/discovery. Preserve report and ACK
  identity throughout.
- Link-quality `SPREAD` and `BURST` profiles must apply only after full-airtime,
  collision, partial-frame, and scripted-failure decisions, with deterministic
  traceable ordinals.

## Deployment Audit Reassessment - 2026-07-11

Terra xhigh reviewer `019f512e-8537-7203-921e-b879b2649f69` read the actual
host file `/tmp/IMEC2-mesh-firmware-audit-20260710.md`. No P0 item is fully
closed in the production app path.

- P0-1, P0-2, P0-3 are Partial: core/helper fixes exist, but three-hop send
  policy, reset-between-ACK custody, and guarded initiator ACCEPT coverage are
  still absent.
- P1-1 through P1-4 and P1-6 are Partial; P1-5 is Closed.
- P1-7 is Partial and regressed: a different-identity deferred route request is
  rejected and the caller discards that result, silently losing work.
- D1 and D2 are Partial with policy/simulator coverage only. D3 is Open: erased
  NVS leaves a production anchor unprovisioned and silent, without an enforced
  deployment contract or autonomous join.
- System-ON low-power failures have bounded recovery, but System-OFF still
  ignores divider/radio shutdown failures.
- The stack guard was Open because its native target could not link; exact
  runtime stack evidence and all-preset RAM proof remained absent at review
  time.

The deployment-audit queue remains a release gate: Channel 9 custody/reset,
route-action ownership and P1-7 disposition, guarded connection acceptance,
discovery persistence/provisioning policy, low-power false-success, and trusted
stack/RAM evidence all require production-path tests.

## Simulator Ownership Refactor Review - 2026-07-11

Terra xhigh reviewer `019f5139-8610-7021-a94a-d1f7bee986fd` found no High and
no behavior regression, but did not accept the split because of one Medium
ownership cycle:

- **Medium:** events dispatches into radio while radio calls the event-owned
  runtime-claim function. Pass a narrow claim callback/context into radio so
  radio no longer imports event/lease ownership.
- **Low:** consolidate three private `interval_overlaps` copies into one pure
  internal owner.
- **Low:** split the 335-line event dispatcher into focused TX, RX, and runtime
  release handlers without changing dispatch order.

The reviewer independently passed 12/12 mesh-integration, 6/6 hardware-model,
12/12 ASan/UBSan integration, frozen reset/scheduler, symbol, CMake-registration,
and whitespace checks. R8/R10 remain closed. The refactor itself remains open
until a different implementer removes the dependency cycle and a follow-up
review accepts it.

## R16/R19 Remediation Checkpoint - 2026-07-11

The production gateway command path now shares one orchestrator context across
BLE decode, priority admission, receive-abort safe-boundary handoff, stateful
flood preparation/send, anchor command dedup/result construction, and gateway
result delivery. Retries clear and re-enter arbitration before resubmission;
the boundary reads live radio ownership rather than a literal DS-TWR state.
Non-command BLE frames retain the decoded shared item before generic routing.

Click preemption now stages a full persistent outbox handoff before cancellation,
commits it before clearing the old outbox, and retains it until the same outbound
identity has been durably re-established. Restart recovery selects the committed
journal over any obsolete primary copy; staged records provide reset fallback.
Callback failures return actionable errors and identify journal recovery as the
owner instead of falsely reporting active runtime custody.

Fresh native verification passed 61/61 tests, including 12/12
`mesh_integration` and 7/7 `hardware_models`; mesh clicker, gateway, and
anchor-1 builds passed. Whole-tree ASan/UBSan built but did not pass because the
unrelated stack diagnostic test indexes element 9 of an 8-element state array;
that stack path remains outside this remediation scope. R16/R19 require a
follow-up adversarial review before closure.

## Forced-Hop Live Regression Checkpoint - 2026-07-11

The live source was the exact `mesh_transmitter_forcedhop` artifact on probe
`E46070D247394D36`: its direct gateway exchanges were contact-only probes, not
an installed direct parent. Every probe was followed by a relay-required route
request and the source remained `ready=0 next=0x0`. Probe
`E46070D247233537` did not contain the current `mesh_anchor_1` artifact (its
vector table differed) and exposed no RTT control block, so no anchor route
reply, propose/accept, or Channel-9 relay connection could form.

The runtime now independently vetoes direct-route installation whenever the
forced-relay preset is compiled, and the simulator carries the same
relay-required request flag. `mesh_forcedhop_connection_scenarios` models a
gateway-reachable source, over-air anchor gateway contact, a forced Channel-5
request/reply through the anchor, over-air propose/accept, six repeated
bidirectional Channel-9 data events, one click-shaped preemption without
premature teardown/repair, and four successful recovery events. It asserts the
source never selects the gateway directly and never changes away from the
anchor parent. No seeded source route or direct-delivery fallback is used.

## Channel-5 Reply and Forced-Hop Hardware Closure - 2026-07-12

The apparent `ret=-5`/`rx_failure=4` route-request CRC regression was not an
FCS mismatch. The failing anchor snapshots were `SYS_STATUS=0x00001700` with
`RX_FINFO=0`: trailing standard-PHR wake claims were reaching a receiver that
had already switched to the extended-PHR control configuration. Extended-PHR
150-byte route requests subsequently decoded correctly. The reply was blocked
by a stale deferred request: `uptime_ms_until_deadline()` intentionally returns
one millisecond after expiry, and the deferred worker treated that value as a
future deadline forever. A dedicated due-time helper now returns zero for
expired deadlines. Valid event proposals captured by that listener are queued,
the RX owner is released, and the proposal is handled before the accepted
contact is closed. The measured ACCEPT reanchor shift was 16-17 ms; the prior
15 ms reservation rejected it, so the explicit bound is now 20 ms, with 17 ms
accepted and 21 ms rejected in native coverage.

`mesh_transmitter_forcedhop` alone treats every valid direct gateway reply as
contact-only, including duplicates. Other roles retain normal direct-route
behavior. `mesh_forcedhop_connection_scenarios` uses production encoding and
radio configuration to deliver the direct probe/replies, route wake/request/
reply, proposal/accept, and repeated bidirectional Channel-9 data; it is labeled
both `mesh_integration` and `hardware_models`. Fresh results were 63/63 native,
14/14 mesh integration, and 8/8 hardware models, with focused and both label
suites also clean under ASan/UBSan.

The final 4 MHz images were forced-hop probe `E46070D247394D36`, ELF
`ac0747442bb59e109ebbe7df9fc1813b520df758a1025e150b9f9b7c7e9a3012`,
and anchor probe `E46070D247233537`, ELF
`1a7e4d611cb5afb3dd2d5099b1d121abc29f27e7b6af60cf1f2e23a82eebc19d`.
Paired logs `connected-forcedhop-transmitter-E46070D247394D36-20260711T234222Z.typescript`
and `connected-anchor-E46070D247233537-20260711T234222Z.typescript` show four
contact-only gateway replies and zero direct-route markers, two decoded anchor
route replies, eight successful ACCEPT realignments, zero ACCEPT abort/failure,
and no reset. The first installed segment ran from ACCEPT at 23.279 s through
the last observed event at 72.628 s: 69 completed Channel-9 events (23 source
sends and 46 expected peer receptions), with no repair or timing expiry. Later
repairs followed 16 consecutive empty receive cycles, the contract's sustained
inactivity teardown, rather than a rapid-repair loop.

## Final Pragmatic Alpha Stabilization Checkpoint - 2026-07-12

No hardware was flashed in this stabilization pass. Fresh isolated native
builds passed full CTest 63/63, `mesh_integration` 14/14, `hardware_models`
8/8, the four Python evidence/policy modules 20/20, and whole-tree ASan/UBSan
CTest 63/63. The native registration manifest contains 63 tests for the 63
`add_test()` entries; `git diff --check` passed and the fresh binaries were
rebuilt before the final CTest listing.

Fresh exact preset builds passed for `mesh_clicker`, `mesh_anchor_1` through
`mesh_anchor_5`, `mesh_gateway`, and `mesh_transmitter_forcedhop`. The tight
linker RAM margins are gateway 4,932 B (3.76%), anchors 6,752 B (5.15%), and
forced-hop transmitter 7,776 B (5.93%); all images fit. The repository stack
verifier still rejects the fresh deployable artifacts on conservative static
headroom thresholds and incomplete compiler-function attribution. This does
not invalidate the functional build or the hardware closure above, but it does
leave verified deployment qualification pending fresh schema-3 runtime captures
for each exact deployable artifact.

`Mesh Integration Coverage Matrix 2026-07-12.md` records the evidence and an
89.3% behavior-row score: 11 Covered rows plus three bounded Partial rows.
The known theoretical limits remain explicit deferrals, including exhaustive
Zephyr callback equivalence, hostile local flash bypass, perfect IPA
attribution, every NVS power-loss phase, probabilistic RF realism, and
all-seven-role runtime captures. The observed forced-hop contact-only relay
regression is Covered by the hardware closure above and is not left as a
partial or deferred claim.
