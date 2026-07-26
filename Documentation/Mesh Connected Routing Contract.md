# Mesh Connected Routing Contract

This is the normative runtime specification for the production-candidate
connected mesh. `MUST`, `MUST NOT`, `SHOULD`, `SHOULD NOT`, and `MAY` are
normative. Each requirement has an immutable identifier; changing an
identifier's meaning requires the governance process below, and a retired
identifier is never reused.

This contract deliberately does not repeat every adjacent detail:

- Follow the [Mesh Connected Routing Walkthrough](<Mesh Connected Routing Walkthrough.md>)
  for the explanatory end-to-end flow.
- Follow the current
  [UWB+BLE Protocols and Strategies](<UWB+BLE Protocols and Strategies 0.3.12.4.md>)
  and `firmware/include/protocol.h` for wire encoding, identifiers, and TLVs.
- Follow [Gateway BLE Streaming](<Gateway BLE Streaming.md>) for selective
  stream framing and host transport details.
- Follow the [Architecture Reset Plan](<Architecture Reset Plan.md>) for the
  accepted staged ownership migration.
- Follow the [Development and Deployment Guide](<Development and Deployment Guide.md>)
  for builds, evidence, transactional deployment, and hardware qualification.
- Follow `mesh_contract_traceability.yaml` for legacy-line provenance,
  implementation ownership, and the explicitly bounded verification status of
  every requirement.

If the wire header, current protocol document, and this contract disagree, the
header controls encoding and this contract controls runtime behavior.

## Governance

### GOV-01 — Contract authority

No implementation change MAY knowingly contradict this contract. Work MUST
stop for explicit user permission when a requested behavior would contradict,
remove, or weaken a requirement.

### GOV-02 — Controlled contract change

Before an intentional behavior change, the proposal MUST list the new behavior,
affected roles, every changed or removed requirement, compatibility impact,
and the tests and hardware checks that will prove it. The traceability manifest
MUST be updated in the same change, but a mapping by itself MUST NOT be
represented as verification.

## Host-owned operation policy

### HOST-01 — Policy boundary

Experiment sequencing and tunable operation policy belong to the GUI or another
host controller. Firmware MUST provide bounded radio and protocol mechanisms;
it MUST NOT choose a hidden prerequisite sequence or require reflashing to
change ordinary operation timing.

### HOST-02 — Ordinary host sequence

Every ordinary GUI-triggered gateway operation MUST be one host-owned sequence:

1. Freeze the target command and its versioned runtime profile.
2. Send a separately correlated Here-I-Am request.
3. Wait for that request's typed successful terminal. Intermediate, stale,
   duplicate, uncorrelated, or BLE-write-complete indications MUST NOT release
   the wait.
4. Send the frozen target only after that terminal. Refresh failure, timeout,
   or disconnect MUST discard the target without sending it.
5. Keep both commands busy as one user operation so another command cannot
   interleave between them.

### HOST-03 — Direct-operation exceptions

Manual Here-I-Am, disconnect, emergency abort or stop, and liveness heartbeats
MUST remain direct operations because preflighting them would recurse, delay
recovery, or deadlock active work. Firmware-generated phases within one survey
belong to that already-preflighted survey and MUST NOT receive independent
preflights.

### HOST-04 — Runtime profile and safety boundary

Each host command MUST carry one compact, versioned runtime profile. The same
accepted profile MUST reach participating anchors, rather than being rebuilt
from separate gateway and anchor constants. The profile MAY set expected
assignment count, assignment budget and response spread; survey discovery
start, slot, round, grace, and budget values; and pair rerun and parallel-lane
caps.

Firmware MUST reject unknown or malformed versions, arithmetic overflow,
out-of-range values, and any combination that cannot fit a complete frame,
retune, and guard. PHY configuration, measured airtime, minimum retune and
guard, local pair receive bounds, route-transport deadlines, antenna delays,
delayed-transmit quantization, and memory capacity remain firmware-owned safety
facts.

### HOST-05 — Profile lifetime, budgets, and useful outcomes

The last accepted profile MUST remain active in RAM for later anchor-originated
delivery. It MUST NOT become persistent release policy: reset restores compiled
safe defaults until a later manual or host-preamble Here-I-Am carries a profile.
The target command MUST repeat the profile so a node that missed the preflight
can configure the operation it receives.

Firmware MUST validate correlated terminal identity, packet structure, bounded
arithmetic, and useful committed results. Exact roster counts, complete
telemetry, zero event loss, and all-node success MAY be stricter host
qualification checks; they MUST NOT turn useful partial assignment or survey
results into total firmware failure. A host budget is an absolute ceiling, not
a sleep; firmware MUST advance on real terminal conditions and reject only a
budget that cannot cover the selected profile's arithmetic worst case. The GUI
SHOULD display its computed bound before sending.

## Role contracts

### ROLE-01 — Clicker

The clicker is the battery-powered initiator of click and ranging events. It
MUST wake nearby anchors on Channel 5, complete the ranging sequence, and submit
the result through connected routing. Its interactive work has priority over
relay traffic; interrupted relay work remains owned and resumes at a safe
boundary.

### ROLE-02 — Anchor

An anchor is a fixed ranging responder and relay. Outside a connection it SHOULD
use low-duty Channel 5 receive windows. In a connection it MUST participate in
the Channel 9 rhythm and retain recurring Channel 5 receive opportunities.

An anchor has at most one upstream and one downstream Channel 9 reservation.
It MUST NOT advertise or accept an unrelated extra rhythm when those slots are
committed. Locally produced click, ranging, and command-result work MUST be
selected before transit payload, but accepted transit custody MUST remain
explicit and retryable. Route depth MUST NOT change this local-first priority.

### ROLE-03 — Gateway and bench traffic sources

The gateway is the mesh sink and host bridge. It MUST remain primarily a
continuous Channel 9 receiver, MUST NOT own a normal upstream/downstream
connection, and MUST originate broad commands and reachability on Channel 5.
Normal route requests are not a gateway Channel 5 receive path; nodes use the
short direct Channel 9 probe before reactive discovery.

`mesh_transmitter*` is a bench traffic source, not a ranging anchor. It MUST NOT
answer click discovery or DS-TWR, and a click-class claim MUST NOT preempt or
reclassify its active route. Direct-or-relayed mode MAY accept a direct gateway
route. Forced-relay mode MUST include at least one anchor hop even when the
gateway is physically reachable. A bench-only forced-hop anchor MUST ignore a
route-class claim sent directly by the gateway when relayed request or control
is required, remain available for the relay's following wake and flood, and
retain normal behavior for relayed route claims and all click claims. Production
anchor presets MUST NOT enable this filter.

A bench transmitter SHOULD otherwise follow production connected routing:
acquire only when no usable path exists, carry delivery on Channel 9 while
connected, retry ACK loss on that connection, and avoid a new wake train until
the connection becomes terminal.

