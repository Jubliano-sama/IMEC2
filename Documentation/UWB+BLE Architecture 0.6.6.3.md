# UWB+BLE Architecture

Version: 0.6.6.3
Previous version: [UWB+BLE Architecture 0.6.6.2](<UWB+BLE Architecture 0.6.6.2.md>)
Status: Current production-candidate architecture

## Changelog

### 2026-07-26 - 0.6.6.3

- Replaced the gateway survey's split global lifecycle with one pure
  `gateway_survey_machine` and one serialized `mesh_route` actor.
- Removed the retired `app_gateway_survey_round` owner and its
  implementation-spelling tests after native, sanitizer, integration,
  hardware-model, and exact-role compile gates passed.
- Made survey deadlines 64-bit, bound asynchronous completions to a generation
  and delivery token, and moved pair planning to the closed collection
  boundary.
- Aligned stack-workload ownership with the runtime queue. Existing gateway
  qualification captures that name the system workqueue no longer describe
  this artifact and must be recaptured before deployment.
- Recorded that this migration changes internal ownership only. It does not
  change the wire format, channel plan, role behavior, retry policy, LEDs, or
  power policy.

## Authority and scope

The [Mesh Connected Routing Contract](<Mesh Connected Routing Contract.md>) is
the behavioral authority for channel ownership, priority, route formation,
delivery, ACKs, custody, gateway admission, and survey control. Packet fields
and timing detail belong in
[UWB+BLE Protocols and Strategies 0.3.12.4](<UWB+BLE Protocols and Strategies 0.3.12.4.md>).
The [Mesh Connected Routing Walkthrough](<Mesh Connected Routing Walkthrough.md>)
describes runtime flow, and the
[Development and Deployment Guide](<Development and Deployment Guide.md>)
owns build, qualification, and deployment procedure.

The production-candidate line consists of three exact presets:

| Preset | Product responsibility |
| --- | --- |
| `mesh_clicker` | Battery clicker. It normally sleeps, wakes for a physical click, arbitrates with peers, discovers anchors, runs DS-TWR, and gives the event a stable identity. |
| `mesh_anchor` | Common anchor. It derives stable identity from nRF FICR, persists gateway-assigned logical order, ranges local clicks, relays accepted traffic, and prioritizes local click custody over transit without deleting transit. |
| `mesh_gateway` | Mesh root and host edge. It receives gateway-bound traffic, commits accepted records, returns gateway ACKs, and originates host-requested control. |

`mesh_transmitter` and `mesh_transmitter_forcedhop` are powered traffic
generators. ML, generic-role, staged-debug, and high-debug presets are
collection, compatibility, or bring-up images rather than alternate product
architectures.

## Radio and BLE boundaries

Every product device has one DWM3000, so channel 5 and channel 9 are serialized
through one radio owner.

- **Channel 5 is the contact, control, ranging, and preemption plane.** It
  carries click wake, discovery, schedules, DS-TWR, route formation,
  assignment, gateway control, and anchor-survey control.
- **Channel 9 is the connected payload plane.** It carries scheduled reports,
  transit traffic, hop ACKs, gateway ACKs, and related delivery work. Connected
  anchors retain bounded channel-5 receive opportunities between channel-9
  turns. The gateway remains a bounded-slice channel-9 receiver rather than a
  normal scheduled peer.
- **BLE is an edge, not a mesh hop.** The gateway exposes packet
  notifications, command writes, and identity to the host. Clickers use a
  bounded channel-37 courtesy hint while UWB remains the admission authority.

Local click work outranks transit at a safe radio boundary, but accepted
transit keeps explicit custody and retry state. Gateway-originated control also
runs at the first safe boundary. The survey ownership migration does not alter
those priorities or introduce a second radio owner.

The compiled connected-radio defaults remain in
[`mesh_radio_timing.h`](../firmware/include/mesh_radio_timing.h). The principal
values are a 400 ms wake train, 3000 us anchor scan slice rescheduled after
380 ms, 12000 us discovery slot, 440 ms channel-9 event interval, 120 ms event
window, and 30000 ms event supervision horizon. Host operation profiles may
change validated orchestration budgets in RAM, but they cannot override PHY
configuration, measured airtime, retune guards, antenna delays, delayed-TX
quantization, or compiled radio invariants.

## Click, routing, and custody

One participant click remains a bounded session:

1. The button wakes the clicker. BLE courtesy and decoded channel-5 activity
   may defer it for a bounded interval.
2. The clicker sends the channel-5 wake train, discovers anchors in complete
   airtime slots, and issues one range schedule.
3. The normal click path requires at least three unique successful anchors and
   requests two DS-TWR samples per scheduled anchor.
4. The click identity may retry inside its absolute report deadline. A failure
   is explicit; it cannot be converted into success by accepting too few
   anchors or using a hidden route fallback.
5. Anchors enqueue local-origin reports. Local custody runs first at the next
   safe mesh opportunity while accepted transit remains retryable.

Route knowledge and channel-9 timing are independent. A remembered next hop
does not prove that a recurring event is currently usable. Normal acquisition
tries bounded direct-gateway contact, then controlled channel-5 request/reply
discovery when a relay path is required.

