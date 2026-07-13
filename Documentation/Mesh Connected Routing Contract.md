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
- After an anchor accepts a gateway-originated channel 5 control follow-up, the
  communication service installs or refreshes the upstream response candidate
  from the frame's actual previous hop before the protocol handler can commit
  state. The remaining control TTL must agree with a direct or bounded relayed
  path. This fresh reverse hint creates no gateway connection or channel 9
  reservation; it only prevents an immediate command response from starting a
  redundant route-discovery cycle. Inconsistent source, hop, TTL, epoch, or
  quality evidence rejects the control frame before it can strand protocol
  state.
- When a connected anchor has gateway-bound packets produced locally by its own
  click/ranging or command-result work, that local-origin work outranks transit
  packets it is relaying for another producer. The anchor may defer, drop, or
  abandon the lower-priority transit route to reclaim queue, channel 9 schedule,
  or route-slot capacity for the local-origin work. Recovery for the displaced
  transit work belongs to the downstream or originating node through retry,
  timeout, and route rediscovery.
- Any request likely to arrive at multiple nodes at nearly the same time must
  include enough random jitter before consequent replies, probes, or
  rebroadcasts that collisions are unlikely for the expected fanout. The jitter
  budget should be justified from packet airtime, retune time, guard time, and
  the maximum expected number of simultaneous responders.
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
- The gateway answers a direct-to-gateway batch only after the final packet
  marker is received. That answer is one gateway batch ACK covering all packets
  from that batch that the gateway accepted.
- A gateway batch ACK is a fully valid gateway ACK for every packet it lists.
  Any node that forwarded those packets must accept the batch ACK and mark all
  listed packets as gateway-accepted.
- In a multi-hop route, hop-level ACKs bubble back one hop at a time toward the
  original transmitter.
- If a gateway ACK and a hop-level ACK are both queued for the same channel 9
  transmit opportunity, the gateway ACK has priority because it reassures the
  original transmitter that the packet was successfully received.
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

Named delivery profiles are part of the communication contract. A caller may
choose the profile that matches its semantics, such as bounded control flood,
reliable uplink, durably owned reliable uplink, control response, or best
effort. A caller must not construct a private profile to make one protocol pass
a narrow timing case. Changing a profile requires testing every protocol that
uses it under collision, busy-radio, retry, route-loss, and deadline pressure.

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

The communication service owns whether an RF attempt actually started.
Deferral before RF begins does not consume an opportunity; once RF begins, the
attempt is counted even if it collides or times out. Every accepted datagram
reaches exactly one terminal result: delivered, deadline expired, attempts
exhausted, permanent failure, or explicit cancellation. Retries preserve the
same packet identity so receiver deduplication and durable custody remain
valid, while immutable source/session/sequence-derived jitter prevents
independent nodes from repeating the same synchronized collision.

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

When an anchor must choose between servicing a lower-priority transit route and
sending gateway-bound packets produced by that anchor, it should abandon the
lower-priority transit reservation and reclaim the needed upstream/downstream
capacity for its own local-origin work. When timing allows, the anchor should
send a busy or end indication on the affected channel 9 connection before
abandoning it, so the peer can drop the reservation promptly. If that notice
would delay the local-origin work, the anchor may abandon the route without
notice; the displaced route is repaired by the downstream or originating node's
retry, timeout, and rediscovery path.

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
   rebroadcast that route request.
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
   must use random jitter so multiple responders are unlikely to collide.
11. A downstream anchor with a usable route, including a route learned from a
   successful direct channel 9 gateway probe, sends a route reply back along
   the reverse path. The gateway itself remains a channel 9 receiver for normal
   route acquisition; it does not send normal channel 5 route replies.
12. Each route reply hop is ACKed on channel 5 so the sender knows the reply was
   received.
13. The packet producer ACKs the final route reply on channel 5.
14. The participants enter the negotiated channel 9 rhythm.

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
- Optional channel 9 timing proposed by the origin for the downstream route.
- A reply-window delay that tells responders how long remains before the origin
  is ready to listen for route replies.
- Optional flags, including forced-relay behavior when a direct gateway answer
  must be ignored.

Route-request TTL broadens across repeated discovery attempts:

- Attempt 1 uses TTL 1, targeting only directly neighboring anchors.
- Attempt 2 uses TTL 2.
- Attempt 3 uses TTL 4.
- Attempt 4 and later attempts use TTL 6.
- Normal route requests must not exceed TTL 6. Any larger emergency route
  request TTL requires explicit user permission and an update to this contract.
- After attempt 4, later attempts keep using TTL 6 while the backoff continues
  to increase.

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

Survey reachability uses the same control-followup receive eligibility as
anchor enumeration. After the `SURVEY_DISCOVERY_START` flood, each participating
anchor has four probe opportunities. Opportunity timing is derived independently
from the anchor identity, survey command identity, and opportunity index. The
first opportunity may expose a deterministic slot collision; later opportunities
use bounded exponential backoff and deterministic jitter so that collision is
not repeated merely because both anchors selected the same initial slot. The
derivation does not depend on synchronized random-number-generator state. An
anchor listens continuously outside its own complete probe airtime, deduplicates
peer probes by anchor identity, and sends one reachability report after the full
four-opportunity horizon. The gateway collection window covers that complete
horizon plus every report slot and grace interval. A terminal `no anchors`
result is valid only after this bounded horizon completes without a unique
eligible report.

Each report may retain up to twelve heard peers. The gateway first builds a
deterministic degree-six-or-less spanning graph from every reported directed
reachability edge, then fills remaining degree with mutual, higher-quality
pairs. If the reported graph cannot be connected within that degree bound, pair
planning fails explicitly instead of silently returning isolated anchors.

Each anchor owns its encoded `SURVEY_DISCOVERY_REPORT` until the gateway has
explicitly acknowledged that exact packet identity. Before the first transport
attempt, the anchor transactionally persists the report bytes, survey identity,
delivery generation, state, and bounded attempt budget. Route waits, gateway
command preemption, tracked-transmit retries, and resets must resume that same
record; admission to an in-memory report queue is not delivery. The durable
record is cleared in two phases only after the gateway ACK is committed. While
one report is pending, a later survey start is explicitly rejected and cannot
overwrite the report whose custody is already owned.

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

Duplicate payload reception must be ACK-sticky. If an anchor receives a packet
that it has already accepted within the packet deduplication window, it may
suppress duplicate processing and must not forward the payload twice. It must
nevertheless include that packet in the hop-level ACK for the applicable
response window, so a lost ACK can be repaired by retransmitting the same
packet without causing another timeout.

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

When any node along a connected route proves through repeated failures that the
gateway cannot be reached through the active path, it invalidates that active
route path, not only the single queued packet. The invalidation clears the
node's failed upstream route, any downstream routes that depend on that
upstream route, and the channel 9 timing reservations for that route epoch.
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
resets its failure count and records recent success.

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

## Efficiency Guidance

These optimizations are allowed when they preserve the invariants above:

- A route request may be embedded in the route-request wake train when the
  complete route-request payload fits. This avoids a separate post-wake control
  packet while keeping the wake train clearly typed as route setup.
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
- A connected anchor may abandon a lower-priority transit route, including its
  upstream channel 9 reservation, when it needs that capacity for its own
  local-origin gateway-bound packets.
- Gateway-originated commands outrank local-origin click report delivery,
  normal payload relay, packet retries, route maintenance, and background mesh
  work at the first safe radio boundary.
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