## Radio ownership and scheduling

### RAD-01 — Channel responsibilities and priority

Channel 5 is the control and click-preemption lane; Channel 9 is the connected
payload and ACK lane. Click/ranging-class Channel 5 traffic MUST preempt Channel
9 relay work. A route request is control traffic, not click traffic, and MUST
NOT steal an imminent connected Channel 9 event.

Blind Channel 5 flooding is reserved for broad discovery, gateway-originated
commands, and Here-I-Am reachability. Ordinary route acquisition MUST use the
typed reactive request/reply exchange.

### RAD-02 — Connected rhythm and full receive windows

A connected anchor MUST alternate bounded Channel 9 work with recurring
Channel 5 receive windows. Channel 9 work MUST be clipped, deferred, or retried
when it would starve a required Channel 5 window.

Each allocated connected Channel 5 window is continuous receive duty for its
complete duration. The receiver MUST rearm immediately after unrelated,
malformed, or route-class activity. Only a valid click/ranging claim MAY end
the window early by transferring ownership to the click sequence.

### RAD-03 — Click handoff and PHY separation

Every anchor Channel 5 owner, including the connected-gap receiver and
low-duty scanner, MUST hand a valid click/ranging claim directly to the same
click sequence. It MUST NOT reclassify the claim as route contact, wait for a
later wake packet, or defer the handoff.

Click wake and discovery MUST use the clicker's standard wake PHY.
Extended-PHR control PHY is reserved for route and mesh-control follow-up.
Normal click discovery MUST keep its normal bounded window and MUST NOT inherit
the longer route listener merely because the anchor is a mesh build.
Discovery slots MUST contain the complete standard-PHY reply airtime plus
transition and clock-jitter guard, with identical spacing for assigned and
deterministic unprovisioned slots. The configured first wake train MUST exceed
the worst-case scanner RX-off gap including startup and retune; an invalid build
MUST fail its guard.

### RAD-04 — Click politeness and continuous click ownership

Before a click wake train, the clicker MUST observe the ledger's continuous
quiet interval. A valid frame or partial PHY progress resets that streak;
undecodable activity is not quiet. Politeness remains bounded, and exhaustion
MUST be reported before click priority proceeds.

After an anchor accepts a click/ranging claim, it MUST remain on Channel 5 at
continuous receive duty through the remaining wake train, discovery, reply,
schedule, inter-sample gaps, and all DS-TWR exchanges. Only required Channel 5
transmissions and immediate PHY transitions MAY interrupt RX. Mesh handback
occurs only after explicit completion or failure.

### RAD-05 — Control-follow-up ownership

A broad control wake MUST identify its follow-up. An unprovisioned anchor that
accepts it MUST enter a bounded extended-PHR window long enough for the maximum
advertised wake train, the sender's transition, and the first flood frame. It
MUST accept the following broadcast without requiring an assigned slot or
misclassifying it as route solicitation.

Non-route-solicitation wakes MUST carry the control-follow-up flag; route
solicitations MUST omit it, and event proposals MUST include it. The sender
MUST leave the configured standard-to-extended-PHR turnaround. After ownership
transfers, repeated standard-PHY claims in the same train MUST NOT pull the
receiver away from the announced extended-PHR frame. Ordinary route listeners
retain click preemption.

### RAD-06 — Direct-gateway control sampling

Before every direct Channel 9 gateway contact, an anchor MUST sample Channel 5.
Detected activity makes the attempt yield without leaving Channel 5. When a
gateway wake begins during an ACK wait, the next retry boundary MUST sample
soon enough to overlap that same continuous wake train; a missing gateway ACK
must not make control undiscoverable.

### RAD-07 — Randomized RF re-entry

Concurrent responders, probes, and rebroadcasts MUST use enough jitter for
their expected fanout, justified by complete airtime, retune, and guard.
Every failed or deferred RF operation MUST re-enter through randomized
exponential backoff. A pre-RF deferral advances its backoff round but consumes
no RF opportunity; only an actual RF start consumes one. Fixed delays MAY be
used only for non-transmitting service polls and protocol-defined spacing after
a successful transmission.

A deferred gateway-control flood MUST retain an independent delayed-work owner
while paused. Resume notification MAY accelerate it but MUST NOT be its sole
liveness edge.

### RAD-08 — Wake-train and bounded-flood politeness

Every wake train MUST run the ledger's pre- and post-train Channel 5 activity
probes. Valid frames and SFD/frame/CRC/bad-frame progress count as activity even
when decoding fails. Activity MUST defer the whole train through the ledger's
randomized exponential range.

A bounded Channel 5 flood MUST retain exactly the ledger's number of real RF
starts. Each opportunity has its own quiet probe; pre-RF activity defers without
consuming it. Successful copies use the ledger spacing. Gateway-control
priority MUST NOT bypass the quiet check. Failed RF opportunities MUST retain
identity-mixed collision-diversifying jitter rather than collapse to one fixed
retry delay.

### RAD-09 — Gateway continuous receiver

The gateway's continuous Channel 9 receiver is a logical horizon, not one
blocking workqueue lease. Driver calls MUST be bounded slices, rearmed after a
short cooperative delay, and recover from a bounded run of immediate errors
without consuming the remaining logical horizon.

When pending control becomes ready between slices, a receive-start refusal is a
safe boundary: schedule the control and let its completion rearm RX. The control
owner MUST retain an independent liveness retry because a normal RX completion
can race the abort request. This design MUST preserve survey, BLE, watchdog,
and delayed-work progress without a material Channel 9 gap.

## Power-state contract

### PWR-01 — Idle anchor

An anchor with no Channel 9 connection SHOULD use low-duty Channel 5 receive and
MAY use DWM3000 sleep between windows, provided wake and retune remain reliable.

### PWR-02 — Connected anchor

A connected anchor SHOULD NOT enter retained or deep DWM3000 sleep between
back-to-back Channel 9 and Channel 5 windows. It MAY keep the radio idle and
ready to retune; the connected rhythm SHOULD have no meaningful sleep gap.

### PWR-03 — Click interruption

A click MUST mark relay work interrupted rather than disconnected. Timers MUST
include the worst-case click duration, and the anchor SHOULD resume the existing
Channel 9 rhythm when it remains valid.

### PWR-04 — Scheduled-owner protection

Low-duty and connected Channel 5 scans MAY use only the bounded gap before the
next required Channel 9 prepare boundary. Before yielding, the scanner MUST
rearm the Channel 9 owner at that exact boundary. A pending reliable local
delivery reserves its next selected-peer transmit event even before RF or ACK
custody begins; unrelated peer schedules remain available.

## Route formation and route state

### ROUTE-01 — Discovery entry and direct probe

