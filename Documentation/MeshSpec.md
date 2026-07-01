Codex, the original recursive route-discovery proposal is close in intent but should not be implemented as written.

The unsafe part is this rule:

If an anchor receives a route request and has no route, it becomes a route seeker itself and starts the same route-finding process.

That creates independent nested discoveries for the same gateway target. In a cold or partitioned mesh, each no-route relay can generate a new request identity, defeating duplicate suppression and making traffic scale badly. The repo should instead implement a bounded same-event flood:

One origin.
One request ID.
One flood epoch.
Relays forward the same request at most a bounded number of times.
Relays do not create child route discoveries for the same gateway target.

This change keeps the existing UWB-owned architecture: channel 5 remains the click/contact lane, channel 9 remains the negotiated payload lane, BLE remains courtesy/debug only, gateway ACK remains the only end-to-end delivery confirmation, and packet age remains the delay-compensation mechanism. The current architecture already says channel 5 carries route discovery/contact refresh, channel 9 carries reports/heartbeats/command results/gateway ACKs after contact, and important gateway-bound packets are not delivered until the gateway ACK returns.

Do not call these floods “Clicks.” In this repo, Click must remain reserved for the user/clicker wake-claim, discovery, schedule, and ranging path. The new generic term is:

flood_epoch

A flood_epoch is a bounded channel-5 control event. It may be used for route solicitation, gateway route advertisement, gateway command broadcast, or maintenance. It must always yield to active click service and required quick channel-5 wake scans.

1. Terminology to use in code and documentation

Use these names consistently:

click service
    Real accepted clicker-originated channel-5 wake/discovery/schedule/ranging work.
    Highest priority after validation.

required channel-5 wake scan
    The existing quick scan needed to avoid missing real UWB wake claims.

channel-5 contact
    Generic channel-5 peer contact for route discovery, route reply, route refresh,
    channel-9 timing negotiation, and command/control.

channel-9 mesh event
    Existing negotiated channel-9 payload window between two neighbors.

flood_epoch
    New bounded channel-5 control dissemination event.
    Used for route solicitation, route advertisement, gateway broadcast command,
    and collection-status broadcasts.

collection_epoch
    New gateway-command result-collection session.
    Used when many or all nodes may need to return COMMAND_RESULT.

parent candidate
    A cached next-hop candidate toward the gateway.

custody ACK
    Hop-local confirmation that the next hop accepted and safely queued/stored
    a packet or result.

gateway ACK
    End-to-end confirmation from the gateway.
    Important packets are complete only after this.

Avoid these names for generic control work:

route click
command click
gateway click
click train for commands

Use:

channel-5 control burst
gateway command flood
route solicitation flood
route advertisement flood
collection-status flood
2. Constants policy

Do not duplicate existing architecture constant values in the new implementation comments or documentation. Refer to existing constants by name.

Existing constants/parameters to reuse by name:

CLICKER_UWB_WAKE_TRAIN
ANCHOR_WAKE_SCAN_INTERVAL
ANCHOR_WAKE_SCAN_WINDOW
ANCHOR_UWB_MESH_RX_INTERVAL
ANCHOR_UWB_MESH_RX_WINDOW
GATEWAY_UWB_MESH_RX_WINDOW
GATEWAY_UWB_MESH_RX_IDLE
GATEWAY_ACK_TIMEOUT
MAX_RETRIES
RETRY_BACKOFF
ROUTE_REDISCOVERY_BUDGET
ROUTE_FRESHNESS_POLICY
DUPLICATE_SUPPRESSION_WINDOW
GATEWAY_COMMAND_RESULT_TIMEOUT
UWB_RANGE_REPLY_DELAY_UUS
CHANNEL9_EVENT_INTERVAL
CHANNEL9_EVENT_WINDOW
CHANNEL9_SUPERVISION_TIMEOUT
CHANNEL9_MAX_MISSED_EVENTS
SURVEY_DISCOVERY_SLOT_COUNT
SURVEY_REPORT_MESH_SLOT_MS

Add these new constants with defaults in the protocol/config layer. These are not replacements for existing timing constants; they only govern the new bounded flood and collection behavior.

#define FLOOD_EPOCH_LOCAL_TTL                 2
#define FLOOD_EPOCH_REGIONAL_TTL              4
#define FLOOD_EPOCH_GLOBAL_TTL                8
#define FLOOD_EPOCH_CRITICAL_TTL              12

#define FLOOD_FORWARD_MAX_NORMAL              1
#define FLOOD_FORWARD_MAX_CRITICAL            2
#define FLOOD_FORWARD_SUPPRESS_AFTER_HEARD    2

#define FLOOD_WAVE_MS                         1400
#define FLOOD_RELAY_BURST_MS                  600
#define FLOOD_RELAY_REPEAT_MS                 40
#define FLOOD_POST_ROOT_GUARD_MS              150

#define C5_POLITE_SNIFF_MS                    6
#define C5_POLITE_BACKOFF_MIN_MS              20
#define C5_POLITE_BACKOFF_MAX_MS              1600
#define C5_POLITE_DEFERRAL_MAX                8

#define RREP_ACK_TIMEOUT_MS                   150
#define RREP_RETRY_COUNT_PER_HOP              4

#define PARENT_CANDIDATE_COUNT                3
#define REVERSE_PATH_CANDIDATE_COUNT          2

#define RELAY_BUSY_RETRY_MIN_MS               500
#define RELAY_BUSY_RETRY_MAX_MS               5000

#define COLLECTION_INITIAL_SPREAD_MIN_MS      30000
#define COLLECTION_INITIAL_SPREAD_PER_NODE_MS 300
#define COLLECTION_MISSING_SPREAD_PER_NODE_MS 500

#define COLLECTION_RETRY_ROUND_0_MS           15000
#define COLLECTION_RETRY_ROUND_1_MS           30000
#define COLLECTION_RETRY_ROUND_2_MS           60000
#define COLLECTION_RETRY_ROUND_3_MS           120000
#define COLLECTION_RETRY_ROUND_STEADY_MS      300000
#define COLLECTION_RETRY_JITTER_PERCENT       25

