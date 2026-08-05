#ifndef APP_MESH_CH9_ACK_H
#define APP_MESH_CH9_ACK_H

#include "mesh_relay.h"
#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_MESH_CH9_ACK_PEER_MAX 2u
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
#define APP_MESH_CH9_ACK_BATCH_ENTRY_MAX 4u
#else
#define APP_MESH_CH9_ACK_BATCH_ENTRY_MAX 8u
#endif
#define APP_MESH_CH9_ACK_RETRY_BASE_MS 50u
#define APP_MESH_CH9_ACK_RETRY_BASE_MAX_MS 400u

struct app_mesh_ch9_tx_ack_entry {
    const struct mesh_outbound *outbound;
    bool acked;
};

struct app_mesh_ch9_tx_ack_result {
    uint8_t acked_now;
    uint8_t unacked_count;
    bool any_match;
    bool all_acked;
};

struct app_mesh_ch9_tx_retry_entry {
    struct mesh_outbound *outbound;
    bool *acked;
};

struct app_mesh_ch9_tx_retry_ops {
    int (*put)(const struct mesh_outbound *outbound, void *ctx);
    int (*get)(struct mesh_outbound *outbound, void *ctx);
    uint8_t (*queue_used)(void *ctx);
    void (*note_drop)(void *ctx);
    void *ctx;
};

struct app_mesh_ch9_tx_retry_result {
    uint8_t requeued;
    uint8_t retained;
    uint8_t dropped;
    uint8_t queued_before;
    uint8_t queued_after;
};

struct app_mesh_ch9_ack_complete_state {
    bool route_test_enabled;
    bool transmitter_role;
    uint8_t report_tx_queue_used;
    bool route_waiting_tx_valid;
    bool ack_batch_valid;
};

enum app_mesh_ch9_ack_queue_result {
    APP_MESH_CH9_ACK_QUEUE_ADDED = 0,
    APP_MESH_CH9_ACK_QUEUE_DUPLICATE,
    APP_MESH_CH9_ACK_QUEUE_REPLACED,
    APP_MESH_CH9_ACK_QUEUE_SUPPRESSED_BY_FORWARDED_ACK,
    APP_MESH_CH9_ACK_QUEUE_FORWARDED_BUSY,
    APP_MESH_CH9_ACK_QUEUE_BATCH_FULL,
    APP_MESH_CH9_ACK_QUEUE_TABLE_FULL,
    APP_MESH_CH9_ACK_QUEUE_SEMANTIC_CONFLICT,
};

struct app_mesh_ch9_ack_batch_entry {
    uint32_t session_id;
    uint32_t packet_id;
    uint16_t seq;
    bool has_packet_id;
};

struct app_mesh_ch9_ack_batch {
    struct mesh_outbound template_ack;
    struct app_mesh_ch9_ack_batch_entry
        entries[APP_MESH_CH9_ACK_BATCH_ENTRY_MAX];
    uint64_t peer_id;
    uint32_t retry_not_before_ms;
    uint16_t retry_round;
    uint8_t count;
    bool valid;
    bool preserve_payload;
    bool retry_deferred;
};

/* One slot for the production upstream and one for the downstream peer. */
struct app_mesh_ch9_ack_table {
    struct app_mesh_ch9_ack_batch batches[APP_MESH_CH9_ACK_PEER_MAX];
};

typedef int (*app_mesh_ch9_ack_flush_fn)(
    const struct mesh_outbound *outbound,
    void *ctx);

enum app_mesh_ch9_timeout_pressure_action {
    APP_MESH_CH9_TIMEOUT_RETRY = 0,
    APP_MESH_CH9_TIMEOUT_DROP_TRANSIT,
    APP_MESH_CH9_TIMEOUT_DEFER_LOCAL,
    APP_MESH_CH9_TIMEOUT_PREEMPT_FOR_LOCAL,
};

void app_mesh_ch9_ack_table_init(struct app_mesh_ch9_ack_table *table);
uint8_t app_mesh_ch9_ack_table_peer_count(
    const struct app_mesh_ch9_ack_table *table);
bool app_mesh_ch9_ack_table_any_pending(
    const struct app_mesh_ch9_ack_table *table);
bool app_mesh_ch9_ack_table_pending_for_peer(
    const struct app_mesh_ch9_ack_table *table,
    uint64_t peer_id);
const struct app_mesh_ch9_ack_batch *app_mesh_ch9_ack_table_get_peer(
    const struct app_mesh_ch9_ack_table *table,
    uint64_t peer_id);

int app_mesh_ch9_ack_table_queue(
    struct app_mesh_ch9_ack_table *table,
    const struct mesh_outbound *ack,
    const struct app_mesh_ch9_ack_batch_entry *entry,
    enum app_mesh_ch9_ack_queue_result *result);
