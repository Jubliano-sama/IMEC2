#ifndef MESH_RELAY_H
#define MESH_RELAY_H

#include "mesh.h"
#include "mesh_capacity.h"
#include "mesh_radio_timing.h"
#include "mesh_route_path.h"
#include "operation_policy.h"
#include "protocol.h"
#include "route.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct uwb_wake_claim_frame;

#define MESH_BROADCAST_ID 0u
#define MESH_RELAY_DOWNLINK_ROUTES 16u
#define MESH_RELAY_ANCHOR_DOWNLINK_ROUTES MESH_CONNECTED_MAX_ANCHORS
#define MESH_RELAY_ANCHOR_DOWNLINK_OVERFLOW_ROUTES \
    (MESH_RELAY_ANCHOR_DOWNLINK_ROUTES - MESH_RELAY_DOWNLINK_ROUTES)
#define MESH_RELAY_DUP_CACHE_SIZE 16u
#define MESH_RELAY_GATEWAY_ACK_ORIGIN_MAX MESH_CONNECTED_MAX_ANCHORS
#define MESH_RELAY_GATEWAY_ACK_GUARANTEED_IDENTITIES_PER_ORIGIN 2u
/* Shared growth pool: a burst source may hold a few more recent identities. */
#define MESH_RELAY_GATEWAY_ACK_OVERFLOW_CAPACITY 6u
#define MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN \
    (MESH_RELAY_GATEWAY_ACK_GUARANTEED_IDENTITIES_PER_ORIGIN + \
     MESH_RELAY_GATEWAY_ACK_OVERFLOW_CAPACITY)
#define MESH_RELAY_GATEWAY_ACK_GUARANTEED_CAPACITY \
    (MESH_RELAY_GATEWAY_ACK_ORIGIN_MAX * \
     MESH_RELAY_GATEWAY_ACK_GUARANTEED_IDENTITIES_PER_ORIGIN)
#define MESH_RELAY_GATEWAY_ACK_CAPACITY \
    (MESH_RELAY_GATEWAY_ACK_GUARANTEED_CAPACITY + \
     MESH_RELAY_GATEWAY_ACK_OVERFLOW_CAPACITY)
/* A failed pair can require ABORT results from both endpoints while each
 * endpoint's ordinary replay partition is still occupied by exact terminal
 * proof debt. Keep those two cleanup identities outside the shared ordinary
 * pool so cleanup can always reach semantic admission without weakening or
 * evicting any unconfirmed record. */
#define MESH_RELAY_GATEWAY_ACK_CLEANUP_RESERVE_CAPACITY 2u
#define MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY \
    (MESH_RELAY_GATEWAY_ACK_CAPACITY + \
     MESH_RELAY_GATEWAY_ACK_CLEANUP_RESERVE_CAPACITY)
#define MESH_RELAY_GATEWAY_ACK_CANDIDATE_BITMAP_BYTES \
    ((MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY + 7u) / 8u)
#define MESH_RELAY_FLOOD_SEEN_SIZE 16u
/*
 * Channel-5 delivery protocol (see Documentation/Channel 5 Delivery Protocol).
 *
 * A parent that answers an uplink with depth MESH_ROUTE_DEPTH_UNREACHABLE has
 * no route of its own.  That is not an RF failure: the frame was heard and
 * refused, so the sender keeps custody, parks that candidate for this long and
 * re-selects immediately instead of burning one of its bounded RF retries.
 */
#define MESH_PARENT_DEAD_END_HOLD_MS 5000u
/*
 * A neighbour with a finite depth answers a broadcast MSG_ROUTE_SOLICIT with a
 * unicast route advert after a uniform delay in [0, this).  The application
 * owns the draw; the core only states the bound so replies from a whole
 * neighbourhood do not collide.
 */
#define MESH_SOLICIT_REPLY_JITTER_MS 40u
/*
 * A receiver that admitted a frame announcing further burst members keeps its
 * radio armed for at most this long between frames before it gives up waiting
 * and sends the single batch ACK it owes.
 */
#define MESH_BATCH_FOLLOWER_GAP_MS 6u
/* Default refusal delay when the application does not supply one. */
#define MESH_RELAY_BACKPRESSURE_RETRY_AFTER_MS 120u
/* Explicit refusal retries are jittered +/- this percentage of retry_after. */
#define MESH_RELAY_BACKPRESSURE_JITTER_PERCENT 25u
/* Local free-slot count has never been supplied by the application. */
#define MESH_RELAY_FREE_SLOTS_UNKNOWN 0xFFu
/*
 * Packets this relay handed to a parent and has not seen gateway-ACKed.  A
 * former parent that lost its route may hand any of them straight back, even
 * to their originator; the memo is what makes that a re-adoption instead of a
 * duplicate drop.  Four covers one click burst per parent with margin.
 */
#define MESH_RELAY_HANDOFF_MEMO_SLOTS 4u
/*
 * Expiry sweeps over the gateway ACK history are pure, idempotent functions of
 * (store contents, now_ms). A repeat pass is skipped while neither input has
 * changed, which collapses the four to six sweeps one received packet used to
 * trigger into one. See gateway_ack_history_expire_stale().
 */
#define MESH_RELAY_SWEEP_INTERVAL_MS 250u
/*
 * Longest payload the per-relay semantic digest memo retains. Mesh reports on
 * this network are ~300 bytes, so this covers click reports, hop ACKs and
 * ordinary command results with margin while costing far less RAM than the
 * 957-byte extended-frame maximum on a part that is already at 96% RAM. A
 * longer payload is still digested correctly, just without the memo.
 */
#define MESH_RELAY_DIGEST_CACHE_PAYLOAD_MAX 512u
#define MESH_RELAY_EVENT_TIMINGS 16u
#define MESH_RELAY_DOWNLINK_MAX_FAILURES ROUTE_MAX_FAILURES
#define MESH_RELAY_ROUTE_DISCOVERY_BACKOFF_BASE_MS 1000u
#define MESH_RELAY_ROUTE_DISCOVERY_BACKOFF_MAX_MS 60000u
/*
 * Channel-5 ACK retransmit backoff.  Every mesh packet now waits one ACK
 * window on channel 5, so a missed gateway/hop ACK must be repaired quickly
 * instead of parking the packet for seconds.  The first retry follows the
 * expired ACK window after a short jittered gap, each further attempt doubles
 * that base, and the complete delay is hard-capped.  Jitter is symmetric
 * (+/-50% of the base) so co-located senders that miss the same ACK do not
 * retransmit in lockstep.
 */
#define MESH_RELAY_ACK_RETRY_BACKOFF_BASE_MS 100u
#define MESH_RELAY_ACK_RETRY_BACKOFF_CAP_MS 2000u
#define MESH_RELAY_ACK_RETRY_BACKOFF_MIN_MS \
    (MESH_RELAY_ACK_RETRY_BACKOFF_BASE_MS - \
     (MESH_RELAY_ACK_RETRY_BACKOFF_BASE_MS / 2u))
#define MESH_RELAY_ACK_RETRY_BACKOFF_FIRST_MAX_MS \
    (MESH_RELAY_ACK_RETRY_BACKOFF_BASE_MS + \
     (MESH_RELAY_ACK_RETRY_BACKOFF_BASE_MS / 2u))
_Static_assert(MESH_RELAY_ACK_RETRY_BACKOFF_FIRST_MAX_MS <= 200u,
               "first ACK retry must follow the ACK window within ~200 ms");
_Static_assert(MESH_RELAY_ACK_RETRY_BACKOFF_CAP_MS >=
                   MESH_RELAY_ACK_RETRY_BACKOFF_FIRST_MAX_MS,
               "ACK retry cap must not undercut the first retry");
/*
 * Legacy supervision envelope.  The actual retransmit schedule is the
 * exponential backoff above; this stays the conservative upper bound that
 * channel-9 supervision, gateway ACK retention and source-age retention are
 * sized against, so it must remain >= the real schedule.
 */
#define MESH_RELAY_RETRY_BACKOFF_MAX_MS \
    (ROUTE_RETRY_BACKOFF_MAX_MS + (ROUTE_RETRY_BACKOFF_MAX_MS / 2u))
_Static_assert(MESH_RELAY_RETRY_BACKOFF_MAX_MS >=
                   MESH_RELAY_ACK_RETRY_BACKOFF_CAP_MS,
               "supervision envelope must cover the ACK retry cap");
#define MESH_RELAY_GATEWAY_ACK_RETRY_BUDGET_MAX_MS \
    ((ROUTE_GATEWAY_ACK_TIMEOUT_MS * (ROUTE_RETRIES_PER_CANDIDATE + 1u)) + \
     ROUTE_RETRY_BACKOFF_FIRST_MS + (ROUTE_RETRY_BACKOFF_FIRST_MS / 2u) + \
     ROUTE_RETRY_BACKOFF_SECOND_MS + (ROUTE_RETRY_BACKOFF_SECOND_MS / 2u) + \
     MESH_RELAY_RETRY_BACKOFF_MAX_MS)
/* Once a strictly newer gateway route epoch has authorized recovery, an
 * unacknowledged transient confirm replays the original after one complete
 * ordinary ACK budget. Time alone is never treated as reboot proof. */
#define MESH_RELAY_GATEWAY_ACK_CONFIRM_REPLAY_MS \
    MESH_RELAY_GATEWAY_ACK_RETRY_BUDGET_MAX_MS
#define MESH_RELAY_GATEWAY_ACK_RECOVERY_QUARANTINE_MS \
    ROUTE_GATEWAY_ACK_TIMEOUT_MS