#define COLLECTION_RESULT_INLINE_C5_MAX_BYTES 32
#define COLLECTION_BUNDLE_TARGET_BYTES        512
#define COLLECTION_BUNDLE_MAX_RECORDS         8

#define COMMAND_RESULT_EXPIRY_DEFAULT_S       86400
#define ROUTE_PARENT_HOLDDOWN_S               30

If the repo already has similarly named constants, reuse the existing names and only add missing ones.

3. Scheduling priority

Do not change the architecture’s radio priority order.

All new route, command, and collection behavior must fit under the existing rule:

1. Active channel-5 click service
2. Required quick channel-5 wake scan
3. Channel-5 route contact or route timing refresh
4. Negotiated channel-9 mesh payload event
5. Retained sleep

If a flood_epoch, route reply, command result, or channel-9 transfer conflicts with active click service, defer the mesh/control work. Do not skip click service to complete mesh payload. This matches the current single-radio scheduling model, where channel-9 work may be clipped/skipped/retried and channel-5 wake/contact work is not skipped just to finish payload traffic.

All timers added by this change must be preemption-aware:

If active click service or required channel-5 wake scanning interrupts a control or payload operation:
    - do not immediately mark route failed;
    - extend or pause that operation's timeout;
    - retry according to its retry class;
    - preserve packet/result state.

Do not add a generic “Click interrupt” type for all control work. Use:

c5_preempted_by_click_service
c5_preempted_by_required_wake_scan
c9_preempted_by_c5_contact
c9_preempted_by_click_service

Expose these in heartbeat/status telemetry if not already present.

4. Data model changes

Add or extend these tables.

4.1 Parent candidate table

Each anchor keeps up to PARENT_CANDIDATE_COUNT gateway parents.

struct mesh_parent_candidate {
    node_id_t next_hop;
    gateway_id_t gateway_id;
    uint16_t route_epoch;

    uint8_t hop_count;
    uint8_t path_quality_min;
    uint16_t route_cost;

    uint8_t relay_capacity_state;
    uint16_t queue_free_hint;
    uint8_t channel9_busy_hint;

    bool channel9_timing_valid;
    mesh_time_t last_observed;
    mesh_time_t last_success;
    mesh_time_t hold_down_until;
};

Keep the current route-cost rule as the baseline:

cost = hop_count * 100 + (100 - link_quality)

Do not replace the architecture’s hop-first behavior. Add capacity only as a tie-breaker or penalty among otherwise comparable candidates. The architecture currently says gateway-bound route selection is primarily by fewer hops, with link quality choosing between same-hop routes.

Recommended parent ordering:

1. lower route_cost
2. route not in hold-down
3. valid channel-9 timing
4. better relay_capacity_state
5. more recent successful gateway ACK path
6. lower next_hop ID
4.2 Flood duplicate cache

Extend duplicate suppression for flood epochs.

struct flood_seen_entry {
    gateway_id_t gateway_id;
    uint16_t gateway_epoch;
    uint32_t flood_epoch_id;
    uint8_t flood_type;

    node_id_t origin_id;
    uint32_t origin_request_id;

    uint8_t best_hop_count;
    uint16_t best_metric;
    node_id_t best_previous_hop;
    node_id_t backup_previous_hop;

    uint8_t forward_count;
    mesh_time_t expires_at;
};

Use the existing duplicate suppression window for ordinary mesh packet identities. For command/result collection, use command/result identity until command expiry or gateway collection close. The current 60-second duplicate cache remains appropriate for normal packet dedupe, but it is too short to identify all-node command results that may retry for minutes or hours.

4.3 Persistent outbox

Important packets and command results need persistent or restart-tolerant state.

struct persistent_outbox_record {
    packet_id_t packet_id;
    gateway_id_t gateway_id;
    uint8_t packet_class;

    uint32_t created_uptime_ms;
    uint32_t age_ms_saturating;

    uint8_t priority;
    uint8_t retry_round;
    uint8_t selected_parent_index;

    bool custody_accepted;
    node_id_t custody_parent;

    bool gateway_acked;
    uint32_t expiry_s;

    uint16_t payload_crc;
    uint16_t payload_len;
};

A record may be deleted only when:

gateway ACK received
application expiry reached
gateway sends explicit cancel/close
operator/debug policy deletes it

For command results, use COMMAND_RESULT_EXPIRY_DEFAULT_S unless the command specifies a stricter expiry.

5. Bounded route solicitation flood

Replace recursive discovery with a bounded same-event route solicitation.

5.1 Route solicitation message

Add or revise:

struct route_solicit {
    mesh_header_t hdr;

    gateway_id_t target_gateway;
    uint16_t requested_gateway_epoch;

    node_id_t origin_id;
    uint32_t request_id;

    uint8_t ttl;
    uint8_t hop_count;

    uint16_t accumulated_cost;
    uint8_t path_quality_min;

    uint8_t min_capacity_state;
    uint8_t flags;

    uint32_t flood_epoch_id;
    uint16_t flood_profile_version;
    uint32_t slot_seed;
};

The identity is:

target_gateway
origin_id
request_id
flood_epoch_id
5.2 Sender behavior

When a node has a gateway-bound packet and no usable parent:

1. Try existing parent candidates in order.
2. If channel-9 timing is stale, refresh channel-5 contact first.
3. If no candidate can be used, start bounded route solicitation.
4. Use existing Route rediscovery budget and Retry backoff by name.
5. Scope attempts:
       attempt 0: FLOOD_EPOCH_LOCAL_TTL
       attempt 1: FLOOD_EPOCH_REGIONAL_TTL
       attempt 2+: FLOOD_EPOCH_GLOBAL_TTL
       critical/maintenance only: FLOOD_EPOCH_CRITICAL_TTL

Do not immediately wake or involve the whole mesh for ordinary route failure.

5.3 Relay behavior

On receive ROUTE_SOLICIT:

