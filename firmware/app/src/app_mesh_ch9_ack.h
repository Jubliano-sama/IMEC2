#ifndef APP_MESH_CH9_ACK_H
#define APP_MESH_CH9_ACK_H

#include "mesh_relay.h"
#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct app_mesh_ch9_tx_ack_entry {
    uint32_t session_id;
    uint16_t seq;
    bool acked;
};

struct app_mesh_ch9_tx_ack_result {
    uint8_t acked_now;
    uint8_t unacked_count;
    bool any_match;
    bool all_acked;
};

struct app_mesh_ch9_tx_retry_entry {
    struct mesh_outbound outbound;
    bool acked;
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

uint8_t app_mesh_ch9_tx_max_in_flight(const struct proto_packet *packet,
                                      uint64_t next_hop_id,
                                      uint8_t configured_max);

bool app_mesh_ch9_tx_timeout_counts_gateway_failure(
    const struct mesh_outbound *outbound,
    uint64_t next_hop_id,
    uint64_t gateway_id);

bool app_mesh_direct_gateway_ack_matches(const struct mesh_outbound *sent,
                                         const struct proto_packet *ack_packet,
                                         const uint8_t *payload,
                                         size_t payload_len,
                                         uint64_t previous_hop_id,
                                         uint64_t gateway_id);

bool app_mesh_ch9_ack_complete_should_close_timing(
    const struct app_mesh_ch9_ack_complete_state *state);

#endif
