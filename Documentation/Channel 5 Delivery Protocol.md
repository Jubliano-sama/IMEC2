# Channel 5 Delivery Protocol (2026-09-03 redesign)

Status: design spec for the work in progress on master. Supersedes the
channel-9 event negotiation, ACK_CONFIRM, direct gateway probe, and the route
request flood described in "Mesh Connected Routing Contract.md" (historical).

Implementation status (2026-09-05): queued anchor reports use one four-packet
Channel-5 delivery bank. A singleton uses the same exchange. Queue removal
publishes bank custody under the admission lock; exact ACK identities mark only
sent members complete, and failed local completion retains that terminal proof
without retransmitting. Partial ACKs and absent routes retain bytes in place.
Retries reselect each packet's next hop, and admission checks the retained bank's
semantic identities as well as the queue. Unrelated RX during ACK waiting is
queued for its normal owner before the sender releases the radio.

Route candidates derive cost from advertised depth and link quality. Ordinary
selection, advertised local depth and packet selection share eligibility;
unreachable depth remains unusable after a temporary hold expires. Correlated
ACK feedback updates depth and credit together and immediately reselects.

The local solicit design below is still incomplete at the sleeping-anchor
application boundary. The prototype continues to use the existing correlated
route-request/reply path when the bank has no route; it never installs a direct
gateway route without an observed exchange. See the
[real-hop audit](Reviews/real-hop-audit-2026-09-05.md) for the remaining solicit,
mixed scan-cadence and large-system qualification gaps.

## 1. Principles

- Every mesh packet travels on UWB channel 5 with the extended-PHR
  `wake_mesh_control` PHY. There is no channel 9.
- The clicker orders click-report delivery. The range schedule is sorted by
  gateway depth (lowest first), then enumeration slot. Anchor `k` in that order
  may transmit its click report no earlier than
  `burst_end + k * MESH_CLICK_DELIVERY_SLOT_MS`.
- Custody moves one hop at a time. A hop is complete when the receiver has the
  packet in RAM and has said so with an ACK. The gateway ACKs on RAM/BLE-stream
  admission; the host GUI never gates the radio.
- ACKs are terminal. There is no ACK_CONFIRM.
- Repair is local. A node that has lost its route says so to its neighbours;
  nobody waits for the gateway to re-flood the network.

## 2. Frames

### 2.1 Report frame (existing types 0x20/0x21/0x22/0x30/0x41/0x44)

Header flags gain one bit, `FLAG_MORE_FOLLOWS` (see protocol.h). Payload gains
one optional TLV:

| TLV | Size | Meaning |
|---|---|---|
| `TLV_BATCH_PENDING` | u8 | Number of further frames the sender holds for this next hop, not counting this one. Absent = 0. |

### 2.2 Hop ACK (`MSG_MESH_HOP_ACK` 0x31) and gateway ACK (`MSG_GATEWAY_ACK` 0x32)

Existing identity payload (`TLV_MESH_ACK_SEQ_LIST`, `TLV_MESH_ACK_SESSION_LIST`,
`TLV_MESH_ACK_PACKET_ID_LIST`) stays; it already lists several packets, which is
the batch bitmap. Two TLVs are added to both ACK types:

| TLV | Size | Meaning |
|---|---|---|
| `TLV_HOP_COUNT` (0x23, existing) | u8 | Responder's current gateway depth. `MESH_ROUTE_DEPTH_UNREACHABLE` (0xFF) = "I have no route; I took nothing". Gateway sends 0. |
| `TLV_BATCH_CREDIT` | u8 | Frames the responder can still take from this sender right now. 0 = stop. |

Rules:
- An ACK with depth `UNREACHABLE` lists no accepted packets. The sender keeps
  custody, marks the candidate dead for `MESH_PARENT_DEAD_END_HOLD_MS`, and
  re-selects.
- An ACK that omits `TLV_HOP_COUNT` is treated as depth `UNREACHABLE` only if it
  also lists no packets (old firmware never does this).
- Credit is a hint for sizing the next burst and for parent selection. It is
  never a promise; the bitmap is the truth.

### 2.3 Route advert (`MSG_GATEWAY_ROUTE_ADV` 0x3C, existing)

