#ifndef MESH_RELAY_H
#define MESH_RELAY_H

#include "mesh.h"
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
#define MESH_RELAY_DUP_CACHE_SIZE 16u
#define MESH_RELAY_EVENT_TIMINGS 16u
#define MESH_RELAY_DOWNLINK_MAX_FAILURES 3u
#define MESH_RELAY_ROUTE_DISCOVERY_MAX_ATTEMPTS 5u
#define MESH_RELAY_ROUTE_DISCOVERY_BACKOFF_BASE_MS 250u
#define MESH_RELAY_ROUTE_DISCOVERY_BACKOFF_MAX_MS 4000u
#define FLOOD_EPOCH_LOCAL_TTL 2u
#define FLOOD_EPOCH_REGIONAL_TTL 4u
#define FLOOD_EPOCH_GLOBAL_TTL 8u
#define FLOOD_EPOCH_CRITICAL_TTL 12u
#define FLOOD_FORWARD_MAX_NORMAL 1u
#define FLOOD_FORWARD_MAX_CRITICAL 2u
#define FLOOD_FORWARD_SUPPRESS_AFTER_HEARD 2u
#define FLOOD_WAVE_MS 1400u
#define FLOOD_RELAY_BURST_MS 600u
#define FLOOD_RELAY_REPEAT_MS 40u
#define FLOOD_POST_ROOT_GUARD_MS 150u
#define C5_POLITE_SNIFF_MS 6u
#define C5_POLITE_BACKOFF_MIN_MS 20u
#define C5_POLITE_BACKOFF_MAX_MS 1600u
#define C5_POLITE_DEFERRAL_MAX 8u
#define RREP_ACK_TIMEOUT_MS 150u
#define RREP_RETRY_COUNT_PER_HOP 4u
#define PARENT_CANDIDATE_COUNT 3u
#define REVERSE_PATH_CANDIDATE_COUNT 2u
#define RELAY_BUSY_RETRY_MIN_MS 500u
#define RELAY_BUSY_RETRY_MAX_MS 5000u
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
#define COMMAND_RESULT_EXPIRY_DEFAULT_S 86400u
#define ROUTE_PARENT_HOLDDOWN_S 30u
#define FLOOD_BETTER_METRIC_MARGIN_PERCENT 10u

enum flood_epoch_type {
    FLOOD_EPOCH_TYPE_ROUTE_SOLICIT = 1u,
    FLOOD_EPOCH_TYPE_GATEWAY_ROUTE_ADV = 2u,
    FLOOD_EPOCH_TYPE_GATEWAY_COMMAND = 3u,
    FLOOD_EPOCH_TYPE_COLLECTION_STATUS = 4u,
};

enum relay_capacity_state {
    RELAY_CAP_GREEN = 0u,
    RELAY_CAP_YELLOW = 1u,
    RELAY_CAP_RED = 2u,
    RELAY_CAP_BLACK = 3u,
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
    bool channel9_timing_valid;
    uint32_t last_observed_ms;
    uint32_t last_success_ms;
    uint32_t hold_down_until_ms;
};

struct flood_seen_entry {
    uint64_t gateway_id;
    uint16_t gateway_epoch;
    uint32_t flood_epoch_id;
    uint8_t flood_type;
    uint64_t origin_id;
    uint32_t origin_request_id;
    uint8_t best_hop_count;
    uint16_t best_metric;
    uint64_t best_previous_hop;
    uint64_t backup_previous_hop;
    uint8_t forward_count;
    uint32_t expires_at_ms;
};

struct persistent_outbox_record {
    uint32_t packet_id;
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
    uint16_t payload_crc;
    uint16_t payload_len;
};

enum mesh_relay_role {
    MESH_RELAY_ROLE_ANCHOR = 1,
    MESH_RELAY_ROLE_GATEWAY = 2,
};

enum mesh_relay_action {
    MESH_RELAY_ACTION_NONE = 0u,
    MESH_RELAY_ACTION_DELIVER_LOCAL = 1u << 1,
    MESH_RELAY_ACTION_FORWARD = 1u << 2,
    MESH_RELAY_ACTION_SEND_GATEWAY_ACK = 1u << 3,
    MESH_RELAY_ACTION_DROP = 1u << 6,
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
};