New and migrated communication uses immutable `node_comm` datagrams. One
delivery owner holds the identity, absolute deadline, actual RF attempts,
route, randomized retry state, pause state, persistence boundary, ACK state,
and terminal result. Pre-RF refusal consumes no RF attempt. Hop ACK transfers
custody to the next relay; gateway ACK proves final acceptance. The gateway
performs semantic validation and host-stream reservation or commit before
sending the ACK that releases upstream custody.

State that promises reset survival is journaled. Stable anchor assignment,
deferred accepted mesh custody, durable survey discovery reports, gateway
collection state, and host-visible click admission have explicit persistence
owners. A volatile in-progress orchestration may be abandoned by reset and
must start later work in a new generation; the contract does not claim
unimplemented universal reset continuation.

## Anchor self-setup survey

Survey discovers participating anchors, measures anchor-to-anchor distances,
and exports those measurements to the host geometry solver. Firmware does not
solve the final office layout.

The gateway survey now has one lifecycle state object:
[`gateway_survey_machine.h`](../firmware/include/gateway_survey_machine.h) and
[`gateway_survey_machine.c`](../firmware/src/gateway_survey_machine.c). The
machine owns discovery admission, the operation and collection horizons,
expected-count policy, round dispatch, GO, observation, per-lane cleanup,
reruns, abort, and terminal reason. It contains no Zephyr work primitive and
requires its caller to serialize every transition.

The Zephyr adapter serializes survey polling, host-command retry, result
timeout, and abort cleanup on the existing `mesh_route` workqueue through
[`app_mesh_route_owner_queue.c`](../firmware/app/src/app_mesh_route_owner_queue.c).
The only cross-queue signal is the DWM3000 receive-abort request used to make a
bounded receive slice yield before the mesh-route actor performs abort
mutation. Owner work intentionally remains runnable while transport admission
is paused so cleanup cannot be stranded.

Each survey begin advances a nonzero generation, and discovery completion also
has a nonzero delivery token. A stale callback, token, survey ID, or round
generation cannot mutate a later operation. The machine uses 64-bit absolute
deadlines; equality is terminal rather than one extra retry. A discovery report
is admitted only after current-operation RF evidence.

Collection closes before the gateway plans pairs. It plans the reachable graph
once, then divides it into conflict-free batches. A batch can retain at most 25
lanes; the safe profile starts with one and a validated host profile may choose
a larger cap. PREPARE and START control remains serialized. After all live
endpoints are armed, one future age-compensated GO releases the batch. Cleanup,
deadline, sample acceptance, and bounded rerun remain independent per lane, so
one failed pair cannot erase a successful peer.

The former `app_gateway_survey_round.c/.h` path is deleted. The small
[`app_gateway_survey_terminal.c`](../firmware/app/src/app_gateway_survey_terminal.c)
adapter maps pure terminal reasons to host-visible command status, while source
boundary tests prove that the composed legacy fragments no longer regain
policy or a second queue owner.

## Power and memory consequences

The migration does not add another thread or workqueue. It reuses the existing
`mesh_route` stack, so the queue move changes attribution rather than adding a
stack. BLE backpressure remains on the system workqueue. The clicker and anchor
sleep/radio policy is unchanged.

The pure survey owner adds 64 bytes of gateway static RAM compared with the
retired round owner. In the exact compile checkpoint used for this version,
`mesh_gateway` linked at 124780 bytes RAM (95.20%) and 446612 bytes flash
(85.18%), a delta of 64 bytes RAM and 1512 bytes flash from the frozen source
baseline. `mesh_anchor` linked at 120608 bytes RAM (92.02%), and
`mesh_clicker` at 97360 bytes RAM (74.28%). Gateway headroom is therefore a
continuing design constraint; future ownership work should delete parallel
state before adding features.

These role builds are compile and link evidence, not deployment
qualification. The shared west workspace contained the recorded Zephyr
Kconfig patch, and Zephyr CMake still selected `ccache` despite
`CCACHE_DISABLE=1`. No board was flashed and no hardware RTT, BLE, RF, stack
watermark, or power measurement was performed for this migration.

## Verification and remaining architecture work

The current stage passed the pure-machine and adapter tests, the full native
suite apart from the deliberately stale pre-regeneration wiki gate, 118
mesh-integration tests, 114 hardware-model tests, and the same native suite
under ASan and UBSan. The exact `mesh_gateway`, `mesh_anchor`, and
`mesh_clicker` presets compile and link.

Those checks prove bounded software behavior and cross-role compilation. They
do not prove RF timing, stack headroom on target, multi-board survey behavior,
reset recovery for volatile operations, or low-power current. Deployment still
requires a fresh exact-artifact qualification capture and the repository-owned
verified flash wrapper.

The next architecture stages remain:

1. centralize application radio admission and safe-boundary handoff;
2. centralize exact delivery custody and terminal accounting;
3. convert the remaining textual fragments into separately compiled modules;
4. split oversized gateway BLE and clicker responsibilities after ownership is
   explicit.

This remains a staged ownership rewrite behind the existing product contract,
not a new protocol. A fixed-topology spanning-tree experiment is still a
simulator option and is not production behavior.