Already carries `TLV_HOP_COUNT`, `TLV_RELAY_CAPACITY_STATE`,
`TLV_QUEUE_FREE_HINT`. `TLV_QUEUE_FREE_HINT` is redefined to mean the same
thing as `TLV_BATCH_CREDIT` (free custody slots). A node with no route
advertises depth `UNREACHABLE`.

### 2.4 Route solicit (`MSG_ROUTE_SOLICIT` 0x46)

Broadcast, TTL 1, empty payload. Any neighbour with a finite depth answers with
a unicast route advert after a random delay in `[0, MESH_SOLICIT_REPLY_JITTER_MS]`
(proposed 40 ms). The gateway answers the same way with depth 0. Nobody
forwards a solicit. The gateway's network-wide Here-I-Am wave is **not**
triggered by a solicit.

## 3. Uplink procedure (anchor to parent or gateway)

```
select next hop            lowest depth < own depth, then best quality,
                           skipping dead-end holds; if none: assume direct
                           gateway once, else solicit and hold
if next hop is a parent and the parent is not known to be listening:
    wake train (C5_CONTACT_PURPOSE_UPLINK)
send frame #1 with TLV_BATCH_PENDING = n-1, FLAG_MORE_FOLLOWS = (n > 1)
wait ACK (MESH_UPLINK_ACK_WAIT_MS)
    depth UNREACHABLE  -> dead end: hold candidate, re-select, no retry here
    credit c           -> send min(c, n-1) more frames back to back,
                          FLAG_MORE_FOLLOWS on all but the last,
                          wait one batch ACK
    no ACK             -> jittered exponential backoff (relay core)
unaccepted frames stay in custody and go in the next burst
```

Single report (the common click case): one frame, one ACK, done.

## 4. Receiver procedure (parent or gateway)

- On a frame with `FLAG_MORE_FOLLOWS` clear and `TLV_BATCH_PENDING` 0: admit,
  ACK immediately. No lookahead hunt.
- On `FLAG_MORE_FOLLOWS` set: admit, keep RX armed for the follower for at most
  `MESH_BATCH_FOLLOWER_GAP_MS` (proposed 6 ms). ACK once after the last frame or
  after the gap expires.
- On `TLV_BATCH_PENDING > 0` (first frame of a burst): ACK with
  `TLV_BATCH_CREDIT = min(pending, free slots)`; then stay in RX for the burst.
- Credit = free custody slots minus a reserve for own reports
  (`MESH_CUSTODY_OWN_RESERVE`, proposed 1).
- The gateway is a receiver with unlimited depth 0 and credit = free BLE
  stream slots minus 1.

## 5. Post-burst listen (click reports only)

Every anchor that received the range schedule stays in channel-5 RX from
`burst_end` until `burst_end + MESH_CLICK_DELIVERY_LISTEN_MS`
(proposed `schedule_len * MESH_CLICK_DELIVERY_SLOT_MS + 150 ms`). During that
window children skip the wake train when their next hop is an anchor that was
in the same schedule. Kconfig `CONFIG_IMEC_CLICK_DELIVERY_PARENT_LISTEN`
(default y); bench overlay `conf/mesh-bench-wake-trains.conf` sets it to n so
wake trains remain testable.

## 6. Route repair

- **Loss detection.** A node marks its route lost when its parent answered
  `UNREACHABLE`, or the parent missed `ROUTE_MAX_FAILURES` consecutive ACK
  windows, or its own depth advert cannot be justified (parent entry expired).
- **Poisoning.** A node with a lost route sets its depth to `UNREACHABLE`,
  answers every uplink with `UNREACHABLE`, and sends one route advert with
  depth `UNREACHABLE` (so children stop using it), then broadcasts a solicit.
- **Re-selection.** Replies to the solicit populate parent candidates. The
  node picks the lowest finite depth. That may be its former child if the child
  has another path; the child then receives its own earlier packets back and
  re-adopts custody (identity match on src/session/seq against its ACK
  history; it is not a duplicate drop).
- **Loop freedom.** Forward only to a candidate with strictly lower depth than
  own depth. A node with `UNREACHABLE` depth forwards to any finite depth.
- **Gateway wave.** The gateway's periodic/commanded Here-I-Am wave stays as
  the global re-synchronisation, on its existing schedule only.

## 7. Retransmission