#define FLOOD_EPOCH_LOCAL_TTL 2u
#define FLOOD_EPOCH_REGIONAL_TTL 4u
#define FLOOD_EPOCH_GLOBAL_TTL MESH_NETWORK_MAX_HOPS
#define FLOOD_EPOCH_CRITICAL_TTL 12u
#define FLOOD_FORWARD_MAX_NORMAL 1u
#define FLOOD_FORWARD_MAX_CRITICAL 2u
#define FLOOD_FORWARD_SUPPRESS_AFTER_HEARD 2u
#define FLOOD_WAVE_MS 1400u
#define FLOOD_RELAY_BURST_MS 600u
#define FLOOD_RELAY_REPEAT_MS 40u
#define FLOOD_RELAY_REPEAT_COUNT 4u
#define FLOOD_POST_ROOT_GUARD_MS 150u
#define FLOOD_RANDOM_BACKOFF_DEFAULT_MAX_MS 4200u
#define FLOOD_RANDOM_BACKOFF_DEFAULT_SLOT_MS 600u
#define FLOOD_DEFAULT_RETRY_COUNT 2u
/* Here-I-Am advances on one shared depth clock. Every sender at a depth
 * chooses a uniform start in a 2.5 s activation window and transmits a 500 ms
 * combined wake/activation train. Once every possible train has ended, the
 * same senders advertise identity and local depth exactly three times across
 * a 1.5 s route-selection window. The following depth cannot start until
 * that complete 4.5 s block has closed. */
#define MESH_GATEWAY_ROUTE_ACTIVATION_START_WINDOW_MS 2500u
#define MESH_GATEWAY_ROUTE_ACTIVATION_TRAIN_MS 500u
#define MESH_GATEWAY_ROUTE_ACTIVATION_ENVELOPE_MS \
    (MESH_GATEWAY_ROUTE_ACTIVATION_START_WINDOW_MS + \
     MESH_GATEWAY_ROUTE_ACTIVATION_TRAIN_MS)
#define MESH_GATEWAY_ROUTE_ADV_WINDOW_MS 1500u
#define MESH_GATEWAY_ROUTE_DEPTH_BLOCK_MS \
    (MESH_GATEWAY_ROUTE_ACTIVATION_ENVELOPE_MS + \
     MESH_GATEWAY_ROUTE_ADV_WINDOW_MS)
#define MESH_GATEWAY_ROUTE_MAX_DEPTH 8u
/* A weak candidate at the deepest supported depth is usable only after one
 * additional empty depth block proves there is no stronger next hop. */
#define MESH_GATEWAY_ROUTE_SELECTION_SETTLE_BLOCKS \
    (MESH_GATEWAY_ROUTE_MAX_DEPTH + 2u)
#define MESH_GATEWAY_ROUTE_SELECTION_SETTLE_MS \
    ((MESH_GATEWAY_ROUTE_SELECTION_SETTLE_BLOCKS * \
      MESH_GATEWAY_ROUTE_DEPTH_BLOCK_MS) + FLOOD_POST_ROOT_GUARD_MS)
/* The root may release its command owner once the route wave is already four
 * RF layers away.  Relays keep the immutable wave clock and finish the deeper
 * blocks autonomously; a partial wave still uses the complete quiet horizon
 * before it may be restarted. */
#define MESH_GATEWAY_ROUTE_COMMAND_RELEASE_MS 20000u
_Static_assert(MESH_GATEWAY_ROUTE_COMMAND_RELEASE_MS >=
                   4u * MESH_GATEWAY_ROUTE_DEPTH_BLOCK_MS,
               "route command release needs four complete separation blocks");
_Static_assert(MESH_GATEWAY_ROUTE_COMMAND_RELEASE_MS <
                   5u * MESH_GATEWAY_ROUTE_DEPTH_BLOCK_MS,
               "route command release should occur during depth four");
#define MESH_GATEWAY_ROUTE_ADV_COPY_SPACING_MS 500u
#define MESH_GATEWAY_ROUTE_ADV_COPY_COUNT (FLOOD_DEFAULT_RETRY_COUNT + 1u)
#define MESH_GATEWAY_ROUTE_ADV_FIRST_COPY_JITTER_MAX_MS \
    (MESH_GATEWAY_ROUTE_ADV_COPY_SPACING_MS - \
     OPERATION_POLICY_RESPONSE_TX_TIMEOUT_MS)
#define MESH_GATEWAY_ROUTE_ADV_RELAY_BURST_MAX_MS \
    MESH_GATEWAY_ROUTE_ADV_WINDOW_MS
#define MESH_GATEWAY_ROUTE_ADV_RELAY_HOP_MAX_MS \
    MESH_GATEWAY_ROUTE_DEPTH_BLOCK_MS
_Static_assert(MESH_GATEWAY_ROUTE_ADV_WINDOW_MS ==
                   MESH_GATEWAY_ROUTE_ADV_COPY_COUNT *
                       MESH_GATEWAY_ROUTE_ADV_COPY_SPACING_MS,
               "route advertisements must fill three equal strata");
_Static_assert(OPERATION_POLICY_RESPONSE_TX_TIMEOUT_MS <=
                   MESH_GATEWAY_ROUTE_ADV_COPY_SPACING_MS,
               "one route advertisement must fit its stratum");
#define MESH_GATEWAY_ROUTE_ACTIVATION_MAGIC0 0x4du
#define MESH_GATEWAY_ROUTE_ACTIVATION_MAGIC1 0x57u
#define MESH_GATEWAY_ROUTE_ACTIVATION_VERSION 1u
#define MESH_GATEWAY_ROUTE_ACTIVATION_LEN 25u
/* CLAIM starts only after the complete route-selection wave. Relays need no
 * per-hop activation lead inside CLAIM itself. */
#define MESH_ENUMERATION_CLAIM_PIPELINE_LEAD_MS 0u
#define MESH_ENUMERATION_CLAIM_RELAY_HOP_MAX_MS \
    (MESH_ENUMERATION_CLAIM_PIPELINE_LEAD_MS + \
     MESH_ENUMERATION_RELAY_MAX_INITIAL_DELAY_MS + \
     (MESH_ENUMERATION_RELAY_COPY_COUNT * \
      OPERATION_POLICY_RESPONSE_TX_TIMEOUT_MS) + \
     MESH_ENUMERATION_RELAY_COPY_TAIL_MS)
#define MESH_GATEWAY_ROUTE_ADV_PAYLOAD_LEN \
    (MESH_GATEWAY_ROUTE_ADV_FIXED_TLV_BYTES + \
     PROTO_TLV_U64_ENCODED_LEN)
#define MESH_GATEWAY_ROUTE_ADV_POLICY_PAYLOAD_LEN \
    (MESH_GATEWAY_ROUTE_ADV_PAYLOAD_LEN + \
     OPERATION_POLICY_ALL_TLVS_LEN)
#define MESH_GATEWAY_ROUTE_ADV_PREARM_POLICY_PAYLOAD_LEN \
    (MESH_GATEWAY_ROUTE_ADV_POLICY_PAYLOAD_LEN + \
     MESH_GATEWAY_ROUTE_ADV_PREARM_TLV_BYTES)
#define C5_POLITE_SNIFF_MS 20u
#define C5_POLITE_BACKOFF_MIN_MS 20u
#define C5_POLITE_BACKOFF_MAX_MS 1600u
#define C5_POLITE_DEFERRAL_MAX 8u
#define RREP_ACK_TIMEOUT_MS 150u
#define RREP_RETRY_COUNT_PER_HOP 4u
#define RREP_RESPONDER_SLOT_MS 25u
#define RREP_RESPONDER_SLOT_COUNT 8u
#define RREP_RESPONDER_JITTER_MAX_MS \
    ((RREP_RESPONDER_SLOT_COUNT - 1u) * RREP_RESPONDER_SLOT_MS)
#define REVERSE_PATH_CANDIDATE_COUNT 2u
#define RELAY_BUSY_RETRY_MIN_MS 500u
#define RELAY_BUSY_RETRY_MAX_MS 5000u
#define MESH_RELAY_RESULT_OFFER_RETRY_BASE_MS 500u
#define MESH_RELAY_RESULT_OFFER_RETRY_MAX_MS 8000u
#define MESH_RELAY_RESULT_OFFER_MAX_RF_ATTEMPTS 6u
#define MESH_RELAY_RESULT_OFFER_EXPIRY_S 60u
#define RELAY_CAPACITY_HINT_VALIDITY_MS 5000u
#define COLLECTION_INITIAL_SPREAD_MIN_MS 30000u
#define COLLECTION_INITIAL_SPREAD_PER_NODE_MS 300u
#define COLLECTION_MISSING_SPREAD_PER_NODE_MS 500u
#define COLLECTION_RETRY_ROUND_0_MS 15000u
#define COLLECTION_RETRY_ROUND_1_MS 30000u
#define COLLECTION_RETRY_ROUND_2_MS 60000u
#define COLLECTION_RETRY_ROUND_3_MS 120000u
#define COLLECTION_RETRY_ROUND_STEADY_MS 300000u
#define COLLECTION_RETRY_JITTER_PERCENT 25u
#define COLLECTION_RESULT_INLINE_C5_MAX_BYTES 32u
#define COLLECTION_BUNDLE_TARGET_BYTES 512u
#define COLLECTION_BUNDLE_MAX_RECORDS 8u
#define MESH_RELAY_RESULT_BUNDLE_RECORDS 2u
#define MESH_RELAY_RESULT_BUNDLE_HOLD_MS 25u
#define MESH_RELAY_OUTBOX_SNAPSHOT_VERSION 5u
#define MESH_RELAY_CHILD_CUSTODY_SNAPSHOT_VERSION 2u
#define COMMAND_RESULT_EXPIRY_DEFAULT_S 86400u
#define MESH_RELAY_GATEWAY_ACK_RETENTION_MS \
    (COMMAND_RESULT_EXPIRY_DEFAULT_S * 1000u)

_Static_assert(COMMAND_RESULT_EXPIRY_DEFAULT_S <= UINT32_MAX / 1000u,
               "gateway ACK retention must fit wrap-safe uptime arithmetic");

enum flood_epoch_type {
    FLOOD_EPOCH_TYPE_ROUTE_SOLICIT = 1u,
    FLOOD_EPOCH_TYPE_GATEWAY_ROUTE_ADV = 2u,
    FLOOD_EPOCH_TYPE_GATEWAY_COMMAND = 3u,
    FLOOD_EPOCH_TYPE_COLLECTION_STATUS = 4u,
};

enum c5_contact_state {
    C5_CONTACT_NONE = 0,
    C5_CONTACT_WAKE_PENDING = 1,
    C5_CONTACT_AWAKE_ACCEPTED = 2,
    C5_CONTACT_EXCHANGE_ACTIVE = 3,
    C5_CONTACT_CLOSING = 4,
};