Gateway-bound custody with no usable route MUST first send a short direct
Channel 9 probe and listen for its ACK before every original request,
rebroadcast, and retry. Direct-or-relayed mode MAY install a successful direct
route. Forced-relay mode and direct-bulk-failure hold-down MUST record contact
but continue discovery. The gateway MUST NOT answer ordinary discovery through
a Channel 5 route reply, and a direct gateway candidate MUST NOT enter event
negotiation.

If the probe is insufficient, the origin MUST send a route-typed wake and one
reactive request control packet, never the blind command-flood path.
Each valid probe is idempotent contact control: the gateway MUST rebuild its ACK
without consuming the durable accepted-packet identity store, so an unheard
reply cannot exhaust custody capacity for other origins.

### ROUTE-02 — Request forwarding

An accepted idle relay MUST record the reverse route. If it lacks a usable
target route and TTL permits, it MAY forward after airtime-sized randomized
jitter. During the delay it MUST keep listening for better candidates, then
repeat the direct probe before deciding whether to reply or rebroadcast.
After rebroadcast it MUST open a subsequent Channel 5 reply window. Click
traffic MAY abandon or defer this propagation and take ownership.

### ROUTE-03 — Reply versus rebroadcast and connected behavior

An idle anchor with capacity and a usable target route MUST answer and MUST NOT
rebroadcast the same request. It MAY rebroadcast only when it has no usable
route and TTL permits. A connected anchor MUST neither answer nor rebroadcast
an unrelated request because it has no additional Channel 9 rhythm; it MAY
service replies and gateway control belonging to its existing route.

### ROUTE-04 — Loop-free answer selection

The candidate used to answer a gateway route request MUST come from the
upstream-candidate snapshot taken before installing the requester's reverse
route. Its next hop MUST differ from the request's physical previous hop, and
its complete ancestry MUST be disjoint from the request ancestry. Split horizon
rejects the two-node loop; disjoint ancestry rejects longer cycles.

### ROUTE-05 — Reply windows, foreign requests, and prompt release

Each reply MUST use randomized responder timing inside the immediate upstream
listener's protected window, after the advertised remaining delay, with enough
space for a complete frame and guard. The complete reverse chain, per-hop reply
ACKs, retunes, and guards MUST fit the origin's TTL-scaled reply budget.
Delayed responders MUST drop rather than transmit outside that window, and
repeated request copies MUST age the remaining delay.

The origin budget MUST also include the initial wake and request frame,
worst-case forwarding delay at the selected TTL, and deepest responder jitter.
Stable discovery inputs MAY bias forwarding and reply timing but MUST NOT be
the sole selector, because repeated deterministic collisions must not persist.

A reply listener that receives another origin's valid request for its local
node or the gateway MUST queue that request and release Channel 5 immediately.
It MUST NOT mistake the foreign request for its awaited reply or keep the radio
until its original deadline.

### ROUTE-06 — Exact bounded ancestry

Every route request MUST carry the origin and requested target, route epoch,
discovery identity, slot seed, hop count, path quality, remaining reply-window
delay, and behavior flags. Proposed Channel 9 timing is optional. Requests,
replies, and gateway advertisements MUST also carry an exact directionally
ordered node path. Receivers MUST reject a missing or malformed mandatory
field, wrong path root or tail, duplicate ID, node-count/hop-count mismatch, or
local-ID cycle. A forwarder MUST append its ID before incrementing hop count and
MUST fail explicitly when the ledger's node bound is exhausted.

Anchors MUST retain exact ancestry for each upstream candidate in role-specific
storage. Current requests MUST use the standalone post-wake control frame
because mandatory ancestry exceeds the wake suffix. An invalid, malformed, or
unqueued suffix MUST leave that fallback listener active.

### ROUTE-07 — TTL and route-control wire bounds

Request attempts MUST use the TTL ladder in the ledger and MUST NOT exceed its
normal maximum without explicit permission and a contract update. Route-control
TTL MUST be validated before route, ancestry, timing, duplicate, or reply state
changes. Requests and advertisements MUST reconstruct their defined origin TTL;
replies and one-hop reply ACKs MUST remain within the ledger. Zero, impossible,
over-depth, and message-cap-plus-one input MUST fail before mutation.
A TTL-one request MAY still be answered by a one-hop anchor with a usable route;
TTL limits further request propagation, and it does not turn the request into a
direct gateway Channel 5 exchange.

Route formation retries MUST use the ledger's doubling base and fresh
percentage jitter. This backoff applies to route formation, not every ACK miss
inside a valid connection.

### ROUTE-08 — Route reply completion and timing

Replies MUST return over the recorded reverse path and be ACKed at each Channel
5 hop; the origin MUST ACK the final reply. A reply starts with the network-wide
hop limit so it remains nonzero at the deepest supported origin.

When a reply accepts optional Channel 9 timing, both peers MUST install that
exact rhythm and use it for waiting custody. A second proposal MAY occur only
when accepted timing is absent or unusable. A reply processed by another RX
owner MUST publish route readiness so the active listener can release in a
bounded slice before the accepted rhythm becomes stale.

### ROUTE-09 — Route evidence and bounded reverse state

Only the first accepted copy of one route epoch, message type, session, and
sequence MAY install or replace a reverse gateway hint. Physical previous hop
is intentionally excluded from that transport identity. Later echoes MAY be
forwarded and delivered to the protocol owner but MUST NOT create another
candidate or erase a newer direct route. A new identity MAY provide new
topology evidence.

Logical reverse routes MUST remain distinct even when several descendants share
one physical next hop and one downstream rhythm. Route-epoch invalidation and
explicit clearing MUST cover inline and overflow storage. Candidate age and an
expired capacity hint alone MUST NOT invalidate physical route evidence. A full
candidate table MAY replace an entry only with better current-epoch evidence;
an older epoch is stale.

## Channel 9 event ownership

### EVT-01 — PROPOSE and ACCEPT

An ACCEPT is valid only for the active proposal to the immediate peer, with
matching physical hop, endpoints, and complete timing. New peers SHOULD echo
the proposal session and sequence; a fresh nonzero compatibility identity MAY
be accepted only under the same active-peer and exact-timing checks.

The successful PROPOSE RF start fixes the phase. Both peers MUST retain that
phase in their clock domains; the responder installs it after ACCEPT starts RF
and the proposer after the matching ACCEPT arrives. Queue latency, retries,
duplicates, and cached replay MUST NOT shift it.

### EVT-02 — Connection identity and sequence domains

The PROPOSE session owns the connection. UPDATE and END MUST reuse it. Each
endpoint MUST retain the immediate peer and independent local and remote
sequence histories. Exact duplicates are inert; same-sequence type or payload
changes conflict; older sequences are stale. A malformed UPDATE MUST NOT
consume sequence, and a valid phase UPDATE installs the complementary peer
direction.