Owned by the relay core. After a missed ACK: `wait = ack_window + U(0, J)`,
then doubling with jitter, capped at `MESH_RETRY_CAP_MS` (proposed 2000 ms),
until the packet's age-out. Retries re-run next-hop selection.

## 8. Timing targets (to be measured on the bench)

| Quantity | Now | Target |
|---|---|---|
| Gateway frame end to ACK RF start | ~60 ms | <= 12 ms |
| Anchor ACK wait window | 250 ms | 40 ms |
| Click delivery slot | 35 ms | 20 ms |
| Parent hop ACK | wake train + 250 ms | listen window, <= 12 ms |

## 9. Implementation notes (2026-09-04, relay core)

The relay core in `firmware/src` implements sections 2, 3, 4 and 6 as follows.
Where the wording below differs from the sections above, the wording below is
what the code does.

### 9.1 Assigned TLV ids

| TLV | Id | Size |
|---|---|---|
| `TLV_BATCH_PENDING` | 0xCD | u8 |
| `TLV_BATCH_CREDIT` | 0xCE | u8 |
| `TLV_BATCH_REMAINING` | 0xCF | u8 |
| `TLV_RETRY_AFTER_MS` | 0x72 (already existed) | u16 |
| `TLV_HOP_COUNT` | 0x23 (already existed) | u8 |

`TLV_BATCH_PENDING` and `TLV_BATCH_REMAINING` are omitted when zero; absent
decodes as zero, so the single-report fast path costs no extra wire bytes.
Credit is capped at `MESH_BATCH_CREDIT_MAX` (0xFE) so 0xFF stays free as the
"unknown" sentinel in local state.

### 9.2 `FLAG_MORE_FOLLOWS` has no wire bit

Section 2.1 assumes a spare header flag. There is none: the one-byte `flags`
field is fully allocated and `FLAG_MORE_FOLLOWS` is defined as `0x00`, so
`proto_packet_flags_more_follows()` always answers false and no encoder can
set it. Burst continuation is carried entirely by `TLV_BATCH_PENDING` on the
first frame and `TLV_BATCH_REMAINING` on each follower. When a header bit or
extension byte becomes available the flag can be re-enabled without changing
any of the batch logic.

### 9.3 Explicit refusal shape and old-firmware compatibility

`TLV_BATCH_CREDIT` - not `TLV_HOP_COUNT` - is the marker that tells a
zero-identity ACK apart from a truncated legacy one. An ACK that names no
accepted identity is accepted by the parser only when `TLV_BATCH_CREDIT` is
present; otherwise it stays malformed exactly as before. An ACK that carries
neither `TLV_HOP_COUNT` nor `TLV_BATCH_CREDIT` is old firmware: depth and
credit read as unknown and custody handling is byte-for-byte unchanged.

Both refusal kinds are produced by `mesh_relay_build_backpressure_ack()`:

- **Dead end** - `TLV_HOP_COUNT = MESH_ROUTE_DEPTH_UNREACHABLE` (0xFF), no
  identities. The sender keeps custody, folds 0xFF into that candidate's
  `hop_count`, parks it for `MESH_PARENT_DEAD_END_HOLD_MS` (5000 ms) without
  touching its `failure_count`, re-selects, and retries immediately. With no
  alternate it poisons its own depth and raises
  `MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED`.
- **Backpressure** - real depth, `TLV_BATCH_CREDIT = 0`, `TLV_RETRY_AFTER_MS`.
  The sender keeps custody and the same parent, applies no hold-down and no
  failure count, and retries at `now + retry_after` jittered by
  +/-`MESH_RELAY_BACKPRESSURE_JITTER_PERCENT` (25 %). A refusal that omits
  `TLV_RETRY_AFTER_MS` uses `MESH_RELAY_BACKPRESSURE_RETRY_AFTER_MS` (120 ms).

Either way the core sets `MESH_RELAY_ACTION_TX_HOP_DEFERRED` (the last free
bit of the 32-bit action word). The application must re-arm its transmit timer
from `relay->pending.retry_after_ms` and must not call
`mesh_relay_note_pending_parent_failure*()` for that frame.

Flow-control TLVs are emitted **before** the accepted-identity list so the
identities stay at the payload tail, exactly where older firmware put them.

### 9.4 Credit source

