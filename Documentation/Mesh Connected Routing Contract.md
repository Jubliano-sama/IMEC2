# Mesh Connected Routing Contract

This document is the high-level contract for connected mesh routing, channel 5
control, channel 9 relay traffic, ACK behavior, route setup, and click
preemption. It is intentionally written without implementation identifiers so
it can be used as the source of truth before changing code.

## Authority Of This Contract

Do not push through a code change that knowingly contradicts this document.
When a change appears to require different behavior, stop and get explicit user
permission before implementing it.

Before such a change is made, present a clear list of the proposed new
behavior. The list must state what behavior changes, which roles are affected,
which invariant is being changed or removed, and what tests or hardware checks
will prove the new behavior.

## Host-Owned Operation Policy

Experiment policy belongs to the GUI or another host controller. Firmware owns
bounded radio and protocol mechanisms, but it must not choose a hidden sequence
of prerequisite operations or require a reflash to change ordinary protocol
timing.

Every ordinary GUI-triggered gateway operation is one host-side sequence:

1. The host freezes the target command and its runtime profile.
2. The host sends a separately correlated "Here I Am" request.
3. The host waits for that request's typed successful terminal. Intermediate,
   stale, duplicate, or uncorrelated telemetry cannot release the wait.
4. The host sends the frozen target command only after that terminal. A failed
   refresh, timeout, or disconnect discards the target without sending it.
5. The host keeps the whole two-command sequence busy as one user operation.

Manual "Here I Am", disconnect, emergency abort or stop, and liveness
heartbeats are direct operations because preflighting them would recurse, delay
recovery, or deadlock an active operation. Firmware-generated phases inside one
survey are also part of that survey and do not receive independent preflights.

Each host command carries one compact, versioned runtime profile. The same
accepted profile is propagated to participating anchors instead of being
reconstructed from separate gateway and anchor constants. The profile may set
the expected assignment count, assignment budget and response spread; survey
discovery start, slot, round, grace and budget values; and pair rerun and
parallel-lane caps. Firmware rejects malformed versions, arithmetic overflow,
values outside stated safe bounds, or combinations that cannot fit a complete
frame plus retune and guard time. PHY configuration, measured frame airtime,
minimum retune/guard time, local pair receive bounds, route transport deadlines,
antenna delays, and delayed-transmit quantization remain firmware-owned safety
facts.

The last successfully accepted profile remains active in RAM for later
anchor-originated click/report delivery. It is not persistent release policy:
after reset, compiled safe defaults apply until the next manually requested or
host-preamble "Here I Am" carries a profile. The target command repeats the
same profile so a node that missed one of the two floods can still configure
the operation it actually receives.

Normal prototype mode validates outcomes: correlated terminal identity,
well-formed packets, bounded arithmetic, and useful committed results. Exact
roster counts, complete telemetry histories, zero event loss, and all-node
success are optional host-side qualification checks; they do not turn useful
partial assignment or survey results into total firmware failure.

## Non-Negotiable Invariants

- Channel 5 is the control and preemption lane. Click/ranging-class channel 5
  traffic has priority over channel 9 relay work.
- Route-request channel 5 traffic is not click/ranging traffic. A connected
  anchor must not let an unrelated route-request wake train preempt an active
  channel 9 rhythm.
- A connected anchor must keep regular channel 5 receive windows available. The
  intended connected rhythm is channel 9 work, then channel 5 receive, then
  channel 9 work, then channel 5 receive.
- Channel 9 work must be clipped, deferred, or retried around required channel
  5 windows. Channel 5 must not be starved by channel 9 relay or ACK work.
- A connected anchor's allocated channel 5 window is a 100 percent receive-duty
  window. The receiver stays armed for the full allocated duration and is
  immediately rearmed after unrelated, malformed, or route-class frames. Those
  frames do not end the window. A valid click/ranging claim is the only normal
  early exit because it transfers ownership to the click sequence.
- Every channel 5 receive owner on an anchor, including the connected-mesh gap
  receiver and the low-duty click scanner, must hand a valid click/ranging wake
  claim directly to the same click sequence. Once decoded as click/ranging
  traffic, the claim must not be reclassified as route contact, left for another
  receiver to catch from a later wake packet, or deferred to a later attempt.
- Click/ranging discovery uses the standard wake PHY used by the clicker. The
  extended-PHR mesh-control PHY is reserved for route and mesh-control follow-up
  traffic and must not be selected merely because the anchor is a mesh build.
- Discovery-reply slots must contain the complete standard-wake PHY airtime of
  a discovery reply plus the required guard for radio transition and bounded
  clock jitter. Assigned and deterministic unprovisioned slots use the same
  spacing; adjacent anchors must not overlap on air.
- The configured click wake train must exceed the worst-case anchor channel 5
  RX-off gap, including startup and retune time. Builds must reject a scan
  interval that cannot guarantee overlap with the first wake train.
- Before starting a click wake train, the clicker requires at least 100 ms of
  back-to-back quiet channel-5 receive time. Any valid frame or partial PHY
  activity resets the quiet streak; undecodable traffic must not count as
  quiet. The politeness phase remains bounded, and exhaustion is reported
  explicitly before click priority proceeds.
- After accepting a valid click/ranging claim, the anchor keeps the DWM3000 on
  channel 5 at 100 percent receive duty through the remaining wake train,
  discovery, discovery reply, schedule reception, inter-sample gaps, and all
  DS-TWR exchanges. Only protocol-required channel 5 transmissions and the
  immediate PHY changes around them may interrupt RX. Low-duty scanning,
  standby, channel 9 work, and mesh handback resume only after the click/ranging
  sequence has completed or failed explicitly.
- A connected anchor should not enter retained or deep DWM3000 sleep between
  back-to-back channel 9 and channel 5 windows. It may keep the radio idle or
  ready for retune.
- Normal route acquisition must not use the blind gateway-command channel 5
  flood mechanism. Route requests are still broadcast route-discovery control
  packets: idle anchors may rebroadcast them when TTL allows, but connected
  anchors must not.
- Blind channel 5 flooding is reserved for broad discovery, gateway command
  delivery, and "Here I Am" style reachability, not routine route requests.
- A broad gateway-command wake claim must identify its control follow-up. An
  unprovisioned anchor that receives it enters a bounded extended-PHR control
  receive window and accepts the following broadcast command; it must not
  misclassify that command train as a route solicitation or require an assigned
  discovery slot before it can receive and answer enumeration.
- The bounded extended-PHR control follow-up window must cover the protocol's
  maximum advertised wake-train duration, including the sender's transition
  from the final wake claim to the first gateway-command flood frame.
- Gateway-originated commands and user-requested "Here I Am" route refresh are
  control-plane traffic. They take priority over queued local-origin click
  report delivery, transit payload relay, ACK retries, route maintenance, and
  background maintenance at the first safe radio boundary.
- The gateway does not run startup or periodic "Here I Am" maintenance and it
  does not hide route-refresh readiness behind a firmware latch. Ordinary host
  software starts each user-requested complex operation with a separately
  correlated "Here I Am", waits for its successful terminal, and only then
  sends the operation command. The firmware executes each received command as
  one bounded mechanism and never silently inserts another preamble.
- After an anchor accepts a gateway-originated channel 5 control follow-up, the
  communication service installs or refreshes the upstream response candidate
  from the frame's actual previous hop before the protocol handler can commit
  state. The remaining control TTL must agree with a direct or bounded relayed
  path. This fresh reverse hint creates no gateway connection or channel 9
  reservation. An initial channel-9-preferred response must first negotiate a
  real PROPOSE/ACCEPT rhythm with that selected non-gateway parent, preserving
  the response as pre-RF custody while negotiation retries. It may start full
  gateway route discovery only when no selected logical parent exists or that
  parent timing repair fails terminally. Inconsistent source, hop, TTL, epoch,
  or quality evidence rejects the control frame before it can strand protocol
  state.
- When a connected anchor has gateway-bound packets produced locally by its own
  click/ranging or command-result work, it selects that local-origin work before
  queued transit payload. Already accepted transit custody remains explicit and
  retryable; local priority must not silently erase it. Route depth does not
  grant a producer higher response or delivery priority.
- Any request likely to arrive at multiple nodes at nearly the same time must
  include enough random jitter before consequent replies, probes, or
  rebroadcasts that collisions are unlikely for the expected fanout. The jitter
  budget should be justified from packet airtime, retune time, guard time, and
  the maximum expected number of simultaneous responders.
- Every failed or deferred RF operation must re-enter through the communication
  service's randomized exponential backoff. A pre-RF deferral advances the
  backoff round but does not consume a transmission opportunity; only an actual
  RF start consumes one. Fixed delays are allowed for non-transmitting service
  polls and protocol-defined spacing after a successful transmission, not for
  another RF attempt.
- The gateway does not own a normal channel 9 connection. It is primarily a
  continuous channel 9 receiver, and it originates mesh commands on channel 5.
- Normal route acquisition does not depend on the gateway receiving channel 5
  route requests. Before every route-request broadcast attempt, including the
  original request, every rebroadcast, and every retry, the broadcaster must
  first try a short direct channel 9 gateway probe. When multiple nodes may
  react to the same route request, their probes and rebroadcasts follow the
  general collision-avoidance jitter rule above. In direct-or-relayed mode, a
  successful probe may satisfy route acquisition. In forced-relay mode, the
  probe must still run, but a direct gateway answer must not satisfy route
  acquisition. After repeated direct-to-gateway payload batch ACK failures, the
  short probe is treated as contact evidence only for the parent hold-down
  window; it must not suppress the channel 5 route request that lets anchors
  offer an alternate route.
- Route-request wake trains must be clearly distinguishable from click/ranging
  wake trains.
- The bench-only forced-hop anchor must ignore a route-class wake claim sent
  directly by the gateway when either relayed route requests or relayed gateway
  control is required. It remains available for the relay's following wake and
  control flood; relayed route-class claims and every click/ranging claim keep
  their normal behavior. This filter is disabled in production anchor presets.
- Every wake train must be channel-5 activity polite. Before transmitting the
  train, and again immediately after the train, the transmitter must sample
  channel 5 for a fixed 20 ms slice. Any valid frame or RF progress that reaches
  SFD/frame/CRC/bad-frame failure status counts as activity, even when the
  packet cannot be decoded. Activity in either slice defers the entire wake
  train and retries it after randomized exponential backoff in the 200-2000 ms
  range. The wake-train politeness slices are probes only; unlike anchor
  low-duty scanning, they do not extend into a longer receive window.