1. Validate network, gateway, epoch, CRC, ttl, and packet age.
2. If active click service is pending or running, defer route processing.
3. Look up flood_seen_entry.
4. If already seen and the new path is not better:
       suppress.
5. If already seen and the new path is meaningfully better:
       update reverse-path candidate but do not reset the whole flood.
6. Record best_previous_hop and backup_previous_hop.
7. If this node has a usable parent route to the gateway:
       schedule ROUTE_REPLY.
8. If ttl remains:
       forward the same ROUTE_SOLICIT, with same origin_id/request_id/flood_epoch_id.
9. Never create a fresh child route request for the same target gateway.

A “meaningfully better” path means:

lower hop_count by at least one
or lower accumulated_cost by FLOOD_BETTER_METRIC_MARGIN if implemented
or better path_quality_min with equal hop_count

If FLOOD_BETTER_METRIC_MARGIN does not exist yet, add it as a percentage constant.

5.4 Forwarding schedule

Forwarding should be blind-ish but bounded.

wave_index = received_hop_count + 1

forward_due =
    flood_start
  + wave_index * FLOOD_WAVE_MS
  + hash(node_id, flood_epoch_id, slot_seed) % available_wave_jitter

During its relay burst, a node repeats the frame every FLOOD_RELAY_REPEAT_MS, but each repeat uses channel-5 politeness:

for each repeat opportunity:
    listen for C5_POLITE_SNIFF_MS
    if quiet:
        transmit
    else:
        skip this repeat

For normal floods:

forward at most FLOOD_FORWARD_MAX_NORMAL times.

For critical gateway floods:

forward at most FLOOD_FORWARD_MAX_CRITICAL times,
but suppress the second forward if at least FLOOD_FORWARD_SUPPRESS_AFTER_HEARD
equivalent forwards were heard.

Never extend a broadcast flood indefinitely because channel 5 was busy. Floods must be bounded.

6. Route reply reliability

The original proposal assumed that reverse-path nodes remain in continuous channel-5 receive and therefore route replies need no ACK. Do not implement that assumption.

Route replies are small and important. They should be hop-by-hop ACKed on channel 5 unless an already-negotiated channel-9 timing path is known-good and explicitly selected for the reply.

6.1 Route reply message
struct route_reply {
    mesh_header_t hdr;

    gateway_id_t gateway_id;
    uint16_t gateway_epoch;

    node_id_t origin_id;
    uint32_t request_id;
    uint32_t flood_epoch_id;

    node_id_t responder_id;
    node_id_t next_hop_to_gateway;

    uint8_t hop_count_to_gateway;
    uint8_t path_quality_min;
    uint16_t route_cost;

    uint8_t relay_capacity_state;
    uint16_t queue_free_hint;
    uint8_t channel9_busy_hint;

    uint16_t reply_nonce;
    uint16_t metric_crc;
};

ACK:

struct route_reply_ack {
    mesh_header_t hdr;

    node_id_t origin_id;
    uint32_t request_id;
    uint32_t flood_epoch_id;

    node_id_t responder_id;
    uint16_t reply_nonce;
    uint16_t metric_crc;
};
6.2 Route reply behavior

At each reverse hop:

1. Send ROUTE_REPLY to best_previous_hop.
2. Wait RREP_ACK_TIMEOUT_MS.
3. Retry up to RREP_RETRY_COUNT_PER_HOP.
4. If best_previous_hop fails and backup_previous_hop exists:
       try backup_previous_hop.
5. If active click service or required channel-5 wake scan preempts the attempt:
       extend/pause the route-reply wait rather than invalidating route.
6. If all reverse paths fail:
       drop reply and let origin retry according to route rediscovery budget.

A node may install the gateway route when it receives a valid route reply, but it should not delete existing alternatives. Store it as a parent candidate and let route selection choose.

7. Gateway route advertisement flood

Add a gateway-originated route advertisement that opportunistically seeds parent candidates. This is not a hard requirement for every packet and should not become chatty.

struct gateway_route_adv {
    mesh_header_t hdr;

    gateway_id_t gateway_id;
    uint16_t gateway_epoch;
    uint32_t gateway_route_seq;

    uint8_t hop_count;
    uint8_t path_quality_min;
    uint16_t route_cost;

    uint8_t gateway_capacity_state;
    uint16_t flood_profile_version;
    uint32_t flood_epoch_id;
    uint32_t slot_seed;
};

Gateway behavior:

1. Send route advertisements during startup, after route/profile changes,
   after force rediscovery, and periodically at a low maintenance rate.
2. Use bounded flood_epoch rules.
3. Do not send route advertisements so often that they compete with click service.

Anchor behavior:

1. Treat a valid gateway_route_adv as a parent candidate update.
2. Compute cost using existing route-cost semantics.
3. Forward the advertisement only if ttl remains and duplicate suppression allows it.
4. Suppress forwarding if already heard enough equivalent advertisements.
5. Do not clear old routes just because an advertisement was missed.

Routes still follow the existing freshness policy: usable until replaced, explicitly cleared, or proven stale by delivery failure. Do not add age-only expiry to routes unless separately requested. The current architecture explicitly separates route knowledge from channel-9 timing freshness.

8. Relay capacity behavior

Replace the hard rule “relay with pending packets must not accept new route request” with a capacity-state rule.

Add:

enum relay_capacity_state {
    RELAY_CAP_GREEN,
    RELAY_CAP_YELLOW,
    RELAY_CAP_RED,
    RELAY_CAP_BLACK,
};

Use:

GREEN:
    Accept route replies, route advertisements, and result custody normally.

YELLOW:
    Accept, but advertise capacity penalty.
    May return RELAY_BUSY with retry_after.

RED:
    Do not accept ordinary new custody.
    Still forward critical gateway floods and route control if safe.

BLACK:
    Unavailable for relay except local mandatory control.

For compatibility with the current conservative relay behavior, a relay that is already waiting for a gateway-bound confirmation may still decline new custody. But it should decline explicitly:

