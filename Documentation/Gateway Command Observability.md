# Gateway Command Observability

## Scope

The connected-routing gateway emits compact typed command events to the existing
gateway BLE packet stream. These records cover:

- `CMD_ASSIGN_DISCOVERY_SLOTS` (`0x0104`), used by the host-facing anchor
  enumeration / "Here I Am" workflow.
- `CMD_SURVEY_REACHABILITY` (`0x0100`) and its automatic pair preparation and
  start commands.
- `CMD_FORCE_REDISCOVERY` (`0x000C`) route refresh status.

The records replace neither command results nor survey pair result packets.
They correlate those existing records into a command timeline. Raw gateway log
text is not sent over BLE.

## BLE Envelope

The event is carried as `MSG_GATEWAY_COMMAND_EVENT` (`0x56`) in the existing
gateway BLE stream record (`magic=0x5747`, stream schema version `1`, record
header length `40`). The stream header's payload length and CRC cover the event
payload. A command event is always one fixed 78-byte payload and is never split
into multiple logical records. GATT may deliver the stream record in transport
chunks; the GUI must buffer until `40 + payload_length` bytes are present.
Stack qualification records BLE pressure only after an asynchronous notify has
actually consumed controller credit or a notify submit transiently fails; CCC
being disabled by itself is transport unavailability, not credit-pressure
evidence.

Terminal command events have BLE queue priority `0`, equal to click records and
higher than normal results, surveys, diagnostics, and status. A terminal event
is retained until its complete stream record is acknowledged by the BLE send
callback. After disconnect or backpressure it is replayed with `REPLAY` and
`SNAPSHOT` set. The latest active non-terminal snapshot is replayed after the
GUI enables notifications following reconnect.

The BLE stream itself owns successfully queued terminal records and never
evicts them for an equal- or lower-priority record. Two terminal records that
could not enter that queue are retained separately, matching the gateway's
two-command host ingress bound. This prevents a later same-kind command from
overwriting an unsent terminal from the preceding command.

Every event has a monotonically increasing `event_seq`. Temporary CCC-off,
disconnect, queue pressure, exhausted notification credit, or a transient GATT
submit failure is backpressure and does not increment `lost_event_count`.
Progress snapshots remain pending and may coalesce to the latest safe state;
terminal records remain separately retained. A sequence gap accompanied by a
larger loss count means an event was irreversibly lost, rather than merely
delayed. The GUI must use the terminal event, not the absence of an error event,
to decide success.

The bounded assignment-table publication batch is retained before command-event
creation. It admits one slot mapping at a time only when BLE notification credit
and stream capacity are available, then advances only after the send callback.
Queue pressure, notification stalls, and disconnect therefore leave that exact
semantic event pending without consuming `event_seq` or incrementing
`lost_event_count`. Assignment-table retransmission reuses the same retained
batch; it does not append another set of mappings. Stage 7, stage 8, and the
terminal event remain behind every slot mapping in that batch, including across
reconnect. Click and already-pending terminal records retain BLE priority.

Survey report progress is reconstructed after collection from the gateway's
retained table of at most 50 accepted reports, so a full BLE queue while reports
arrive cannot erase anchor membership. Pair-start and pair-result progress is
admitted one pair at a time. If telemetry has no custody capacity, orchestration
pauses at the next between-pair boundary while command state and radio deadlines
already in flight remain unchanged; it never allocates a 1225-event pair log.

## Event Payload V1

All multibyte integers are little-endian.

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 1 | `schema_version`, exactly `1` |
| 1 | 1 | `record_len`, exactly `78` |
| 2 | 1 | `command_kind` |
| 3 | 1 | `stage` |
| 4 | 1 | flags |
| 5 | 1 | attempt/retry number |
| 6 | 1 | terminal/current `command_status` |
| 7 | 1 | bounded observability reason |
| 8 | 2 | command ID |
| 10 | 2 | gateway route epoch |
| 12 | 4 | host correlation ID |
| 16 | 4 | gateway operation sequence: assignment epoch or survey ID |
| 20 | 4 | original host packet session ID |
| 24 | 2 | original host packet sequence |
| 26 | 2 | reserved, must be zero |
| 28 | 4 | monotonic event sequence |
| 32 | 8 | anchor ID, zero when not applicable |
| 40 | 8 | pair initiator ID, zero when not applicable |
| 48 | 8 | pair responder ID, zero when not applicable |
| 56 | 8 | previous mesh hop ID, zero when unavailable |
| 64 | 2 | current progress count |
| 66 | 2 | total count, zero while unknown |
| 68 | 2 | success count |
| 70 | 2 | failure count |
| 72 | 2 | deduplicated-repeat count |
| 74 | 2 | cumulative BLE enqueue loss count |
| 76 | 1 | mesh hop count, zero when unavailable |
| 77 | 1 | assigned/discovery slot, `255` when unavailable |

