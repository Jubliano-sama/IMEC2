#ifndef MESH_RELAY_H
#define MESH_RELAY_H

#include "mesh.h"
#include "mesh_capacity.h"
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

#define MESH_BROADCAST_ID 0u
#define MESH_RELAY_DOWNLINK_ROUTES 16u
#define MESH_RELAY_ANCHOR_DOWNLINK_ROUTES MESH_CONNECTED_MAX_ANCHORS
#define MESH_RELAY_ANCHOR_DOWNLINK_OVERFLOW_ROUTES \
    (MESH_RELAY_ANCHOR_DOWNLINK_ROUTES - MESH_RELAY_DOWNLINK_ROUTES)
#define MESH_RELAY_DUP_CACHE_SIZE 16u
#define MESH_RELAY_GATEWAY_ACK_ORIGIN_MAX MESH_CONNECTED_MAX_ANCHORS
#define MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN 4u
#define MESH_RELAY_GATEWAY_ACK_CAPACITY \
    (MESH_RELAY_GATEWAY_ACK_ORIGIN_MAX * \
     MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN)
#define MESH_RELAY_GATEWAY_ACK_CANDIDATE_BITMAP_BYTES \
    ((MESH_RELAY_GATEWAY_ACK_CAPACITY + 7u) / 8u)
#define MESH_RELAY_FLOOD_SEEN_SIZE 16u
#define MESH_RELAY_EVENT_TIMINGS 16u
#define MESH_RELAY_DOWNLINK_MAX_FAILURES ROUTE_MAX_FAILURES
#define MESH_RELAY_ROUTE_DISCOVERY_BACKOFF_BASE_MS 1000u
#define MESH_RELAY_ROUTE_DISCOVERY_BACKOFF_MAX_MS 60000u
#define MESH_RELAY_RETRY_BACKOFF_MAX_MS \
    (ROUTE_RETRY_BACKOFF_MAX_MS + (ROUTE_RETRY_BACKOFF_MAX_MS / 2u))
#define MESH_RELAY_GATEWAY_ACK_RETRY_BUDGET_MAX_MS \
    ((ROUTE_GATEWAY_ACK_TIMEOUT_MS * (ROUTE_RETRIES_PER_CANDIDATE + 1u)) + \
     ROUTE_RETRY_BACKOFF_FIRST_MS + (ROUTE_RETRY_BACKOFF_FIRST_MS / 2u) + \
     ROUTE_RETRY_BACKOFF_SECOND_MS + (ROUTE_RETRY_BACKOFF_SECOND_MS / 2u) + \
     MESH_RELAY_RETRY_BACKOFF_MAX_MS)
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
#define MESH_GATEWAY_ROUTE_ADV_PAYLOAD_LEN \
    (MESH_GATEWAY_ROUTE_ADV_FIXED_TLV_BYTES + \
     PROTO_TLV_U64_ENCODED_LEN)
#define MESH_GATEWAY_ROUTE_ADV_POLICY_PAYLOAD_LEN \
    (MESH_GATEWAY_ROUTE_ADV_PAYLOAD_LEN + \
     OPERATION_POLICY_ALL_TLVS_LEN)
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
#define MESH_RELAY_OUTBOX_SNAPSHOT_VERSION 4u
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