int app_mesh_ch9_ack_table_queue_forwarded(
    struct app_mesh_ch9_ack_table *table,
    const struct mesh_outbound *ack,
    enum app_mesh_ch9_ack_queue_result *result);
int app_mesh_ch9_ack_table_build_peer(
    const struct app_mesh_ch9_ack_table *table,
    uint64_t peer_id,
    struct mesh_outbound *outbound);
bool app_mesh_ch9_ack_table_clear_peer(
    struct app_mesh_ch9_ack_table *table,
    uint64_t peer_id);
bool app_mesh_ch9_ack_table_retry_ready(
    const struct app_mesh_ch9_ack_table *table,
    uint64_t peer_id,
    uint32_t now_ms);
uint32_t app_mesh_ch9_ack_table_retry_wait_ms(
    const struct app_mesh_ch9_ack_table *table,
    uint64_t peer_id,
    uint32_t now_ms);
int app_mesh_ch9_ack_table_note_send_failure(
    struct app_mesh_ch9_ack_table *table,
    uint64_t peer_id,
    uint32_t now_ms,
    uint32_t attempt_entropy,
    uint32_t *delay_ms_out);

/* The batch remains owned by the table unless the flush callback succeeds. */
int app_mesh_ch9_ack_table_flush_peer(
    struct app_mesh_ch9_ack_table *table,
    uint64_t peer_id,
    app_mesh_ch9_ack_flush_fn flush,
    void *ctx);

int app_mesh_ch9_tx_ack_apply(const struct proto_packet *ack_packet,
                              const uint8_t *payload,
                              size_t payload_len,
                              struct app_mesh_ch9_tx_ack_entry *entries,
                              uint8_t entry_count,
                              struct app_mesh_ch9_tx_ack_result *result);

int app_mesh_ch9_tx_requeue_unacked(struct app_mesh_ch9_tx_retry_entry *entries,
                                    uint8_t entry_count,
                                    uint32_t now_ms,
                                    const struct app_mesh_ch9_tx_retry_ops *ops,
                                    struct app_mesh_ch9_tx_retry_result *result);

bool app_mesh_ch9_tx_should_track_ack(const struct proto_packet *packet,
                                      bool relay_collection_result_active);

bool app_mesh_ch9_tx_should_track_sent(const struct mesh_outbound *sent,
                                       uint64_t local_id);
bool app_mesh_ch9_core_ack_wait_active(const struct mesh_pending_tx *pending,
                                       bool relay_tx_active);
bool app_mesh_ch9_core_pending_allows_rx(const struct mesh_pending_tx *pending,
                                         bool relay_tx_active);

uint8_t app_mesh_ch9_tx_max_in_flight(const struct proto_packet *packet,
                                      uint64_t next_hop_id,
                                      uint8_t configured_max);
bool app_mesh_ch9_tx_requires_tracked_single(const struct proto_packet *packet,
                                             uint64_t next_hop_id,
                                             uint8_t configured_max);
bool app_mesh_ch9_retry_next_local_tx_prepare_ms(
    const struct mesh_event_timing *timing,
    uint16_t minimum_guard_ms,
    uint32_t *prepare_ms);
/* event_start_ms is an armed plan boundary; wrapped uptime zero is valid. */
bool app_mesh_ch9_wait_plan_retry_delay_ms(uint32_t now_ms,
                                          uint32_t event_start_ms,
                                          uint16_t minimum_guard_ms,
                                          uint32_t *delay_ms);

bool app_mesh_ch9_tx_timeout_counts_route_failure(
    const struct mesh_outbound *outbound,
    uint64_t next_hop_id,
    uint64_t gateway_id);

/*
 * local_origin_priority_needs_capacity is an explicit caller-owned pressure
 * signal. A downstream reservation alone never authorizes dropping transit
 * custody.
 */
enum app_mesh_ch9_timeout_pressure_action
app_mesh_ch9_timeout_pressure_decide(const struct mesh_outbound *outbound,
                                     bool anchor_role,
                                     bool downstream_reserved,
                                     bool local_origin_priority_needs_capacity,
                                     uint64_t local_id);

bool app_mesh_direct_gateway_ack_matches(const struct mesh_outbound *sent,
                                         const struct proto_packet *ack_packet,
                                         const uint8_t *payload,
                                         size_t payload_len,
                                         uint64_t previous_hop_id,
                                         uint64_t gateway_id);

bool app_mesh_ch9_ack_complete_should_close_timing(
    const struct app_mesh_ch9_ack_complete_state *state);

#endif