- A bounded channel-5 flood burst has exactly four transmission opportunities.
  Every opportunity is preceded by its own 20 ms channel-5 quiet check. Channel
  activity defers that opportunity after randomized exponential backoff but
  does not consume it; four actual RF starts are required unless the absolute
  deadline or a permanent failure ends the burst. Successful repeated sends use
  the normal 40 ms repeat spacing. Gateway-command priority schedules this work
  before lower-priority mesh work, but does not bypass the per-opportunity
  activity check.
- An anchor has at most one upstream channel 9 connection and one downstream
  channel 9 connection. A connected anchor must not answer new route requests
  because no extra channel 9 rhythm is available for a second route.
- An idle anchor that is not already committed to a channel 9 route answers a
  route request when it has a usable route to the requested target. It
  rebroadcasts only when it does not have a usable route, has capacity, and the
  route request TTL allows further propagation.
- Click/ranging wake trains do preempt connected relay mode. After click
  handling, the anchor should return to the existing channel 9 rhythm when the
  connection is still valid.
- A synthetic `mesh_transmitter*` load generator is not a ranging anchor. It
  must never answer click discovery or DS-TWR, and receiving a click-class wake
  claim must not preempt or reclassify its active mesh route. This keeps the
  load source transmitting while the real anchor exercises click priority.
- Hop-level ACK and gateway-level ACK are separate. Hop ACK proves next-hop
  reception. Gateway ACK proves final gateway acceptance.
- For gateway-local click reports, command results, result bundles, survey
  discovery reports, and survey pair results, final acceptance is delayed
  until the complete message is validated and any required protocol state is
  durably committed. The gateway reserves capacity for the complete host record
  before accepting transport custody or applying a protocol mutation, commits
  a newly accepted record to the BLE stream before sending
  its semantic completion response, and cancels the reservation for an exact
  duplicate so one uninterrupted gateway stream emits one committed record.
  Before those commits, the gateway must neither remember the packet as a
  duplicate nor emit its gateway ACK. A full host queue, rejected message, or
  failed persistence attempt therefore remains retryable end to end. The
  journal clear and BLE notification are separate durable steps: a gateway
  reset after notification completion but before NVS clear may replay the exact
  accepted packet, so BLE host delivery is at-least-once across reset. The GUI
  keeps a bounded, session-scoped cache keyed by the complete packet identity
  and exact payload across reconnects; an exact replay remains one visible,
  merged host record, while a same-identity payload or flag mutation
  remains visible as a conflict. For gateway journal identity, message type,
  semantic flags, endpoints, session/sequence, payload length, and durable
  payload bytes remain immutable; relay-local TTL and message age may change
  on a retry and are excluded from duplicate matching. After the commits,
  duplicate reception is ACK-sticky and must not apply the protocol mutation
  or gateway-side BLE delivery twice. Collection results remain in
  collection-EACK custody, so a
  generic gateway ACK cannot complete their sender-side transaction. The
  gateway keeps each accepted ACK identity for at least the sender's maximum
  custody lifetime; the shorter route deduplication window cannot expire an ACK
  while the sender may still retry.
  A GUI-process restart clears the session cache, and no persistent host
  acknowledgement exists here, so this contract does not claim end-to-end
  exactly-once delivery across host restart. Cache eviction under host memory
  pressure has the same residual gap for an old replay, which is why a
  persistent host acknowledgement or host-side journal would be required for
  a stronger end-to-end guarantee.
- For relay paths, ACKs are sent in the sender's next channel 9 transmit
  window. This applies to hop-level ACKs and to gateway-level ACKs that are
  bubbling back through anchors.
- Anchor-to-anchor channel 9 transmit windows may carry multiple packets when
  the slot budget allows it. A hop-level ACK may therefore acknowledge multiple
  packets at once, and every listed packet is fully hop-acked by the receiving
  anchor.
- When a node forwards packets directly to the gateway on channel 9, delivery is
  batch-oriented. The sender must calculate how many packets fit in the current
  channel 9 transmit slot while still leaving enough budget to listen for the
  gateway's reply. It must not transmit more packets than that budget allows.
- Direct-to-gateway channel 9 packets in the same slot carry a batch identity
  and a marker that identifies the final packet in that batch.
- For a single accepted direct-to-gateway packet that requests a gateway ACK,
  the gateway attempts that ACK in the same channel 9 radio turn after the
  fixed turnaround guard. Only a failed RF start hands the exact ACK to the
  reliable control-response service. Queueing it before the immediate attempt
  can miss the sender's bounded receive window even though semantic acceptance
  already succeeded.
- The gateway answers a direct-to-gateway batch only after the final packet
  marker is received. That answer is one gateway batch ACK covering all packets
  from that batch that the gateway accepted.
- A gateway batch ACK is a fully valid gateway ACK for every packet it lists.
  Any node that forwarded those packets must accept the batch ACK and mark all
  listed packets as gateway-accepted.
- In a multi-hop route, hop-level ACKs bubble back one hop at a time toward the
  original transmitter.
- A relay that receives a gateway ACK for transit work keeps the original
  packet and outbox in custody while it hands that exact ACK toward the child.
  Queue refusal, a conflicting same-peer ACK already in the bounded ACK table,
  send failure, or reset before the child-directed ACK transmits cannot mark
  the transit packet gateway-accepted. The ACK table may recognize an exact
  duplicate but must not replace an older unsent forwarded gateway ACK with a
  different one. While that exact child handoff is pending, another copy of
  the same gateway ACK is inert: it cannot enqueue another child ACK, refresh
  the handoff deadline, or otherwise change custody. Only a successful
  child-directed channel 9 transmission
  completes the relay's original transit custody. If the bounded handoff
  window expires, the relay requeues the immutable original packet so the
  gateway can repeat its ACK; that recovery does not count as an upstream
  parent failure because gateway acceptance already proved that path.
- If a gateway ACK and a hop-level ACK are both queued for the same channel 9
  transmit opportunity, the gateway ACK has priority because it reassures the
  original transmitter that the packet was successfully received.
- Failure of an upstream parent clears timing and routes through that failed
  peer, but it does not erase a current-epoch reverse downlink through a
  different child. A gateway ACK may already be returning over that downlink;
  the downlink terminates through its own bounded failures, explicit route
  clearing, or route-epoch invalidation.
- A hop ACK should extend or reset the gateway ACK timeout window because it
  proves that the packet is still making progress through the mesh.
- A missed hop ACK, gateway ACK, or gateway batch ACK must not cause a new wake
  train or route renegotiation while the channel 9 connection is still alive.
  Retry the unacknowledged packet or packets in the next suitable channel 9
  transmit opportunity.
- ACK retry timing must tolerate the worst-case click-handling interruption.
- A channel 9 connection ends only by explicit close or by sustained inactivity,
  such as several missed channel 9 receive cycles. A temporary click does not
  close the connection by itself.

## Communication Service Boundary

Protocol state machines submit datagrams through one node-communication
boundary. They provide the immutable payload and packet identity, a named
delivery profile, the destination, and an absolute deadline. They do not tune
retry counts, backoff, channel priority, route acquisition, ACK timing, or
custody rules independently. Those policies belong to the communication
service, so survey, assignment, Here-I-Am, click reporting, and later protocols
cannot drift into incompatible transport behavior.

One logical packet has exactly one custody owner at a time. The protocol owns a
delivery handle and its application transaction; the delivery service alone
owns the frozen packet, persistence, route selection, RF attempts, hop and
gateway ACK accounting, retries, and terminal status. A facade, radio scheduler,
survey journal, route-wait slot, or channel-9 pending lane may reference that
owner but must not start a second deadline or retry state machine for the same
packet. Ownership transfer is explicit and atomic, so `accepted`, `RF started`,
`hop ACKed`, `gateway confirmed`, and `caller terminal` cannot disagree about
which component must act next.

Named delivery profiles are part of the communication contract. A caller may
choose the profile that matches its semantics, such as bounded control flood,
reliable uplink, durably owned reliable uplink, control response, or best
effort. A caller must not construct a private profile to make one protocol pass
a narrow timing case. Changing a profile requires testing every protocol that
uses it under collision, busy-radio, retry, route-loss, and deadline pressure.

Protocol control and progress messages have explicit service priority over
ordinary reliable traffic, transit work, diagnostics, and background
maintenance. This includes gateway control floods, assignment claims and ACKs,
gateway-command results, survey reports, and transport control responses. The
communication service reserves admission capacity for this class, while each
protocol remains responsible for either consuming a terminal delivery event or
explicitly abandoning its handle; superseding a request must atomically cancel
and reap the old handle so terminal records cannot consume capacity forever.
When the gateway has already queued the transport ACK owed for an accepted
protocol response, a later gateway control flood cannot overtake that ACK.
They share the highest service priority, so FIFO order releases the sender's
response custody before the next command can require another response.

A reliable backend may have only one datagram awaiting final acknowledgement.
Additional reliable datagrams wait on that shared resource without consuming an
RF attempt or advancing randomized retry backoff. Control traffic on an
independent eligible lane remains able to preempt the wait. When the owner
reaches a terminal state, the communication service wakes all resource waiters,
reapplies delivery-profile priority, and preserves FIFO order within one
profile. Newer ordinary traffic must never pass an older resource waiter until
the older request either starts RF or reaches its explicit deadline.

Communication callbacks freeze and enqueue work, then return to their owning
radio or system worker. They must not run a complete route search, reliable
delivery, click range sequence, or protocol collection loop on that callback's
stack. The communication queue owns delivery and route-wait work; an anchor
hands an accepted click to its UWB sequence queue. A route wait has its own work
item and deadline and must not move, reuse, or cancel the ACK timeout of an
already active relay transmission.

A logical request keeps one immutable packet identity and payload across every
communication attempt. The communication service may defer or retry that same
datagram, but a protocol must not allocate a new sequence number merely because
an RF attempt or a short result-wait window ended. A matching application result
is accepted exactly once while the logical transaction remains inside its
absolute deadline; duplicates are harmless, conflicting payloads fail closed,
and a late result cannot advance a later transaction.

Any operation that can commit remote state has an explicit cleanup phase. If
delivery fails, the logical deadline expires, or the caller cancels, cleanup is
required whenever the remote side effect may have occurred. Survey PREPARE is
the canonical two-phase case: every accepted PREPARE ends in the matching START
or an idempotent ABORT, with an anchor-side prepared lease as the final bounded
safety net. That lease remains active through local START_PENDING and is cleared
only when DS-TWR actually begins, so a permanently busy radio cannot strand the
anchor between control acceptance and execution. The coordinator cannot skip a
pair while leaving an anchor prepared indefinitely.

A command-ID-only `SURVEY_ABORT` addressed to the gateway is an idempotent
local recovery command. It terminates an active survey, leaves bounded remote
cleanup owned by the survey worker, emits one successful abort terminal, and
never enters mesh routing as a command to the gateway itself.