`mesh_relay_local_credit()` = free slots minus `MESH_CUSTODY_OWN_RESERVE` (1),
where the free-slot count is whatever the application last passed to
`mesh_relay_set_local_free_slots()`. The gateway passes
`GATEWAY_BLE_STREAM_QUEUE_DEPTH - gateway_ble_stream_depth()`. Until that call
is made the core assumes one custody slot while idle and none while busy, so
an unconfigured node advertises credit 0 - never a promise it cannot keep.

### 9.5 Route adverts and solicit replies

`mesh_relay_build_solicit_reply()` returns an ordinary
`MSG_GATEWAY_ROUTE_ADV`: `src_id` is the gateway and `dst_id` is broadcast on
the wire, because that is what every receiver's advert validator checks. Only
the radio next hop is the single node that asked, and the TTL is the exact
depth complement (`MESH_NETWORK_MAX_HOPS - depth`) so it is never re-flooded.
A node with `MESH_ROUTE_DEPTH_UNREACHABLE` returns `PROTO_ERR_NOT_FOUND` and
stays silent. The application applies the `[0, MESH_SOLICIT_REPLY_JITTER_MS)`
draw itself.

Route adverts still do not carry `TLV_QUEUE_FREE_HINT`: the advert's TLV set
is exactly validated on receipt and adding a field there is a wire change with
no current consumer. The credit a peer reports in its ACK is folded into that
candidate's `queue_free_hint` instead, which is the field parent selection
already reads.

### 9.6 Loop freedom and poisoning

`mesh_relay_local_depth()` is 0 at the gateway, `selected->hop_count + 1` at an
anchor, and `MESH_ROUTE_DEPTH_UNREACHABLE` when the upstream is poisoned, held
down, expired or absent. Poisoning is set by a dead end with no alternate, by
exhausting `ROUTE_RETRIES_PER_CANDIDATE` on every candidate, and by advert
expiry that leaves no selectable route; it is lifted by any freshly upserted
upstream candidate or a gateway ACK.

`mesh_relay_select_next_hop_for_packet()` enforces
`mesh_relay_depth_is_forwardable()` for gateway-bound packets: a candidate is
usable only when its advertised depth is strictly lower than the local depth,
except that a node at `UNREACHABLE` may use any finite-depth neighbour -
including a former child. Ordinary selection is loop free by construction, so
the filter only bites once a candidate has been poisoned to 0xFF by a dead-end
ACK or has aged out.

### 9.7 Batching split

The core owns the arithmetic and the application owns radio timing:

- `mesh_relay_note_batch_pending(n-1)` then
  `mesh_relay_append_batch_pending()` stamps the first frame.
- A positive ACK's credit `c` becomes `min(c, pending)` burst-eligible
  followers; `mesh_relay_next_burst_frame()` hands out each one with the
  `TLV_BATCH_REMAINING` value it must carry, zero on the last.
- `mesh_relay_note_rx_batch_frame()` / `mesh_relay_rx_expects_more()` tell the
  receiver to keep its radio armed and defer its single batch ACK until the
  burst ends or `MESH_BATCH_FOLLOWER_GAP_MS` (6 ms) elapses without a
  follower. The gap timer itself is the application's.

### 9.8 Re-adoption bound

`mesh_relay_note_parent_handoff()` remembers up to
`MESH_RELAY_HANDOFF_MEMO_SLOTS` (4) packets this node handed to a parent and
has not seen gateway-ACKed. A packet whose `src_id` is this node, returned by
exactly that parent and still unacknowledged, is re-adopted into custody by
`mesh_relay_handle_rx()` (actions `RETRANSMIT | CUSTODY_ACCEPTED` plus a hop
ACK back to the returning parent) instead of being rejected as a self-
addressed frame. If the custody slot is busy the core answers with an explicit
backpressure ACK, so the returning parent keeps the bytes rather than losing
them.

## 10. Removed

Channel 9 PHY config in mesh roles, `MSG_MESH_EVENT_PROPOSE/ACCEPT/UPDATE/END`,
`MSG_GATEWAY_ACK_CONFIRM`, `MSG_ROUTE_REQ/REPLY/REPLY_ACK`,
`MSG_GATEWAY_ROUTE_REQ`, `MSG_RELAY_BUSY`, direct gateway probe, ch9 ACK table,
gateway RX slicing. Message ids stay reserved, never reused.