enum c5_contact_purpose {
    C5_CONTACT_PURPOSE_ROUTE_SOLICIT = 1u,
    C5_CONTACT_PURPOSE_ROUTE_REPLY = 2u,
    C5_CONTACT_PURPOSE_ROUTE_CONTACT_REFRESH = 3u,
    C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD = 4u,
    C5_CONTACT_PURPOSE_COLLECTION_EACK_FLOOD = 5u,
    C5_CONTACT_PURPOSE_RESULT_OFFER_GRANT = 6u,
    C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION = 7u,
    /* Gateway-bound data handed to a parent anchor behind a wake train. */
    C5_CONTACT_PURPOSE_UPLINK = 8u,
};

struct c5_contact_context {
    uint64_t peer_id;
    uint32_t contact_id;
    uint8_t purpose;
    bool peer_was_woken;
    bool accepted;
    uint32_t opened_at_ms;
    uint32_t last_frame_at_ms;
    uint32_t expires_at_ms;
    enum c5_contact_state state;
};

enum ch9_event_state {
    CH9_EVENT_NONE = 0,
    CH9_EVENT_GRANTED = 1,
    CH9_EVENT_TX_PAYLOAD = 2,
    CH9_EVENT_WAIT_CUSTODY_ACK = 3,
    CH9_EVENT_COMPLETE = 4,
    CH9_EVENT_BUSY_RETRY_LATER = 5,
    CH9_EVENT_WINDOW_EXPIRED = 6,
    CH9_EVENT_PREEMPTED_BY_C5 = 7,
};

struct mesh_parent_candidate {
    uint64_t next_hop;
    uint64_t gateway_id;
    uint16_t route_epoch;
    uint8_t hop_count;
    uint8_t path_quality_min;
    uint16_t route_cost;
    uint8_t relay_capacity_state;
    uint16_t queue_free_hint;
    uint8_t channel9_busy_hint;
    uint32_t capacity_observed_at_ms;
    uint32_t capacity_valid_until_ms;
    bool capacity_hint_valid;
    bool channel9_timing_valid;
    uint32_t last_observed_ms;
    uint32_t last_success_ms;
    uint32_t hold_down_until_ms;
};

struct flood_seen_entry {
    uint64_t gateway_id;
    uint32_t gateway_epoch;
    uint32_t flood_epoch_id;
    uint8_t flood_type;
    uint64_t origin_id;
    uint32_t origin_request_id;
    uint8_t best_hop_count;
    uint16_t best_metric;
    uint64_t best_previous_hop;
    uint64_t backup_previous_hop;
    uint32_t forward_due_ms;
    uint8_t forward_count;
    uint8_t heard_count;
    uint32_t expires_at_ms;
    bool valid;
};

enum mesh_relay_delivery_state {
    MESH_RELAY_DELIVERY_NONE = 0,
    MESH_RELAY_DELIVERY_WAIT_LOCAL_CUSTODY_ACK = 1,
    MESH_RELAY_DELIVERY_CUSTODY_ACCEPTED = 2,
    MESH_RELAY_DELIVERY_WAIT_GATEWAY_ACK = 3,
    MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK = 4,
    MESH_RELAY_DELIVERY_GATEWAY_ACKED = 5,
    MESH_RELAY_DELIVERY_EXPIRED = 6,
    MESH_RELAY_DELIVERY_COLLECTION_CLOSED = 7,
    MESH_RELAY_DELIVERY_RESULT_GRANT_ATTEMPTS_EXHAUSTED = 8,
};

enum mesh_relay_status {
    MESH_RELAY_ERR_RESULT_GRANT_ATTEMPTS_EXHAUSTED = -2000,
    MESH_RELAY_ERR_RESULT_GRANT_DEADLINE_EXPIRED = -2001,
    MESH_RELAY_ERR_OUTBOX_EXPIRED = -2002,
};

struct mesh_outbox_record {
    bool valid;
    enum mesh_relay_delivery_state delivery_state;
    uint32_t packet_id;
    uint32_t session_id;
    uint16_t seq;
    uint64_t gateway_id;
    uint8_t packet_class;
    uint32_t created_uptime_ms;
    uint32_t age_ms_saturating;
    uint8_t priority;
    uint8_t retry_round;
    uint8_t selected_parent_index;
    bool custody_accepted;
    uint64_t custody_parent;
    bool gateway_acked;
    uint32_t expiry_s;
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    /* Wire-integrity diagnostic; semantic equality uses semantic_digest. */
    uint16_t payload_crc;
    uint16_t payload_len;
};

enum mesh_relay_role {
    MESH_RELAY_ROLE_ANCHOR = 1,
    MESH_RELAY_ROLE_GATEWAY = 2,
    MESH_RELAY_ROLE_CLICKER = 3,
};

enum mesh_relay_action {
    MESH_RELAY_ACTION_NONE = 0u,
    MESH_RELAY_ACTION_ENUMERATION_PREARM = 1u << 0,
    MESH_RELAY_ACTION_DELIVER_LOCAL = 1u << 1,
    MESH_RELAY_ACTION_FORWARD = 1u << 2,
    MESH_RELAY_ACTION_SEND_GATEWAY_ACK = 1u << 3,
    /* An exact next-hop ACK transferred an eligible gateway-bound packet
     * into the parent's RAM custody. The source may retire its Channel-9
     * exchange. The authoritative TABLE now commits enumeration directly. */
    MESH_RELAY_ACTION_TX_NEXT_HOP_CUSTODY_ACCEPTED = 1u << 4,
    /*
     * The next hop answered explicitly instead of staying silent: either a
     * dead-end ACK (depth UNREACHABLE, nothing accepted) or backpressure
     * (real depth, credit 0, retry-after).  Custody is retained and the relay
     * has already re-selected or rescheduled; the application must re-arm its
     * transmit timer and must NOT count this as an RF failure.
     */
    MESH_RELAY_ACTION_TX_HOP_DEFERRED = 1u << 5,
    MESH_RELAY_ACTION_DROP = 1u << 6,
    /* The original gateway ACK was authenticated. The immutable source packet
     * remains the owner while its compact ACK_CONFIRM is generated transiently. */
    MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING = 1u << 7,
    MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED = 1u << 8,
    MESH_RELAY_ACTION_RETRANSMIT = 1u << 9,
    MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED = 1u << 10,
    MESH_RELAY_ACTION_SEND_ROUTE_REQ = 1u << 11,
    MESH_RELAY_ACTION_SEND_ROUTE_REPLY = 1u << 12,
    MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY = 1u << 13,
    MESH_RELAY_ACTION_SEND_HOP_ACK = 1u << 14,
    MESH_RELAY_ACTION_TX_HOP_PROGRESS = 1u << 15,
    MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK = 1u << 16,
    MESH_RELAY_ACTION_ROUTE_REPLY_ACKED = 1u << 17,
    MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV = 1u << 18,
    MESH_RELAY_ACTION_SEND_RELAY_BUSY = 1u << 19,
    MESH_RELAY_ACTION_SEND_RESULT_BUSY = 1u << 20,
    MESH_RELAY_ACTION_TX_RELAY_BUSY = 1u << 21,
    MESH_RELAY_ACTION_SEND_RESULT_GRANT = 1u << 22,
    MESH_RELAY_ACTION_CUSTODY_ACCEPTED = 1u << 23,
    MESH_RELAY_ACTION_TX_COLLECTION_RETRY = 1u << 24,
    MESH_RELAY_ACTION_TX_COLLECTION_CLOSED = 1u << 25,
    MESH_RELAY_ACTION_UPDATE_ROUTE_REQ = 1u << 26,
    MESH_RELAY_ACTION_TX_RESULT_GRANT_TERMINAL = 1u << 27,
    /* The original transit outbox remains owned until this ACK is sent. */
    MESH_RELAY_ACTION_TRANSIT_GATEWAY_ACK_FORWARD_PENDING = 1u << 28,
    MESH_RELAY_ACTION_INSTALL_OPERATION_POLICY = 1u << 29,
    MESH_RELAY_ACTION_CHILD_CUSTODY_CHANGED = 1u << 30,
};

/* A matching gateway ACK cannot reach its retained transit origin until the
 * app repairs that exact child route on Channel 5. Keep bit 31 outside the C
 * enum because its values must fit signed int on the native pre-C2X gate. */
#define MESH_RELAY_ACTION_TRANSIT_GATEWAY_ACK_ROUTE_REPAIR \
    (UINT32_C(1) << 31)

enum mesh_relay_tx_state {
    MESH_RELAY_TX_IDLE = 0,
    MESH_RELAY_TX_WAIT_GATEWAY_ACK = 1,
    MESH_RELAY_TX_WAIT_RETRY_BACKOFF = 2,
    MESH_RELAY_TX_WAIT_RESULT_GRANT = 3,
    MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD = 4,
    /* The retry/expiry policy is terminal, but the exact application owner
     * must consume the terminal record before the relay releases its RAM. */
    MESH_RELAY_TX_WAIT_TERMINAL_COMMIT = 5,
};

struct mesh_outbound {
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    uint16_t payload_len;
    uint8_t radio_channel;
    uint8_t flood_retry_count;
    bool queued_at_valid;
    bool earliest_tx_valid;
    /*
     * Optional custody provenance for a response handed to another owner.
     * Ordinary packets leave this zero. Keep the 32-bit member here so the
     * following 64-bit custody IDs do not leave a repeated alignment hole in
     * every queued outbound record.
     */
    union {
        uint32_t handoff_owner_generation;
        /* Gateway route advertisements never carry handoff provenance, so
         * reuse that word for the root-relative start of this sender's depth
         * block instead of enlarging every queued outbound record. */
        uint32_t route_wave_start_ms;
    };
    uint64_t next_hop_id;
    /*
     * Physical child that handed this transit packet to the local relay.
     * This is local custody metadata, never wire data.  Queue and route-wait
     * owners retain it so the eventual gateway ACK retraces the exact custody
     * edge instead of consulting a newer or shorter mutable downlink.
     */
    uint64_t ingress_previous_hop_id;
    uint32_t queued_at_ms;
    uint32_t earliest_tx_ms;
};

struct mesh_gateway_route_activation {
    uint64_t gateway_id;
    uint32_t gateway_route_seq;
    uint16_t gateway_epoch;
    uint16_t relay_window_starts_in_ms;
    uint16_t route_adv_starts_in_ms;
    uint8_t sender_depth;
};