The communication service owns whether an RF attempt actually started.
Deferral before RF begins does not consume an opportunity; once RF begins, the
attempt is counted even if it collides or times out. Every accepted datagram
reaches exactly one terminal result: delivered, deadline expired, attempts
exhausted, permanent failure, or explicit cancellation. Retries preserve the
same packet identity so receiver deduplication and durable custody remain
valid, while immutable source/session/sequence-derived jitter prevents
independent nodes from repeating the same synchronized collision.

Protocol-priority command responses retain sixteen RF opportunities within the
caller's absolute deadline. This lets a survey PREPARE or START result outlive
the gateway's required four-copy channel-5 control flood and return during the
following channel-9 receive horizon. A survey pair control with an accepted
reverse route uses a 30-second direct-route deadline plus 15 seconds for each
additional hop, up to 75 seconds at the survey TTL limit; missing or invalid
route-depth evidence keeps the conservative 90-second ceiling. An explicit
host operation budget clips that natural deadline but is never divided among
future phases. The bounded control flood itself still requires exactly four
real RF opportunities.

A targeted gateway command follows the accepted reverse path hop by hop on
channel 5. A relay captures a non-local `MSG_COMMAND` or
`MSG_SURVEY_PAIR_PREPARE` only when it has a current-epoch downlink for that
exact destination, the next hop is a valid downstream peer, and forwarding
cannot loop to the previous hop. It decrements TTL and emits a wake train plus
the bounded four-copy control flood toward that next hop without replacing its
own upstream route. Multiple target-specific downlinks may name the same next
hop, so several origins can share a relay without collapsing their identities
or deleting one another's route state. This control lane may forward while an
unrelated reliable custody record waits for channel 9; the control must not
modify or complete that custody record, while ordinary transit remains busy.

An anchor relay retains reverse routes for all 50 supported descendant
identities by extending its sixteen-entry inline table with anchor-only
storage. The gateway and clicker do not allocate that overflow. These logical
routes do not create extra radio schedules: descendants that name one physical
next hop still share the anchor's single downstream channel-9 reservation.
Route-epoch invalidation and explicit route clearing apply to both inline and
overflow entries, so capacity cannot preserve stale control paths.

For every legacy zero-round or synchronized nonzero-round survey pair,
accepting a PREPARE or START response that advances to another control phase
keeps channel 9 available for one continuous 3000 ms response-ACK settle
interval before the next channel-5 control flood or GO. An exact duplicate
response is ACKed and restarts that interval, because the duplicate proves the
anchor did not receive an earlier ACK.

A synchronized nonzero round instead completes every endpoint's PREPARE and
START delivery before it sends one common future GO. All armed endpoints derive
the same execution instant from that GO's age-compensated execute delay. That
delay reserves one complete worst-case synchronous broadcast-forward horizon
for every RF hop, including randomized relay backoff, the wake train, the relay
burst, and its guard. Local delivery may follow forwarding on the shared mesh
worker without making an otherwise valid GO late, including at a forced-hop
anchor. The responder receive window starts at that local instant and covers
only the bounded local execution skew plus the initiator's DS-TWR timeout,
including the complete RF airtime at the admitted boundary. A frame that
extends beyond the window is a timeout. Route-depth PREPARE, START,
command-result, and report delivery deadlines remain independent transport
bounds and must never enlarge this local UWB receive window.

An exact duplicate START command keeps the existing command-result custody. A
newer START command sequence for the same prepared pair supersedes that custody:
the anchor installs the new START identity and detaches the old delivery handle
under one state lock, then abandons the old handle outside the lock. A terminal
callback releases START only when both its handle and START command identity
still match, so delayed delivery work from attempt N cannot release or abort
attempt N+1.

A survey sample is usable geometry only when its range status is `RANGE_OK`
and its distance is greater than 50 mm. An unusable report remains ACK-eligible
but does not consume the sample index, so the other pair participant may still
supply a usable result before the continuous observation window ends. If both
participants report the same sample, a usable result outranks an unusable one
regardless of reporter priority; reporter priority only selects between results
with the same usability. If both participants prove that every still-missing
sample is unusable, the gateway does not wait out the remaining observation
window: it cleans up both roles and repeats the complete PREPARE/START/range
sequence. An unusable result still present at the end of the continuous window
causes the same rerun even when only one participant reported it. Each pair has
at most two reruns, so persistent bad geometry reaches one explicit pair
failure instead of looping or being counted as success.

Every pair result from a synchronized nonzero round carries that exact round
generation in `TLV_SURVEY_ROUND_ID`. The gateway compares it with the live
batch generation before pair lookup or sample-mask mutation. A missing,
previous, or future generation is stale even when the survey ID, endpoints,
sample count, and sample index are otherwise identical; this prevents delayed
custody from attempt N completing a rerun of the same pair in attempt N+1.
Legacy zero-round results remain valid only in the legacy zero-round
coordinator path. Rejected stale result custody terminates at the sender's
existing bounded pair-result delivery deadline rather than being ACKed as a
current result.

The bounded-control-flood profile always runs four real RF opportunities, even
when an earlier transmission succeeds, because a blind broadcast has no link
ACK that can prove delivery. Successful opportunities repeat after 40 ms;
failed RF opportunities use deterministic jitter derived from the immutable
source, session, and sequence identity so independent senders do not replay the
same collision schedule. A pre-RF busy decision remains a deferred opportunity
and consumes none of the four. The facade freezes accepted envelopes in a
fixed-capacity compact queue and rejects a full queue or an oversized payload
explicitly; it never falls through to a direct-route send or a second private
retry queue.

Ordinary production deliveries retain a 192-byte inline frozen payload so four
queue records do not reserve four maximum-size UWB frames. The gateway also has
one protocol-priority owner for one exact maximum-size control payload. This is
required for the 921-byte 50-anchor assignment table. A second large control
request is rejected until the first reaches a consumed terminal event; it must
not overwrite the first payload or enlarge every ordinary queue record.

Gateway survey start, assignment claim, and assignment-table workflows track
the terminal event for their exact bounded-control handle. Collection may
accept correlated responses after the first real RF copy while the remaining
copies continue independently. Normal prototype success depends on useful
committed responses and the final correlated operation terminal, not on a
complete diagnostic reconstruction of every redundant flood copy. Queue
admission, pre-RF deferral, zero-attempt failure, cancellation, and a blocked
worker still cannot masquerade as delivery. `No anchors` is valid only after at
least one real start attempt and the configured response horizon without any
useful committed response; exhausted or permanent RF failure keeps its explicit
radio terminal reason.

Assignment CLAIM and ACK response custody must remain live through that full
bounded-control blackout and the following collection window. Its absolute
deadline therefore covers every reliable protocol-response attempt at the
maximum retry backoff plus gateway-ACK wait; the generic 12-second command
result deadline is not sufficient for this path.

Assignment responders use the same randomized response policy regardless of
route depth. Route depth affects how long an accepted response remains in
custody and the host watchdog estimate, but it never moves a deeper anchor
ahead of a nearer anchor. The runtime profile supplies the response spread,
retry limit, and per-hop custody allowance within firmware-enforced bounds.

Each assignment phase has one logical bounded-control flood followed by one
response collection window from the runtime profile. A known expected count is
an early-completion hint and an optional host qualification target; it is not a
firmware all-or-nothing quorum. When the collection window closes, the gateway
publishes a table for every valid claim it received. An anchor is part of the
committed usable roster only after its table ACK is accepted, but a missing ACK
does not revoke other anchors' assignments. Unused slots may remain as gaps so
one missing ACK never requires renumbering anchors that already committed.

Phase ordering depends on causal transport ownership instead of guessed quiet
intervals. The next phase may start when every accepted response from the
current phase is either gateway-ACKed or still held by one explicit delivery
owner that can complete independently of the next control flood. Duplicate
responses are ACKed without restarting an operation-wide sleep. Assignment
ends as soon as the configured window closes and the committed partial roster
is known; the host watchdog is calculated from the installed profile and
observed maximum route depth rather than one compiled 235-second minimum.

The host may choose any command budget within the shared safe range that covers
its selected profile. Firmware rejects only a budget that cannot cover the
profile's arithmetic worst case. The GUI displays its computed bound before
sending and advances immediately on the correlated terminal instead of waiting
for the cap.

Valid assignment CLAIM and ACK results are internal protocol controls, so the
gateway does not make their semantic acceptance depend on BLE stream capacity.
They still require packet identity, bounds, duplicate handling, state commit,
and transport ACK finalization. Host-visible assignment mapping reports both
the committed anchors and the claimed-but-unacknowledged anchors so partial
success is explicit.

An anchor owns one exact discovery response per assignment operation epoch and
phase. Repeated flood envelopes for that same epoch and phase retain the
existing response delay, frozen payload, and communication handle even when
their transport packet identity differs; only a different phase or operation
epoch may supersede that response under the assignment state machine.

Discovery-slot assignment distinguishes the operation epoch from the table
generation. The generation is the common nonzero command sequence, flood epoch,
and packet session identity; anchors persist it with a fingerprint of the full
sorted table. Within one operation epoch, only a newer generation may replace
the committed table, an equal generation is idempotent only when the fingerprint
matches, and an older or conflicting table is rejected. An omitted anchor
persists an unprovisioned tombstone with the same identity, so reset cannot make
an old smaller table authoritative again. An exact table replay may re-arm a
missing ACK after the earlier response delivery has terminated. When collection
closes, gateway mapping telemetry identifies committed ACKed entries separately
from claimed-but-unacknowledged entries. Before emitting a successful host
result, the gateway persists only the committed sorted IDs as its registered
membership roster under a deterministic nonzero 16-bit epoch derived from the
assignment epoch. Admission or persistence failure for one entry excludes that
entry and reports it; it does not revoke already committed entries.

The service can be quiesced, paused, resumed, or stopped without letting a
protocol steal transport state. A pause has one generation-checked owner and a
bounded lease. Absolute protocol deadlines continue while paused, while
relative retry timers resume from their remaining delay. If the radio owner
does not quiesce after the lease expires, bounded recovery aborts the owner and
then deliberately allows the watchdog to reset the node rather than leaving
communication paused forever.

The current mesh report runtime remains the compatibility backend while this
boundary is being extracted. New or migrated protocol code must enter through
the node-communication facade; it must not add another direct queue, route,
flood, retry, or radio-ownership path. Compatibility aliases do not change the
wire protocol or imply that the gateway owns a connection: gateway delivery is
still unscheduled channel 9 reception followed by the required ACK behavior.

## Roles

### Clicker

The clicker is the user-facing, battery-powered device. Its primary job is to
initiate a click or ranging event, wake nearby anchors on channel 5, complete
ranging, and report the result path through the mesh.

