# Mesh Connected Routing Walkthrough

This is the plain-English walkthrough of the production-candidate
`mesh_gateway`, `mesh_anchor`, and `mesh_clicker` protocol. It describes the
current source behavior. Automated validation and hardware qualification are
separate: a green simulator and exact-role build prove the software contract,
while a multi-board smoke test is still required before calling a particular
binary hardware-qualified.

## The Whole Flow

```text
User clicks a GUI command
  -> GUI freezes that command and its runtime settings
  -> GUI sends a separately correlated Here-I-Am
  -> GUI waits for the successful Here-I-Am terminal event
  -> GUI sends the frozen command
  -> gateway and anchors run the requested bounded protocol
  -> useful partial results are retained
  -> one correlated terminal event closes the user operation
```

The gateway firmware does not send Here-I-Am at boot or on a timer. It also
does not hide a readiness latch in front of assignment or survey. Sequencing
belongs to the GUI, so changing the experiment flow does not require reflashing
the gateway.

## Here-I-Am and GUI Command Sequencing

For an ordinary gateway command, the GUI creates two different command
identities. The first is Here-I-Am; the second belongs to the requested
operation. A BLE write completion is not enough to advance the sequence. The
GUI waits for the typed terminal event whose command kind, command ID,
correlation ID, host session, and host sequence all match the preflight.

If Here-I-Am fails, times out, or the BLE link disconnects, the target command
is discarded without being transmitted. A stale or duplicate terminal from an
older operation cannot release the wait. The GUI keeps both commands busy as
one atomic user action, so another click cannot interleave a different command
between them.

Manual Here-I-Am is direct because preflighting it would recurse. Abort, stop,
disconnect, and liveness heartbeat operations are also direct because delaying
them behind route maintenance could prevent recovery. Internal survey phases
belong to the already-preflighted survey and do not run another Here-I-Am.

Here-I-Am sends one logical bounded channel-5 flood with four real RF starts.
A busy radio before RF does not consume one of those four opportunities. The
successful terminal proves the gateway completed its own flood custody; it
does not claim that every possible anchor heard the announcement. Assignment
and survey provide the stronger per-anchor evidence.

## Runtime Settings in the Packet

The GUI freezes one versioned operation-policy profile and puts byte-identical
copies in both Here-I-Am and the target command. Gateways install the accepted
profile in RAM, repeat the relevant part in generated control packets, and
route advertisements preserve the complete profile through every relay hop.
After reset, firmware safe defaults apply until a host sends another profile.

The current profile exposes these settings:

- Assignment: expected anchor count, total operation budget, and the equal
  randomized response spread.
- Survey discovery: future start delay, slot duration, slot count, round count,
  report grace, and total operation budget.
- Pair ranging: maximum reruns and maximum simultaneous non-conflicting pairs.

PHY selection, measured airtime, minimum retune guards, the 1.15-second local
ranging receive bound, and memory/frame capacity remain firmware-owned because
values below those physical limits cannot work. Malformed policy versions and
unsafe values are rejected; optional host qualification can impose stricter
expectations without turning a useful partial firmware result into failure.

Default GUI values are:

| Setting | Default |
|---|---:|
| Assignment budget | 235,209 ms |
| Assignment response spread | 1,000 ms |
| Survey discovery start delay | 6,000 ms |
| Discovery slots | 6 x 40 ms |
| Discovery rounds | 4 |
| Discovery report grace | 250 ms |
| Survey budget | 600,000 ms |
| Pair samples | 1, configurable through 4 |
| Pair reruns | 2 |
| Concurrent-pair cap | automatic, up to 25 |

Budgets are absolute failure ceilings, not sleeps. Healthy phases advance as
soon as their real terminal conditions are met. The long 600-second survey
budget covers a large topology with failures and cleanup; it is not the normal
time for a survey.

## Routes and Packet Custody

Routes do not expire merely because time passed. Once a route is known, the
node uses it directly and does not rediscover it before each packet. Channel-9
timing can become stale independently and can be repaired without deleting the
logical route.