struct mesh_gateway_route_adv_snapshot {
    uint32_t gateway_route_seq;
    uint32_t queued_at_ms;
    uint32_t enumeration_prearm_epoch;
    uint32_t enumeration_prearm_hold_ms;
    uint16_t gateway_epoch;
    uint16_t packet_seq;
    uint16_t capacity_validity_interval_ms;
    uint8_t gateway_capacity_state;
    uint8_t operation_policy_tlvs[OPERATION_POLICY_ALL_TLVS_LEN];
    uint8_t operation_policy_tlvs_len;
    bool operation_policy_present;
    bool enumeration_prearm_present;
    bool enumeration_survey_follows;
    bool valid;
};

struct mesh_downlink_entry {
    uint64_t target_id;
    uint64_t next_hop_id;
    uint64_t gateway_id;
    uint32_t route_epoch;
    union {
        uint32_t last_seen_ms;
        uint32_t discovery_session_id;
    };
    uint32_t discovery_flood_epoch_id;
    uint8_t hop_count;
    uint8_t quality;
    uint8_t failure_count;
    bool valid;
};

struct mesh_duplicate_entry {
    uint64_t src_id;
    uint64_t dst_id;
    uint32_t session_id;
    uint32_t last_seen_ms;
    uint32_t busy_response_at_ms;
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint16_t seq;
    uint16_t busy_response_interval_ms;
    uint8_t msg_type;
    bool semantic_identity_valid;
    bool delivery_accepted;
    bool valid;
};

_Static_assert(sizeof(struct mesh_duplicate_entry) == 72u,
               "duplicate cache must retain full semantic commitments");

struct mesh_command_replay_window {
    uint64_t forwarded;
    uint32_t newest_command_seq;
    /*
     * Only the newest transport attempt may be redelivered locally for
     * application admission retry. Bind that exception to the complete
     * immutable command; older seen attempts remain transport-stale.
     */
    uint8_t newest_semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    bool initialized;
};

_Static_assert(sizeof(struct mesh_command_replay_window) == 48u,
               "command replay window must retain one full commitment");

/*
 * External gateway-only acceptance history. The production gateway overlays
 * this store on anchor-only batch state instead of charging every relay role.
 * Semantic identities remain exact terminal-proof authority after their
 * phase deadline. Each source is guaranteed four ordinary identities, while
 * a shared overflow lets one source retain its complete 16-report queue.
 * Overflow admission is bounded separately so a noisy source cannot consume
 * another roster member's four guaranteed identities. The two serialized
 * pair endpoints share two ABORT-result reserve entries. Once a source reaches
 * its 16-identity bound, another ordinary identity may replace only a
 * confirmed source-local tombstone.
 * Assignment candidates use normal partitions within the append-only
 * 50-member roster bound, never a 51st origin. A candidate identity is
 * reserved only after the gateway assignment state machine validates the
 * exact phase, epoch, hash, membership, and operation state. After the prior
 * owner is terminal and its exact durable roster is loaded, enumeration-start
 * reconciliation retains member histories and retires every absent origin.
 */
struct mesh_gateway_ack_identity_entry {
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint32_t session_id;
    uint32_t expires_at_ms;
    uint16_t seq;
    uint8_t msg_type;
    /* Low six bits encode owner index + 1; high bits hold identity flags. */
    uint8_t owner_state;
};

struct mesh_gateway_ack_store {
    /*
     * Structure-of-arrays avoids alignment padding for every origin.
     * Identity counts are derived by scanning the identity table, avoiding a
     * separate mutable count that could diverge from its owner records.
     */
    uint64_t origin_src_ids[MESH_RELAY_GATEWAY_ACK_ORIGIN_MAX];
    uint32_t origin_batch_ids[MESH_RELAY_GATEWAY_ACK_ORIGIN_MAX];
    struct mesh_gateway_ack_identity_entry
        identities[MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY];
    uint8_t candidate_identity_bits[
        MESH_RELAY_GATEWAY_ACK_CANDIDATE_BITMAP_BYTES];
    /* ACK-history retention and exact ACK_CONFIRM reception are independent
     * facts. Phase owners query this bitmap before causally later RF. */
    uint8_t confirmed_identity_bits[
        MESH_RELAY_GATEWAY_ACK_CANDIDATE_BITMAP_BYTES];
};

struct mesh_upstream_ancestry_entry {
    struct mesh_route_path path;
    uint64_t next_hop_id;
    uint32_t route_epoch;
    /* Nonzero only when a gateway route advertisement proved this path. */
    uint32_t gateway_route_seq;
    bool valid;
};

/*
 * Anchor-only overflow for reverse routes learned from a large descendant
 * fan-out. The pointer shares the gateway-only ACK-store slot in mesh_relay,
 * so gateway and clicker RAM do not pay for this storage.
 */
struct mesh_anchor_downlink_store {
    struct mesh_downlink_entry
        entries[MESH_RELAY_ANCHOR_DOWNLINK_OVERFLOW_ROUTES];
    struct mesh_upstream_ancestry_entry
        upstream_ancestry[ROUTE_MAX_CANDIDATES];
};

_Static_assert(sizeof(struct mesh_gateway_ack_identity_entry) == 44u,
               "gateway ACK identity must retain a full semantic commitment");
_Static_assert(MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN >=
                   MESH_RELAY_GATEWAY_ACK_GUARANTEED_IDENTITIES_PER_ORIGIN,
               "gateway ACK per-origin capacity must cover its guarantee");
_Static_assert(MESH_RELAY_GATEWAY_ACK_GUARANTEED_CAPACITY == 100u,
               "gateway ACK history must guarantee two slots to 50 members");
_Static_assert(MESH_RELAY_GATEWAY_ACK_CANDIDATE_BITMAP_BYTES == 14u,
               "gateway ACK candidate bitmap must cover every identity");
_Static_assert(sizeof(struct mesh_gateway_ack_store) == 5384u,
               "gateway ACK store must fit role-overlaid static storage");
_Static_assert(MESH_RELAY_GATEWAY_ACK_CAPACITY == 106u,
               "gateway ACK history must preserve fleet and burst bounds");
_Static_assert(MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY == 108u,
               "gateway ACK history must reserve both pair cleanup results");
_Static_assert(MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY <= UINT8_MAX,
               "per-origin identity counts must not overflow");
_Static_assert(MESH_RELAY_ANCHOR_DOWNLINK_ROUTES >=
                   MESH_RELAY_DOWNLINK_ROUTES,
               "anchor downlink capacity must include the inline table");
_Static_assert(MESH_RELAY_ANCHOR_DOWNLINK_ROUTES <= UINT8_MAX,
               "downlink rollback indices must fit one byte");
_Static_assert(ROUTE_MAX_CANDIDATES == 3u,
               "anchor ancestry store must cover every upstream candidate");

struct mesh_relay_event_timing_entry {
    uint64_t next_hop_id;
    struct mesh_event_timing timing;
    uint8_t direction;
    bool valid;
};

enum mesh_relay_channel9_direction {
    MESH_RELAY_CHANNEL9_DIRECTION_UNKNOWN = 0,
    MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM = 1,
    MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM = 2,
    MESH_RELAY_CHANNEL9_DIRECTION_AMBIGUOUS = 3,
};

enum mesh_relay_channel9_guard_reason {
    MESH_RELAY_CHANNEL9_GUARD_OK = 0,
    MESH_RELAY_CHANNEL9_GUARD_REPLACED_PEER = 1,
    MESH_RELAY_CHANNEL9_GUARD_AMBIGUOUS_NEW_PEER = 2,
    MESH_RELAY_CHANNEL9_GUARD_AMBIGUOUS_ACTIVE_PEER = 3,
    MESH_RELAY_CHANNEL9_GUARD_TOO_MANY_PEERS = 4,
    MESH_RELAY_CHANNEL9_GUARD_DIRECTION_BUSY = 5,
    MESH_RELAY_CHANNEL9_GUARD_INTERVAL_CONFLICT = 6,
};

struct mesh_relay_channel9_guard_status {
    enum mesh_relay_channel9_guard_reason reason;
    enum mesh_relay_channel9_direction direction;
    enum mesh_relay_channel9_direction conflict_direction;
    uint64_t conflict_peer_id;
    uint8_t active_peer_count;
};

struct mesh_pending_tx {
    enum mesh_relay_tx_state state;
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    uint16_t payload_len;
    uint8_t radio_channel;
    uint64_t next_hop_id;
    uint32_t gateway_ack_deadline_ms;
    uint32_t retry_after_ms;
    uint32_t queued_at_ms;
    bool result_offer_active;
    bool gateway_ack_forward_pending;
    /* Immutable physical handoff captured when the gateway ACK is accepted. */
    union {
        uint64_t gateway_ack_forward_previous_hop_id;
        struct {
            uint32_t gateway_ack_confirm_started_ms;
            uint32_t gateway_ack_confirm_route_epoch;
        };
    };
    union {
        /* Exact child edge retained from original transit admission. */
        uint64_t transit_previous_hop_id;
        /* Same edge after the matching gateway ACK owns its return handoff. */
        uint64_t gateway_ack_forward_next_hop_id;
    };
    uint32_t gateway_ack_forward_owner_generation;
    uint8_t busy_retry_round;
    bool hop_ack_observed_since_send;
    bool gateway_ack_confirm_pending;
    /* Bitset: newer-epoch recovery authorization and pre-replay quarantine. */
    uint8_t gateway_ack_recovery_flags;
    /* Consecutive missed HOP_ACKs for this exact ordinary report and parent.
     * Route history must not make a fresh packet skip its bounded retries. */
    uint8_t parent_hop_ack_miss_count;
};

struct mesh_relay_outbox_snapshot {
    uint16_t version;
    enum mesh_relay_role role;
    uint64_t local_id;
    uint64_t gateway_id;
    struct mesh_outbox_record record;
    struct mesh_pending_tx pending;
    uint32_t route_epoch;
    uint32_t snapshot_at_ms;
    bool valid;
};

