# UWB+BLE Protocols and Strategies

Version: 0.3.12.4

Previous version: [UWB+BLE Protocols and Strategies 0.3.12.3](<UWB+BLE Protocols and Strategies 0.3.12.3.md>)

This is the flattened current v1 protocol description. The numeric wire
registry and shared-envelope limits are defined by
[`protocol.h`](../firmware/include/protocol.h). Connected-routing behavior,
radio ownership, custody, and retry invariants are defined by the
[Mesh Connected Routing Contract](<Mesh Connected Routing Contract.md>). If
this prose disagrees with either authority, the header controls wire encoding
and the routing contract controls runtime behavior.

The compact system view is
[UWB+BLE Architecture 0.6.6.3](<UWB+BLE Architecture 0.6.6.3.md>), and the
same behavior is traced end to end in the
[Mesh Connected Routing Walkthrough](<Mesh Connected Routing Walkthrough.md>).

## Changelog

### 2026-07-26 - 0.3.12.4

- Replaced the accumulated historical body with one present-tense protocol
  description backed by the wire header and routing contract.
- Removed the retired survey reserve-horizon algorithm and old flood defaults.
  Survey discovery now uses continuous randomized rounds, and the current flood
  defaults are 4,200 ms maximum randomized backoff, 600 ms slots, two retries,
  and a four-copy relay burst.
- Clarified immutable packet identity, transport and application correlation,
  ACK-gated durable admission, synchronized survey GO, BLE framing boundaries,
  and explicit failure behavior.

## Shared packet envelope

Shared mesh packets use magic `0xC1` and protocol version `0x01`. Multi-byte
values are little-endian. The encoded packet ends with CRC-16/CCITT-FALSE over
the complete header and payload.

| Bytes | Field |
| --- | --- |
| `0` | Magic, `0xC1` |
| `1` | Version, `0x01` |
| `2` | Message type |
| `3` | Semantic flags |
| `4..11` | Source node ID |
| `12..19` | Destination node ID |
| `20..23` | Session ID |
| `24..25` | Packet sequence |
| `26` | Remaining TTL |
| `27` | Payload length for a standard header, or `0xFF` for an extended header |
| `28..31` | Standard-header message age |
| `28..29`, `30..33` | Extended payload length and message age |
| after header | Payload, then two-byte CRC |

A standard header is 32 bytes and represents payload lengths `0..254`. An
extended header is 34 bytes and is required from 255 bytes through the
958-byte maximum. The largest shared packet is therefore 994 bytes:
34-byte header, 958-byte payload, and two-byte CRC. A decoder rejects the wrong
magic or version, a noncanonical extended length, an unsupported message type,
a declared-length mismatch, an oversize payload, or a bad CRC before exposing
the packet to a protocol owner.

Shared payloads use `type:u8, length:u8, value:length` TLVs. Required fields and
their exact widths are message-family contracts; repeated TLVs are valid only
where that family explicitly defines aggregation. The complete registry stays
in [`protocol.h`](../firmware/include/protocol.h) rather than being copied into
this document.

Compact UWB wake, discovery, schedule, DS-TWR, and radio-diagnostic frames have
their own fixed layouts. Their numeric IDs share the registry, but they are not
accepted merely by placing them inside the shared packet envelope.

## Message families

| Range | Family and purpose |
| --- | --- |
| `0x08..0x0D` | Compact UWB wake, click discovery, schedule, mesh wrapper, and release frames |
| `0x10..0x19` | Compact DS-TWR, UWB diagnostics, and anchor-pair radio frames |
| `0x20..0x22` | Click, self-test, and anchor heartbeat reports |
| `0x30..0x3F` | Mesh payload, hop/gateway ACK, route, event-timing, busy, and gateway-route control |
| `0x40..0x45` | Host command, result, grant/bundle, and collection-EACK traffic |
| `0x50..0x56` | Survey reachability, pair control/result, discovery report, and gateway operation events |
| `0x7F` | Explicit protocol error |

IDs `0x01` and `0x02` are retired. IDs `0x33` and `0x34` are reserved and must
not be emitted as legacy route beacons. Message-specific validation is still
required after the envelope is decoded; belonging to one of these ranges does
not make an arbitrary payload valid.