struct relay_busy {
    mesh_header_t hdr;

    packet_id_t packet_id;
    uint16_t retry_after_ms;
    uint8_t capacity_state;
    node_id_t optional_alternate_parent;
};

This is better than silently dropping, because the upstream node can back off, try another parent, or keep the packet queued.

The current architecture says a busy relay may drop new packets and the previous sender will retry. Keep that as a safe fallback, but prefer explicit RELAY_BUSY where the packet identity is known.

9. Gateway broadcast command to all nodes

The current gateway command path is single-command and target-oriented. Keep the existing one-outstanding-command behavior for interactive USB commands, but add a separate collection_epoch mode for all-node commands. The current architecture already sends commands through the mesh envelope and returns COMMAND_RESULT, with COMMAND_BUSY for a second USB command while one is outstanding.

Add command scope:

enum command_scope {
    CMD_SCOPE_SINGLE_NODE,
    CMD_SCOPE_GROUP,
    CMD_SCOPE_ALL_REGISTERED,
    CMD_SCOPE_ALL_HEARD,
};

Add response mode:

enum command_response_mode {
    CMD_RESPONSE_NONE,
    CMD_RESPONSE_ACK_ONLY,
    CMD_RESPONSE_SMALL_RESULT,
    CMD_RESPONSE_LARGE_RESULT,
};

Add:

struct gateway_command_flood {
    mesh_header_t hdr;

    gateway_id_t gateway_id;
    uint16_t gateway_epoch;
    uint32_t command_seq;
    uint32_t flood_epoch_id;

    enum command_scope scope;
    enum command_response_mode response_mode;

    uint16_t membership_epoch;
    uint16_t expected_node_count;

    uint8_t ttl;
    uint8_t priority;

    uint32_t execute_delay_ms;
    uint32_t command_expiry_s;

    uint32_t collection_epoch_id;
    uint32_t collection_slot_seed;

    uint16_t payload_len;
    uint16_t payload_crc;
    uint8_t payload[];
};

Behavior:

1. Gateway sends gateway_command_flood as a bounded flood_epoch.
2. Anchors validate network/gateway/epoch/scope/CRC.
3. Anchors store command_seq in command duplicate cache.
4. Anchors execute the command at the requested delay or immediately.
5. Anchors forward the same flood event at most the bounded number of times.
6. Anchors never execute the same command_seq twice unless explicitly marked repeatable.
7. Anchors create COMMAND_RESULT only if response_mode requires it.

CMD_SCOPE_ALL_REGISTERED means the gateway expects results from a known membership roster. CMD_SCOPE_ALL_HEARD is best-effort and cannot prove that an unknown silent node was missed.

For strict “all nodes responded” semantics, use:

CMD_SCOPE_ALL_REGISTERED + membership_epoch
10. Scheduled result collection for all-node commands

Do not allow every node to immediately return COMMAND_RESULT after an all-node command. Use a scheduled collection_epoch.

10.1 Result identity
struct command_result_id {
    gateway_id_t gateway_id;
    uint16_t gateway_epoch;
    uint32_t command_seq;
    node_id_t node_id;
    uint32_t node_boot_counter;
    uint16_t result_seq;
};

Every COMMAND_RESULT includes this identity and a CRC. The gateway deduplicates by this identity.

10.2 Initial send schedule

Each node computes:

collection_spread_ms =
    max(COLLECTION_INITIAL_SPREAD_MIN_MS,
        expected_node_count * COLLECTION_INITIAL_SPREAD_PER_NODE_MS)

initial_due =
    command_flood_end
  + hash(node_id, command_seq, collection_slot_seed) % collection_spread_ms

Nodes should not all transmit at command_flood_end.

If the node has a usable parent, it sends through the normal routed path. If channel-9 timing exists, use channel 9. If not, refresh channel-5 contact first. Small results may be carried inline on channel 5 if they fit under COLLECTION_RESULT_INLINE_C5_MAX_BYTES; larger results use channel-9 custody transfer.

10.3 Result offer/grant

For large results, use offer/grant before occupying channel 9.

struct result_offer {
    mesh_header_t hdr;

    struct command_result_id result_id;
    uint16_t result_len;
    uint16_t result_crc;
    uint8_t priority;
};

Parent responds:

struct result_grant {
    mesh_header_t hdr;

    struct command_result_id result_id;
    uint8_t granted_channel;
    uint16_t max_bytes;
    uint32_t event_offset_hint;
};

or:

struct result_busy {
    mesh_header_t hdr;

    struct command_result_id result_id;
    uint16_t retry_after_ms;
    uint8_t capacity_state;
    node_id_t optional_alternate_parent;
};
10.4 Custody and gateway ACK

A relay must not send custody ACK until it has safely stored or reserved space for the result. Custody ACK means only:

The next hop accepted responsibility.

It does not mean:

The gateway received the result.

The original result source should keep the persistent result record until the gateway ACKs that result or the collection closes/expands according to command policy.

The architecture already treats reports, survey results, heartbeats, and command results as delivered only when the gateway ACK returns, so this change extends that rule to all-node command collection rather than replacing it.

11. Result bundling at relays

Relays should bundle child results to reduce upstream channel-9 events.

struct result_bundle {
    mesh_header_t hdr;

    gateway_id_t gateway_id;
    uint16_t gateway_epoch;
    uint32_t command_seq;
    uint32_t collection_epoch_id;

    uint16_t bundle_id;
    uint8_t record_count;
    uint16_t bundle_crc;

    struct command_result_record records[];
};

Limits:

Target bundle size: COLLECTION_BUNDLE_TARGET_BYTES
Maximum records:    COLLECTION_BUNDLE_MAX_RECORDS

Relay behavior:

1. Hold accepted child results briefly to bundle if safe.
2. Do not wait so long that gateway ACK timeout behavior becomes misleading.
3. Forward bundle toward parent using channel 9 when timing is valid.
4. If channel-9 timing is stale, refresh channel-5 contact.
5. Keep custody state until upstream custody ACK.
6. Keep enough identity data to suppress duplicates.
12. Gateway collection EACK / missing report