Before accepting replacement ownership, the responder MUST check the live owner
and at least the ledger's retired-session history. A retained session is stale;
only a genuinely newer proposal may replace a live owner. New local operations
MUST use fresh boot-seeded identities so delayed operation N cannot mutate N+1.

### EVT-03 — Boot incarnation

Every PROPOSE MUST carry the ledger's nonzero boot incarnation, chosen once per
sender boot and never reused within it. Remote sequence ordering is scoped by
that incarnation, so a fresh non-retired incarnation MAY restart at sequence
one while the active incarnation must advance.

Retired incarnations MUST remain paired with retired sessions. Missing, zero,
malformed, or retired incarnation evidence MUST fail before event-owner or
timing mutation. UPDATE and END retain their independent sequence domains.

## Communication service and custody

### COMM-01 — Single service boundary

Protocol machines MUST submit immutable payload, packet identity, named
delivery profile, destination, and 64-bit absolute deadline through one
node-communication boundary. Callers MUST NOT tune route acquisition, ACK
timing, retries, backoff, or custody independently.

### COMM-02 — One custody owner

One logical packet MUST have exactly one custody owner. The protocol owns its
transaction and delivery handle; the communication service alone owns the
frozen packet, persistence, route selection, RF attempts, ACK accounting,
retries, and terminal result. Ownership transitions among accepted, RF-started,
hop-ACKed, gateway-confirmed, and caller-terminal MUST be explicit and atomic.

### COMM-03 — Profiles, priority, and terminal consumption

A caller MAY select only a named profile matching its semantics. It MUST NOT
invent a private profile for one timing case; changing a profile requires
collision, busy-radio, route-loss, retry, and deadline tests for all users.

Protocol control and progress traffic MUST have reserved admission and priority
over ordinary reliable traffic, transit, diagnostics, and maintenance. Every
protocol MUST consume a terminal event or explicitly abandon its handle.
Supersession MUST atomically cancel and reap the old handle. An ACK already
owed for an accepted response and a later gateway control share top priority;
FIFO ordering MUST send the ACK first.

### COMM-04 — Reliable-resource fairness

A reliable backend MAY have only one datagram awaiting final acknowledgement.
Additional reliable work MUST wait without consuming an RF attempt or
advancing backoff. Independent eligible control MAY preempt that wait. Terminal
release MUST wake all waiters, reapply profile priority, and preserve FIFO
within a profile; newer ordinary traffic MUST NOT pass an older waiter before
that waiter starts RF or reaches its deadline.

### COMM-05 — Bounded callback and queue ownership

Callbacks MUST freeze and enqueue, then return to their owning worker. They MUST
NOT run a complete route search, reliable delivery, click sequence, or
collection loop on the callback stack. The communication queue owns delivery
and route wait; the anchor UWB queue owns accepted clicks. Route wait MUST have
its own work item and deadline and MUST NOT move, reuse, or cancel an active
relay ACK timeout.

### COMM-06 — Immutable request identity

A logical request MUST retain one packet identity and exact payload across all
attempts. A protocol MUST NOT allocate a new sequence because RF, ACK, or a
short result wait ended. A matching application result commits once within its
absolute deadline; exact duplicates are harmless, conflicting bytes or flags
fail closed, and late results MUST NOT advance a later transaction.

### COMM-07 — Cleanup and recovery commands

Any operation that may commit remote state MUST own cleanup after failure,
deadline, or cancellation. Every accepted survey PREPARE MUST end in matching
START or idempotent ABORT. Its prepared lease MUST remain through local
START_PENDING and clear only when DS-TWR begins.

Gateway-local SURVEY_ABORT MUST bypass the ordinary safe-boundary command queue,
cancel queued survey starts, terminate the active operation, leave bounded
remote cleanup with the survey owner, emit one successful terminal, and never
enter mesh routing. An operation MUST NOT block its own recovery command.

### COMM-08 — RF accounting and terminality

Only the communication service decides whether RF started. Pre-RF deferral MUST
consume no opportunity; collision or timeout after RF start MUST consume one.
Every accepted datagram MUST reach exactly one terminal: delivered,
deadline-expired, attempts-exhausted, permanent failure, or explicit
cancellation. Retries MUST preserve bytes and identity and use immutable
identity-derived plus fresh collision-diversifying jitter.

### COMM-09 — Frozen capacity

The service MUST enforce the ledger's ordinary and priority frozen-record
capacities. A full queue or oversized payload MUST fail explicitly and MUST NOT
fall through to direct delivery or another private retry queue. A second
maximum-size control MUST wait or be rejected without overwriting the first or
enlarging every ordinary record.

### COMM-10 — Pause, resume, and compatibility boundary

Pause, resume, quiesce, and stop MUST NOT transfer protocol ownership. Pause has
one generation-checked owner and bounded lease. Absolute deadlines continue;
relative retry delays resume from their remainder. If the radio does not
quiesce after the lease, bounded recovery MUST abort it and deliberately allow
the watchdog to reset rather than remain paused forever.

The current mesh-report runtime is a compatibility backend. New or migrated
protocols MUST enter through the facade and MUST NOT create another route,
queue, flood, retry, radio-owner, or terminal path.

## Architecture ownership

### ARCH-01 — Staged single-owner migration

The accepted [Architecture Reset Plan](<Architecture Reset Plan.md>) governs
orchestration migration. Every new or migrated long-running path MUST have one
serialized owner, immutable identity and generation, a 64-bit absolute
deadline, actual RF-attempt accounting, an independent liveness edge, and one
terminal result. A migration MUST delegate or delete the replaced owner in the
same stage and MUST NOT change wire format, timing, power policy, or runtime
semantics as an unreviewed side effect. Frozen legacy paths remain explicit
architecture debt until migrated; this target model MUST NOT be described as
already universal.

## Delivery and ACKs

### ACK-01 — Gateway semantic admission before ACK

For click reports, command results, result bundles, survey discovery reports,
and pair results, the gateway MUST validate the complete message, reserve the
complete host record, commit required durable protocol state, and commit the
new host-stream record before its semantic completion response or gateway ACK.
Before that boundary it MUST NOT cache the packet as accepted. Malformed or
stale input, full host capacity, and persistence failure remain retryable
upstream. After commit, exact duplicates are ACK-sticky and MUST NOT repeat
state mutation or uninterrupted-stream delivery. Collection results remain in
collection-EACK custody; a generic gateway ACK MUST NOT complete them.
The gateway MUST retain each accepted ACK identity for at least the sender's
maximum custody lifetime; a shorter route-dedup window MUST NOT expire it while
the sender may still retry.

### ACK-02 — Durable click and host-stream semantics