A clicker wake train represents interactive work. It has priority over relay
traffic. Anchors that are currently relaying on channel 9 should pause or defer
that relay work long enough to handle the click, then return to the previous
channel 9 rhythm if the connection is still valid.

### Anchor

The anchor is a fixed relay and ranging responder. Outside a mesh connection it
uses low-duty channel 5 receive windows so it can hear clickers or route setup
requests while saving power.

Inside a mesh connection the anchor is not a sleepy scanner. It participates in
the channel 9 relay rhythm and keeps recurring channel 5 receive windows
available for clicks and control. It should treat channel 5 as the preemption
and control lane, and channel 9 as the scheduled payload lane.

An anchor has only one upstream route slot and one downstream route slot.
Upstream means the route direction toward the gateway. Downstream means the
route direction back toward the original packet producer. While both slots are
active, the anchor cannot reserve an additional channel 9 rhythm for an
unrelated route. In that state, answering a new unrelated route request is
incorrect: it would advertise a path that the anchor has no reserved channel 9
window to service.

When an anchor chooses its next gateway-bound payload, locally produced click,
ranging, or command-result work comes before transit payload. The transit owner
keeps its accepted packet and retries after the local item reaches a safe
handoff boundary. Capacity pressure is reported as busy rather than silently
discarding transit custody. This is a simple local-first queue rule; neither
route depth nor origin depth changes the priority.

An anchor should not react to route-request wake trains as if they were clicks.
If the anchor already has an active channel 9 connection, a route-request wake
train should not hold up the next required channel 9 slot, and the anchor
should not answer it. A connected anchor may still handle route replies or
gateway-originated control that belong to the existing route rhythm; those are
not new route requests.

### Gateway

The gateway is the sink for mesh traffic and the bridge to the host side. It
is primarily a continuous channel 9 receiver. It receives channel 9 payloads,
sends gateway-level ACKs, and may originate broad control or discovery traffic.
It does not reserve a normal channel 9 upstream/downstream connection like an
anchor. Normal route-request broadcasts and rebroadcasts are not a gateway
channel 5 receive path; nodes that want to discover whether the gateway is
directly reachable use the direct channel 9 gateway probe before they broadcast
or rebroadcast route requests.

Gateway commands and broad reachability messages may use blind channel 5 flood
behavior when there is no established connection or when the goal is explicitly
to reach unknown listeners.

The gateway may also send a "Here I Am" route-refresh packet when requested by
the host over BLE. This is a gateway-originated reachability announcement, not
a normal route request from a transmitter.

### Transmitter

The transmitter is a test and diagnostic role. It creates gateway-bound mesh
traffic so the anchor and gateway relay behavior can be tested without a full
clicker/ranging session.

The transmitter has two route-acquisition modes:

- Direct-or-relayed mode: the transmitter may contact the gateway directly if
  the gateway is reachable, or use an anchor as a relay when direct contact is
  not available.
- Forced-relay mode: the transmitter requires at least one anchor hop before
  the gateway. This mode exists to test anchor relay behavior even when the
  gateway is physically nearby.

The transmitter should follow the same connected routing rules as a normal
packet producer: acquire a route when no valid connection exists, use channel 9
while connected, retry packet delivery on channel 9 when ACKs are missed, and
avoid re-sending route wake trains while the connection remains alive.

## Outside A Connection

When no channel 9 connection exists, anchors should be in low-duty channel 5
receive mode. This state is allowed to use DWM3000 sleep between receive
windows, provided wake and retune timing remains reliable.

Route acquisition should proceed as follows:

1. The packet producer has gateway-bound traffic and no valid channel 9 path.
2. Before broadcasting a route request, it sends a short direct channel 9
   gateway probe and listens for the gateway ACK. If this succeeds in
   direct-or-relayed mode, the producer may install the direct gateway route and
   skip the route request. If this succeeds in forced-relay mode, or during the
   parent hold-down window after repeated direct-to-gateway payload batch ACK
   failures, the producer records that direct gateway contact was possible, but
   it still continues route acquisition because the direct probe is not
   sufficient route evidence for that context.
3. If the direct probe does not satisfy route acquisition, it sends a
   route-request wake train on channel 5. The wake train must be
   typed as route setup, not click/ranging.
4. After the wake train, it sends a route-request control packet. Under normal
   conditions this is the route-discovery broadcast described below, not the
   blind gateway-command flood path.
5. An idle anchor receives the route request, records the reverse route toward
   the origin, and decides whether it can relay toward the requested target.
6. If the idle anchor already has a usable route to the requested target, it
   answers the route request directly with a route reply and does not
   rebroadcast that route request. Split horizon is mandatory: a route whose
   selected next hop is the request's physical previous hop is not usable for
   this response, because advertising it back to that child would create a
   two-node upstream loop after either side enters hold-down.
7. If the idle anchor does not have a usable route to the requested target and
   the route request TTL still allows another hop, it may rebroadcast the route
   request after the route-request forwarding delay. Because the original route
   request may have reached multiple anchors at nearly the same time, the
   forwarding delay must include random jitter before any consequent direct
   gateway probe or rebroadcast. The jitter window must be large enough, given
   the direct-probe airtime, route-request airtime, and retune/guard time, that
   collisions are unlikely for the expected fanout, with eight simultaneous
   receivers as the normal design target. After that jitter, the anchor first
   runs the short direct channel 9 gateway probe. If the probe gives that anchor
   a usable gateway route, the anchor answers the upstream request with a route
   reply instead of rebroadcasting. If the probe does not give a usable route,
   the rebroadcast proceeds.
8. A rebroadcasting anchor must then open a subsequent channel 5 route-reply
   listening window so it can catch a reply from a downstream responder and
   forward that reply back along the reverse path.
9. Click/ranging traffic keeps priority during the forwarding delay and the
   subsequent route-reply listening window. If the anchor sees a click/ranging
   wake train during this process, it may abandon the route-request propagation
   attempt and service the click/ranging exchange instead.
10. Each responder sends its route reply in a responder-selected reply slot
   inside the immediate upstream node's channel 5 route-reply listening window.
   Across all hops, the reply chain must still fit inside the original
   origin's advertised reply budget. The slots must be large enough for one
   complete route-reply packet plus the required guard time, and each responder
   must use random jitter so multiple responders are unlikely to collide. The
   reply starts with the network-wide maximum hop limit, and each reverse-path
   forward decrements it, so the packet still has a nonzero TTL when it reaches
   an origin at the deepest supported request depth.
11. A downstream anchor with a usable route, including a route learned from a
   successful direct channel 9 gateway probe, sends a route reply back along
   the reverse path. The gateway itself remains a channel 9 receiver for normal
   route acquisition; it does not send normal channel 5 route replies. A direct
   gateway candidate is therefore immediately usable without negotiated
   channel 9 timing, and route-ready handling must never send an event proposal
   to the gateway. Event negotiation is only for an anchor next hop.
12. Each route reply hop is ACKed on channel 5 so the sender knows the reply was
    received.
13. The packet producer ACKs the final route reply on channel 5.
14. If the route reply accepts and echoes the origin's optional channel 9
    timing, both immediate peers install that exact rhythm and the producer
    uses it for the waiting uplink. A second timing proposal is allowed only
    when the accepted rhythm is absent or no longer usable; route-ready
    handling must not replace a fresh accepted rhythm before its first data
    opportunity.
15. If another receive owner processes that route reply and publishes the
    usable route while the origin's channel 5 reply listener is still active,
    that listener must observe route readiness through a bounded receive slice
    and release the radio as a successful capture. It must not hold channel 5
    until the original reply deadline and let the accepted channel 9 rhythm
    become stale.
16. For gateway control floods, only the first accepted RF copy of one exact
    route epoch, message type, session, and sequence may install or replace the
    receiver's reverse gateway hint. Later direct or relayed echoes of that
    same transport identity may still be forwarded and passed to the protocol
    owner, but they cannot create an alternate upstream candidate or remove a
    newer direct route. The physical previous hop is deliberately excluded
    from this identity: changing only the relay path is what makes a flood echo
    dangerous. A new control identity can provide fresh route evidence, so
    real topology changes remain discoverable.

During channel 9 event negotiation, an ACCEPT is valid only while the matching
proposal to that immediate peer is active, the physical previous hop and packet
source are that peer, the destination is the proposer, and the complete timing
shape matches the proposal. New peers should echo the proposal session and
sequence in ACCEPT. For compatibility with the last stable connected-routing
release, a peer may instead use a fresh nonzero ACCEPT packet identity; the
active-peer and exact-timing checks still apply, so an unrelated or stale
negotiation cannot install timing.

The successful PROPOSE RF transmission defines the channel 9 phase. The
proposer retains the phase reanchored to that actual transmission, while the
responder retains the same proposal phase decoded in its own clock domain. The
responder installs that retained phase only after its ACCEPT physically
transmits, and the proposer installs its own retained proposal phase after the
matching ACCEPT arrives. Queue latency, ACCEPT retry delay, an exact duplicate
ACCEPT, or a cached ACCEPT replay must not move an established phase; those
responses confirm the proposal rather than proposing a new schedule.

The PROPOSE session is also the wire-visible ownership identity for the whole
connection operation. UPDATE and END must reuse that session; an unrelated
UPDATE or END session cannot replace timing or tear down a newer operation.
Each endpoint retains the immediate peer, operation session, and separate local
and remote control-sequence histories because the peers' packet sequence
domains are independent. An exact duplicate UPDATE or END is inert, the same
sequence with different type or payload is a conflict, and an older sequence is
stale. The responder classifies a PROPOSE against the active owner and a bounded
history of at least eight retired sessions before reserving an ACCEPT or
replacing timing. A retained session is stale, and a proposal with an older
control sequence cannot displace a live owner; a genuinely newer proposal may
start a replacement connection. New local proposals use a fresh boot-seeded
operation identity that does not reuse any retained owner slot, so delayed work
from operation N cannot mutate operation N+1 even when both start in the same
uptime tick. A malformed UPDATE does not consume its sequence. A valid UPDATE
carries the sender's even/odd transmit phase, and the receiver installs the
complementary phase for the same event counter.

Every EVENT_PROPOSE also carries a nonzero 64-bit boot incarnation chosen once
per sender boot. Remote control-sequence ordering is scoped by that incarnation:
the first proposal from a new, non-retired incarnation may restart at sequence
one after a peer reset, while a proposal using the active incarnation must still
be newer than the retained sequence. Each endpoint retains the incarnations of
retired peer sessions alongside the retired session history; a nonce in that
history is stale when it is no longer the active peer incarnation, so a delayed
proposal from before a reset remains stale even when it reuses a low sequence or
names a fresh session. Zero is invalid on the wire, and an incarnation is never
reused within the sender's boot lifetime. A missing, zero, or malformed nonce
is rejected before the receiver mutates its event owner or timing state; this
fail-closed rule applies to EVENT_PROPOSE only, while UPDATE and END retain
their independent sequence domains.