Add a gateway collection status broadcast. This is separate from ordinary per-packet gateway ACKs.

struct gateway_collection_eack {
    mesh_header_t hdr;

    gateway_id_t gateway_id;
    uint16_t gateway_epoch;
    uint32_t command_seq;
    uint32_t collection_epoch_id;

    uint16_t membership_epoch;
    uint16_t expected_count;
    uint16_t received_count;

    uint8_t eack_format;
    uint8_t retry_round;

    uint32_t next_retry_spread_ms;
    bool collection_open;

    uint8_t payload[];
};

Supported eack_format:

EACK_FORMAT_ROSTER_BITMAP
EACK_FORMAT_EXPLICIT_RECEIVED_LIST
EACK_FORMAT_EXPLICIT_MISSING_LIST

Do not use a Bloom filter for strict result delivery. A Bloom false positive could cause a node to believe its result arrived when it did not.

Gateway behavior:

1. Track expected nodes for membership_epoch.
2. Track received result IDs.
3. Deduplicate duplicates.
4. Periodically flood gateway_collection_eack.
5. If many nodes are missing, send roster bitmap.
6. If few nodes are missing, send explicit missing list.
7. Close collection only when:
       all expected results received,
       command expiry reached,
       or operator/server cancels it.

Node behavior:

If my result is ACKed in EACK:
    drop active retry state.

If my result is missing and collection_open:
    retry in the next retry round.

If I did not hear any EACK:
    continue retrying according to collection retry schedule.

If collection closed and my result is still not ACKed:
    keep diagnostic state, but stop RF retry unless command policy says persistent-critical.
13. Generous but non-spammy retry logic

The retry logic should be patient. “Eventually arrive” means the node keeps custody state and retries across route changes, sleep, congestion, and click-service preemption. It does not mean blind immediate retransmission.

For command results:

Round 0:
    initial hashed collection slot.

Round 1:
    COLLECTION_RETRY_ROUND_0_MS ± COLLECTION_RETRY_JITTER_PERCENT.

Round 2:
    COLLECTION_RETRY_ROUND_1_MS ± COLLECTION_RETRY_JITTER_PERCENT.

Round 3:
    COLLECTION_RETRY_ROUND_2_MS ± COLLECTION_RETRY_JITTER_PERCENT.

Round 4:
    COLLECTION_RETRY_ROUND_3_MS ± COLLECTION_RETRY_JITTER_PERCENT.

Round 5+:
    COLLECTION_RETRY_ROUND_STEADY_MS ± COLLECTION_RETRY_JITTER_PERCENT.

At each retry round:

1. If current parent has custody or upstream progress was recently seen:
       wait for gateway EACK unless the parent is proven stale.

2. If no custody parent exists:
       try primary parent.

3. If primary returns RESULT_BUSY:
       wait retry_after with jitter.

4. If primary fails:
       try backup parent.

5. If all parents fail:
       run route solicitation:
           local TTL,
           regional TTL,
           global TTL,
           critical TTL only for critical command/payload class.

6. If active click service preempts:
       pause/extend timers; do not count that as route failure.

7. Continue until gateway EACK, command expiry, explicit cancel, or storage policy expiry.

Do not reset all nodes into another global result storm after each EACK. Only missing nodes retry. The collection EACK must suppress successful nodes.

14. Route failure and local repair

When channel-9 send or gateway ACK fails:

1. Use existing gateway ACK timeout and hop-progress ACK behavior by name.
2. If downstream progress continues, keep waiting according to existing rules.
3. If selected parent exhausts retry policy, place it in ROUTE_PARENT_HOLDDOWN_S.
4. Try backup parent candidate.
5. If no parent works, run bounded route solicitation.
6. Do not global flood merely because one next hop failed.
7. Preserve the packet/result in outbox.

Keep CMD_FORCE_REDISCOVERY, but make it use the new machinery:

1. Target first attempts to report command result using current route.
2. Then invalidates upstream/downlink/channel-9 timing state.
3. Then starts bounded route solicitation toward the gateway.
4. Gateway may also send a gateway_route_adv flood afterward.

This matches the current maintenance intent while avoiding recursive route storms. The architecture already defines CMD_FORCE_REDISCOVERY as gateway-originated maintenance that reports first, invalidates route/timing state, and starts bounded rediscovery.

15. Channel-5 politeness rules

Every channel-5 transmit path added by this change must use politeness.

For reliable unicast control:

1. Listen for C5_POLITE_SNIFF_MS.
2. If quiet, transmit.
3. If busy, exponential/random backoff up to C5_POLITE_BACKOFF_MAX_MS.
4. Retry up to C5_POLITE_DEFERRAL_MAX.
5. If still busy, return RETRY_LATER to caller.

For repeated flood bursts:

1. Listen before each repeat.
2. If quiet, transmit that repeat.
3. If busy, skip that repeat.
4. Do not extend the flood burst indefinitely.

This distinction is important:

Reliable unicast may wait and retry.
Flood broadcast must remain bounded.
16. Packet age handling

All forwarded route/control/payload messages that use the mesh envelope must continue updating packet age.

For gateway command floods with scheduled execution:

effective_execute_delay =
    command_execute_delay - packet_age

Clamp at zero if already late.

For collection results:

result packet age starts when the result is created.
Relays add queue time before forwarding.
Retransmissions include waiting time.
Gateway uses packet age to understand stale results.

This follows the current architecture’s packet-age model, where packets carry saturating millisecond age and relays add queue/relay time so the gateway can reason about delay without synchronized clocks.

17. Gateway broadcast command flow

Implement this flow for all-node gateway commands:

Gateway:
    create command_seq
    create flood_epoch_id
    choose scope
    choose membership_epoch
    choose expected_node_count
    choose response_mode
    choose collection_epoch_id if response required
    send gateway_command_flood using bounded flood_epoch