Unknown schema versions, record lengths, kinds, stages, flags, statuses,
reasons, or nonzero reserved bytes are malformed. The GUI should discard that
record, preserve its current command state, and surface an unsupported or
malformed telemetry warning. It must not reinterpret unknown values.

## Values

Command kinds:

| Value | Meaning |
|---:|---|
| 1 | anchor enumeration / assignment |
| 2 | anchor-to-anchor survey |
| 3 | gateway route refresh |

Stages:

| Value | Meaning |
|---:|---|
| 1 | accepted |
| 2 | queued for highest-priority gateway command dispatch |
| 3 | dispatching at a safe radio boundary |
| 4 | channel-5 flood attempt |
| 5 | retry/backoff |
| 6 | distinct anchor enumerated; may repeat later with its assigned slot |
| 7 | enumeration collection complete |
| 8 | assignment table or survey pair schedule ready |
| 9 | pair started |
| 10 | pair succeeded |
| 11 | pair failed |
| 12 | command terminal |

Flags are `TERMINAL=0x01`, `SNAPSHOT=0x02`, `REPLAY=0x04`, and
`DUPLICATE=0x08`. Reasons are: `0 none`, `1 invalid request`, `2 busy`,
`3 no anchors`, `4 capacity`, `5 radio`, `6 timeout`, `7 malformed response`,
`8 route unavailable`, `9 retry exhausted`, `10 pair incomplete`, `11 pair
range failed`, `12 aborted`, `13 internal`, and `14 survey radio preparation failed`.

## GUI Correlation

Group the timeline by `(command_kind, correlation_id, host_session_id,
host_seq)`. Use `gateway_sequence` to distinguish the gateway's assignment
epoch or survey ID within that host command. Sort by `event_seq`; replayed
records keep their original event sequence and replace, rather than append to,
the same timeline position.

For enumeration, deduplicate stage-6 records by anchor ID. The first record can
carry previous-hop and hop-count metadata. A later record for the same anchor
can add its assigned slot. The stage-12 record gives the authoritative terminal
enumerated, success/failure, duplicate, and loss counts.

For survey, existing `MSG_SURVEY_DISCOVERY_REPORT` and
`MSG_SURVEY_PAIR_RESULT` records remain the detailed result payloads. Correlate
them by survey ID and anchor or pair IDs. Command events add accepted/queued,
enumeration completion, schedule size, pair start/outcome, retry/backoff, and
terminal totals. `PAIR_INCOMPLETE` means fewer unique sample indices arrived
than requested. `PAIR_RANGE_FAILED` means at least one received sample had a
non-`RANGE_OK` status.

The survey terminal keeps the specific failure class observed during pair
processing; it does not rewrite every failed pair as `PAIR_RANGE_FAILED`.
When several pairs fail differently, the deterministic precedence is
`INTERNAL`, `RETRY_EXHAUSTED`, `ROUTE_UNAVAILABLE`, `RADIO`,
`PAIR_RANGE_FAILED`, then `PAIR_INCOMPLETE`. A survey with one valid discovery
report and no possible pair completes successfully. A survey with zero valid
discovery reports ends with `NO_ANCHORS`; user interfaces should render that
survey-specific condition as "No survey reports were received", because it
does not mean that no powered or normally enumerable anchors exist.

Survey discovery runs four collision-diversified probe opportunities before
the gateway closes collection. A terminal `NO_ANCHORS` reason therefore means
that no unique eligible anchor report arrived across the complete expanded
probe and report horizon; it must not be emitted after only the initial slot.

## Bounds And Scheduling

Enumeration and survey discovery are capped at 50 anchors. Each survey report
is capped at eight peers, and the planner caps each anchor at six pairs. Survey
runtime result accounting is capped at 16 samples per pair. Compile-time guards
bind these capacities and keep the discovery table publisher below a 4 KiB
local-frame budget.

Accepted and queued command progress obtains either BLE-queue custody or the
single retained progress-snapshot custody before command dispatch can proceed.
The retained admission snapshot is stage 2 (`QUEUED`), which is also proof that
stage 1 (`ACCEPTED`) occurred; stage 1 may therefore be coalesced rather than
emitted as a separate record when bounded transport storage is under pressure.
Event flow control never changes channel-5 priority, an already-started radio
deadline, retries, or survey decisions. Pair orchestration pauses only at a safe
between-pair boundary. Temporary transport refusal leaves the event pending and
does not update loss state; only exhausted bounded semantic storage can report
irreversible loss.