Gateway click custody MUST be journaled through BLE stream commit. Journal
clear and notification are separate durable steps, so reset may replay an exact
accepted record; BLE delivery is at-least-once across gateway reset. The GUI
MUST retain a bounded session cache keyed by complete identity and exact payload
across reconnects: an exact replay merges, while same-identity mutation remains
visible. A host-process restart or cache eviction remains an explicit
exactly-once gap.

The BLE boundary MUST admit a complete protocol-valid maximum-size click
payload. Click records MAY displace only lower-priority diagnostics or status,
never ACK-gated committed records. Click diagnostics MAY be independently
fragmented. Click, Channel 9 delivery, and gateway-ACK latency TLVs MUST appear
only when measured; unknown latency is an absent TLV, never zero.

Gateway journal identity MUST include message type, semantic flags, endpoints,
session, sequence, payload length, and durable payload bytes. Relay-local TTL
and message age MAY change and MUST be excluded from duplicate matching.

### ACK-03 — Hop custody and ACK-sticky duplicates

Hop ACK and gateway ACK are distinct: hop ACK transfers custody to the next
anchor; gateway ACK proves final acceptance. Anchor-to-anchor slots MAY carry
multiple packets and one hop ACK MAY list all accepted packets. Omitted packets
remain pending. Hop ACKs and returning gateway ACKs MUST be scheduled in the
sender's next eligible Channel 9 transmit window and bubble one hop at a time.
A higher-priority gateway ACK or required safety or terminal response MAY take
that safe boundary, but the displaced ACK retains custody and MUST use the next
eligible window; lower-priority traffic MUST NOT displace it.

An exact accepted duplicate MUST suppress another payload mutation or forward
but MUST be listed in the applicable ACK. A BUSY-rejected identity MAY repeat
BUSY only after its advertised minimum retry interval; earlier copies MUST NOT
create a train or refresh deferral indefinitely.

### ACK-04 — Direct-gateway batches

A direct sender MUST calculate how many complete frames fit while reserving
retune, guard, and the gateway reply window. All packets in the turn MUST carry
one batch identity and a final marker. The gateway MUST answer after the final
marker with one batch ACK listing every accepted identity. Listed packets are
gateway-accepted; omitted or lost packets retain custody for later Channel 9
retry.

For one accepted ACK-requesting packet, the gateway MUST first attempt the ACK
in the same radio turn after the fixed guard. Only failure to start RF MAY hand
that exact ACK to reliable response custody.

### ACK-05 — Returning gateway ACK custody

A relay returning a gateway ACK MUST retain the original transit packet until
the exact child-directed ACK transmission completes successfully. Queue refusal,
conflicting same-peer ACK state, RF-start failure, completion failure, or reset
MUST NOT complete transit custody. The ACK table MAY recognize an exact
duplicate but MUST NOT replace an older unsent ACK or extend its deadline. On
bounded handoff expiry, the immutable payload MUST be requeued so the gateway
can reproduce the ACK; this is not an upstream parent failure.

Gateway ACK MUST precede hop ACK in one transmit opportunity. Failure of an
upstream parent MUST clear routes and timing through that peer but preserve a
current-epoch reverse downlink through a different child for an ACK already in
flight. Hop ACK SHOULD extend or reset the gateway-ACK deadline.

### ACK-06 — Connected retry and event-bound scheduling

A missed hop, gateway, or batch ACK MUST retry the same pending packets on the
live Channel 9 connection and MUST NOT start a wake train or route negotiation.
ACK timing MUST tolerate the worst-case click interruption. Route acquisition
restarts only after the connection or selected route becomes terminal.

The service MUST preserve the scheduler's exact next prepare boundary. Readiness
before the event MUST rearm the same delivery at that boundary without
consuming an attempt; generic randomized polls MUST NOT replace deterministic
slot waits. Channel 5 reply listeners MUST enqueue accepted event control and
release the radio before the communication worker processes it; synchronous
queue draining and recursive delivery are forbidden.

### ACK-07 — Collection EACK custody

Collection EACK has a nonzero 16-bit sequence present in both header and
mandatory payload; wrap is 65535 to 1, while the 8-bit retry round is only a
backoff hint. The gateway MUST durably freeze exact header and payload before RF.
Channel 9, Channel 5 recovery, and reset MUST reuse those bytes; concurrent new
results belong to a later update.

Collection start MUST durably freeze its exact expected roster, bounded by the
ledger. A result outside that roster MUST be rejected, and at most one accepted
payload identity per expected node may create a host record. An exact retry MAY
rearm a missed EACK but MUST NOT create a second record or replace accepted
content. A closed collection with unfinished EACK custody MUST block a new
collection.

Channel 5 recovery completes only after the ledger's four actual EACK starts.
Pre-RF deferral retains the current opportunity; there is one resumable
four-frame flood and no outer multiplication. After delivery, the gateway MUST
durably commit the next collection identity and state before clearing old
custody. Result mutation, EACK retry, persistence, and the `eack_pending`
transition MUST share one serialized owner. Collection start and every accepted
result MUST set `eack_pending`; only the durable closed-round commit clears it,
and a later exact duplicate MAY rearm it after a missed final EACK.

## Gateway control and reachability

### GCTL-01 — Control-plane priority

Gateway-originated commands and host-requested Here-I-Am are highest-priority
mesh control at the first safe radio boundary. They MUST precede local click
report delivery, transit, retries, route maintenance, and background work.
Already-started timing-critical ranging MAY run until its defined safe abort
point. Priority derives from gateway origin, not a command allowlist.

### GCTL-02 — Broad flood propagation

Broad gateway control MUST propagate outward on Channel 5, not Channel 9.
Each relay MUST use randomized forwarding delay, listen for competing copies
while waiting, choose the best observed path, validate relevance and freshness,
execute local or broadcast commands, and forward with a new wake train.
Gateway commands SHOULD use a small bounded number of attempts, and every retry
MUST carry its own wake train.

A garbled, colliding, or otherwise undecodable command frame MUST leave the
relay listening for a bounded later copy. Command responses return through the
normal response path while the gateway remains or resumes Channel 9 receive.

Broadcast-command relay deduplication MUST use source, destination, and
mandatory nonzero command sequence; the envelope session MAY span several
commands. Exact sequence retries are inert, but a later sequence in the same
session MUST remain deliverable.

### GCTL-03 — Targeted control forwarding

A relay MAY capture a non-local targeted command or survey PREPARE only with a
current-epoch downlink for the exact destination, a valid downstream next hop,
and no loop to the physical previous hop. It MUST decrement TTL and forward a
wake plus bounded flood without replacing its upstream route.

Several target routes MAY share one physical next hop while retaining distinct
identities. This independent control lane MAY run while unrelated reliable
custody waits, but it MUST NOT mutate or complete that custody.

### GCTL-04 — Control PHY and response opportunity

