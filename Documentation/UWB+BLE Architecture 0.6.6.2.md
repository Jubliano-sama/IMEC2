# UWB+BLE Architecture

Version: 0.6.6.2
Previous version: [UWB+BLE Architecture 0.6.6.1](<UWB+BLE Architecture 0.6.6.1.md>)
Status: Current production-candidate architecture

## Changelog

### 2026-07-26 - 0.6.6.2

- Replaced the inherited historical body with one current, source-backed
  description of the production-candidate roles, radio planes, BLE edge, click
  path, routing, custody, persistence, and anchor survey.
- Corrected the connected-routing timing table to the compiled defaults in
  [`mesh_radio_timing.h`](../firmware/include/mesh_radio_timing.h), including
  the 3000 us anchor scan slice.
- Described the current randomized multi-round survey discovery and
  synchronized concurrent-lane pair runtime instead of the retired
  one-pair-only flow.
- Separated firmware-owned radio safety facts from the RAM-only operation
  profile that a host may change within validated bounds.
- Recorded the current architecture debt and the staged ownership migration;
  this version does not change wire behavior.

## Authority and scope

The [Mesh Connected Routing Contract](<Mesh Connected Routing Contract.md>) is
the behavioral authority for channel ownership, priority, route formation,
delivery, ACKs, custody, gateway admission, and survey control. This document
is the compact system view. Packet fields belong in
[UWB+BLE Protocols and Strategies 0.3.12.4](<UWB+BLE Protocols and Strategies 0.3.12.4.md>),
the detailed execution narrative is in the
[Mesh Connected Routing Walkthrough](<Mesh Connected Routing Walkthrough.md>),
and build and deployment procedures belong in the
[Development and Deployment Guide](<Development and Deployment Guide.md>).

The current product line is the three exact presets selected in
[`firmware/app/CMakeLists.txt`](../firmware/app/CMakeLists.txt):

| Preset | Product responsibility |
| --- | --- |
| `mesh_clicker` | Battery clicker. It normally sleeps, wakes for a physical click, arbitrates with other clickers, discovers anchors, initiates DS-TWR, and gives the range event a stable identity. |
| `mesh_anchor` | Common production anchor. It derives a stable network identity from nRF FICR, persists gateway-assigned discovery state, ranges local clicks, relays accepted traffic, and selects local reports ahead of transit without deleting transit custody. |
| `mesh_gateway` | Mesh root and host edge. It listens for unscheduled gateway-bound channel-9 traffic, commits accepted records, returns gateway ACKs, and originates host-requested control on channel 5. |

`mesh_transmitter` and `mesh_transmitter_forcedhop` are traffic generators.
The ML, generic-role, high-debug, and forced-hop anchor presets are collection,
compatibility, or bench images; they are not alternate production
architectures. Exact role and deployment meaning is maintained in the
[Development and Deployment Guide](<Development and Deployment Guide.md>).

## System boundaries

Every product device has one DWM3000, so channel 5 and channel 9 are serialized
through one radio owner. The two UWB channels are separate protocol planes:

- **Channel 5 is the contact, control, ranging, and preemption plane.** Normal
  click wake, discovery, schedules, and DS-TWR use its standard wake/range PHY.
  Route and gateway-control follow-ups use the bounded mesh-control PHY.
  Gateway commands, assignment, Here-I-Am, and survey control also propagate
  on this plane.
- **Channel 9 is the connected payload plane.** Neighboring anchors negotiate
  recurring event timing for reports, transit payload, hop ACKs, and related
  delivery work. A connected anchor keeps recurring channel-5 receive windows
  between channel-9 turns. The gateway is a continuous, bounded-slice
  channel-9 receiver and does not own a normal scheduled gateway connection.

A valid click/ranging claim transfers the anchor directly into the click
sequence. Channel-9 work is clipped or deferred around that sequence and around
required channel-5 windows. Route-request traffic does not receive click
priority, while gateway-originated control runs at the first safe radio
boundary. These priorities are contract behavior, not caller-specific timing
patches.

BLE has two deliberately narrow uses:

- The gateway exposes a connected GATT host edge with packet notifications,
  command writes, and a readable gateway identity. Host-stream admission is
  part of gateway delivery, but BLE never carries an anchor-to-anchor mesh hop
  or replaces UWB ranging. The service and backpressure path are implemented
  in [`app_gateway_ble.c`](../firmware/app/src/app_gateway_ble.c) and
  [`app_gateway_ble_stream.c`](../firmware/app/src/app_gateway_ble_stream.c).
- Clickers use a bounded channel-37 BLE courtesy exchange while checking the
  UWB channel-5 gate. It may defer one clicker behind a higher-precedence peer,
  but it is advisory: UWB activity and the UWB click protocol remain the
  admission authority. The current gate is in
  [`app_clicker.c`](../firmware/app/src/app_clicker.c).

## Compiled connected-radio defaults