struct mesh_route_discovery_state {
    uint64_t target_id;
    uint8_t attempts;
    /* Zero accepts any route depth; otherwise the current request requires
     * this exact number of relay anchors between the origin and target. */
    uint8_t required_hop_count;
    uint32_t next_request_id;
    uint32_t current_request_id;
    uint32_t next_request_ms;
    uint32_t completed_request_id;
    uint32_t completed_request_until_ms;
    uint64_t completed_reply_previous_hop_id;
    uint8_t completed_reply_digest[SEMANTIC_DIGEST_SHA256_LEN];
    bool active;
};

struct mesh_route_reply_ack_expectation {
    uint64_t peer_id;
    uint8_t reply_commitment[SEMANTIC_DIGEST_SHA256_LEN];
    uint32_t session_id;
    bool active;
};

struct mesh_result_bundle_entry {
    struct command_result_id result_id;
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint16_t payload_len;
    /* Required on the RESULT_BUNDLE wire; never used as semantic equality. */
    uint16_t payload_crc;
    uint32_t message_age_ms;
    uint32_t queued_at_ms;
    uint8_t payload[RESULT_BUNDLE_RECORD_MAX_PAYLOAD_LEN];
    bool valid;
};

struct mesh_result_bundle_queue {
    uint64_t gateway_id;
    uint16_t gateway_epoch;
    uint32_t command_seq;
    uint32_t collection_epoch_id;
    uint32_t due_ms;
    uint8_t record_count;
    bool active;
    struct mesh_result_bundle_entry records[MESH_RELAY_RESULT_BUNDLE_RECORDS];
};

struct mesh_result_offer_reservation {
    struct command_result_id result_id;
    uint64_t child_id;
    uint8_t result_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint16_t result_len;
    bool valid;
};

struct mesh_relay_child_custody_snapshot {
    uint16_t version;
    enum mesh_relay_role role;
    uint64_t local_id;
    uint64_t gateway_id;
    uint32_t snapshot_at_ms;
    struct mesh_result_bundle_queue result_bundle;
    struct mesh_result_offer_reservation result_offer_reservation;
    bool valid;
};

struct mesh_relay_diagnostics {
    uint8_t flood_suppression_count;
    uint8_t route_reply_retry_count;
    uint8_t busy_response_count;
};

#define MESH_RELAY_HANDOFF_VALID 0x01u
#define MESH_RELAY_HANDOFF_GATEWAY_ACKED 0x02u

/*
 * One packet this relay originated and handed to a parent.  Kept until the
 * gateway ACK for it is observed, so a packet returned by a parent that has
 * since lost its route can be recognised as ours and re-adopted.
 */
struct mesh_relay_handoff_memo {
    uint64_t parent_id;
    uint32_t session_id;
    uint16_t seq;
    uint8_t flags;
};

/*
 * Batch sequencing state.  The core owns the arithmetic (how many frames are
 * still owed, how many the peer has just authorised, what REMAINING each
 * follower must carry); the application owns radio timing.
 */
struct mesh_relay_batch_state {
    /* Peer whose burst the receive side is currently admitting. */
    uint64_t rx_peer_id;
    /* Latest instant at which a follower frame is still expected. */
    uint32_t rx_deadline_ms;
    /* Frames the sender still holds for the current next hop. */
    uint8_t tx_pending;
    /* Frames the last ACK authorised from that backlog. */
    uint8_t tx_credit;
    /* Followers the receive side is still waiting for. */
    uint8_t rx_remaining;
    /* Free admission slots the application last reported for this node. */
    uint8_t local_free_slots;
};

/* One semantic commitment over a (packet header, payload) pair. */
struct mesh_packet_digest {
    uint8_t sha[SEMANTIC_DIGEST_SHA256_LEN];
    bool valid;
};

/*
 * Single-entry memo for mesh_packet_semantic_digest(). One received packet is
 * digested by duplicate classification, ACK-history admission, ACK
 * construction, ACK-history store/confirm and duplicate store: six SHA-256
 * passes over the same bytes, roughly 0.4 ms each on a 64 MHz nRF52833
 * without hardware crypto.
 *
 * The memo is keyed on the complete digest preimage (every committed header
 * field plus the payload bytes), never on pointers, so a hit returns exactly
 * the digest a recomputation would produce. Comparing the key costs one
 * memcmp of the payload, roughly two orders of magnitude cheaper than the
 * hash it replaces.
 */
struct mesh_relay_digest_cache {
    struct mesh_packet_digest digest;
    uint64_t src_id;
    uint64_t dst_id;
    uint32_t session_id;
    uint16_t seq;
    uint16_t payload_len;
    uint8_t msg_type;
    uint8_t flags;
    uint8_t payload[MESH_RELAY_DIGEST_CACHE_PAYLOAD_MAX];
};

struct mesh_relay {
    enum mesh_relay_role role;
    uint64_t local_id;
    uint64_t gateway_id;
    struct route_table upstream;
    struct mesh_downlink_entry downlinks[MESH_RELAY_DOWNLINK_ROUTES];
    struct mesh_duplicate_entry duplicates[MESH_RELAY_DUP_CACHE_SIZE];
    struct mesh_command_replay_window command_replay;
    struct flood_seen_entry flood_seen[MESH_RELAY_FLOOD_SEEN_SIZE];
    struct mesh_relay_event_timing_entry event_timings[MESH_RELAY_EVENT_TIMINGS];
    struct mesh_pending_tx pending;
    struct mesh_outbox_record outbox_record;
    struct mesh_route_discovery_state route_discovery;
    struct mesh_route_reply_ack_expectation route_reply_ack_expectation;
    struct mesh_result_bundle_queue result_bundle;
    struct mesh_result_offer_reservation result_offer_reservation;
    /* Runtime-only expiry. Snapshot import/export is a model boundary, not a
     * production reset promise. Reservation.valid arms this deadline; zero is
     * a valid wrapped deadline. */
    uint32_t result_offer_reservation_deadline_ms;
    struct mesh_relay_diagnostics diagnostics;
    union {
        struct mesh_gateway_ack_store *gateway_ack_store;
        struct mesh_anchor_downlink_store *anchor_downlink_store;
    };
    uint8_t duplicate_next;
    uint8_t flood_seen_next;
    uint16_t next_seq;
    /*
     * Highest fully handled gateway-route advertisement sequence for the
     * current upstream epoch. Zero means no advertisement has committed; zero
     * is not a valid wire sequence. Kept last so native builds consume the
     * structure's existing tail padding.
     */
    uint32_t gateway_route_adv_seq;
    /* Monotonic RAM-local owner identity for transit ACK handoffs. */
    uint32_t next_handoff_owner_generation;
    /* Memoized semantic digest of the packet currently being processed. */
    struct mesh_relay_digest_cache digest_cache;
    /* Timestamp of the last gateway ACK-history expiry sweep. */
    uint32_t gateway_ack_sweep_at_ms;
    bool gateway_ack_sweep_valid;
    /* Channel-5 delivery: batch sequencing and admission credit. */
    struct mesh_relay_batch_state batch;
    /* Channel-5 delivery: outstanding parent handoffs, for re-adoption. */
    struct mesh_relay_handoff_memo handoff_memo[MESH_RELAY_HANDOFF_MEMO_SLOTS];
    uint8_t handoff_memo_next;
    /*
     * The local upstream is known lost: every advert and ACK this node emits
     * carries MESH_ROUTE_DEPTH_UNREACHABLE until a fresh parent is selected.
     */
    bool upstream_poisoned;
};

struct mesh_relay_result {
    uint32_t actions;
    int status;
    /* Exactly one primary relay action is produced by a receive/tick path. */
    union {
        struct mesh_outbound forward;
        struct mesh_outbound route_request;
        struct mesh_outbound route_reply;
        struct mesh_outbound gateway_route_adv;
        struct mesh_outbound retransmit;
        struct mesh_outbound terminal;
    };
    /* Exactly one immediate response is produced for the same input frame. */
    union {
        struct mesh_outbound gateway_ack;
        struct mesh_outbound hop_ack;
        struct mesh_outbound busy;
        struct mesh_outbound result_grant;
    };
    /* Route replies may require this ACK beside either primary or busy output. */
    struct mesh_outbound route_reply_ack;
    uint64_t route_reply_backup_next_hop_id;
    uint64_t route_discovery_target_id;
    uint32_t enumeration_prearm_epoch;
    uint32_t enumeration_prearm_hold_ms;
    struct operation_policy_set operation_policy;
    bool enumeration_survey_follows;
    bool route_reply_backup_valid;
    /* The relay committed an in-RAM route ordering transition. */
    bool route_state_changed;
};

void mesh_relay_init(struct mesh_relay *relay,
                     enum mesh_relay_role role,
                     uint64_t local_id,
                     uint64_t gateway_id,
                     uint32_t route_epoch);
/* Seed ordering state in an otherwise empty RAM relay model. This helper does
 * not load storage and makes no reset-recovery promise. */
int mesh_relay_seed_route_freshness(
    struct mesh_relay *relay,
    uint32_t route_epoch,
    uint32_t gateway_route_adv_seq);
/*
 * Configured gateway routes use the same epoch transition as wire-learned
 * routes. Validation is mutation-free before the application commits it.
 */
int mesh_relay_validate_configured_gateway_route(
    const struct mesh_relay *relay,
    const struct route_candidate *candidate,
    bool *epoch_changed);
int mesh_relay_upsert_configured_gateway_route(
    struct mesh_relay *relay,
    const struct route_candidate *candidate);
void mesh_gateway_ack_store_init(struct mesh_gateway_ack_store *store);
int mesh_relay_attach_gateway_ack_store(struct mesh_relay *relay,
                                        struct mesh_gateway_ack_store *store);
/*
 * Caller serializes these with gateway RX. Candidate reservation is permitted
 * only after the assignment semantic gate has fully validated the candidate;
 * a full existing source uses the ordinary source-local replacement policy.
 * Reconciliation must run only after terminal and membership-publication gates:
 * it preserves exact append-only roster members and retires every absent
 * origin so P members leave exactly 50 - P candidate partitions.
 */
int mesh_relay_reserve_gateway_ack_candidate(struct mesh_relay *relay,
                                             uint64_t candidate_id,
                                             uint32_t now_ms);
/*
 * A strictly validated RFC1982-newer source boot proves that the source can
 * no longer emit ACK_CONFIRM for gateway ACKs accepted before that reboot.
 * Preserve those exact semantic identities for duplicate repair, but make
 * them source-locally replaceable and discard any nonsemantic reservation.
 * The caller must serialize this with gateway RX and supply the boot proof.
 */