In direct-or-relayed transmitter mode, a direct gateway route may satisfy route
acquisition. In forced-relay transmitter mode, a direct gateway route must not
satisfy route acquisition; the route must include at least one anchor hop.

If route acquisition fails, route-request retries may use backoff. Every retry
again starts with the short direct channel 9 gateway probe before any channel 5
route-request wake train or broadcast. The retry is for establishing a
connection, not for every later packet failure.

### Route-Request Propagation And Timing

Route requests are controlled route-discovery broadcasts. They are not blind
gateway-command floods, but they are allowed to propagate through otherwise
idle anchors.

Each route request carries enough information for receivers and relays to
preserve the discovery identity and timing:

- The origin, requested target, route epoch, flood/discovery identity, slot
  seed, hop count, and path quality.
- One mandatory bounded route-node path containing at most nine unicast node
  IDs. A request lists nodes from its origin through its current transmitter;
  a route reply or gateway advertisement lists nodes from its target or gateway
  through its current transmitter. Its node count is exactly hop count plus
  one, and the final ID must be the physical previous hop at the receiver.
- Optional channel 9 timing proposed by the origin for the downstream route.
- A reply-window delay that tells responders how long remains before the origin
  is ready to listen for route replies.
- Optional flags, including forced-relay behavior when a direct gateway answer
  must be ignored.

Every route-node path is exact rather than a hop-depth approximation. A
receiver rejects a missing path, malformed length, repeated ID, unexpected
root or tail, or a path that already contains its own ID. A forwarder appends
its local ID before incrementing hop count and refuses to forward when the
nine-node bound is exhausted. An anchor retains the exact path for each of its
three upstream candidates in role-specific sidecar storage, so the gateway's
tightly bounded route-candidate RAM does not grow. When answering a gateway
route request, the selected upstream path must be disjoint from the complete
request path. This ancestry check prevents cycles involving three or more
anchors; the immediate-next-hop split-horizon check remains mandatory because
it rejects the two-node case directly.

Route-request TTL broadens across repeated discovery attempts:

- Attempt 1 uses TTL 1, targeting only directly neighboring anchors.
- Attempt 2 uses TTL 2.
- Attempt 3 uses TTL 4.
- Attempt 4 and later attempts use TTL 6.
- Normal route requests must not exceed TTL 6. Any larger emergency route
  request TTL requires explicit user permission and an update to this contract.
- After attempt 4, later attempts keep using TTL 6 while the backoff continues
  to increase.

Route-control TTL is admitted before any route, ancestry, timing, duplicate,
or reply state changes. Every accepted control packet has nonzero remaining
TTL. For a route request, remaining TTL plus the carried hop count must recover
one of the defined origin waves 1, 2, 4, or 6. A gateway advertisement must
recover the network-wide origin TTL of 8 from that same sum. A route reply may
carry remaining TTL 1 through 8, and a route-reply ACK is a one-hop control with
TTL exactly 1. A receiver rejects any other combination instead of treating an
over-depth or zero-TTL frame as route evidence.

The generic extended envelope is not the route-control wire budget. Receivers
and public builders cap route-request payloads at 139 bytes, route replies at
164 bytes, gateway-route advertisements at 138 bytes, and route-reply ACKs at
34 bytes. The request bound reflects the deepest nonzero-TTL packet in the
six-hop discovery wave, the advertisement bound reflects the deepest
nonzero-TTL packet in the eight-hop gateway flood, and the reply bound includes
the maximum nine-node ancestry. A longer padded frame is malformed even when
it still fits the generic packet buffer.

After each route request attempt, the next allowed request time uses jittered
exponential backoff:

- Attempt 1 schedules the next request after a 1000 ms base plus
  percentage-based jitter.
- Attempt 2 uses a 2000 ms base plus percentage-based jitter.
- Attempt 3 uses a 4000 ms base plus percentage-based jitter.
- Attempt 4 uses an 8000 ms base plus percentage-based jitter.
- Later attempts continue doubling the base, but the base is capped at
  60000 ms. The total delay is the capped base plus percentage-based jitter.
- Percentage-based jitter means the jitter range is derived from the selected
  base delay, not from a fixed millisecond window.
- Percentage-based jitter must include fresh randomness per attempt. Stable
  inputs such as node identity or route identity may bias the jitter window, but
  they must not be the only entropy source because two anchors that choose the
  same deterministic offset would otherwise collide repeatedly.
- The backoff is for route formation only. It is not used for every missed ACK
  during an otherwise valid channel 9 connection.

An idle anchor that receives a route request should:

1. Ignore it if it is malformed, stale, duplicated within the route-discovery
   deduplication window, or has no TTL left for the needed action.
2. Ignore it if the anchor is already connected or otherwise unable to reserve
   the route capacity that it would advertise.
3. Record the reverse path toward the origin when the request is accepted.
4. If it can reach the requested target, send a route reply on channel 5 after
   the requested reply-window delay plus responder-slot jitter. The chosen slot
   must be inside the immediate upstream node's route-reply listening window,
   large enough for one complete route-reply packet and guard time, and still
   fit within the original origin's advertised reply budget. A responding
   anchor does not rebroadcast the same route request.
5. If it cannot reach the target and TTL allows another hop, rebroadcast the
   same route-discovery identity with TTL decremented, hop count incremented,
   updated path quality, and the reply-window timing adjusted.
6. After rebroadcasting, open a subsequent channel 5 route-reply listening
   window to catch any downstream reply that must be forwarded back to the
   origin.
7. If click/ranging traffic appears during the forwarding delay or subsequent
   route-reply listening window, abandon or defer this route propagation and
   service the click/ranging exchange first.

Route-request rebroadcast delay must spread relays out enough to avoid a
single collision burst. The forwarding delay should be a base forwarding wave
with random jitter. Stable inputs such as the route-discovery slot seed, relay
identity, and hop count may bias the delay, but they must not be the only
selector because repeated deterministic collisions can otherwise persist across
attempts. The anchor must keep listening during the base-plus-random delay so
it can hear better route candidates and avoid rebroadcasting a worse path when
a better one is found first.

The route-reply listening window at the origin must widen as route-request TTL
widens. Larger TTL means more possible rebroadcast hops, more forwarding delay,
and a longer worst-case route-reply return time. The origin must keep the reply
window open long enough to cover:

- The route-request wake train and post-wake route-request control packet.
- The worst-case route-request forwarding delay for the selected TTL.
- Responder-side route-reply slot jitter at the deepest responder level.
- The route-reply transmission over the reverse path.
- Per-hop route-reply ACK retries and guards.
- Retune, scheduling, and channel 5 guard time.

Responder-side route-reply slots must include random jitter. Stable
route-discovery inputs such as discovery identity, slot seed, responder
identity, and hop count may be used as inputs, but they must not be the sole
slot selector. The selected slot must remain inside the immediate upstream
node's route-reply listening window, and the full reply chain must remain
inside the original origin's advertised reply budget. A responder must not
answer before the reply-window delay has elapsed, and a delayed responder must
drop the reply rather than transmit outside the protected reply window.

When a route request carries a reply-window delay, every sender or rebroadcaster
must treat that value as remaining time until the reply window starts. Repeated
or delayed route-request transmissions must update the remaining delay so that
responders do not answer before the origin is listening.

## During A Connection

In the connected state, the radio schedule should be dominated by recurring
channel 9 and channel 5 windows with no meaningful sleep gap. The intended
shape is:

1. Channel 9 receive or transmit window for scheduled relay traffic.
2. Channel 5 receive window for clicks, preemption, and control.
3. Channel 9 receive or transmit window for the next scheduled relay turn.
4. Channel 5 receive window again.

Channel 5 receive windows are not optional background scans. They are part of
the connected schedule. Channel 9 should be planned around them.

If a click/ranging wake train is heard during a channel 5 window:

1. The anchor marks relay work as interrupted, not disconnected.
2. The anchor handles the click/ranging exchange.
3. Any packet retry and ACK timers account for the click-handling duration.
4. The anchor returns to the existing channel 9 rhythm when the connection is
   still valid.

If a route-request wake train is heard during a connected channel 5 window:

1. The anchor recognizes it as route setup, not click/ranging.
2. The anchor does not allow it to preempt an imminent channel 9 slot.
3. The anchor ignores the route request while it remains connected.
4. Downstream route maintenance while connected is handled by route replies or
   gateway-originated control in the existing rhythm, not by accepting a new
   route request.

## Gateway-Originated Commands And Route Refresh

Gateway-originated commands and user-requested "Here I Am" route refresh are
the highest-priority mesh control traffic. They must move before local-origin
click report delivery, transit payload relay, packet retries, route
maintenance, and background maintenance. Click/ranging preemption remains the
interactive channel 5 path, but queued gateway-originated control is the first
mesh work to service at the earliest safe radio boundary. This broad priority
rule does not require corrupting an already-started timing-critical ranging
exchange; if such an exchange has a defined safe abort point, the gateway
control may be serviced there, otherwise it is serviced immediately afterward.
This priority is assigned by gateway origin, not by a command-ID allowlist, and
therefore includes anchor discovery and future gateway commands.

The gateway's continuous channel 9 receiver must release a pending control
handoff even when the handoff becomes ready between receive windows. A receive
start rejected because that handoff now owns the radio is itself a safe
boundary: the gateway schedules the pending control there and leaves receive
rearming to the control completion path. Retrying the receive window while the
handoff gate remains closed would strand both the control and all later host
commands.

The same continuous receiver is a logical 30-second receive horizon, not a
30-second system-workqueue lease. Each driver receive invocation is capped at a
bounded work slice and yields after a bounded run of immediate recoverable
errors. It then rearms the same logical service, allowing survey deadlines,
control transitions, BLE custody, watchdog progress, and other delayed work to
run without opening a material Channel 9 listening gap.

Gateway commands propagate away from the gateway until every reachable anchor
has received them:

1. The host sends a command to the gateway over BLE.
2. The gateway marks the command as priority control traffic.
3. The gateway sends the command on channel 5 using the gateway-command wake
   train and flood path. It does not propagate gateway commands through channel
   9.
4. The channel 5 flood carries randomized forwarding controls so anchors do
   not all retransmit at once. Each receiving anchor waits a base delay plus a
   randomized delay before forwarding.
5. During that base-plus-randomized delay, the anchor keeps listening on
   channel 5 for other copies of the same gateway-originated message. This
   allows it to compare candidate routes and choose the best route, not merely
   the first route it heard.