The following values are copied directly from
[`mesh_radio_timing.h`](../firmware/include/mesh_radio_timing.h). They are the
production-candidate compiled radio defaults, not measurements inferred from
logs and not host survey settings.

| Source constant | Value | Use |
| --- | ---: | --- |
| `MESH_RADIO_ANCHOR_SCAN_RX_US` | 3000 us | Low-duty anchor channel-5 receive slice |
| `MESH_RADIO_ANCHOR_SCAN_RESCHEDULE_MS` | 380 ms | Delay before the next low-duty scan attempt |
| `MESH_RADIO_ACTIVITY_COMPLETION_US` | 15000 us | Bounded activity-completion allowance |
| `MESH_RADIO_WAKE_TRAIN_MS` | 400 ms | Continuous wake-train duration |
| `MESH_RADIO_DISCOVERY_SLOT_US` | 12000 us | Normal click discovery-reply slot |
| `MESH_RADIO_EVENT_INTERVAL_MS` | 440 ms | Default channel-9 event interval |
| `MESH_RADIO_EVENT_WINDOW_MS` | 120 ms | Default channel-9 event window |
| `MESH_RADIO_EVENT_FIRST_DELAY_MS` | 500 ms | First proposed event delay |
| `MESH_RADIO_EVENT_ACCEPT_DELAY_MS` | 80 ms | Accepted-event start delay |
| `MESH_RADIO_EVENT_CONTROL_REFERENCE_MS` | 10 ms | Event-control reference offset |
| `MESH_RADIO_EVENT_GUARD_MS` | 30 ms | Event retune and timing guard |
| `MESH_RADIO_EVENT_TX_OFFSET_MS` | 15 ms | Default TX offset inside an event |
| `MESH_RADIO_EVENT_ACCEPT_REALIGN_SLOP_MS` | 20 ms | Accepted-event realignment tolerance |
| `MESH_RADIO_EVENT_MAX_MISSES` | 16 | Miss limit before timing failure |
| `MESH_RADIO_EVENT_SUPERVISION_MS` | 30000 ms | Event-timing supervision horizon |
| `MESH_RADIO_EVENT_RX_LATE_GUARD_MS` | 60 ms | Late receive guard |
| `MESH_RADIO_WAKE_SNIFF_US` | 20000 us | Wake/contact sniff slice |
| `MESH_RADIO_WAKE_POLITENESS_CHECK_US` | 20000 us | Wake politeness check slice |
| `MESH_RADIO_WAKE_OPPORTUNITIES` | 4 | Bounded wake/control opportunities |

The host runtime profile is a different layer. Its current safe defaults are
defined in
[`operation_policy.h`](../firmware/include/operation_policy.h): assignment uses
expected count `0`, a 235209 ms budget, and 1000 ms response spread; survey
discovery uses a 6000 ms start delay, 40 ms slots, six slots, four rounds,
250 ms report grace, and a 600000 ms operation budget; pair execution permits
two reruns and defaults to one concurrent lane.

The host may replace those operation values within the validated ranges. The
accepted profile is RAM-only and resets to compiled defaults after reboot.
It cannot override PHY configuration, measured frame airtime, retune and guard
minima, antenna delays, local pair receive bounds, delayed-transmit
quantization, route transport deadlines, or the compiled connected-radio
invariants above. Parsing and installation are implemented by
[`operation_policy.c`](../firmware/src/operation_policy.c) and
[`app_operation_policy.c`](../firmware/app/src/app_operation_policy.c).

## Normal click flow

The current click path is a bounded session, implemented by
[`app_clicker.c`](../firmware/app/src/app_clicker.c),
[`uwb_session.c`](../firmware/src/uwb_session.c), and the wire limits in
[`uwb.h`](../firmware/include/uwb.h):

1. A physical click wakes the clicker. BLE courtesy and decoded channel-5
   politeness may defer it, but the check is bounded.
2. The clicker sends the 400 ms channel-5 wake train. The anchor's 3000 us
   low-duty scan slice is rescheduled after 380 ms, and the build guards the
   wake-overlap relationship. Once an anchor accepts the claim, it remains on
   channel 5 through discovery, schedule reception, and DS-TWR.
3. Anchors answer discovery in complete-airtime 12000 us slots. A normal click
   requires at least three discovered anchors; one or two are explicitly
   released before retry. The schedule can carry at most eight anchors.
4. The clicker sends one range schedule and runs addressed DS-TWR inside the
   400 ms burst. The current normal configuration requests two samples per
   anchor and uses a minimum 33000 us exchange stride.
5. The click succeeds after three unique anchors produce `RANGE_OK`. Otherwise
   the same click identity can retry for at most six attempts inside the
   15000 ms click-report deadline. Failures are explicit rather than silently
   accepting fewer anchors.
6. Anchors queue the resulting click records as local-origin work. Local click
   delivery runs ahead of transit at the next safe mesh opportunity, while
   already accepted transit remains owned and retryable.

## Connected routing, custody, and persistence