int mesh_relay_note_gateway_origin_reboot(struct mesh_relay *relay,
                                          uint64_t source_id);
int mesh_relay_reconcile_gateway_ack_membership(
    struct mesh_relay *relay,
    const uint64_t *member_ids,
    size_t member_count);
void mesh_anchor_downlink_store_init(struct mesh_anchor_downlink_store *store);
int mesh_relay_attach_anchor_downlink_store(
    struct mesh_relay *relay,
    struct mesh_anchor_downlink_store *store);
size_t mesh_relay_downlink_capacity(const struct mesh_relay *relay);
const struct mesh_downlink_entry *mesh_relay_downlink_at(
    const struct mesh_relay *relay,
    size_t index);
const struct mesh_downlink_entry *mesh_relay_find_downlink(const struct mesh_relay *relay,
                                                           uint64_t target_id);
const struct mesh_downlink_entry *mesh_relay_find_current_downlink(
    const struct mesh_relay *relay,
    uint64_t target_id);
/* Select only among paths proven by the newest fully handled Here-I-Am.
 * This is the operation-scoped parent set used by enumeration and survey;
 * older same-epoch routes remain available to ordinary delivery failover but
 * cannot impersonate the current activation wave. */
const struct route_candidate *mesh_relay_current_gateway_route(
    const struct mesh_relay *relay,
    uint32_t now_ms);
/*
 * Forced-depth bench anchors must not infer parent depth from a command's TTL:
 * a deeper anchor can rebroadcast a short copy that looks locally correct.
 * Require the current gateway route advertisement to prove that the physical
 * ingress parent is on an exact, strictly descending gateway path.
 */
int mesh_relay_validate_forced_gateway_control_parent(
    const struct mesh_relay *relay,
    uint64_t previous_hop_id,
    uint8_t required_gateway_relay_hops);
/*
 * An accepted gateway-originated channel-5 control frame proves a fresh
 * reverse first hop for the immediate response. origin_ttl is the TTL used by
 * the gateway before any relay forwarded the frame; the stored upstream hop
 * count is derived from the received packet TTL.
 */
int mesh_relay_note_gateway_control_reverse_route(
    struct mesh_relay *relay,
    const struct proto_packet *packet,
    uint64_t previous_hop_id,
    uint8_t link_quality,
    uint8_t origin_ttl,
    uint32_t now_ms);
/*
 * A locally accepted gateway-originated broadcast command flood proves a
 * fresh reverse first hop even before any route discovery runs. The caller
 * supplies the physical ingress hop and the command's own flood epoch;
 * origin_ttl is the TTL used by the gateway before any relay forwarded the
 * frame and the stored upstream hop count is derived from the received
 * packet TTL against it. Cost-based selection keeps any existing better
 * parent selected. A later validated Here-I-Am sequence is fresh physical
 * evidence and clears the observed parent's failures and hold-down; exact
 * duplicate sequences are rejected before this boundary.
 */
int mesh_relay_note_flood_parent_candidate(struct mesh_relay *relay,
                                           const struct proto_packet *packet,
                                           uint64_t previous_hop_id,
                                           uint8_t link_quality,
                                           uint8_t origin_ttl,
                                           uint32_t route_epoch,
                                           uint32_t now_ms);
void mesh_relay_remove_direct_gateway_route(struct mesh_relay *relay);
int mesh_relay_select_next_hop(const struct mesh_relay *relay,
                               uint64_t dst_id,
                               uint64_t *next_hop_id);
int mesh_relay_select_next_hop_for_packet(
    const struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t now_ms,
    uint64_t *next_hop_id);
uint16_t mesh_route_reply_nonce(uint64_t origin_id,
                                uint64_t target_id,
                                uint32_t session_id,
                                uint32_t flood_epoch_id);
int mesh_relay_set_channel9_timing(struct mesh_relay *relay,
                                   uint64_t next_hop_id,
                                   const struct mesh_event_timing *timing);
int mesh_relay_set_channel9_timing_guarded(struct mesh_relay *relay,
                                           uint64_t next_hop_id,
                                           const struct mesh_event_timing *timing,
                                           uint8_t max_active_peers,
                                           struct mesh_relay_channel9_guard_status *status);
int mesh_relay_check_channel9_timing_guarded_direction(
    struct mesh_relay *relay,
    uint64_t next_hop_id,
    const struct mesh_event_timing *timing,
    enum mesh_relay_channel9_direction direction,
    uint8_t max_active_peers,
    struct mesh_relay_channel9_guard_status *status);
int mesh_relay_set_channel9_timing_guarded_direction(
    struct mesh_relay *relay,
    uint64_t next_hop_id,
    const struct mesh_event_timing *timing,
    enum mesh_relay_channel9_direction direction,
    uint8_t max_active_peers,
    struct mesh_relay_channel9_guard_status *status);
void mesh_relay_clear_channel9_timing(struct mesh_relay *relay,
                                      uint64_t next_hop_id);
void mesh_relay_abandon_transit_reservations(struct mesh_relay *relay);
int mesh_relay_abandon_upstream_parent_at(
    struct mesh_relay *relay,
    uint64_t expected_parent_id,
    uint32_t now_ms,
    enum route_delivery_action *action_out);
void mesh_relay_invalidate_upstream_route(struct mesh_relay *relay);
void mesh_relay_invalidate_active_route_path(struct mesh_relay *relay);
void mesh_relay_clear_routes_preserve_epoch(struct mesh_relay *relay);
void mesh_relay_invalidate_routes(struct mesh_relay *relay);
int mesh_relay_require_channel9_event(const struct mesh_relay *relay,
                                      uint64_t next_hop_id,
                                      const struct mesh_channel5_requirements *requirements,
                                      uint32_t now_ms,
                                      struct mesh_event_plan *plan);
int mesh_relay_require_channel9_tx_event(const struct mesh_relay *relay,
                                         uint64_t next_hop_id,
                                         const struct mesh_channel5_requirements *requirements,
                                         uint32_t now_ms,
                                         struct mesh_event_plan *plan);
uint8_t mesh_relay_expire_channel9_timings(struct mesh_relay *relay,
                                           uint32_t now_ms);
uint8_t mesh_relay_expire_routes(struct mesh_relay *relay, uint32_t now_ms);
int mesh_relay_build_route_request(struct mesh_relay *relay,
                                   uint64_t target_id,
                                   struct mesh_outbound *out,
                                   uint32_t now_ms);
int mesh_relay_build_route_request_with_timing(
    struct mesh_relay *relay,
    uint64_t target_id,
    const struct mesh_event_timing *proposed_channel9_timing,
    uint32_t timing_reference_ms,
    struct mesh_outbound *out,
    uint32_t now_ms);
int mesh_relay_build_route_request_with_timing_flags(
    struct mesh_relay *relay,
    uint64_t target_id,
    const struct mesh_event_timing *proposed_channel9_timing,
    uint32_t timing_reference_ms,
    uint8_t request_flags,
    uint16_t route_reply_rx_delay_ms,
    struct mesh_outbound *out,
    uint32_t now_ms);
int mesh_relay_build_gateway_route_adv(struct mesh_relay *relay,
                                       uint32_t gateway_route_seq,
                                       uint32_t now_ms,
                                       struct mesh_outbound *out);
int mesh_relay_build_gateway_route_adv_with_policy(
    struct mesh_relay *relay,
    uint32_t gateway_route_seq,
    uint32_t now_ms,
    const struct operation_policy_set *operation_policy,
    struct mesh_outbound *out);
int mesh_relay_capture_gateway_route_adv_snapshot(
    struct mesh_relay *relay,
    uint32_t gateway_route_seq,
    uint32_t now_ms,
    struct mesh_gateway_route_adv_snapshot *snapshot);
int mesh_relay_capture_gateway_route_adv_snapshot_with_policy(
    struct mesh_relay *relay,
    uint32_t gateway_route_seq,
    uint32_t now_ms,
    const struct operation_policy_set *operation_policy,
    struct mesh_gateway_route_adv_snapshot *snapshot);
int mesh_relay_build_gateway_route_adv_from_snapshot(
    const struct mesh_relay *relay,
    const struct mesh_gateway_route_adv_snapshot *snapshot,
    struct mesh_outbound *out);
int mesh_gateway_route_activation_encode(
    const struct mesh_gateway_route_activation *activation,
    uint8_t *out,
    size_t out_cap,
    size_t *written);
int mesh_gateway_route_activation_decode(
    const uint8_t *data,
    size_t len,
    struct mesh_gateway_route_activation *activation);
int mesh_gateway_route_activation_wake_decode(
    const uint8_t *data,
    size_t len,
    struct uwb_wake_claim_frame *claim,
    struct mesh_gateway_route_activation *activation);
int mesh_relay_gateway_route_activation_for_outbound(
    const struct mesh_outbound *route_adv,
    uint32_t now_ms,
    struct mesh_gateway_route_activation *activation);
uint32_t mesh_gateway_route_activation_start_offset_ms(uint64_t sender_id,
                                                       uint32_t route_seq,
                                                       uint8_t sender_depth);
uint32_t mesh_gateway_route_adv_copy_offset_ms(uint64_t sender_id,
                                               uint32_t route_seq,
                                               uint8_t sender_depth,
                                               uint8_t copy_index);
int mesh_relay_prepare_route_request(struct mesh_relay *relay,
                                     uint64_t target_id,
                                     uint32_t now_ms,
                                     uint32_t random_value,
                                     struct mesh_outbound *out);
int mesh_relay_prepare_route_request_with_timing(
    struct mesh_relay *relay,
    uint64_t target_id,
    const struct mesh_event_timing *proposed_channel9_timing,
    uint32_t timing_reference_ms,
    uint32_t now_ms,
    uint32_t random_value,
    struct mesh_outbound *out);
int mesh_relay_prepare_route_request_with_timing_flags(
    struct mesh_relay *relay,
    uint64_t target_id,
    const struct mesh_event_timing *proposed_channel9_timing,
    uint32_t timing_reference_ms,
    uint8_t request_flags,
    uint16_t route_reply_rx_delay_ms,
    uint32_t now_ms,
    uint32_t random_value,
    struct mesh_outbound *out);