6. The gateway command should be resent for a small number of attempts so
   anchors can still receive it after an initial collision or missed wake.
   Every attempt includes its own gateway-command wake train; a retry must not
   depend on an anchor or relay having received an earlier attempt's wake.
7. Each anchor that receives the command validates that it is new and relevant.
8. The receiving anchor executes the command if it is addressed to that anchor
   or is a broadcast command.
9. The receiving anchor forwards the command onward on channel 5 using a new
   wake train and the same randomized-delay forwarding rules.
10. Each downstream anchor repeats the same process, so the command bubbles
   outward from the gateway one channel 5 hop at a time.
11. Anchors that detect a garbled, corrupted, or colliding channel 5 command
    frame should continue listening for a while because another retry or
    delayed forwarding attempt is likely to follow.
12. Command responses return toward the gateway through the normal response
    path, and the gateway resumes or remains in channel 9 receive while waiting
    for those responses.

The relay deduplication identity for a broadcast `MSG_COMMAND` is its mandatory
nonzero `TLV_COMMAND_SEQ`, together with source and destination. The mesh
envelope session may intentionally name a longer operation, such as an anchor
survey containing several GO rounds. Exact retries of one command sequence are
inert, while a later command sequence in that same operation session must still
be delivered and forwarded once.

Any non-route-solicitation wake train that announces a following channel 5
control frame sets `FLAG_CONTROL_FOLLOWUP`. Connected anchors use that bit to
hand the complete follow-up exchange to the control listener instead of
continuing a short gap scan. Route-solicitation wakes deliberately omit it so a
maintenance request cannot steal an imminent channel 9 slot; event-timing
proposals include it so the receiver can return the matching ACCEPT.
After the final standard-PHR control wake claim, the sender leaves the
configured inter-PHY turnaround before transmitting the extended-PHR control
frame. This bound lets every listening anchor restore the control PHY, including
an off-target anchor already inside another control exchange. Once a non-click
control wake has transferred ownership to that bounded listener, repeated
standard-PHR claims from the same train do not trigger click probes or pull the
receiver away from the announced extended-PHR frame; ordinary route listeners
retain click preemption.

Survey reachability uses the same control-followup receive eligibility as
anchor enumeration. After the `SURVEY_DISCOVERY_START` flood, each participating
anchor repeats a simple randomized announce/listen round count supplied by the
runtime profile. In each round it chooses a randomized transmit slot and listens
for the rest of the round. A pre-RF deferral retries inside the remaining survey
window; it does not invalidate neighbors already heard. The gateway collection
window covers the configured rounds, report spreading, route-depth delivery
estimate, and host-selected grace interval.

Each anchor deduplicates peer announcements by stable anchor identity and sends
the useful set it heard when its discovery window ends. A report is useful even
when some planned transmissions were deferred or collided. One directed
observation is enough to create a reachable pair because signal strength is
carried with that observation; reciprocal observation may improve diagnostics
but is not an admission gate. `No anchors` is valid only when no useful report
or neighbor observation was committed before the configured deadline.

Each report may retain up to twelve heard peers. The gateway builds pair
candidates from every directed reachability edge, preferring stronger edges
when the degree cap requires a choice. A disconnected or partial graph produces
all reachable pair work and reports isolated anchors separately; it does not
turn useful components into total survey failure.

Pair roles use deterministic endpoint ordering. Route depth is used to estimate
control and result deadlines, not to grant deeper anchors priority or to choose
which endpoint ranges first.

A pair's neighborhood is the union of both endpoints and every peer either
endpoint reports. Pairs in the same ranging round must have disjoint
neighborhoods, including no shared endpoint or third peer. When the complete
map is unavailable, a hop-depth difference of at least two may prove separation
conservatively; unknown or adjacent depths serialize. The gateway arms every
pair in a round independently, then sends one common future GO time. Each pair
retains independent result, cleanup, retry, and deadline custody, so one failed
pair cannot cancel successful pairs in the same round. An anchor rejects a
second simultaneous reservation, making any neighborhood approximation affect
throughput rather than correctness.

Each anchor owns its encoded `SURVEY_DISCOVERY_REPORT` until the gateway has
explicitly acknowledged that exact packet identity. Before the first transport
attempt, the anchor transactionally persists the report bytes, survey identity,
delivery generation, state, and bounded attempt budget. Route waits, gateway
command preemption, tracked-transmit retries, and resets must resume that same
record; admission to an in-memory report queue is not delivery. The durable
record is cleared in two phases only after the gateway ACK is committed. While
one report is pending, a later survey start is explicitly rejected and cannot
overwrite the report whose custody is already owned.

Reachability-report custody follows the selected upstream route depth: direct,
two-hop, three-hop, and four-hop reports retain 5000, 9000, 13000, and 17000 ms
respectively after their eligible transmit time. Missing or invalid route depth
uses the conservative four-hop value. The gateway collection tail must cover
that maximum at build time, so a report cannot expire locally after a late
multihop RF opportunity while the gateway has already stopped accepting it.

If the first journal write is temporarily unavailable, the exact encoded report
and peer list remain the active generation's staging candidate in RAM. The
anchor retries that same candidate and does not release the survey generation
until durable custody succeeds; it must not replace the peer list with a newly
encoded empty report merely because persistence was busy.

Each real report attempt has a persisted token. The attempt budget is consumed
before RF can start, so a reset after transmission cannot grant a free retry;
a pre-RF refusal refunds only the matching token. Late ACK, preemption, and
post-send callbacks may update only that token and exact packet identity. The
journal owns route-discovery retry itself and never occupies or overwrites the
generic single route-wait packet slot. Old-schema or corrupt journal records are
quarantined and cleared with an observable diagnostic; failure to clear the bad
record must not stall anchor startup.

When the gateway semantically accepts a current-survey discovery report, that
report is also fresh reverse-path evidence for the report's anchor. The gateway
retains the report anchor, the immediate previous hop, and the observed link
quality in the bounded 50-report survey context. It may install that hint as a
current-epoch downlink immediately before each survey prepare or start command.
The hint is route evidence only: it does not create a gateway connection or a
channel 9 timing reservation. Reinstalling the one target needed by the current
command lets a 50-anchor survey work without requiring all reverse routes to fit
simultaneously in the smaller general downlink table.

Reverse evidence is retained only after all of these checks succeed: the frame
arrived on channel 9 for local gateway delivery, requested a gateway ACK, its
survey and packet identities agree, its survey is the currently active survey,
its report payload is valid, and its immediate previous hop and link quality are
valid. The first accepted report for an anchor owns that anchor's peer
relationships and reverse hint for the survey. Transport duplicates and later
valid packets for the same anchor are acknowledged and counted, but they do not
replace or refresh that first accepted state. Stale, malformed, wrong-channel,
or rejected reports do not create a hint. The installed downlink uses the
gateway's current route epoch rather than an epoch supplied by the report. A
direct report has the anchor itself as previous hop; a relayed report may use a
different immediate next hop because semantic acceptance binds the hint to the
originating anchor.
Automatic pair orchestration and host-issued manual pair control share this
same rule: before every prepare or start, the gateway reinstalls the retained
hint and submits the command through the node-communication bounded channel-5
wake-and-flood profile. A manual command must not fall back to one tracked
channel-9 send merely because its target is explicit.
The present mesh envelope uses network identity and CRC checks, not keyed STS or
a packet MAC, so this rule prevents stale and accidental identity poisoning but
must not be described as hostile-RF authentication.

Collection start freezes the exact expected-node roster, with a supported
maximum of 50 anchors, as part of the durable collection state. The gateway
rejects a result from a node outside that roster and accepts at most one payload
identity per expected node. An exact retry may re-arm a missed collection EACK
but cannot create a second host record or replace the accepted result. A closed
collection with unfinished EACK custody blocks a new collection so the old
senders cannot be orphaned by command reuse.

Every gateway collection EACK has an explicit nonzero 16-bit packet sequence.
That sequence appears both in the mesh header and in the mandatory encoded EACK
payload field, and a receiver accepts the EACK only when those values match the
decoded gateway and collection-command identity. A transport retry keeps the
same sequence and bytes. A later logical EACK update advances the independent
packet sequence, wrapping from 65535 to 1; the 8-bit `retry_round` saturates and
is only a backoff hint, never the EACK deduplication identity.

Before any EACK RF attempt, the gateway freezes and durably stores the exact
mesh header and encoded payload. Channel 9 attempts, channel 5 recovery, and a
retry resumed after reset all reuse those bytes. A persistence refusal is a
pre-RF deferral, so it cannot be reported as a successful EACK round or consume
the frozen custody. Results accepted while an older EACK is in flight belong to
the following logical update and cannot mutate that snapshot.

Channel 5 EACK recovery completes only after four actual EACK frames have
started on air for the frozen packet identity. A busy radio, protocol preemption,
quiet-channel refusal, or other pre-RF deferral preserves the current
opportunity and resumes it with randomized exponential scheduling; it cannot
advance or release EACK custody. The EACK builder therefore uses one resumable
four-frame flood and no outer flood repetition, preventing both partial-burst
success and accidental multiplication to sixteen frames.

After the frozen EACK is delivered, the gateway advances the collection round
only as a transaction: it first durably stores the next collection identity and
state, then clears the old EACK custody. A reset or persistence failure before
that commit must therefore resume the old exact EACK safely. Result mutation,
EACK preparation and retry, and collection persistence all share the serialized
mesh-route queue owner so separate workqueues cannot race those state changes.
The persisted `eack_pending` bit distinguishes a closed collection whose final
EACK is still owed from one whose final EACK was committed: collection start and
every accepted result set it, and only the durable closed-round commit clears
it. Boot creates a fresh EACK only when that bit is set, while a later duplicate
result re-arms it so a node that missed the final broadcast can still finish.

Gateway commands must not be blocked behind ordinary packet retries. If a
local-origin payload, a transit payload, a retry, and a gateway command all need
radio time, the gateway command's channel 5 propagation is serviced first. The
delayed local-origin payload, transit payload, or retry remains queued and is
attempted later as long as the relevant connection remains valid.

The gateway may send a "Here I Am" packet when requested by the host over BLE.
This is priority gateway-originated control traffic. The purpose of this packet
is route refreshment and reachability repair:

1. The host requests a route refresh over BLE.
2. The gateway emits a priority "Here I Am" announcement on channel 5.
3. Anchors receive it through the gateway-command wake train and channel 5
   flood path.
4. Each receiver uses the same base-plus-randomized forwarding delay and retry
   behavior used for other gateway-originated commands. During that delay it
   keeps listening for competing copies so it can refresh to the best observed
   gateway route.
5. Each anchor that receives the announcement refreshes its knowledge of the
   gateway path and the current route epoch.
