# Channel 5 report delivery

Current implementation reference, 2026-09-07. This replaces the mixed proposal/implementation text from the September 3 redesign. Radio ownership, wake timing and survey exclusivity are defined in the [Mesh contract](<Mesh Connected Routing Contract.md>).

## Custody and admission

Queued local and forwarded gateway-bound reports use one bounded four-packet Channel-5 delivery bank in `firmware/app/src/app_mesh_report_delivery.inc`. A singleton uses that same bank. Admission publishes bank custody under the admission lock before removing the queue entry; retained-bank identities participate in duplicate checks. An idle relay, a busy relay and a source without a current route must preserve the same ownership guarantee.

Each retained packet has an immutable semantic identity. An exact ACK completes only identities actually sent in the current exchange. Partial ACKs leave all unaccepted bytes owned by the bank. Failure to complete local retirement retains the terminal proof without sending the already accepted packet again. Retry selection re-evaluates each packet's next hop; absent routes retain bytes rather than manufacturing a direct-gateway path.

Duplicate history is not custody evidence. An exact returning report passes through application admission again: bytes still held in the queue or bank deduplicate there, while bytes released after an earlier hop ACK must be reacquired before another ACK is permitted. Full capacity, an exhausted TTL or an absent forward path cannot produce a history-only custody ACK. This remains necessary when stale parent information temporarily creates a routing loop.

A hop ACK transfers custody to the immediate receiver. The gateway's ACK follows bounded RAM/BLE-stream admission, independently of whether the GUI has already displayed the packet. Production C5 bank completion has no ACK_CONFIRM handshake and no periodic Channel-9 turn. This RAM custody is not a promise to survive power loss. The user-facing host records and any explicitly durable configuration have separate owners.

## Wire fields

The shared definitions and validators in `firmware/include/protocol.h` and the native relay modules are authoritative for numeric IDs and closed envelopes.

| Field | Meaning |
|---|---|
| Existing ACK identity lists | Exact accepted packets; omitted identities retain sender custody |
| `TLV_BATCH_PENDING` | Further frames proposed after the first packet |
| `TLV_BATCH_REMAINING` | Followers remaining after this frame |
| `TLV_BATCH_CREDIT` | Receiver's bounded current admission hint |
| `TLV_HOP_COUNT` | Receiver's advertised gateway depth; 0 at the gateway, 0xFF unreachable |
| `TLV_RETRY_AFTER_MS` | Backpressure retry hint |

`FLAG_MORE_FOLLOWS` has no allocated wire bit and is defined as zero. Continuation therefore uses the two batch TLVs. A design mentioning a usable header flag is stale. Flow-control fields precede accepted identities. A zero-identity refusal needs explicit credit to distinguish it from a malformed truncated legacy ACK; parsers retain their explicit compatibility handling for legacy ACKs that omit depth/credit.

## Exchange and backpressure

The sender selects an observed usable parent, performs the required wake/contact when that peer is not known to be listening, sends the first packet and waits for its correlated ACK. Positive credit permits bounded followers. Receivers retain a bounded follower window and ACK the accepted batch; a single packet does not require an unbounded lookahead. Unrelated RX encountered during ACK waiting goes to its validated normal owner before radio release.

Credit limits attempted admission; it never transfers custody by itself. A finite-depth zero-credit refusal retains the selected parent and schedules backpressure retry without charging RF-route failure. An unreachable-depth refusal accepts no identities, marks that candidate unusable for its hold and reselects. Missed ACKs use bounded relay retry/backoff. Route changes cannot turn a partial ACK into success for unsent packets.

The relay core derives credit from application-provided free custody capacity minus the own-report reserve. Gateway admission is also constrained by BLE payload bytes and queue records; a slot count alone must not certify available capacity. Exact acceptance remains the authority when those hints become stale.

## Route handling and implementation limits

Parent ranking uses observed local link quality and depth, subject to expiry, holds and forwardability. Correlated ACK feedback updates route depth and credit together. An unreachable candidate does not become valid merely because its temporary hold elapsed.

Local `MSG_ROUTE_SOLICIT` and reply helpers exist in the native core. The application still contains and uses correlated `MSG_ROUTE_REQ`/reply recovery at the sleeping-anchor boundary. The old redesign's claim that all route-request/reply messages were removed was incorrect. Likewise, legacy Channel-9 event-control code remains compiled/tested in parts of the tree; its existence does not make it the production report transport.

Post-ranging listening can let scheduled anchors receive child reports without another wake; its eligibility and duration are implementation-owned and configuration-dependent. A retained local receive window cannot establish that every possible parent is awake. ACK latency targets and proposed retry timings from the original redesign are not measured guarantees.

## Verification

The mandatory mesh integration and hardware-model gates are in [AGENTS.md](../AGENTS.md). Delivery regressions should cover local and transit admission, idle/busy relay, no route, partial/exact/stale ACKs, backpressure, route changes and delayed drain. A direct gateway success does not exercise relay custody; forced-hop success does not prove physical RF isolation or a 30-node deployment. Dated results remain in `Reviews/` and `logs/`, with their original evidence boundaries.