bool mesh_relay_route_discovery_backoff_pending(
    const struct mesh_relay *relay,
    uint64_t target_id,
    uint32_t now_ms,
    uint32_t *remaining_ms);
int mesh_relay_note_direct_gateway_route(struct mesh_relay *relay,
                                         uint32_t now_ms);
int mesh_relay_build_route_reply_for_request(struct mesh_relay *relay,
                                             const struct proto_packet *packet,
                                             const uint8_t *payload,
                                             size_t payload_len,
                                             uint64_t previous_hop_id,
                                             uint32_t now_ms,
                                             uint32_t random_value,
                                             struct mesh_outbound *out);
void mesh_relay_note_route_discovery_ready(struct mesh_relay *relay,
                                           uint64_t target_id);
void mesh_relay_reset_route_discovery(struct mesh_relay *relay);
uint32_t mesh_relay_retry_backoff_ms(uint8_t failure_count, uint32_t random_value);
uint32_t mesh_relay_route_discovery_backoff_ms(uint8_t attempt_count,
                                               uint32_t random_value);
uint32_t mesh_relay_collection_retry_delay_ms(uint32_t base_delay_ms,
                                              uint32_t random_value);
bool mesh_route_request_reply_rx_delay_ms(const struct mesh_outbound *out,
                                          uint16_t *delay_ms);
int mesh_route_request_set_reply_rx_delay_ms(struct mesh_outbound *out,
                                             uint16_t delay_ms);
int mesh_relay_append_status_tlvs(const struct mesh_relay *relay,
                                  uint8_t *payload,
                                  size_t payload_cap,
                                  size_t *offset);
bool mesh_relay_tx_active(const struct mesh_relay *relay);
bool mesh_relay_packet_requires_channel9_payload_event(
    const struct proto_packet *packet);
bool mesh_relay_packet_can_queue_gateway_report(
    const struct mesh_relay *relay,
    const struct proto_packet *packet);
bool mesh_relay_tx_active_local_collection_result(const struct mesh_relay *relay);
bool mesh_relay_result_bundle_pending(const struct mesh_relay *relay);
uint32_t mesh_relay_result_bundle_due_ms(const struct mesh_relay *relay);
void mesh_relay_result_bundle_note_forwarded(struct mesh_relay *relay,
                                             const struct mesh_outbound *out);
int mesh_relay_export_outbox_snapshot(struct mesh_relay *relay,
                                      uint32_t now_ms,
                                      struct mesh_relay_outbox_snapshot *snapshot);
int mesh_relay_restore_outbox_snapshot(struct mesh_relay *relay,
                                       const struct mesh_relay_outbox_snapshot *snapshot,
                                       uint32_t now_ms);
int mesh_relay_export_child_custody_snapshot(
    const struct mesh_relay *relay,
    uint32_t now_ms,
    struct mesh_relay_child_custody_snapshot *snapshot);
int mesh_relay_restore_child_custody_snapshot(
    struct mesh_relay *relay,
    const struct mesh_relay_child_custody_snapshot *snapshot,
    uint32_t now_ms);
void mesh_relay_cancel_tx(struct mesh_relay *relay);
int mesh_relay_cancel_tx_if_matches(struct mesh_relay *relay,
                                    const struct mesh_outbound *out);
int mesh_relay_bind_transit_previous_hop(
    struct mesh_relay *relay,
    const struct mesh_outbound *out,
    uint64_t previous_hop_id);
/* After the exact transit owner is retained and bound, its physical ingress
 * edge is authoritative for later gateway controls addressed to that source. */
int mesh_relay_commit_transit_reverse_route(
    struct mesh_relay *relay,
    const struct mesh_outbound *out,
    uint32_t now_ms);
/* Same reverse-route commit for an exact immutable outbound already retained
 * by an external queue rather than by relay->pending. */
int mesh_relay_commit_queued_transit_reverse_route(
    struct mesh_relay *relay,
    const struct mesh_outbound *out,
    uint64_t previous_hop_id,
    uint32_t now_ms);
bool mesh_relay_defer_tx(struct mesh_relay *relay,
                         uint32_t now_ms,
                         uint32_t random_value);
bool mesh_relay_can_defer_tx(const struct mesh_relay *relay);
uint32_t mesh_relay_outbox_expiry_s_for_packet(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);
int mesh_relay_defer_pending_retry(struct mesh_relay *relay,
                                   uint32_t retry_at_ms);
int mesh_relay_note_retransmit_deferred(struct mesh_relay *relay,
                                        const struct mesh_outbound *out,
                                        uint32_t retry_at_ms);
int mesh_relay_note_pending_parent_failure(struct mesh_relay *relay,
                                           uint32_t now_ms,
                                           uint32_t random_value,
                                           struct mesh_relay_result *result);
int mesh_relay_note_pending_parent_failure_status(struct mesh_relay *relay,
                                                  uint32_t now_ms,
                                                  uint32_t random_value,
                                                  uint32_t *actions,
                                                  int *status);
int mesh_relay_start_tx(struct mesh_relay *relay,
                        const struct proto_packet *packet,
                        const uint8_t *payload,
                        size_t payload_len,
                        uint32_t now_ms,
                        struct mesh_outbound *out);
int mesh_relay_retain_channel9_tx_wait(struct mesh_relay *relay,
                                       const struct proto_packet *packet,
                                       const uint8_t *payload,
                                       size_t payload_len,
                                       uint32_t now_ms,
                                       uint32_t retry_at_ms,
                                       struct mesh_outbound *out);
int mesh_relay_start_result_offer(struct mesh_relay *relay,
                                  const struct proto_packet *packet,
                                  const uint8_t *payload,
                                  size_t payload_len,
                                  uint32_t now_ms,
                                  struct mesh_outbound *out);
int mesh_relay_start_channel9_tx(struct mesh_relay *relay,
                                 const struct proto_packet *packet,
                                 const uint8_t *payload,
                                 size_t payload_len,
                                 const struct mesh_channel5_requirements *requirements,
                                 uint32_t now_ms,
                                 struct mesh_event_plan *plan,
                                 struct mesh_outbound *out);
void mesh_relay_note_channel9_success(struct mesh_relay *relay,
                                      uint64_t next_hop_id,
                                      uint32_t event_start_ms);
void mesh_relay_note_channel9_unobserved_turn(struct mesh_relay *relay,
                                              uint64_t next_hop_id,
                                              uint32_t event_start_ms);
void mesh_relay_note_channel9_rx(struct mesh_relay *relay,
                                 uint64_t next_hop_id,
                                 uint32_t planned_event_start_ms,
                                 uint32_t observed_packet_ms);
void mesh_relay_note_channel9_missed(struct mesh_relay *relay,
                                     uint64_t next_hop_id,
                                     struct mesh_event_diagnostics *diagnostics);
void mesh_relay_note_tx_sent(struct mesh_relay *relay,
                             const struct mesh_outbound *out,
                             uint32_t now_ms);
/*
 * Shorten the gateway-ACK deadline that mesh_relay_note_tx_sent() armed for
 * this exact packet to the moment the application's own synchronous ACK
 * receive turn ends.  ROUTE_GATEWAY_ACK_TIMEOUT_MS stays the fail-safe upper
 * bound for senders that cannot observe the ACK slice themselves; a sender
 * that already listened for the complete first-hop ACK window must not make
 * the relay core wait another two seconds before its ordinary miss handling
 * (failure accounting, jittered retry backoff, route repair) runs.
 *
 * The deadline is only ever moved earlier, and only while the packet is still
 * waiting for its first proof of first-hop progress: once a hop ACK has been
 * observed the window belongs to the longer multi-hop gateway-ACK return path
 * that the caller's short receive turn says nothing about.
 *
 * Returns PROTO_OK when the deadline now expires at or before deadline_ms,
 * PROTO_ERR_STALE when first-hop progress already owns the window, and
 * PROTO_ERR_MALFORMED when no matching packet is waiting for a gateway ACK.
 */
int mesh_relay_note_gateway_ack_window_elapsed(struct mesh_relay *relay,
                                               const struct mesh_outbound *sent,
                                               uint32_t deadline_ms);
int mesh_relay_validate_route_request(const struct mesh_relay *relay,
                                      const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint64_t previous_hop_id,
                                      uint32_t now_ms);
int mesh_relay_validate_gateway_route_adv(
    const struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id);
int mesh_relay_validate_route_reply(const struct mesh_relay *relay,
                                    const struct proto_packet *packet,
                                    const uint8_t *payload,
                                    size_t payload_len,
                                    uint64_t previous_hop_id,
                                    uint32_t now_ms);
int mesh_relay_accept_route_reply_ack(struct mesh_relay *relay,
                                      const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint64_t previous_hop_id);
void mesh_relay_abandon_route_reply_ack(
    struct mesh_relay *relay,
    const struct mesh_outbound *route_reply);
void mesh_relay_note_route_reply_retry(struct mesh_relay *relay);
void mesh_relay_note_delivery_failure_at(struct mesh_relay *relay,
                                         uint64_t dst_id,
                                         uint32_t now_ms);
int mesh_relay_tick(struct mesh_relay *relay,
                    uint32_t now_ms,
                    struct mesh_relay_result *result);
int mesh_relay_tick_with_random(struct mesh_relay *relay,
                                uint32_t now_ms,
                                uint32_t random_value,
                                struct mesh_relay_result *result);

/* ---------------------------------------------------------------------------
 * Channel-5 delivery protocol: depth, credit, explicit backpressure, batching
 * and local route repair.
 * ------------------------------------------------------------------------- */

/*
 * This node's current gateway depth.  Zero for the gateway; otherwise the
 * selected parent's advertised depth plus one, or
 * MESH_ROUTE_DEPTH_UNREACHABLE when the upstream is poisoned, held down,
 * expired or absent.  This is the value every route advert, solicit reply and
 * ACK this node emits must carry.
 */
uint8_t mesh_relay_local_depth(const struct mesh_relay *relay, uint32_t now_ms);
/* Call only after correlating an ACK with this peer and an owned transmission. */
void mesh_relay_note_ack_route_feedback(struct mesh_relay *relay,
                                         uint64_t previous_hop_id,
                                         const struct mesh_ack_flow_control *flow,
                                         uint32_t now_ms);