Anchor on receive:
    validate command
    store command_seen
    forward bounded flood if ttl remains
    execute once
    if response required:
        create command_result
        persist result
        schedule result in collection_epoch

Anchor result send:
    wait hashed collection slot
    choose best parent
    if small result and C5 path appropriate:
        send inline with ACK/custody
    else:
        send result_offer
        receive result_grant
        send result_data over channel 9
    wait custody ACK
    wait gateway EACK / gateway ACK

Relay:
    custody-ACK only after safe storage/reservation
    bundle results opportunistically
    forward upward through parent candidates
    handle busy with result_busy/retry_after

Gateway:
    receive result or bundle
    dedupe by command_result_id
    update collection state
    periodically flood gateway_collection_eack
    close when complete/expired/cancelled
18. Gateway route advertisement flow

Implement this as a maintenance optimization, not as a constant flood.

Gateway:
    send gateway_route_adv after startup/profile/route epoch changes
    send gateway_route_adv after force rediscovery
    send low-rate maintenance adv if enabled

Anchor:
    receive adv
    update parent candidate
    forward bounded flood if useful
    suppress duplicates
    do not delete old usable route solely because adv was missed

This makes ordinary reporting cheaper because many anchors will already have parent candidates before a packet arrives, but it does not require constant beaconing.

19. Route solicitation flow

Implement this when a node actually has a packet and no usable parent.

Sender:
    packet queued
    no usable parent
    start route_solicit flood_epoch with local TTL

Relay:
    record reverse path
    forward same route_solicit if ttl remains
    if has usable gateway parent, send route_reply

Responder:
    route_reply follows reverse path
    each hop requires route_reply_ack
    each hop may install/update parent candidate

Origin:
    collect candidate replies within route-discovery budget
    choose best route by existing route_cost plus capacity tie-breakers
    negotiate/refresh channel-9 timing if needed
    send packet

Do not implement recursive child discovery.

20. Failure modes this change must avoid

Add tests or bench scripts for these.

Cold mesh:
    No anchor has a parent route.
    Expected: one bounded flood per route attempt, not nested recursive floods.

Dense mesh:
    Many anchors can reply.
    Expected: reply jitter/suppression prevents route reply storm.

All-node command:
    Every node produces COMMAND_RESULT.
    Expected: result transmissions spread by collection_epoch hashing.

Missing EACK:
    Node result was delivered to relay but gateway EACK was not heard.
    Expected: node retains state and retries later, not immediately spam.

Busy relay:
    Relay cannot accept custody.
    Expected: RELAY_BUSY with retry_after or upstream tries backup parent.

Click service during channel-9 result:
    Active click service preempts mesh.
    Expected: channel-9 session pauses/retries; route is not instantly invalidated.

Required wake scan during control:
    Required channel-5 wake scan interrupts channel-9/control.
    Expected: timeout extension, not route failure.

Stale channel-9 timing:
    Route is still known but timing expired.
    Expected: channel-5 contact refresh before payload.

Duplicate command flood:
    Node hears the same command through multiple relays.
    Expected: execute once, forward boundedly.

Duplicate result:
    Gateway receives same result through multiple paths.
    Expected: dedupe by command_result_id.

Node reboot:
    Sequence numbers may repeat.
    Expected: boot counter prevents duplicate confusion.

Gateway roster mismatch:
    All-registered command uses old membership.
    Expected: membership_epoch defines who is expected.

Partition:
    Node cannot reach gateway for a long time.
    Expected: persistent result remains queued until expiry/cancel/connectivity returns.
21. Implementation checklist for Codex

Apply the change in this order.

1. Add terminology/comments:
       flood_epoch
       collection_epoch
       parent_candidate
       custody ACK
       gateway EACK

2. Add new constants:
       flood constants
       collection constants
       route reply ACK constants
       capacity constants

3. Extend protocol fields:
       flood_epoch_id
       gateway_epoch
       command_seq
       collection_epoch_id
       slot_seed
       node_boot_counter where needed

4. Add parent candidate table:
       keep top PARENT_CANDIDATE_COUNT candidates

5. Replace recursive route discovery:
       same-event forwarding only
       no child route request for same gateway target

6. Add route reply ACK:
       ROUTE_REPLY
       ROUTE_REPLY_ACK
       retry/backup reverse path

7. Add gateway route advertisement:
       bounded flood update of parent candidates

8. Add relay capacity:
       GREEN/YELLOW/RED/BLACK
       RELAY_BUSY / RESULT_BUSY with retry_after

9. Add all-node command scope:
       CMD_SCOPE_ALL_REGISTERED
       CMD_SCOPE_ALL_HEARD
       collection_epoch fields

10. Add collection result scheduling:
       hashed initial spread
       retry rounds
       persistent result state

11. Add gateway collection EACK:
       roster bitmap
       explicit missing list
       explicit received list

12. Add result bundling:
       result_bundle
       gateway dedupe

13. Update telemetry:
       c5 preemptions
       c9 preemptions
       flood suppressions
       route reply retries
       collection pending count
       result duplicate count
       busy responses
       parent hold-downs

14. Add tests:
       cold mesh
       dense mesh
       all-node command
       click-service preemption
       route reply loss
       relay busy
       missing EACK
       duplicate command/result
22. Acceptance criteria

The change is complete when these are true:

A no-route relay never starts an independent child route discovery for the same gateway request.

A route request has one origin_id/request_id/flood_epoch_id identity across all relays.

Each relay forwards a flood event only within configured bounds.

Route replies are ACKed hop by hop or explicitly retried/failed.

A gateway command can target all registered nodes.

All responding nodes compute deterministic spread slots instead of replying immediately.

Command results persist until gateway ACK/EACK, command expiry, or explicit cancel.

Gateway can broadcast collection EACK/missing status.

Successful nodes stop retrying after EACK.

Missing nodes retry generously with jitter.

Busy relays respond with retry_after where possible.

Click service and required channel-5 wake scans preempt mesh/control safely.

Channel-9 timing stale does not delete route knowledge; it triggers channel-5 contact refresh.