An actual completed delivery that misses its gateway ACK increments the
selected parent's failure state. The same route is retried with randomized
backoff, and only the fourth real failure invalidates that parent and places it
in a 30-second hold-down. A pre-RF refusal, queue pressure, local radio
preemption, or a packet that never started RF does not count as evidence that
the route is broken. An alternate remembered parent is tried before route
discovery.

Delivery is hop-by-hop:

- A hop ACK means the next relay accepted custody.
- A gateway ACK means the gateway semantically accepted the packet.
- The current custody owner retains the immutable packet until the next owner
  has accepted it or an explicit terminal failure returns it for retry.
- Duplicates are processed once but remain ACK-eligible, so losing an ACK does
  not lose the original data.
- Multiple descendants may share one physical relay schedule while keeping
  separate logical routes and packet identities.

Every logical packet has one retry/deadline owner in the communication service.
Protocol coordinators retain transaction state and delivery handles, but they
do not start a second private retry loop for the same packet. This removes the
class of failures where one layer declared success while another still owned a
retry or deadline.

## Selfish Anchors and Priority

An anchor sends data produced by its own click, ranging, or command-result work
before transit payload waiting in its relay queue. Accepted transit custody is
not erased; it remains queued and resumes after the local item reaches a safe
handoff. A deeper source gets no extra priority over a shallower source.

Gateway control remains control-plane work and runs at the first safe radio
boundary. An ACK already owed for an accepted protocol response remains ahead
of a later equal-priority control flood, so the next command cannot strand the
previous sender. Click/ranging radio ownership can preempt mesh work, but the
interrupted mesh packet remains owned and retryable.

## Anchor Enumeration and Slot Assignment

Assignment uses a CLAIM phase followed by a TABLE phase:

1. The gateway floods CLAIM with a new assignment epoch and the runtime
   assignment policy.
2. Every anchor waits `100 ms + random(0..response_spread-1)` before its first
   claim. All anchors use the same rule; hop depth does not change priority.
3. Each claim contains the anchor's stable hardware-derived ID and observed
   gateway hop count. The gateway deduplicates claims and records the physical
   reverse hop used to reach each anchor.
4. The gateway sorts the stable IDs, assigns logical slots, and floods one
   table. Each receiving anchor validates and persists its entry before it
   returns an ACK.
5. The gateway commits and publishes every ACKed entry. Missing expected
   anchors and missing table ACKs appear as counts in telemetry, while a
   non-empty useful subset may still finish with `COMMAND_OK`. Zero committed
   anchors is a real failure.

The expected-anchor count is an early-completion hint and an optional host
qualification target. While expected anchors are still absent, the collection
window stays conservative instead of shrinking after a shallow claim. Response
custody and quiet-settle time scale with observed hops, so a small direct setup
can finish sooner without clipping a deeper setup.

## Survey Neighbor Discovery

Survey discovery answers one simple question: which anchors can hear which
other anchors on UWB channel 5? It does not rely on the gateway route graph.

The gateway floods a future start time plus the slot and round settings. Every
anchor compensates for packet age, then stays in the same continuous discovery
window. In each round it hashes its anchor ID, survey ID, and round number to a
slot, listens before its own slot, transmits one short probe, and listens after
it. With the defaults, the RF discovery window is
`6 slots x 40 ms x 4 rounds = 960 ms` after the future start delay.

There is no reserved second half and no rule that all four probes must reach RF
for the report to be useful. A missed or collided probe is simply absent; later
rounds choose independently mixed slots. Each anchor reports the peers it did
hear, with signal strength and quality. One-way evidence is enough to create a
candidate pair, so the reverse direction need not repeat the same observation.

The gateway accepts valid partial reports and builds a bounded-degree connected
pair graph when the observations support one. An isolated anchor or a missing
edge remains explicit rather than being invented from route state.

## Safe Concurrent Pair Ranging

The gateway first assigns pairs to conflict-free planner rounds. A pair's
neighborhood is the union of both endpoints and every peer either endpoint
reported. Two pairs may share a round only when those unions are disjoint:
they cannot share an endpoint, a reported third peer, or the same physical
reverse relay. That last rule matters when several logical routes all pass
through one anchor with one radio.