The flags byte distinguishes control follow-up, route setup, gateway-ACK
requirements, diagnostic and click accounting, error, and range-only
semantics. Flags are part of semantic identity. A relay may change TTL and
message age as a packet moves, but it must not mutate the semantic flags or
payload.

## Identity, correlation, and duplicate handling

The envelope provides three nested identity levels:

1. A 64-bit source and destination identify the communicating nodes. A zero
   destination is broadcast only for message families that explicitly allow it.
2. A nonzero 32-bit session identifies the application or connection
   operation, while the 16-bit sequence identifies a packet within that
   operation.
3. Application protocols add correlation TLVs where the envelope is not enough.
   A command result, for example, is bound to gateway ID and epoch, command
   sequence, node ID and boot counter, and result sequence.

One logical request keeps the same envelope identity and exact payload across
every deferral and retry. A sender must not allocate a new sequence merely
because an RF attempt, ACK window, or short response wait ended. An exact
duplicate is idempotent and ACK-sticky: it does not repeat the semantic
mutation, but it is acknowledged again so a lost ACK can be repaired. Reusing
the same identity with different semantic flags, payload length, or payload
bytes is a conflict and fails closed.

Broadcast `MSG_COMMAND` has one deliberate refinement. Its relay-dedup identity
is source, destination, and the mandatory nonzero `TLV_COMMAND_SEQ`, because
the envelope session may name a longer operation containing several survey GO
rounds. An exact retry of one command sequence is inert, while a later command
sequence in the same survey session remains deliverable.

Channel-9 event negotiation uses the PROPOSE session as the connection
operation owner. UPDATE and END reuse that session, and each endpoint keeps
independent local and remote control-sequence histories. Every PROPOSE also
carries a nonzero per-boot 64-bit incarnation, so a peer may restart its
sequence domain after reset without making delayed pre-reset proposals current
again. Missing, zero, retired, or conflicting ownership is rejected before
timing changes.

The packet CRC provides corruption detection, not authentication. The current
envelope has no keyed MAC, so node identity and route evidence must not be
described as hostile-RF authentication.

## Channel 5 and channel 9

Channel 5 is the control and preemption lane. Click/ranging wake and discovery
use the standard wake PHY and take priority. Route and gateway-control
follow-ups use the extended-PHR control PHY and must be explicitly identified
as control follow-ups. Route requests are control traffic, not click traffic,
and a connected anchor does not let routine route discovery steal an imminent
channel-9 event.

Channel 9 carries connected relay data and ACKs. An anchor has at most one
upstream and one downstream channel-9 connection. Its connected rhythm
alternates bounded channel-9 work with full allocated channel-5 receive
windows; channel-9 work is clipped or deferred when it would starve channel 5.
The gateway owns no normal channel-9 connection. It listens continuously in
bounded cooperative slices, and direct anchors use an unscheduled gateway turn
with a same-turn ACK window.

Blind channel-5 flooding is reserved for broad discovery, gateway-originated
commands, and Here-I-Am reachability. Routine route requests use the reactive
route-request/reply exchange. Each bounded flood opportunity performs its own
channel-5 quiet check; a pre-RF busy decision defers the opportunity and does
not consume it.

The current relay-flood defaults in
[`mesh_relay.h`](../firmware/include/mesh_relay.h) are:

| Control | Default |
| --- | ---: |
| Maximum randomized relay backoff | 4,200 ms |
| Randomized backoff slot | 600 ms |
| Additional flood retries | 2 |
| Copies in each relay burst | 4 |

The retry count applies to additional bursts, not to copies within one burst.
The four successful copies use 40 ms spacing. A busy radio, failed quiet check,
or another pre-RF refusal retains the current copy; it cannot convert a partial
burst into success.

## Route discovery and event timing

When gateway-bound custody has no usable route, every discovery attempt first
tries a short direct channel-9 gateway probe. If that probe is insufficient,
the origin sends a typed channel-5 route request. Only an idle anchor with
capacity may answer or rebroadcast it; a connected anchor retains its existing
radio rhythm. Split horizon and bounded route ancestry prevent two-node and
longer cycles.

Route-request attempts widen through TTL `1`, `2`, `4`, then `6`; later attempts
remain at `6`. Their randomized backoff starts at 1,000 ms, doubles per attempt,
and caps the base at 60,000 ms. Fresh per-attempt randomness is required so two
nodes cannot replay the same collision indefinitely. A route reply returns
through the recorded channel-5 reverse path and is ACKed at each hop.

