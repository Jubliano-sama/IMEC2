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
- A connected anchor should not enter retained or deep DWM3000 sleep between
  back-to-back channel 9 and channel 5 windows. It may keep the radio idle or
  ready for retune.
- Normal route acquisition must not use the blind gateway-command channel 5
  flood mechanism. Route requests are still broadcast route-discovery control
  packets: idle anchors may rebroadcast them when TTL allows, but connected
  anchors must not.
- Blind channel 5 flooding is reserved for broad discovery, gateway command
  delivery, and "Here I Am" style reachability, not routine route requests.
- Gateway-originated commands and user-requested "Here I Am" route refresh are
  control-plane traffic. They take priority over normal payload relay, ACK
  retries, and background maintenance.
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
  acquisition.
- Route-request wake trains must be clearly distinguishable from click/ranging
  wake trains.
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
   direct-or-relayed mode, the producer installs the direct gateway route and
   does not broadcast a route request. If this succeeds in forced-relay mode,
   the producer records that direct gateway contact was possible, but it still
   continues route acquisition because the final route must include at least
   one anchor hop.
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
the highest-priority mesh control traffic. They must move before normal
gateway-bound payloads, packet retries, and background maintenance.
Click/ranging preemption remains the interactive channel 5 path, but once the
radio can safely resume mesh work, queued gateway-originated control is the
first mesh work to service.

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

Gateway commands must not be blocked behind ordinary packet retries. If a
normal payload, a retry, and a gateway command all need radio time, the gateway
command's channel 5 propagation is serviced first. The delayed payload or retry
remains queued and is attempted later as long as the connection remains valid.

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
   listed remain pending for retry.
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
after the 30 second hold-down expires, or it may be refreshed by a newer valid
route advertisement or route reply. A successful delivery through a selected
parent resets its failure count and records recent success.

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
  gateway route; forced-relay mode must continue route discovery after a direct
  gateway answer.
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
- Click/ranging wake trains preempt channel 9, but route-request wake trains do
  not.
- Gateway-originated commands outrank normal payload relay and packet retries.
- Gateway "Here I Am" route refresh can be triggered by BLE and propagates
  outward without creating extra anchor route slots.
- Hop ACK extends or resets the gateway ACK timeout.
- Anchor-to-anchor channel 9 TX can send multiple packets per slot, and a
  hop-level ACK can acknowledge multiple listed packets from that receive window.
- Gateway ACK has priority over hop-level ACK when both are ready for the same
  channel 9 transmit opportunity.
- Direct-to-gateway channel 9 TX calculates batch capacity with the gateway
  batch ACK receive window included before sending.
- Direct-to-gateway packets carry a batch identity and final-packet marker.
- The gateway sends one batch ACK after the final packet marker, and nodes treat
  every packet listed in that ACK as fully gateway-accepted.
- Missing entries from a gateway batch ACK remain queued for channel 9 retry
  without starting a new wake train.
- ACK retry survives the worst-case click-handling duration.
- Packet retry during an active connection does not send a new wake train.
- Exhausting the retry budget for an active route invalidates that route epoch
  and clears dependent downstream route state and channel 9 reservations.
- Sustained missed channel 9 cycles eventually return the anchor to low-duty
  channel 5 scanning.