enum mesh_relay_tx_state {
    MESH_RELAY_TX_IDLE = 0,
    MESH_RELAY_TX_WAIT_GATEWAY_ACK = 1,
    MESH_RELAY_TX_WAIT_RETRY_BACKOFF = 2,
};

struct mesh_outbound {
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    uint16_t payload_len;
    uint8_t radio_channel;
    uint64_t next_hop_id;
    uint32_t queued_at_ms;
    uint32_t earliest_tx_ms;
};

struct mesh_downlink_entry {
    uint64_t target_id;
    uint64_t next_hop_id;
    uint64_t gateway_id;
    uint32_t route_epoch;
    uint32_t last_seen_ms;
    uint8_t hop_count;
    uint8_t quality;
    bool valid;
};

struct mesh_duplicate_entry {
    uint8_t msg_type;
    uint64_t src_id;
    uint64_t dst_id;
    uint32_t session_id;
    uint32_t last_seen_ms;
    uint16_t seq;
    bool valid;
};

struct mesh_relay_event_timing_entry {
    uint64_t next_hop_id;
    struct mesh_event_timing timing;
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
};

struct mesh_route_discovery_state {
    uint64_t target_id;
    uint8_t attempts;
    uint32_t next_request_ms;
    bool active;
};

struct mesh_relay {
    enum mesh_relay_role role;
    uint64_t local_id;
    uint64_t gateway_id;
    struct route_table upstream;
    struct mesh_downlink_entry downlinks[MESH_RELAY_DOWNLINK_ROUTES];
    struct mesh_duplicate_entry duplicates[MESH_RELAY_DUP_CACHE_SIZE];
    struct mesh_relay_event_timing_entry event_timings[MESH_RELAY_EVENT_TIMINGS];
    struct mesh_pending_tx pending;
    struct mesh_route_discovery_state route_discovery;
    uint8_t duplicate_next;
    uint16_t next_seq;
};

struct mesh_relay_result {
    uint32_t actions;
    int status;
    struct mesh_outbound forward;
    struct mesh_outbound gateway_ack;
    struct mesh_outbound route_request;
    struct mesh_outbound route_reply;
    struct mesh_outbound route_reply_ack;
    struct mesh_outbound retransmit;
    struct mesh_outbound hop_ack;
};

void mesh_relay_init(struct mesh_relay *relay,
                     enum mesh_relay_role role,
                     uint64_t local_id,
                     uint64_t gateway_id,
                     uint32_t route_epoch);
const struct mesh_downlink_entry *mesh_relay_find_downlink(const struct mesh_relay *relay,
                                                           uint64_t target_id);
int mesh_relay_select_next_hop(const struct mesh_relay *relay,
                               uint64_t dst_id,
                               uint64_t *next_hop_id);
int mesh_relay_set_channel9_timing(struct mesh_relay *relay,
                                   uint64_t next_hop_id,
                                   const struct mesh_event_timing *timing);
int mesh_relay_set_channel9_timing_guarded(struct mesh_relay *relay,
                                           uint64_t next_hop_id,
                                           const struct mesh_event_timing *timing,
                                           uint8_t max_active_peers,
                                           struct mesh_relay_channel9_guard_status *status);
void mesh_relay_clear_channel9_timing(struct mesh_relay *relay,
                                      uint64_t next_hop_id);
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
int mesh_relay_prepare_route_request(struct mesh_relay *relay,
                                     uint64_t target_id,
                                     uint32_t now_ms,
                                     uint32_t random_value,
                                     struct mesh_outbound *out);
void mesh_relay_note_route_discovery_ready(struct mesh_relay *relay,
                                           uint64_t target_id);
void mesh_relay_reset_route_discovery(struct mesh_relay *relay);
uint32_t mesh_relay_retry_backoff_ms(uint8_t failure_count, uint32_t random_value);
uint32_t mesh_relay_route_discovery_backoff_ms(uint8_t attempt_count,
                                               uint32_t random_value);
int mesh_relay_append_status_tlvs(const struct mesh_relay *relay,
                                  uint8_t *payload,
                                  size_t payload_cap,
                                  size_t *offset);
bool mesh_relay_tx_active(const struct mesh_relay *relay);
void mesh_relay_cancel_tx(struct mesh_relay *relay);
int mesh_relay_start_tx(struct mesh_relay *relay,
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
void mesh_relay_note_delivery_failure(struct mesh_relay *relay,
                                      uint64_t dst_id);
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

#ifdef __cplusplus
}
#endif

#endif