A direct gateway candidate needs no recurring timing. An anchor next hop either
accepts timing carried by the route reply or negotiates it through
EVENT_PROPOSE/EVENT_ACCEPT. The successful PROPOSE transmission fixes the
phase; ACCEPT confirms that phase and must not shift it because of queue
latency or retry delay. EVENT_UPDATE may adjust a live connection only under
the same owner, and EVENT_END can close only the matching live session.

Once connected, a missed hop or gateway ACK retries the same packet on channel
9 without a new wake train. Route acquisition restarts only after explicit
invalidation, terminal route failure, or connection supervision expiry. Click
handling interrupts the rhythm without implicitly deleting it.

## Custody, ACKs, and durable admission

One logical packet has exactly one custody owner. The application owns a
delivery handle; the communication service owns the frozen packet, route
selection, persistence, RF attempts, ACK timers, retries, and terminal result.
A queue, radio scheduler, route-wait slot, or survey journal may reference that
owner but may not start a second retry or deadline state machine.

A hop ACK transfers custody only to the next anchor. Packets omitted from a hop
ACK remain pending. Gateway ACK proves final gateway acceptance and has
priority over a hop ACK when both are due in one transmit window. A hop ACK is
progress and extends the gateway-ACK wait; it is never final application
delivery.

Direct-to-gateway channel-9 delivery is batched. Every packet in a turn carries
the batch identity, one packet marks the end, and the sender reserves enough
time to receive one gateway batch ACK. Only listed packets become
gateway-accepted; missing entries retain custody for a later channel-9 retry.
For a single accepted packet, the gateway first attempts the ACK in that same
radio turn.

For host-visible click reports, command results, result bundles, survey
discovery reports, and survey pair results, the gateway must complete semantic
validation, reserve the complete host record, commit required durable state,
and commit stream admission before emitting the gateway ACK or semantic
completion response. A malformed payload, stale transaction, full host queue,
or persistence failure remains retryable upstream because it has not been
accepted. The duplicate cache is populated only after acceptance.

Accepted click records are journaled across reset and BLE delivery is
at-least-once: reset after host notification but before journal clear may replay
the exact record. The host suppresses exact replays within its bounded session
cache. This is not an end-to-end exactly-once guarantee across a host-process
restart.

Collection results use collection EACK custody rather than a generic gateway
ACK. A generic ACK cannot complete a collection sender. The EACK snapshot is
durable and immutable across channel-9 attempts, four-copy channel-5 recovery,
and reset until the next collection state is committed.

## Host-owned operation profiles

Experiment sequencing and tunable operation policy belong to the host. An
ordinary GUI operation freezes its target command and one versioned profile,
sends a separately correlated Here-I-Am, waits for that exact successful
terminal, and only then sends the frozen target. The same profile accompanies
both messages so a node that missed the preflight still receives the policy for
the operation it executes.

`TLV_OPERATION_POLICY` (`0xAE`) is repeatable. Every v1 value begins with
`version, family, flags`; the supported families are defined in
[`operation_policy.h`](../firmware/include/operation_policy.h):

| Family | Host-owned values |
| --- | --- |
| Assignment | Expected anchor count, total operation budget, equal randomized response spread |
| Survey discovery | Start delay, slot duration/count, round count, report grace, total operation budget |
| Survey pair | Maximum reruns and maximum parallel pair lanes |

Firmware rejects unknown versions, wrong lengths, overflow, unsafe bounds, and
profiles that cannot fit physical airtime and guard requirements. PHY choice,
measured airtime, retune/guard time, DS-TWR receive bounds, antenna delays,
transport custody, and delayed-transmit quantization remain firmware-owned.
Accepted profiles persist in RAM only; compiled safe defaults return after
reset.

The old nominal-plus-reserve survey horizon is retired. Discovery is one
continuous set of randomized rounds. A pre-RF refusal retries inside the
remaining operation window and does not erase peers already heard, restart the
window, or consume a transmission attempt.

## Anchor survey protocol

Survey discovery and pair ranging are separate correlated stages:

1. The gateway floods `MSG_SURVEY_DISCOVERY_START` with the frozen discovery
   profile and survey identity.
2. In every configured round, each anchor chooses a randomized probe slot and
   listens during the rest of that same continuous round. Anchors deduplicate
   announcements by stable node ID. One directed observation is useful pair
   evidence; reciprocal hearing improves diagnostics but is not required.