/*
 * Declare the local upstream lost (parent dead end, retries exhausted on every
 * candidate, or advert expiry).  Depth immediately reads UNREACHABLE.
 */
void mesh_relay_poison_upstream(struct mesh_relay *relay);
bool mesh_relay_upstream_poisoned(const struct mesh_relay *relay);

/*
 * Application-supplied admission capacity: the number of free slots this node
 * can still take frames into right now.  The gateway supplies its BLE-stream
 * free-record count; an anchor supplies its free custody slots.  Until this is
 * called the core assumes one slot when it is idle and none while it is busy.
 */
void mesh_relay_set_local_free_slots(struct mesh_relay *relay,
                                     uint16_t free_slots);
/* Credit this node currently advertises: free slots minus the own reserve. */
uint8_t mesh_relay_local_credit(const struct mesh_relay *relay);

/*
 * Build the explicit refusal a receiver owes a sender it cannot admit: an ACK
 * of the matching type naming no accepted identity, carrying this node's real
 * depth, TLV_BATCH_CREDIT 0 and TLV_RETRY_AFTER_MS.  Use this instead of
 * dropping the frame silently.  retry_after_ms of zero selects
 * MESH_RELAY_BACKPRESSURE_RETRY_AFTER_MS.
 */
int mesh_relay_build_backpressure_ack(struct mesh_relay *relay,
                                      const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint64_t previous_hop_id,
                                      uint16_t retry_after_ms,
                                      uint32_t now_ms,
                                      struct mesh_relay_result *result);

/*
 * Unicast route advert answering a broadcast MSG_ROUTE_SOLICIT.  Returns
 * PROTO_ERR_NOT_FOUND when this node has no finite depth to offer.  The
 * application delays the transmission by a uniform draw in
 * [0, MESH_SOLICIT_REPLY_JITTER_MS).
 */
int mesh_relay_build_solicit_reply(const struct mesh_relay *relay,
                                   const struct proto_packet *solicit,
                                   uint64_t previous_hop_id,
                                   uint32_t now_ms,
                                   struct mesh_outbound *out);

/*
 * Batch sequencing.  The application declares how many further frames it holds
 * for the current next hop; the core stamps TLV_BATCH_PENDING on the first
 * frame, converts the ACK's credit into burst-eligible followers, and stamps
 * TLV_BATCH_REMAINING on each of them.
 */
int mesh_relay_note_batch_pending(struct mesh_relay *relay,
                                  uint8_t frames_for_next_hop);
uint8_t mesh_relay_batch_pending(const struct mesh_relay *relay);
uint8_t mesh_relay_batch_credit(const struct mesh_relay *relay);
/* Stamp TLV_BATCH_PENDING on a first burst frame under construction. */
int mesh_relay_append_batch_pending(const struct mesh_relay *relay,
                                    uint8_t *payload,
                                    size_t payload_cap,
                                    size_t *offset);
/*
 * Claim the next burst-eligible follower.  Returns PROTO_OK and the value the
 * frame must carry in TLV_BATCH_REMAINING while credit is left, or
 * PROTO_ERR_NOT_FOUND when the burst is over and the sender must wait for the
 * batch ACK.
 */
int mesh_relay_next_burst_frame(struct mesh_relay *relay,
                                uint8_t *remaining_out);
void mesh_relay_reset_batch(struct mesh_relay *relay);

/*
 * Receive side: note an admitted frame's batch framing, then ask whether more
 * of that burst is still expected.  While it is, the caller keeps the radio
 * armed and defers its single batch ACK.
 */
int mesh_relay_note_rx_batch_frame(struct mesh_relay *relay,
                                   uint64_t previous_hop_id,
                                   const uint8_t *payload,
                                   size_t payload_len,
                                   uint32_t now_ms);
bool mesh_relay_rx_expects_more(const struct mesh_relay *relay,
                                uint32_t now_ms);
uint32_t mesh_relay_rx_batch_deadline_ms(const struct mesh_relay *relay);
void mesh_relay_clear_rx_batch(struct mesh_relay *relay);

/*
 * Loop freedom.  A candidate is forwardable only when its advertised depth is
 * strictly lower than this node's, except that a node whose own depth is
 * UNREACHABLE may use any finite-depth neighbour - including a former child.
 */
bool mesh_relay_depth_is_forwardable(const struct mesh_relay *relay,
                                     uint8_t candidate_depth,
                                     uint32_t now_ms);

/*
 * Re-adoption.  A packet whose src_id is this node, returned by the parent it
 * was handed to and never gateway-ACKed, is ours again rather than a duplicate.
 */
void mesh_relay_note_parent_handoff(struct mesh_relay *relay,
                                    const struct proto_packet *packet,
                                    uint64_t parent_id);
bool mesh_relay_should_readopt_returned_packet(const struct mesh_relay *relay,
                                               const struct proto_packet *packet,
                                               uint64_t previous_hop_id);
void mesh_relay_note_handoff_gateway_acked(struct mesh_relay *relay,
                                           uint32_t session_id,
                                           uint16_t seq);
int mesh_relay_handle_rx(struct mesh_relay *relay,
                         const struct proto_packet *packet,
                         const uint8_t *payload,
                         size_t payload_len,
                         uint64_t previous_hop_id,
                         uint8_t link_quality,
                         uint32_t now_ms,
                         struct mesh_relay_result *result);
/*
 * Complete transit custody only after the exact child-directed gateway ACK
 * returned by the pending-forward action has physically sent. Queue refusal,
 * send failure, or reset before this commit leaves the original outbox live.
 */
int mesh_relay_commit_transit_gateway_ack_forward(
    struct mesh_relay *relay,
    const struct mesh_outbound *forwarded_ack,
    uint32_t now_ms,
    uint32_t *actions);
/*
 * A topology-operation source retains its own end-to-end retry until it sees
 * the gateway ACK. After one bounded child-contact repair fails, a relay may
 * release only this exact retained ACK handoff and let that source retry by
 * its selected or alternate route. Ordinary data must keep using the physical
 * commit API above.
 */
int mesh_relay_release_topology_gateway_ack_forward(
    struct mesh_relay *relay,
    const struct mesh_outbound *retained_ack,
    uint32_t now_ms);
int mesh_relay_commit_gateway_ack_confirm_terminal(
    struct mesh_relay *relay,
    const struct proto_packet *confirm_packet,
    const uint8_t *confirm_payload,
    size_t confirm_payload_len,
    uint32_t now_ms);
int mesh_relay_commit_next_hop_custody_terminal(
    struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN]);
/*
 * Materialize the current source-local ACK_CONFIRM wire image without
 * replacing its immutable original pending packet. This is also the exact
 * terminal-cleanup identity an adapter must snapshot before handling its ACK.
 */
int mesh_relay_pending_gateway_ack_confirm_wire(
    const struct mesh_relay *relay,
    uint32_t now_ms,
    struct proto_packet *confirm_packet,
    uint8_t *confirm_payload,
    size_t confirm_payload_capacity,
    size_t *confirm_payload_len);
/*
 * Prove that an ACK_CONFIRM names an exact gateway-accepted semantic record.
 * Parsing the confirmation alone is insufficient for phase barriers: the
 * retained gateway ACK history is the authority that the corresponding ACK
 * was actually issued for these bytes.
 */
int mesh_relay_gateway_ack_confirm_history_match(
    struct mesh_relay *relay,
    const struct proto_packet *confirm_packet,
    const uint8_t *confirm_payload,
    size_t confirm_payload_len,
    struct mesh_gateway_ack_confirm_identity *identity);
bool mesh_relay_gateway_delivery_confirmation_pending(
    const struct mesh_relay *relay,
    uint64_t src_id,
    uint8_t msg_type,
    uint32_t session_id,
    uint32_t now_ms);
bool mesh_relay_gateway_identity_confirmation_pending(
    const struct mesh_relay *relay,
    uint64_t src_id,
    uint8_t msg_type,
    uint32_t session_id,
    uint16_t seq,
    uint32_t now_ms);
bool mesh_relay_gateway_operation_confirmation_pending(
    const struct mesh_relay *relay,
    uint8_t msg_type,
    uint32_t session_id,
    uint32_t now_ms);
bool mesh_relay_gateway_origin_confirmation_pending(
    const struct mesh_relay *relay,
    uint64_t src_id,
    uint32_t now_ms);
/* Release an expired/exhausted local outbox only after the application owner
 * has consumed the exact terminal raw record. */
int mesh_relay_commit_terminal_release(
    struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);
/*
 * Commit a gateway-local delivery only after the protocol owner has accepted
 * the exact item. On success, result contains the exact gateway ACK action and
 * binds duplicate handling to the accepted full semantic commitment. Exact
 * Exact retries are ACK-sticky; collection result and bundle retries return
 * to the semantic owner so it can re-arm a missed collection EACK.
 */
int mesh_relay_commit_gateway_delivery(struct mesh_relay *relay,
                                       const struct proto_packet *packet,
                                       const uint8_t *payload,
                                       size_t payload_len,
                                       uint64_t previous_hop_id,
                                       uint32_t now_ms,
                                       struct mesh_relay_result *result);
/*
 * Targeted commands are transport-visible before their application result is
 * admitted. Commit local duplicate suppression only after the anchor has
 * reserved and queued that result; otherwise an exact retry must redeliver.
 */
int mesh_relay_commit_anchor_command_delivery(
    struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t now_ms);
/*
 * Roll back only transport forwarding admission after the platform proves
 * that no RF copy was sent and no deferred retry owner was retained.  Local
 * semantic state remains committed and must be independently idempotent.
 */
int mesh_relay_rollback_forward_admission(
    struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    const struct mesh_relay_result *admission);
int mesh_relay_handle_rx_with_random(struct mesh_relay *relay,
                                     const struct proto_packet *packet,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     uint64_t previous_hop_id,
                                     uint8_t link_quality,
                                     uint32_t now_ms,
                                     uint32_t random_value,
                                     struct mesh_relay_result *result);
int mesh_relay_handle_rx_with_random_radio(
    struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    uint8_t link_quality,
    int8_t link_rsl_dbm,
    bool link_rsl_valid,
    uint32_t now_ms,
    uint32_t random_value,
    struct mesh_relay_result *result);

#ifdef __cplusplus
}
#endif

#endif