Gateway-control follow-up MUST use the extended-PHR control PHY from the wake
claim through every follow-up frame. The receiver MUST keep that ownership
through the bounded exchange. A command response uses protocol-priority reliable
custody and the ledger's response opportunities and natural route-depth
deadline. The bounded control flood itself still requires four real starts.

### GCTL-05 — Host-requested Here-I-Am only

The gateway MUST NOT run startup or periodic Here-I-Am or hide command readiness
behind a firmware latch. On explicit host request, Here-I-Am MUST use priority
control, refresh route epoch and best observed gateway path while flooding
outward, and MUST NOT create extra anchor route slots. Click work keeps its
preemption contract.

Response-priority refresh deadlines MUST have a separate armed flag. A wrapped
32-bit zero is a valid deadline value and MUST NOT serve as an unarmed sentinel.

### GCTL-06 — Reverse hint before protocol mutation

Before an anchor protocol handler commits a gateway-originated control, the
communication service MUST install or refresh the upstream candidate from the
actual previous hop. Source, hop, remaining TTL, route epoch, and quality MUST
describe a valid direct or bounded relayed path or the control fails before
state mutation.

The hint creates no gateway connection or timing. A Channel-9-preferred response
MUST first negotiate a real rhythm with a selected non-gateway parent while
retaining pre-RF custody. Full discovery MAY begin only when no logical parent
exists or timing repair fails terminally.

## Anchor assignment

### ASN-01 — CLAIM, TABLE, and partial roster

Assignment MUST use one CLAIM bounded flood and collection window followed by
one TABLE bounded flood and collection window. The expected count is an
early-completion hint and optional host qualification target, not an
all-or-nothing firmware quorum. The gateway MUST publish every valid claim at
window close. An anchor joins the usable roster only after its table ACK;
missing ACKs MUST NOT revoke others, and unused logical slots MAY remain gaps.

At least one real RF start and the configured response horizon are required
before a no-response outcome. Queue admission, pre-RF deferral, cancellation,
worker blockage, and zero-attempt failure MUST retain their explicit terminal
reason.
Collection MAY accept correlated responses after the first real flood copy
while the remaining required copies continue, but successful operation still
requires useful committed responses and its final correlated terminal.

### ASN-02 — Response spread and custody

All assignment responders MUST use the same randomized response policy
regardless of route depth. The profile controls spread, retry limit, and per-hop
custody within safety bounds. Depth MAY extend custody and host estimates but
MUST NOT grant priority. CLAIM and table-ACK custody MUST cover the complete
bounded-control blackout and collection window; a generic short result deadline
is insufficient.

### ASN-03 — Causal phase and host reporting

The next assignment phase MAY start only when each accepted response is
gateway-ACKed or has one explicit owner capable of completing independently.
Exact duplicates MUST be ACKed without restarting an operation-wide sleep.
The operation MUST end when its configured window closes and committed partial
roster is known; the host estimate derives from the installed profile and
observed route depth.

CLAIM and table ACK are internal controls and MUST NOT depend on BLE stream
capacity. They still require identity validation, bounded state commit,
duplicate handling, and transport ACK finalization. Host mapping MUST
distinguish committed anchors from claimed-but-unacknowledged anchors.

### ASN-04 — Epoch, generation, and durable membership

An anchor owns one exact response per operation epoch and phase. Repeated flood
envelopes MUST retain its delay, payload, and handle; only a new phase or epoch
MAY supersede it.

Table generation is a common nonzero command sequence, flood epoch, and packet
session. Anchors MUST persist it with the full sorted-table fingerprint.
A newer generation may replace the table; equal generation is idempotent only
with the same fingerprint; old or conflicting input fails. An omitted anchor
MUST persist an unprovisioned tombstone. Exact replay MAY rearm a missing ACK.
Before host success, the gateway MUST persist only committed sorted IDs under a
deterministic nonzero 16-bit membership epoch and report per-entry admission
failures without revoking prior commits.

## Anchor survey

### SUR-01 — Continuous randomized discovery

Survey reachability MUST use the same control-follow-up eligibility as
enumeration. The gateway floods one correlated discovery start and profile.
Each anchor remains in one continuous configured set of randomized announce and
listen rounds, deduplicates stable identities, and retries pre-RF deferral
within the remaining window without erasing peers, restarting the window, or
consuming an RF attempt.

One directed observation is useful pair evidence; reciprocity MAY improve
diagnostics but is not required. No-anchors is valid only after a real start,
the full configured report horizon, and no useful committed report or
observation. The gateway collection horizon MUST include every configured
round, report spreading, route-depth delivery allowance, and host-selected
grace.

### SUR-02 — Peer bounds and partial graph

Each report MUST obey the ledger's peer cap. The gateway MUST build candidates
from every directed edge and SHOULD prefer stronger edges when degree must be
bounded. Disconnected components and isolated anchors MUST retain all reachable
pair work and explicit isolation; they MUST NOT turn useful partial topology
into total failure.

### SUR-03 — Durable discovery-report ownership

Before first transport attempt, an anchor MUST transactionally persist the
exact encoded report, survey identity, generation, state, and bounded attempt
budget. Route waits, control preemption, retries, and reset MUST resume the same
record. A later survey MUST be rejected while report custody remains.
The durable record MUST clear in two phases only after exact gateway-ACK commit.

Each real attempt consumes a persisted token before RF; a matching pre-RF
refusal MAY refund it. Late callbacks may mutate only that token and packet
identity. The journal MUST own its route retry without occupying the generic
route-wait slot. A transient first-write failure retains the exact RAM candidate.
Old or corrupt records MUST be quarantined with diagnostics, and cleanup failure
MUST NOT stall startup.

### SUR-04 — Report custody and collection tail

Reachability-report custody MUST scale with selected route depth using the
ledger's direct through four-hop values; unknown depth uses the conservative
maximum. Gateway collection MUST cover that maximum at build time.

An optional expected count MAY close collection only after the complete
emission horizon and an exact unique-report count. A shortfall retains the full
tail then fails explicitly; over-count fails after emission rather than
truncating; absence of a count keeps the full conservative tail.

### SUR-05 — First-accepted reverse evidence

The first semantically accepted current-survey report for an anchor MUST freeze
its peer set and reverse hint. It is eligible only after local gateway delivery
on Channel 9, gateway-ACK request, matching survey and packet identities,
current operation, valid payload, previous hop, and quality. Later valid or
duplicate reports are ACKed and counted but MUST NOT replace or refresh that
state. Rejected, stale, malformed, or wrong-channel input stores no hint.

The gateway MAY reinstall the hint under its current route epoch immediately
before target control. It is route evidence only, creates no connection or
timing, and supports the ledger's full survey roster without fitting all
downlinks into the general table.

### SUR-06 — Pair planning and lane isolation