Existing architecture constants are referenced by name, not duplicated with new values.
23. Final design rule

The repo should follow this rule everywhere:

Flood outward rarely, boundedly, and idempotently.
Converge inward patiently, scheduled, custody-ACKed, and gateway-ACKed.

That gives the simplicity of blind-ish gateway-controlled flooding without the recursive amplification failure mode, and it gives all-node command results a realistic path back to the gateway without every node transmitting at once.

24. Addendum: channel-5 contact, finite channel-9 events, and capacity hint lifetime

This addendum clarifies how the bounded flood and collection machinery must use the existing wake/contact architecture. It is part of the implementation contract.

24.1 Channel-5 exchanges with sleeping peers must begin with the existing wake mechanism

Any channel-5 exchange that expects a sleeping peer to receive MUST begin with the existing UWB wake train / wake-claim mechanism.

This applies particularly to:

route solicitation to a sleeping peer
route reply to a sleeping reverse-path peer
route/contact refresh
gateway command flood forwarding to sleeping anchors
collection-status flood forwarding
result offer to a sleeping parent
channel-9 timing negotiation with a sleeping parent

Do not invent a new "route wake," "command wake," or "collection wake" mechanism. Use the existing UWB wake train / wake-claim path by name.

Once peers are awake and inside an accepted channel-5 contact/control exchange, every individual frame inside that exchange does not need a fresh full wake train.

Correct behavior:

sleeping peer expected:
    begin with existing UWB wake train / wake-claim mechanism

peer already awake inside accepted C5 contact/control exchange:
    send the next control frame using normal C5 politeness and exchange timing

contact/control exchange closed or timed out:
    future sleeping-peer contact must wake again

Do not accidentally add a full wake train before every ROUTE_REPLY, ROUTE_REPLY_ACK, RESULT_OFFER, RESULT_GRANT, or RESULT_BUSY frame once both peers are already awake inside the same accepted exchange. That would waste energy and stretch control latency.

24.2 Add explicit C5 contact state

Model channel-5 control as a bounded contact state, not as isolated frames.

Add a state similar to:

```
enum c5_contact_state {
    C5_CONTACT_NONE,
    C5_CONTACT_WAKE_PENDING,
    C5_CONTACT_AWAKE_ACCEPTED,
    C5_CONTACT_EXCHANGE_ACTIVE,
    C5_CONTACT_CLOSING,
};
```

Each active contact should know:

```
struct c5_contact_context {
    node_id_t peer_id;
    uint32_t contact_id;
    uint8_t purpose;

    bool peer_was_woken;
    bool accepted;

    mesh_time_t opened_at;
    mesh_time_t last_frame_at;
    mesh_time_t expires_at;
};
```

Relevant purposes:

route_solicit
route_reply
route_contact_refresh
gateway_command_flood
collection_eack_flood
result_offer_grant
channel9_timing_negotiation

The important rule is:

C5_CONTACT_NONE + sleeping peer expected:
    wake train / wake-claim first

C5_CONTACT_EXCHANGE_ACTIVE:
    no fresh wake train per individual frame

24.3 Channel-9 mesh events are finite payload windows

A channel-9 mesh event is not an open-ended session. It is a finite negotiated payload window.

It closes when any of these happens:

1. The granted payload transfer completes and the required hop/custody ACK completes.
2. The event window expires.
3. The receiver returns BUSY or RETRY_LATER.
4. The event is preempted by higher-priority channel-5 work.
5. The local scheduler must return to required channel-5 wake scan or active click service.

A channel-9 event MUST NOT remain open waiting for gateway ACK or collection EACK.

Gateway ACK/EACK is end-to-end delivery confirmation. It may arrive later through a future channel-9 event, a refreshed channel-5 contact followed by channel-9 timing, or another routed path. It must not hold the original channel-9 payload window open.

Correct interpretation:

hop/custody ACK:
    closes local payload responsibility for this event

gateway ACK / gateway collection EACK:
    closes source-level delivery state, but not the channel-9 event

24.4 Update channel-9 event state machine

Update the channel-9 event state machine to make finite payload windows explicit.

Suggested states:

```
enum ch9_event_state {
    CH9_EVENT_NONE,
    CH9_EVENT_GRANTED,
    CH9_EVENT_TX_PAYLOAD,
    CH9_EVENT_WAIT_CUSTODY_ACK,
    CH9_EVENT_COMPLETE,
    CH9_EVENT_BUSY_RETRY_LATER,
    CH9_EVENT_WINDOW_EXPIRED,
    CH9_EVENT_PREEMPTED_BY_C5,
};
```

Close the event on:

CH9_EVENT_COMPLETE
CH9_EVENT_BUSY_RETRY_LATER
CH9_EVENT_WINDOW_EXPIRED
CH9_EVENT_PREEMPTED_BY_C5

Do not add:

CH9_EVENT_WAIT_GATEWAY_ACK
CH9_EVENT_WAIT_EACK

Those belong to the persistent outbox / collection state machine, not the channel-9 event state machine.

24.5 Where this matters most

Route replies:

If a reverse-path peer is sleeping or may have returned to low-duty scan, the reply path must first re-establish channel-5 contact using the existing wake mechanism.

Inside one accepted route-reply exchange, these must not each trigger a fresh full wake train:

ROUTE_REPLY
ROUTE_REPLY_ACK
backup reverse path retry metadata

Result offer/grant:

If a child wants to send a command result to a sleeping parent:

1. Wake/contact parent first.
2. Send RESULT_OFFER.
3. Receive RESULT_GRANT or RESULT_BUSY.
4. Use an optional channel-9 event if granted.

Once the parent accepted contact, RESULT_OFFER, RESULT_GRANT, and RESULT_BUSY are normal frames inside the contact exchange.

Gateway command and collection floods:

A relay forwarding a gateway command or collection EACK toward sleeping anchors must use the existing wake train / wake-claim mechanism for the beginning of that channel-5 flood/contact opportunity.

Do not rename this as a Click. It is still channel-5 control using the existing wake mechanism, not click service.