Route knowledge and channel-9 timing are separate. An anchor can remember a
next hop while its recurring event timing is absent or stale. Normal route
acquisition first tries the unscheduled direct-gateway channel-9 contact, then
uses channel-5 request/reply discovery when a relay path is needed. Anchors
retain one upstream and one downstream channel-9 relationship; the gateway
does not consume one of those scheduled relationships.

New and migrated protocol code submits immutable datagrams through the
[`node_comm` interface](../firmware/include/node_comm.h). Its application
adapter is [`app_node_comm.c`](../firmware/app/src/app_node_comm.c). One
delivery owner controls the packet identity, absolute deadline, actual RF
attempt count, route selection, randomized retry, pause state, persistence,
hop ACK, gateway ACK, and terminal result. A pre-RF refusal consumes no
transmission opportunity. Every accepted request terminates once as delivered,
expired, exhausted, permanently failed, or cancelled.

Hop ACK transfers custody to the next relay; it is progress, not end-to-end
success. The producer releases an important gateway-bound packet only when the
gateway ACK names that exact identity. At the gateway, semantic validation and
host-stream reservation/commit happen before the ACK that releases upstream
custody. Queue pressure therefore produces retained retryable work or an
explicit rejection, not a false success.

State that must survive reset is journaled rather than reconstructed from a
success log. Current persistence includes stable anchor assignment state,
deferred accepted mesh custody, durable survey discovery reports, gateway
collection state, and host-visible click admission. The application persistence
boundary is in
[`app_mesh_persistence.c`](../firmware/app/src/app_mesh_persistence.c);
survey report ownership is in
[`app_anchor_survey_discovery.c`](../firmware/app/src/app_anchor_survey_discovery.c);
gateway collection journaling is in
[`gateway_collection_journal.c`](../firmware/src/gateway_collection_journal.c).
Transient storage failure blocks or retries the owner; it does not authorize
dropping the only accepted copy.

## Anchor self-setup survey

Survey measures anchor-to-anchor distances for an off-device geometry solver.
Firmware discovers, schedules, ranges, transports, and reports; it does not
solve the final office layout.

The current discovery is randomized and multi-round. A host-preflighted survey
command carries the runtime profile, the gateway floods a future
`SURVEY_DISCOVERY_START`, and each participating anchor performs the configured
announce/listen rounds. In every round it chooses a randomized slot, transmits
when it receives a real radio opportunity, and listens for the rest of the
round. A one-way peer observation is useful. Each anchor durably owns its
reachability report until the gateway ACKs that exact packet.

The gateway builds reachable pairs from the reports and plans conflict-free
ranging rounds. Pairs share a round only when their endpoint neighborhoods and
known reverse paths are disjoint; unknown or conflicting relationships
serialize. The planner is in
[`survey_pair_planner.c`](../firmware/src/survey_pair_planner.c).

Pair control is serialized so each PREPARE and START response receives its ACK
settle window, but ranging is lane-based rather than one-pair-only. After every
endpoint in the live batch is armed, the gateway sends one future,
age-compensated GO. All lanes in that batch then range against the same round
generation, while result, cleanup, deadline, and rerun custody remain
independent per lane. The runtime can retain up to 25 lanes; the compiled safe
profile starts with one and the host can select a larger validated cap. The
pure lane state is in
[`survey_pair_round_runtime.c`](../firmware/src/survey_pair_round_runtime.c),
and the gateway wrapper is
[`app_gateway_survey_round.c`](../firmware/app/src/app_gateway_survey_round.c).

A result is usable geometry only when it reports `RANGE_OK` with a positive
distance. Missing or unusable samples cause complete pair cleanup and a bounded
rerun, up to the profile's two-rerun limit. A stale round generation cannot
complete a later rerun. Failure in one lane cannot cancel successful lanes in
the same round, and abort remains an idempotent bounded cleanup operation.

## Current architecture debt and reset

The behavior above is current, but its Zephyr orchestration is still spread
across very large composed translation units. Textual `.inc` splits improved
navigation without creating real ownership boundaries, so globals, work items,
deadlines, retry state, radio handoffs, and custody can still cross modules in
ways that are difficult to review.

The accepted [Architecture Reset Plan](<Architecture Reset Plan.md>) therefore
does not replace the working protocol in one rewrite. It freezes growth with
[`architecture_boundaries.json`](../firmware/architecture_boundaries.json) and
migrates one state owner at a time:

1. Extract a pure gateway survey state machine with typed events and actions.
2. Centralize application radio admission and safe-boundary handoff.
3. Centralize exact delivery custody and terminal accounting.
4. Convert textual fragments into separately compiled modules, deleting the
   old owner only after equivalence, liveness, exact-role, and required hardware
   evidence pass.

Until those stages complete, the contract and sources linked above remain the
current behavior. The reset is an ownership and maintainability migration; a
new fixed-topology protocol remains a separate simulator experiment and is not
production behavior.