Pair roles MUST use deterministic endpoint ordering; depth affects estimates,
not role or priority. A pair neighborhood contains both endpoints and every
peer either reported. Pairs MAY share a round only when neighborhoods and known
reverse relay roots are disjoint. When the full map is unavailable, a depth
difference of at least two MAY conservatively prove separation; unknown or
adjacent depth serializes.

Each lane MUST own independent result, cleanup, rerun, and deadline state.
An anchor MUST reject a second simultaneous reservation, so approximation may
reduce throughput but not correctness.

### SUR-07 — PREPARE and START path parity

Before every automatic or host-issued manual PREPARE and START, the gateway
MUST reinstall the target's retained current-epoch hint and submit through the
same node-communication bounded Channel 5 wake-and-flood profile. An explicit
target or timing-free hint MUST NOT fall back to one unscheduled Channel 9
transmission.

The wake and every follow-up MUST use the same extended-PHR control PHY. This
applies equally to dedicated PREPARE and command-based START. Manual and
automatic control MUST have the same bounded delivery, response, and cleanup
semantics.

### SUR-08 — Control responses and ACK settle

Every accepted PREPARE or START result that advances the phase MUST leave
Channel 9 available for the ledger's continuous response-ACK settle before the
next control flood or GO. An exact duplicate MUST be ACKed and restart the
interval. Responses retain the ledger's opportunities and route-depth deadline;
an explicit host budget MAY clip that natural deadline but MUST NOT divide the
remaining time among future phases.

### SUR-09 — Synchronized GO and local RF window

For a nonzero synchronized round, the gateway MUST complete PREPARE and START
for every endpoint before one common future GO. Each endpoint derives the same
instant from age-compensated delay. The delay MUST reserve a complete
synchronous forward horizon per RF hop, including randomized relay backoff,
wake, burst, and guard.

The responder window begins at that instant and covers only bounded local skew,
the initiator timeout, and complete frame airtime. A frame extending past it is
a timeout; transport deadlines MUST NOT enlarge it. Temporary capacity or a
retryable terminal with zero RF starts MUST regenerate GO with a fresh future
instant within the operation deadline. Permanent pre-RF failure MUST terminate
explicitly. Forwarding and local delivery MAY share one worker, including on a
forced-hop anchor, but that sequencing MUST NOT make an otherwise valid GO late.

### SUR-10 — GO custody, batch progress, and host ordering

After GO submission, the gateway MUST poll the exact custody at a bounded
interval until first RF or terminal. The pre-RF phase MUST NOT become a
zero-delay worker loop. Cleanup that makes a batch complete MUST immediately
advance or terminate the batch.

Under BLE backpressure, a pair-start boundary MUST be committed before waiting
for RF completion or emitting pair success or failure. A consumer MUST NOT see
a terminal pair outcome for an unannounced pair.

### SUR-11 — START and result generations

An exact duplicate START retains existing response custody. A newer sequence
for the same prepared pair MUST atomically install the new identity, detach and
abandon the old handle, and permit terminal callbacks to release state only
when both handle and command identity still match.

Every synchronized result MUST carry its exact nonzero round generation.
Gateway comparison MUST occur before pair lookup or sample-mask mutation.
Missing, previous, or future generation is stale even when other identity
matches and MUST NOT be ACKed as a current result. Legacy zero-round results are
valid only in the legacy zero-round coordinator. Rejected stale custody ends
through its existing bounded pair-result deadline, not a new current-result ACK.

### SUR-12 — Geometry and bounded rerun

A sample is usable only when status is `RANGE_OK` and distance is positive;
there is no 50 mm floor. Unusable results remain transport-ACK-eligible but do
not fill the sample. Usable data outranks unusable data for the same index;
reporter priority breaks ties only at equal usability.

When both reports prove remaining samples unusable, or unusable/missing data
remains at the end of the same continuous observation window, the gateway MUST
clean both roles and rerun the complete pair. It MUST enforce the ledger's rerun
cap, then emit one explicit failure without looping or cancelling successful
independent pairs.

### SUR-13 — Survey cleanup and partial terminal

Every armed or possibly prepared endpoint MUST receive matching START or
idempotent ABORT, with the prepared lease as the bounded final safety net.
Cancellation, timeout, delivery failure, rerun, lane retirement, and operation
abort MUST retain one cleanup owner and one terminal boundary.

Useful committed discovery components and successful pairs MUST survive
missing anchors, collisions, isolated nodes, or another pair's failure.
Terminal telemetry MUST distinguish planned, successful, failed, duplicate,
isolated, and incomplete work; silence and false total failure are forbidden.

## Failure, reset, and route recovery

### FAIL-01 — Fail-closed validation and capacity

Malformed envelope, wire cap, TTL, ancestry, profile, identity, generation,
route, queue, or persistence state MUST fail before protected mutation or ACK.
Unsupported relay action, partial airtime, collision, missing route, and
capacity exhaustion MUST remain explicit. Implementations MUST NOT add seeded
routes, direct-delivery fallbacks, or success fallbacks that conceal a broken
production path.

### FAIL-02 — Parent failure accounting

Only a completed RF send followed by terminal gateway-ACK timeout MAY increment
parent failure. Pre-RF policy, admission, route, or local-send refusal MUST NOT.
Retries MUST use the ledger's failure-count delays. The fourth real failure
MUST invalidate the active path and put the physical parent in the ledger's
hold-down, anchored to the actual wrap-safe failure time; it MUST NOT
permanently blacklist that parent.

### FAIL-03 — Active-path invalidation

Terminal active-parent failure MUST invalidate the active route epoch and path,
not one packet. It MUST clear the failed upstream route, downstream entries
through that peer, timing reservations, and dependent discovery state while
preserving a current-epoch downlink through another child for an ACK in flight.
The node SHOULD send or queue invalidation downstream. Propagation MAY stop at
a hop failure; unseen descendants then expire by their own supervision.

### FAIL-04 — Alternate selection and healing

During hold-down, selection MUST skip the parent and try the best valid
alternate before discovery. Valid fresh route evidence or successful delivery
MUST clear failure and hold-down; expiry MUST trigger reselection even without
new evidence. A direct-gateway parent held down after bulk ACK failure treats
short probe success as contact only until hold-down expires or direct payload
succeeds.

Gateway downlinks follow the same three-retry/fourth-failure rule. Pre-RF busy
or queue refusal MUST remain communication custody and MUST NOT count as a
downlink delivery failure.

Discovery SHOULD start or restart only when no valid parent exists, selected
parent retries are exhausted with no alternate, the route epoch is explicitly
invalidated without a candidate, or connection supervision declares the route
dead. One missed ACK inside a live retry budget MUST NOT restart discovery.

### FAIL-05 — Bounded failure and partial outcomes