3. Each anchor freezes and durably owns one exact
   `MSG_SURVEY_DISCOVERY_REPORT` until the gateway ACKs that packet identity.
   A later survey cannot overwrite pending report custody. The first accepted
   report for an anchor owns its peer set and current-survey reverse hint;
   later exact reports are ACKed but do not rewrite it.
4. The gateway forms pair candidates from accepted directed edges. Pairs may
   share a synchronized round only when their endpoint neighborhoods and known
   reverse relay roots are disjoint. Every pair keeps independent result,
   cleanup, rerun, and deadline custody.
5. In the synchronized path, PREPARE and START carry the nonzero
   `TLV_SURVEY_ROUND_ID` (`0xAF`). START arms the endpoint but does not begin
   ranging. After every endpoint in the round is armed, the gateway broadcasts
   one `CMD_SURVEY_GO` (`0x0105`) with a common future execute delay. Each
   receiver compensates for packet age and derives the same execution instant.
   A GO that reaches a retryable terminal with zero RF starts is regenerated
   with a fresh future instant inside the operation deadline; a permanent
   failure ends the affected work explicitly.
6. `MSG_SURVEY_PAIR_RESULT` carries the survey, endpoints, nonzero round
   generation, sample index/count, distance, and range status. A synchronized
   result is current only when its round generation matches the live batch.
   Delayed results from an earlier rerun cannot complete a later one.

A survey distance is usable geometry only when `RANGE_STATUS` is `RANGE_OK` and
the distance is positive. There is no 50 mm floor. An unusable result may still
be transport-valid and ACKed, but it does not fill a usable sample. A usable
report outranks an unusable duplicate for that sample. Persistent missing or
unusable geometry triggers the configured bounded reruns and then one explicit
pair failure; it never loops forever or cancels successful independent pairs.

Useful partial discovery is retained. Missing anchors, isolated components,
collided probes, or one failed pair are reported explicitly instead of turning
already committed peers and pairs into a false total failure.

## BLE framing boundary

The gateway GATT UUID names are from the device perspective: the host writes
commands to `PACKET_RX_UUID` and subscribes to notifications on
`PACKET_TX_UUID`.

Host command ingress is a byte stream of COBS-encoded shared packets terminated
by `0x00`. A complete COBS frame may span several ATT writes. Direct
gateway-originated host packets use the same framing on egress. Accepted
mesh-originated records use the selective 40-byte-header record format defined
by [Gateway BLE Streaming](<Gateway BLE Streaming.md>), which preserves packet
identity and supports the full 958-byte payload. Both egress forms may be split
across several ATT notifications.

ATT write and notification boundaries are transport chunks, not protocol
boundaries. A host or gateway parser commits nothing until the enclosing COBS
frame or selective stream record is complete and validated. Disconnect,
notification-credit pressure, and queue refusal are explicit transport states;
they must not make an unadmitted record look gateway-accepted.

## Failure semantics

| Condition | Required behavior |
| --- | --- |
| Bad magic, version, length, type, CRC, TLV width, or required field | Reject before protocol mutation |
| Stale session, round, epoch, boot incarnation, or command sequence | Reject without advancing current ownership |
| Exact accepted duplicate | Suppress repeated mutation and payload forwarding, but reproduce the owed ACK |
| Same identity with different payload or semantic flags | Treat as a conflict and fail closed |
| Queue, radio, or persistence refusal before RF starts | Retain custody, apply bounded backoff, and consume no RF opportunity |
| Collision, timeout, or failure after RF starts | Consume the attempt and retry the same immutable packet when policy allows |
| Host-record admission or durable commit failure | Emit no gateway ACK; upstream custody remains retryable |
| Absolute deadline, attempt exhaustion, permanent error, or cancellation | Emit exactly one terminal result and perform any required idempotent cleanup |
| Partial assignment or survey outcome | Preserve useful committed results and report missing or failed parts explicitly |

The communication service terminates every accepted delivery as delivered,
deadline-expired, attempts-exhausted, permanently failed, or explicitly
cancelled. Silence, a zero-attempt “success,” replacement by newer work, and an
ACK emitted before semantic acceptance are protocol failures.

These wire rules, the architecture, and the walkthrough describe the same
current production-candidate line without creating a second behavioral
authority.