6. Anchors forward the announcement onward on channel 5 so more distant anchors
   can refresh their gateway path.
7. The refresh does not create extra route slots on anchors that are already at
   their one-upstream, one-downstream route capacity.
8. The refresh does not replace click/ranging preemption rules. If a click is
   being handled, the anchor returns to the connected rhythm afterward and then
   services the pending gateway-originated control work.

The gateway tracks a response-priority refresh deadline with a separate armed
flag. A wrapped `uint32_t` deadline of zero remains a valid future deadline;
clearing response priority drops the armed flag instead of assigning a special
deadline value.

## Packet Delivery And ACKs

Packet delivery should preserve connection state.

1. A producer queues one or more packets for the gateway.
2. If a channel 9 connection is valid, the producer sends eligible packets in
   the next suitable channel 9 transmit window.
3. If the next hop is another anchor, the sender may send multiple packets in
   the same channel 9 transmit window when the slot budget allows it. The
   receiving anchor queues a hop-level ACK for the packets it received and sends
   that ACK in its next channel 9 transmit window.
4. A hop-level ACK can acknowledge one packet or multiple packets. Every packet
   listed in that hop ACK is considered received by the next hop. Packets not
   listed remain pending for channel 9 retry. A packet is not removed from the
   sender's pending set merely because it was transmitted; hop-level custody is
   transferred only when the hop ACK lists that packet.
5. The packet continues toward the gateway.
6. Each relay hop bubbles hop-level ACKs back toward the original transmitter
   one hop at a time.
7. If the next hop is the gateway, the sender treats the channel 9 transmit
   opportunity as a bounded batch. Before sending, it calculates how many frames
   fit in the current channel 9 slot after reserving time for the gateway batch
   ACK receive window, retune/turnaround time, and guards.
8. The sender assigns every packet in that direct-to-gateway slot the same batch
   identity. The sender also marks which packet is the final packet in the
   batch. It sends only as many packets as fit before the reserved ACK-listen
   budget starts.
9. The gateway does not ACK every packet immediately. It waits until it receives
   the packet marked as final for that batch, then sends one gateway batch ACK.
   That ACK lists all packets from the batch that the gateway accepted.
10. The direct sender switches from channel 9 transmit to channel 9 receive for
   the reserved reply window and listens for that gateway batch ACK before
   attempting more channel 9 transmissions.
11. If a gateway batch ACK is received, every listed packet is considered
    gateway-accepted. Packets from the same batch that are not listed remain
    pending and are retried in a later channel 9 transmit opportunity.
12. If the final packet or the batch ACK is lost, the sender times out the batch
    ACK wait, keeps the unacknowledged packets pending, and retries them on
    channel 9 while the connection remains alive. It must not start a new wake
    train just because the batch ACK was missed.
13. If a gateway ACK and hop-level ACK compete for the same transmit window while
    ACKs are bubbling through anchors, the gateway ACK is sent first.
14. The original producer treats a gateway ACK or gateway batch ACK as final
    delivery for every packet it explicitly acknowledges.

Hop ACK is a progress signal. When a hop ACK is received, the gateway ACK timer
should be extended or reset because the packet is still moving through a known
path.

If a hop ACK is missed, retry the same packet on channel 9 while the connection
is alive. If a gateway ACK or gateway batch ACK is missed but hop ACKs, partial
batch progress, or other progress continue, keep the connection and retry on
channel 9. Do not restart the wake train or route acquisition unless the
connection is declared dead.

The communication service must preserve the scheduler's next channel 9 prepare
boundary. If a frozen delivery is ready before its transmit event, the service
re-arms that same delivery at the prepare boundary and consumes no RF attempt;
randomized exponential backoff applies to contention and failed attempts, not
to polling for a deterministic connection slot. Dropping this boundary can make
every service poll arrive too late for an otherwise healthy connection.
While any reliable local delivery is pending, its next local transmit event is
a required channel 9 activity on the selected next-hop connection even before
RF or ACK custody begins. Unrelated peer timings remain available. Connected
channel 5 gap scans and the low-duty scanner must stop at the selected event's
prepare boundary; they may use only the earlier bounded gap.

Likewise, an anchor's low-duty channel 5 scanner may defer for an approaching
channel 9 event only after re-arming the channel 9 worker at that event's exact
prepare boundary. Deferral without an armed owner can silently skip every ACK
transmit window until unrelated work happens to restart the worker.

Channel 5 reply listeners enqueue accepted event-control frames and release the
radio before the communication worker processes them. They must not drain the
general receive queue synchronously from inside route discovery, because result
handling can request another route and recursively re-enter the complete radio
and delivery path. The dedicated communication queue provides the prompt
post-RX response without an unbounded call chain.

Duplicate payload reception must be ACK-sticky. If an anchor receives a packet
that it has already accepted within the packet deduplication window, it may
suppress duplicate processing and must not forward the payload twice. It must
nevertheless include that packet in the hop-level ACK for the applicable
response window, so a lost ACK can be repaired by retransmitting the same
packet without causing another timeout. If the packet was rejected as busy,
duplicate reception may repeat the BUSY response only after the advertised
minimum retry interval; copies decoded before that boundary cannot create a
BUSY train or continually refresh the sender's deferral.

## Route Candidate Retry And Invalidation

Route candidates represent possible parents or next hops toward a target. A
candidate should not be discarded just because it is old; route age alone is not
a permanent invalidation reason. Capacity hints may expire, but an expired
capacity hint does not invalidate the route candidate by itself.

When a packet is sent through a selected parent and gateway delivery is not
confirmed:

1. The first failed gateway-ACK cycle increments the selected parent's failure
   count and retries the same parent after a jittered retry delay.
2. The retry delay is based on the selected parent's failure count:
   1500 ms for failure count 1, 3000 ms for failure count 2, and 6000 ms for
   failure count 3 and later, with implementation jitter applied around those
   bases.
3. The selected parent is retried for 3 failures.
4. On the next failure after those 3 retries, the active route through that
   parent is considered unable to reach the gateway and becomes a route
   invalidation event for the active route epoch and path.
5. The failed parent is not permanently deleted as a physical candidate. It is
   moved into parent hold-down for 30 seconds.
6. While a parent is in hold-down, route selection skips it and tries the best
   alternate valid candidate if one exists.
7. If an alternate candidate exists, the packet is retried through that
   alternate without sending a new route request first, but the failed active
   route path remains invalidated.
8. If no valid alternate candidate exists, route discovery is needed and a new
   route request may be sent according to the route-request backoff rules above.

Only a completed RF send followed by a terminal gateway-ACK wait may increment
the selected parent's delivery-failure count. A pre-RF admission, route,
policy, or local-send failure defers or restarts discovery without consuming
that count. Every parent failure is recorded with the actual wrap-safe uptime,
so the 30-second hold-down begins at the fourth real failure even after the
system has been running for more than 30 seconds.

When any node along a connected route proves through repeated failures that the
gateway cannot be reached through the active path, it invalidates that active
route path, not only the single queued packet. The invalidation clears the
node's failed upstream route, downstream entries that use the failed peer, and
the channel 9 timing reservations for that route epoch. A current-epoch reverse
mapping through a different child remains available for an ACK already in
flight, while its timing is repaired or expires through its own bounded rules.
Where possible, the node sends or queues a route-invalidation notice downstream
along the dependent route so children and the original producer stop using the
dead path quickly. Invalidation propagation stops at a hop-level delivery
failure; nodes beyond that failed hop must still age out through their own
missed-activity rules if they did not receive the invalidation.

A parent candidate is permanently invalidated only by route-table invalidation
events, not by ordinary ACK retry failures:

- A newer route epoch replaces the old epoch; candidates from older epochs are
  stale and are cleared.
- An explicit route invalidation clears upstream routes, downstream routes,
  channel 9 timing reservations, and active route-discovery state.
- Exhausting the retry budget for the active selected path is an explicit route
  invalidation for that active route epoch and dependent downstream state. It
  does not permanently blacklist the physical parent beyond the hold-down rule.
- A candidate with an older epoch than the current table is rejected as stale.
- A candidate may be replaced when the candidate table is full and a better
  current-epoch candidate is learned.

Parent hold-down is temporary. A held-down parent may become selectable again
after the 30 second hold-down expires, or immediately when the same candidate is
learned again through any valid route evidence: route advertisement, route
reply, direct gateway probe, or another route-discovery result. Rediscovering a
candidate means the failed downstream route is assumed to have healed or been
replaced, so the candidate's failure count and hold-down are cleared before
route selection runs. A successful delivery through a selected parent also
resets its failure count and records recent success. The next route-maintenance
or delivery pass after the deadline must run selection again, even when no
candidate aged out, so a sole held-down parent becomes usable without needing
new route evidence.

The direct gateway probe has one special recovery mode. If the held-down parent
is the gateway itself because repeated direct-to-gateway payload batches missed
their gateway batch ACK, the short probe is not treated as valid direct bulk
route evidence until the parent hold-down window expires or a direct payload
delivery succeeds. The probe still runs before each route request, but a
successful answer is contact-only and the channel 5 route request continues so
idle anchors can offer an alternate gateway route.

Route discovery should be started or restarted only when:

- There is no selected valid parent for the target.
- The selected parent exhausted its retry budget and no alternate candidate is
  available.
- The route table was explicitly invalidated or moved to a newer epoch that no
  longer has a candidate for the target.
- The existing channel 9 connection is declared dead by the teardown rules.

Route discovery should not be restarted merely because one hop ACK, gateway ACK,
or gateway batch ACK was missed while the connection and parent candidate are
still within their retry budgets.

Gateway downlinks follow the same remembered-route principle. One target
command timeout increments that exact downlink's failure count but leaves the
route selectable. The first three failures are retry evidence, and the fourth
invalidates the downlink. Fresh accepted reverse-route evidence replaces the
entry and resets its failure count. A command that merely reaches a pre-RF busy
or queue refusal must be retained by the communication service and must not be
reported as a terminal downlink delivery failure.

## Connection Teardown

A channel 9 connection remains valid across transient interruptions. It should
return to low-duty channel 5 scanning only when one of these happens:

- An explicit close or route invalidation occurs.
- The peer misses enough channel 9 receive cycles to exceed the inactivity
  threshold.
- Route state is stale and no hop-level progress is observed.
- The gateway or anchor explicitly rejects the route.

The inactivity threshold must be long enough to tolerate worst-case
click-handling time plus normal retune and scheduling jitter.