Every live operation MUST preserve its absolute deadline, attempt count,
liveness owner, and exactly one terminal result through preemption, pause, and
cleanup. An operation MAY preserve those guarantees through reset only when its
owner durably persists the state needed to resume or report it. A volatile
operation abandoned by reset MUST start any later work in a new generation so
stale callbacks or identities cannot complete it, and its requirement and
verification evidence MUST identify reset as an unqualified loss boundary.

No-response is valid only after at least one real RF start and the complete
configured horizon. Useful partial assignment or survey state MUST be reported
rather than erased while its operation remains live; zero-attempt success,
silent replacement, and ACK before semantic acceptance are failures.

## Connection teardown

### END-01 — Teardown threshold

A connection MUST survive transient clicks and isolated ACK loss. It SHOULD
return to low-duty Channel 5 only after explicit close or invalidation,
sustained missed Channel 9 cycles, stale route state with no hop progress, or
explicit route rejection. The inactivity threshold MUST cover worst-case click
handling, retune, and scheduling jitter.

### END-02 — EVENT_END and residual ownership

EVENT_END is authoritative only for the active immediate-peer session. The
sender becomes terminal after successful RF and clears its local timing and
peer work; the receiver does so only after matching decode. A lost END MAY leave
asymmetry only within the receiver's existing supervision bound.

Expiry, invalidation, reset recovery, or explicit failure MUST abandon the
remaining owner, clear peer timing and callbacks, and restore normal idle duty.
Duplicate or stale END MUST NOT complete twice, refresh a deadline, clear newer
ACK custody, or mutate a later negotiation.

## Security statement

### SEC-01 — Integrity is not hostile-RF authentication

CRC, route identity, epoch, previous-hop, and quality validation detect
corruption and stale or accidental poisoning. The current mesh has no keyed MAC
or keyed STS and MUST NOT be described as authenticating hostile RF. Likewise,
gateway journaling and a bounded GUI session cache MUST NOT be described as
end-to-end exactly-once delivery across host restart.

## Permitted efficiency work

### OPT-01 — Behavior-preserving optimizations

Optimizations MAY shorten a reply listener after a usable reply is selected and
ACKed, calculate slots from actual airtime plus guard, carry multiple packets
and hop ACKs in one anchor slot, and batch direct-gateway packets.

A future embedded request suffix MAY replace the standalone request only when
the complete equivalent payload fits and is successfully queued. Under the
ledger's current ancestry and suffix sizes it does not fit. Malformed or
queue-full embedding MUST leave the standalone fallback open. No optimization
may weaken another requirement.

## Verification and release evidence

### VER-01 — Required behavioral proof

Changes to routing, Channel 5/9 scheduling, sleep, wake trains, ACKs, discovery,
event timing, gateway control, custody, survey, click preemption, capacity, or
teardown MUST identify the requirements they preserve and add a worst-case
test or build-time guard. Focused native coverage MUST include malformed input,
full capacity, pre-RF refusal, actual-RF loss, duplicate/conflict identity,
stale generation, deadline, cleanup, and liveness where applicable.

The mandatory traceability validator MUST enforce unique contract IDs, one
manifest entry per ID, the complete taxonomy, valid referenced files, and no
orphan IDs. It validates document structure, not runtime behavior.
Run it directly with
`python3 firmware/tests/mesh_integration/test_mesh_contract_traceability.py`.

### VER-02 — Evidence layers and deployment

Native tests, mesh integration, hardware models, exact production-preset builds,
static stack gates, typed runtime evidence, BLE observation, and requested
multi-board smoke tests prove different boundaries and MUST NOT be substituted
for one another. Known partial or deferred coverage MUST remain explicit in the
manifest. Hardware deployment and qualification MUST follow the
[Development and Deployment Guide](<Development and Deployment Guide.md>);
local mapping or a green simulator alone MUST NOT be called hardware proof.

## Normative numeric and capacity ledger

The values below are part of the referenced requirements. A detail document MAY
derive subordinate timing from them but MUST NOT silently replace them.

| Area | Normative value |
| --- | --- |
| Connected anchor Channel 5 window | `100%` receive duty for the allocated window |
| Clicker quiet streak before wake | at least `100 ms` continuous quiet |
| Wake-train activity probes | fixed `20 ms` before and `20 ms` after |
| Activity retry range | randomized exponential `200..2000 ms` |
| Bounded Channel 5 flood | exactly `4` actual RF starts; one `20 ms` quiet check per opportunity; `40 ms` after each successful copy |
| Gateway logical Channel 9 receive horizon | `30 s`, implemented as bounded cooperative slices |
| Protocol-priority response | `16` RF opportunities inside its absolute deadline |
| Survey response ACK settle | one continuous `3000 ms`; exact duplicate restarts it |
| Survey control response deadline | `30 s + 15 s` per additional hop; supported maximum `75 s`; missing/invalid depth uses `90 s` |
| Route-request TTL waves | `1`, `2`, `4`, then `6`; later attempts remain `6` |
| Gateway advertisement / reply / reply-ACK TTL | advertisement origin `8`; reply remaining `1..8`; reply ACK exactly `1` |
| Route ancestry | at most `9` node IDs; maximum ancestry TLV `74 bytes`; a tenth append fails |
| Current request suffix fallback | minimum request `72 bytes`; wake suffix carries at most `55` request-payload bytes, so standalone control is mandatory |
| Route-control payload caps | request `139`, reply `164`, gateway advertisement `138`, reply ACK `34` bytes |
| Route formation backoff bases | `1000`, `2000`, `4000`, `8000 ms`, then double with base capped at `60000 ms`, plus fresh percentage jitter |
| Expected route-forward fanout | jitter sized for normally up to `8` simultaneous receivers |
| Upstream candidate ancestry storage | exact paths for `3` candidates on anchors |
| Communication frozen records | `4` ordinary records with `192-byte` inline payloads; `1` priority owner for a `921-byte` assignment table |
| Reverse-route capacity | `16` inline entries; an anchor supports `50` total descendant reverse routes without adding gateway/clicker overflow storage |
| Full shared/click payload | up to `958 bytes` at the BLE admission boundary |
| Survey topology | up to `12` heard peers per report and `50` anchors per roster/context |
| Reachability-report custody | direct through four-hop: `5000`, `9000`, `13000`, `17000 ms`; unknown depth uses `17000 ms` |
| Pair rerun | at most `2` complete reruns |
| Collection EACK | nonzero `16-bit` sequence, wrap `65535 -> 1`, and exactly `4` actual Channel 5 recovery starts |
| Retired event ownership | at least `8` retired sessions and their boot incarnations |
| EVENT_PROPOSE boot incarnation | fresh nonzero `64-bit` value per sender boot |
| Parent retry and invalidation | `1500`, `3000`, `6000 ms` bases; fourth real failure invalidates; `30 s` hold-down |