Channel-9 result delivery:

A result transfer over channel 9 ends when the result or bundle is transferred and the required custody ACK finishes, or the event closes for one of the listed reasons.

The source or relay then waits for gateway ACK/EACK in persistent delivery state, not inside the channel-9 event.

24.6 Relay capacity hints are short-lived

Relay capacity state is only a short-lived hint. It is not route truth.

Every advertised or observed capacity state MUST carry one of:

observation_time
valid_until
validity_interval

Add or confirm fields like:

```
struct relay_capacity_hint {
    uint8_t capacity_state;          // GREEN/YELLOW/RED/BLACK/UNKNOWN
    uint16_t queue_free_hint;
    uint8_t channel9_busy_hint;

    mesh_time_t observed_at;
    mesh_time_t valid_until;
};
```

When the capacity hint expires:

parent route remains usable
route candidate remains in parent table
capacity_state becomes UNKNOWN
capacity penalties are not applied unless policy says UNKNOWN is mildly penalized
route is not deleted
parent is not placed in hold-down

Expired capacity by itself MUST NOT:

delete the route
clear channel-9 timing
invalidate parent candidate
trigger route rediscovery
place parent in hold-down
cause force rediscovery

Only real delivery evidence may stale a route:

repeated missing gateway ACKs on selected path
UWB send failure proving next hop unusable
explicit route clear
new route epoch replacement
downlink command failure according to existing policy

24.7 Parent candidate table update

Change the parent candidate entry from this:

```
uint8_t relay_capacity_state;
uint16_t queue_free_hint;
uint8_t channel9_busy_hint;
```

to this:

```
struct mesh_parent_candidate {
    node_id_t next_hop;
    gateway_id_t gateway_id;
    uint16_t route_epoch;

    uint8_t hop_count;
    uint8_t path_quality_min;
    uint16_t route_cost;

    bool channel9_timing_valid;

    uint8_t relay_capacity_state;
    uint16_t queue_free_hint;
    uint8_t channel9_busy_hint;
    mesh_time_t capacity_observed_at;
    mesh_time_t capacity_valid_until;

    mesh_time_t last_observed;
    mesh_time_t last_success;
    mesh_time_t hold_down_until;
};
```

Selection logic should do:

```
if (now > parent.capacity_valid_until) {
    effective_capacity_state = RELAY_CAP_UNKNOWN;
} else {
    effective_capacity_state = parent.relay_capacity_state;
}
```

Do not do:

```
if (now > parent.capacity_valid_until) {
    delete_parent(parent);
}
```

24.8 Add UNKNOWN capacity state

Add:

```
enum relay_capacity_state {
    RELAY_CAP_UNKNOWN,
    RELAY_CAP_GREEN,
    RELAY_CAP_YELLOW,
    RELAY_CAP_RED,
    RELAY_CAP_BLACK,
};
```

Recommended route-selection behavior:

GREEN:
    preferred among equal route-cost candidates

UNKNOWN:
    usable; neutral or slight penalty

YELLOW:
    usable with penalty

RED:
    avoid for new custody unless no better candidate or packet is urgent

BLACK:
    do not select for new custody unless emergency policy explicitly allows it

UNKNOWN must not mean failed.

24.9 Result collection and capacity expiration

During all-node command result collection, capacity hints will frequently go stale because many nodes may be quiet or asleep between collection rounds.

That must be safe.

Correct behavior:

capacity expired:
    still try parent if route is best/available
    refresh contact if channel-9 timing is stale
    update capacity from RESULT_GRANT / RESULT_BUSY / custody ACK / observed queue hints

If parent returns RESULT_BUSY:

update capacity hint
set valid_until
retry after provided retry_after with jitter
try backup parent if useful

If no current capacity hint exists:

treat as UNKNOWN
do not flood rediscovery solely to refresh capacity

24.10 Things to double-check

Audit the implementation for these specific mistakes.

1. No channel-5 unicast/control exchange to a sleeping peer should skip the existing UWB wake train / wake-claim mechanism.
2. No code should send a full wake train before every individual frame once a C5 contact/control exchange is already accepted.
3. Gateway command flood, route solicitation flood, and collection EACK flood should not be named or treated as Click service.
4. Route replies must distinguish peer already awake in contact from peer may be sleeping and needs wake/contact first.
5. RESULT_OFFER / RESULT_GRANT / RESULT_BUSY must be inside C5 contact semantics.
6. Channel-9 event state must close after payload + hop/custody ACK, BUSY/RETRY_LATER, preemption, or event expiry.
7. Channel-9 event state must not wait for gateway ACK or collection EACK.
8. Gateway ACK/EACK waiting belongs to persistent outbox / collection_epoch state.
9. Expired relay capacity hint must become UNKNOWN, not route invalid.
10. Parent hold-down must be caused by delivery failure or explicit policy, not by stale capacity metadata.
11. Expired capacity must not clear channel-9 timing.
12. Expired capacity must not start route rediscovery.
13. Capacity hints from RELAY_BUSY / RESULT_BUSY must include observation/validity timing.
14. Tests should include a route that remains valid while capacity hint expires.
15. Tests should include a channel-9 transfer that completes locally and later receives gateway ACK in a separate event.

24.11 Acceptance criteria for this addendum

This addendum is implemented correctly when:

A sleeping peer is always woken through the existing UWB wake train / wake-claim mechanism before a C5 exchange that expects it to receive.

An awake peer inside an accepted C5 exchange does not receive redundant full wake trains before every frame.

A channel-9 event closes after local payload transfer and required hop/custody ACK, without waiting for gateway ACK/EACK.

Gateway ACK/EACK continues to drive persistent delivery completion outside the channel-9 event.

Relay capacity hints expire to UNKNOWN.

Expired capacity does not delete routes, invalidate parents, clear timing, start rediscovery, or place parents in hold-down.

Capacity refresh happens opportunistically through later contact, BUSY/RETRY_LATER, grants, custody ACKs, route replies, or route advertisements.