Complete peer reports are authoritative. If a report hit its peer-capacity
limit and may be truncated, the planner uses the requested shortcut: endpoints
whose gateway hop depths differ by at least two are treated as logically
separated unless their retained reverse routes still prove a shared relay.
Uncertain pairs are serialized. The GUI's concurrency value is only an upper
cap; it cannot force conflicting pairs to run together.

A large safe planner round may be split into batches of at most 25 lanes. Each
lane owns its exact pair, sample masks, rerun count, cleanup state, and terminal
telemetry, so one failed pair does not cancel unrelated lanes.

## PREPARE, START, and the Common GO

Controls are delivered serially because gateway control has one transport
owner, but radio ranging is concurrent:

1. The gateway PREPAREs the initiator and responder for every live lane.
2. It STARTs each responder and then each initiator. For a nonzero survey round,
   START arms the local role but does not begin DS-TWR.
3. Only after every live lane is armed does the gateway broadcast one GO with
   the survey ID, nonzero round ID, and a future execute delay.
4. Anchors accept only a matching GO and derive the local execution instant
   from its remaining age-compensated delay. A duplicate GO is harmless.
5. Every pair in that batch starts from the same logical barrier, while the
   planner has already ensured that their radio neighborhoods do not overlap.

The GO delay is dynamic. A direct or one-hop topology uses 2.5 seconds; every
additional relay hop adds 4.2 seconds, which covers one complete randomized
relay-forwarding horizon. This shortens small setups without assuming that a
maximum-depth flood arrives instantly.

## Ranging and Timing

The local UWB receive window is independent of mesh delivery time. For each
sample, the responder has a 1,150 ms window: 1,000 ms for bounded cross-anchor
execution skew plus the initiator's 150 ms DS-TWR timeout. Samples have a 10 ms
gap, and the gateway adds a 50 ms settle margin. A four-sample healthy pair is
therefore budgeted at `4 x (1,150 + 10) + 50 = 4,690 ms`, rather than four
90-second waits.

Mesh control timeouts scale separately with the endpoint route: 30 seconds for
a direct/one-hop command plus 15 seconds per additional hop, with 90 seconds
used when depth is unknown or outside the supported bound. Those values are
failure ceilings for PREPARE/START result delivery. They never enlarge the
local UWB responder window.

An anchor's prepared-state lease is 660 seconds so the maximum 600-second host
operation still has cleanup margin after a reset or lost command. It is a
safety cap, not a delay on the healthy path. Successful START and GO proceed
immediately according to their normal barriers.

## Results, Partial Success, and Cleanup

A range sample is useful when its status is successful and its distance is
physically usable. Either endpoint may report it. Duplicate reports are
accepted idempotently, and a usable report wins over an unusable report for the
same sample index.

When all requested samples for one lane are present, that lane succeeds without
waiting for unrelated lanes. A lane with missing or unusable samples is cleaned
up and rerun as a complete pair up to the configured limit. Persistent failure
produces one explicit pair failure, while other lanes and later batches keep
running. Survey terminal telemetry contains planned, successful, failed, and
duplicate counts, so partial geometry is normal prototype output rather than a
silent all-or-nothing loss.

PREPARE is always paired with START or an idempotent ABORT. Cancellation,
delivery failure, timeout, and rerun all clean whichever endpoints may have
accepted state. The prepared lease is the final safety net if cleanup traffic
itself is lost.

## What Is Validated

Native tests cover packet encoding, policy propagation, route lifetime,
assignment partial completion, pair planning, lane isolation, cleanup, GO
barriers, and timing boundaries. Mesh integration and hardware-model tests add
multi-hop routes, shared relays, collisions, complete-frame RX containment,
SPI and BLE delays, watchdog timing, queue pressure, route loss, and retry
custody. Exact Zephyr builds compile `mesh_clicker`, `mesh_anchor`, and
`mesh_gateway`, catching role-composition and static memory errors that native
tests cannot see.

Hardware proof still requires the exact built artifacts on mapped probes, typed
RTT lifecycle evidence, BLE observation, and a multi-board topology that shows
direct, relayed, shared-relay, route-loss, assignment, survey discovery, and
concurrent non-conflicting pair behavior. Software gates should pass before
that flash, but they do not substitute for the RF smoke test.