An EVENT_END is authoritative only for the active immediate-peer operation
session. The sender becomes terminal after a successful RF transmit and clears
its local timing and pending peer-scoped work; the receiver does the same only
after decoding the matching END. If that frame is lost, the resulting
terminal/active asymmetry is valid only while the receiver's existing timing is
still inside its supervision bound. Expiry, route invalidation, reset recovery,
or an explicit send failure must abandon the remaining owner, clear peer timing
and pending callbacks, and restore the role's normal idle duty cycle. Replayed
or duplicated END frames cannot complete the operation twice, refresh the
deadline, clear newer ACK custody, or alter a later negotiation.

## Efficiency Guidance

These optimizations are allowed when they preserve the invariants above:

- A future compact route-request schema may be embedded in the route-request
  wake train only when its complete equivalent payload fits. The current
  mandatory ancestry makes the smallest request 72 bytes while the standard
  wake suffix can carry only 55 request-payload bytes, so current requests use
  the standalone post-wake control frame. A malformed suffix or a full receive
  queue is not a successful embedding and must leave the standalone listener
  open.
- A rebroadcasting anchor does not need to burn the full downstream route-reply
  listening window after it has received, selected, and ACKed a usable
  downstream route reply.
- Route-reply slots should be calculated from actual packet airtime plus guard
  time rather than from a needlessly large fixed slot.
- Anchor-to-anchor channel 9 transmit windows should use multi-packet sends and
  multi-packet hop ACKs when the slot budget allows.
- Direct-to-gateway channel 9 transmit windows should use batch identity,
  final-packet markers, and gateway batch ACKs so the gateway does not need to
  answer every packet individually.

## Testing And Review Expectations

Changes to mesh routing, channel 5/channel 9 scheduling, DWM3000 sleep behavior,
ACK retry, route discovery, or click preemption should prove which invariants
they preserve.

Useful tests or guards include:

- Route-request wake train does not use the blind flood path during normal
  route acquisition.
- Route-request attempts use TTL 1, then TTL 2, then TTL 4, then TTL 6 for
  attempt 4 and later.
- Route-control receivers reject TTL zero before mutation, reject request and
  advertisement hop/TTL combinations that cannot reconstruct their defined
  origin TTL, and still allow a TTL=1 request to be answered without further
  rebroadcast.
- Every route-control full builder fits its message-specific payload cap, and
  exact cap-plus-one input is rejected before route or ancestry state changes.
- Route-request attempt backoff starts at a 1000 ms base, doubles
  exponentially, caps the base at 60000 ms, and applies percentage-based
  jitter with fresh randomness per attempt.
- Idle anchors with a usable route to the target answer the route request and
  do not rebroadcast it. This includes TTL=1 route requests: TTL limits further
  route-request rebroadcast depth, not whether a one-hop anchor with a usable
  route may reply.
- For gateway discovery, a TTL=1 channel 5 route request means "ask one-hop
  anchors whether they already have a usable gateway route." It is not a direct
  gateway request; direct gateway contact is handled by the separate short
  channel 9 probe before the route request or rebroadcast.
- Idle anchors without a usable route may rebroadcast accepted route requests
  when TTL allows, then open a channel 5 route-reply listening window for any
  downstream reply. If TTL does not allow another hop, they do not rebroadcast.
- A route-reply listener that receives another origin's valid route request for
  its own node or the gateway queues that request and releases channel 5
  immediately. This applies both while the relay is acquiring its own gateway
  route and while it is answering a child's route-solicit wake. The request is
  protocol work, not the awaited route reply, so it does not complete the
  listener's original discovery attempt.
- A relay may answer a gateway route request only from the upstream candidate
  snapshot selected before it installs the requester's reverse route, and that
  candidate's next hop must not be the requester. This split-horizon rule keeps
  simultaneous origins and shared relays from creating a two-node parent loop.
- Route requests, replies, and gateway advertisements reject duplicate IDs,
  local-ID cycles, roots or tails inconsistent with their direction, and any
  node-count mismatch with hop count. A relay does not answer a request when
  its selected gateway ancestry intersects the request ancestry, including a
  three-node loop that immediate split horizon alone cannot see. A fresh
  disjoint advertisement may replace that candidate and make the reply usable.
- A nine-node ancestry encodes in one 74-byte TLV and still fits the maximum
  route control payload. A tenth append fails explicitly instead of truncating
  or emitting a wire path whose hop count no longer matches.
- Current route requests take the standalone post-wake path because mandatory
  ancestry exceeds the standard wake suffix. Any invalid or unqueued suffix
  keeps the post-wake fallback listener active.
- Every original route request, rebroadcast, and retry first attempts the short
  direct channel 9 gateway probe. Direct-or-relayed mode may accept the direct
  gateway route; forced-relay mode and direct-bulk-failure recovery must
  continue route discovery after a direct gateway answer.
- Route-request rebroadcast jitter is sized from airtime and guard-time math so
  the expected fanout, normally up to eight receivers, is unlikely to collide
  while probing or rebroadcasting.
- Click/ranging traffic can preempt and abandon route-request rebroadcast or
  downstream route-reply listening.
- Connected anchors do not answer or rebroadcast unrelated route requests.
- Route replies from multiple responders use reply slots large enough for one
  packet plus guard time, with random responder jitter to reduce repeated
  collisions.
- Route-reply slots fit the immediate upstream node's channel 5 listening
  window while the full reply chain remains inside the origin's advertised
  reply budget.
- A route reply returns over the deepest supported route-request path with a
  nonzero TTL at the origin; each reverse relay decrements the same packet TTL.
- Connected anchors schedule recurring channel 5 windows.
- Channel 9 is deferred or clipped when it would starve channel 5.
- Each connected channel 5 window remains continuously armed until its deadline;
  receiving an unrelated frame does not shorten the window.
- Click/ranging wake trains preempt channel 9, but route-request wake trains do
  not.
- A click/ranging claim caught by any anchor channel 5 listener enters the
  normal click sequence immediately and is never consumed as route contact.
- Normal click discovery stays on the standard wake PHY and uses the normal
  bounded discovery window; it does not inherit the longer mesh-route listener
  or extended-PHR configuration.
- The first wake train is longer than the worst-case configured channel 5
  RX-off gap, and a build-time guard rejects configurations that violate this.
- After a valid click/ranging claim, the anchor remains continuously on channel
  5 through discovery, schedule reception, and every DS-TWR exchange before any
  channel 9 or low-duty work resumes.
- Local-origin click/ranging or command-result packets outrank transit packets
  originated by another node.
- Local-origin priority selects the next work item but does not silently erase
  accepted transit custody; displaced transit remains queued with one owner.
- Gateway-originated commands outrank local-origin click report delivery,
  normal payload relay, packet retries, route maintenance, and background mesh
  work at the first safe radio boundary.
- An ACK already owed for an accepted protocol response runs before a later
  gateway control flood, so sequential command phases cannot strand the
  responder's single reliable-response owner.
- Sequential and synchronized-round survey controls wait through the continuous
  response-ACK settle interval, and an exact duplicate restarts it, so one lost
  ACK cannot overlap the next phase's channel-5 flood or GO.
- A synchronized survey round arms every endpoint before one common future GO.
  The GO delay covers one complete synchronous forward horizon per RF hop, and
  its responder window covers bounded local execution skew plus the initiator
  DS-TWR timeout and complete frame airtime; one-hop and maximum-depth command
  timeouts remain independent, and a frame ending beyond the local window does
  not decode.
- The gateway BLE packet stream accepts a complete maximum-size click report;
  it must not reject a protocol-valid click payload as oversize. Under queue
  pressure, click records may displace lower-priority diagnostic or status
  records.
- Click-report diagnostics are bounded and may be fragmented independently of
  the core click result; their wire representation belongs in the protocol
  document. Click, channel-9 delivery, and gateway-ACK latency TLVs are included
  only when that latency was actually measured. Unknown latency is represented
  by an absent TLV, never a zero-valued placeholder.
- Gateway "Here I Am" route refresh can be triggered by BLE and propagates
  outward without creating extra anchor route slots.
- The gateway performs no startup or periodic route refresh. GUI and headless
  host flows preflight ordinary assignment and survey commands with a separately
  correlated "Here I Am" and wait for its success; firmware does not enforce a
  hidden readiness latch or auto-preamble.
- One anchor relay can learn 50 distinct descendant reverse routes and forward
  a targeted gateway control to every destination, including entries beyond
  the sixteen-route inline table, without increasing gateway role storage.
- Hop ACK extends or resets the gateway ACK timeout.
- Anchor-to-anchor channel 9 TX can send multiple packets per slot, and a
  hop-level ACK can acknowledge multiple listed packets from that receive window.
- Packets missing from a hop-level ACK remain queued for channel 9 retry; a
  duplicate packet inside the deduplication window is still listed in the next
  hop-level ACK response window even if its payload is not processed again.
- Gateway ACK has priority over hop-level ACK when both are ready for the same
  channel 9 transmit opportunity.
- Direct-to-gateway channel 9 TX calculates batch capacity with the gateway
  batch ACK receive window included before sending.
- Direct-to-gateway packets carry a batch identity and final-packet marker.
- The gateway sends one batch ACK after the final packet marker, and nodes treat
  every packet listed in that ACK as fully gateway-accepted.
- A survey reachability report remains durably owned across route wait,
  preemption, timeout, and reset until an exact gateway ACK commits it; a later
  survey cannot silently replace the pending report.
- The first current-survey report accepted directly or through a relay retains
  the anchor's peer relationships and reverse hint. Exact transport duplicates
  and later valid packets for that anchor are ACKed and counted without changing
  either, while stale, malformed, wrong-channel, and identity-mismatched reports
  retain none.
- Before every survey pair prepare and start command, the gateway reinstalls the
  target's current-epoch reverse hint on demand, then uses the bounded priority
  channel 5 wake-and-flood executor. A timing-free hint must never be treated as
  proof that one unscheduled transmission can reach a sleeping target. The wake
  claim and every follow-up frame use the same extended-PHR control PHY; this
  includes the dedicated `MSG_SURVEY_PAIR_PREPARE` packet as well as the
  `MSG_COMMAND` start packet. Tests
  cover an empty route table after gateway reset, 20 direct anchors, 50 mixed
  direct and relayed anchors, and pressure beyond the general downlink-table
  capacity. The source boundary also proves that automatic and host-issued
  manual prepare/start commands share that exact communication-service path.
- Installing a survey reverse hint does not create a gateway connection or a
  channel 9 timing reservation, and CRC-only frame validation is not represented
  as hostile-RF authentication.
- Missing entries from a gateway batch ACK remain queued for channel 9 retry
  without starting a new wake train.
- ACK retry survives the worst-case click-handling duration.
- Packet retry during an active connection does not send a new wake train.
- Exhausting the retry budget for an active route invalidates that route epoch
  and clears dependent downstream route state and channel 9 reservations.
- Sustained missed channel 9 cycles eventually return the anchor to low-duty
  channel 5 scanning.