struct persistent_outbox_record {
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
    MESH_RELAY_ACTION_DELIVER_LOCAL = 1u << 1,
    MESH_RELAY_ACTION_FORWARD = 1u << 2,
    MESH_RELAY_ACTION_SEND_GATEWAY_ACK = 1u << 3,
    MESH_RELAY_ACTION_DROP = 1u << 6,
    /*
     * The original gateway ACK was authenticated, but source custody now
     * belongs to a compact durable ACK-confirm packet rather than being done.
     */
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

enum mesh_relay_tx_state {
    MESH_RELAY_TX_IDLE = 0,
    MESH_RELAY_TX_WAIT_GATEWAY_ACK = 1,
    MESH_RELAY_TX_WAIT_RETRY_BACKOFF = 2,
    MESH_RELAY_TX_WAIT_RESULT_GRANT = 3,
    MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD = 4,
    /*
     * The retry/expiry policy is terminal, but application-owned durable
     * producers must be retired before the relay may release this exact raw
     * record or overwrite its last persisted copy.
     */
    MESH_RELAY_TX_WAIT_TERMINAL_COMMIT = 5,
};

struct mesh_outbound {
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    uint16_t payload_len;
    uint8_t radio_channel;
    uint64_t next_hop_id;
    uint32_t queued_at_ms;
    uint32_t earliest_tx_ms;
    uint8_t flood_retry_count;
    bool queued_at_valid;
    bool earliest_tx_valid;
};

struct mesh_gateway_route_adv_snapshot {
    uint32_t gateway_route_seq;
    uint32_t queued_at_ms;
    uint16_t gateway_epoch;
    uint16_t packet_seq;
    uint16_t capacity_validity_interval_ms;
    uint8_t gateway_capacity_state;
    uint8_t operation_policy_tlvs[OPERATION_POLICY_ALL_TLVS_LEN];
    uint8_t operation_policy_tlvs_len;
    bool operation_policy_present;
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
 * A new explicit batch may retire only the same origin's prior batched
 * identities. Each source has a fixed four-identity partition; a fifth
 * accepted identity replaces only that source's shortest-lived record.
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
        identities[MESH_RELAY_GATEWAY_ACK_CAPACITY];
    uint8_t candidate_identity_bits[
        MESH_RELAY_GATEWAY_ACK_CANDIDATE_BITMAP_BYTES];
};

struct mesh_upstream_ancestry_entry {
    struct mesh_route_path path;
    uint64_t next_hop_id;
    uint32_t route_epoch;
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
_Static_assert(MESH_RELAY_GATEWAY_ACK_CANDIDATE_BITMAP_BYTES == 25u,
               "gateway ACK candidate bitmap must cover every identity");
_Static_assert(sizeof(struct mesh_gateway_ack_store) == 9432u,
               "gateway ACK store must fit role-overlaid static storage");
_Static_assert(MESH_RELAY_GATEWAY_ACK_CAPACITY == 200u,
               "gateway ACK history must cover 50 four-packet members");
_Static_assert(MESH_RELAY_GATEWAY_ACK_CAPACITY <= UINT8_MAX,
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
    uint8_t busy_retry_round;
    bool hop_ack_observed_since_send;
};

struct mesh_relay_outbox_snapshot {
    uint16_t version;
    enum mesh_relay_role role;
    uint64_t local_id;
    uint64_t gateway_id;
    struct persistent_outbox_record record;
    struct mesh_pending_tx pending;
    uint32_t route_epoch;
    uint32_t snapshot_at_ms;
    bool valid;
};

struct mesh_route_discovery_state {
    uint64_t target_id;
    uint8_t attempts;
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
    struct persistent_outbox_record outbox_record;
    struct mesh_route_discovery_state route_discovery;
    struct mesh_route_reply_ack_expectation route_reply_ack_expectation;
    struct mesh_result_bundle_queue result_bundle;
    struct mesh_result_offer_reservation result_offer_reservation;
    /*
     * Runtime-only expiry. The persisted version-2 custody snapshot retains
     * the full result commitment; a restored reservation receives a bounded
     * lease instead of becoming permanent. Reservation.valid arms this
     * deadline; zero is a valid wrapped deadline.
     */
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
    struct operation_policy_set operation_policy;
    /*
     * A route-state transition commits before the platform performs the
     * consequent work. Preserve the prior advertisement high-water state so
     * forwarding rollback and durable-state diagnostics retain its exact
     * predecessor.
     */
    uint32_t forward_admission_previous_gateway_route_adv_seq;
    /*
     * Route epoch/freshness changes must become durable before the platform
     * installs policy, forwards control, or resumes epoch-bound custody.
     * Preserve the predecessor so tests and failure handling can prove the
     * transition was monotonic.
    */
    uint32_t route_state_previous_epoch;
    bool route_reply_backup_valid;
    bool forward_admission_gateway_epoch_changed;
    bool route_state_changed;
    bool route_state_durable;
};

void mesh_relay_init(struct mesh_relay *relay,
                     enum mesh_relay_role role,
                     uint64_t local_id,
                     uint64_t gateway_id,
                     uint32_t route_epoch);
/*
 * Restore only the durable ordering state after mesh_relay_init(), before
 * epoch-bound outbox or child-custody snapshots are admitted.
 */
int mesh_relay_restore_route_freshness(
    struct mesh_relay *relay,
    uint32_t route_epoch,
    uint32_t gateway_route_adv_seq);
/*
 * Configured gateway routes use the same epoch transition as wire-learned
 * routes. Validation is mutation-free so an application can durably reserve
 * a newer epoch before committing the route.
 */
int mesh_relay_validate_configured_gateway_route(
    const struct mesh_relay *relay,
    const struct route_candidate *candidate,
    bool *epoch_changed);
int mesh_relay_upsert_configured_gateway_route(
    struct mesh_relay *relay,
    const struct route_candidate *candidate);
/*
 * Mark the result only after platform persistence has completed and read back.
 * Transport rollback may release provisional forwarding admission, but it
 * must never regress route ordering after this boundary.
 */
int mesh_relay_mark_route_state_durable(
    const struct mesh_relay *relay,
    struct mesh_relay_result *result);
void mesh_gateway_ack_store_init(struct mesh_gateway_ack_store *store);
int mesh_relay_attach_gateway_ack_store(struct mesh_relay *relay,
                                        struct mesh_gateway_ack_store *store);
/*
 * Caller serializes these with gateway RX. Candidate reservation is permitted
 * only after the assignment semantic gate has fully validated the candidate;
 * a full existing source uses the ordinary source-local replacement policy.
 * Reconciliation must run only after terminal and durable-publication gates:
 * it preserves exact append-only roster members and retires every absent
 * origin so P members leave exactly 50 - P candidate partitions.
 */
int mesh_relay_reserve_gateway_ack_candidate(struct mesh_relay *relay,
                                             uint64_t candidate_id,
                                             uint32_t now_ms);
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
/* Caller must first accept a current-survey local gateway report and its RX metadata. */
int mesh_relay_note_gateway_survey_reverse_route(struct mesh_relay *relay,
                                                 uint64_t target_id,
                                                 uint64_t next_hop_id,
                                                 uint8_t quality,
                                                 uint32_t now_ms);
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
void mesh_relay_remove_direct_gateway_route(struct mesh_relay *relay);
int mesh_relay_select_next_hop(const struct mesh_relay *relay,
                               uint64_t dst_id,
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
int mesh_outbound_set_flood_packet_age_ms(struct mesh_outbound *out,
                                          uint32_t age_ms);
int mesh_relay_append_status_tlvs(const struct mesh_relay *relay,
                                  uint8_t *payload,
                                  size_t payload_cap,
                                  size_t *offset);
bool mesh_relay_tx_active(const struct mesh_relay *relay);
bool mesh_relay_packet_requires_channel9_payload_event(
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
void mesh_relay_note_channel9_tx(struct mesh_relay *relay,
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
int mesh_relay_commit_gateway_ack_confirm_terminal(
    struct mesh_relay *relay,
    const struct proto_packet *confirm_packet,
    const uint8_t *confirm_payload,
    size_t confirm_payload_len,
    uint32_t now_ms);
/*
 * Release an expired/exhausted local outbox only after every application
 * producer has durably consumed the exact terminal raw record.
 */
int mesh_relay_commit_terminal_release(
    struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);
/*
 * Commit a gateway-local delivery only after the protocol owner has accepted
 * the exact item. On success, result contains the exact gateway ACK action and
 * binds duplicate handling to the accepted full semantic commitment. Exact
 * survey retries are ACK-sticky; collection result and bundle retries return
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

#ifdef __cplusplus
}
#endif

#endif
