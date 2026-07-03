#ifndef APP_MESH_SMOKE_FAST_H
#define APP_MESH_SMOKE_FAST_H

#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MESH_SMOKE_FAST_LATENCY_SAMPLES 32u
#define MESH_SMOKE_FAST_MISSING_TRACKED 16u

enum mesh_smoke_fast_ch9_state {
    MESH_SMOKE_FAST_CH9_NONE = 0,
    MESH_SMOKE_FAST_CH9_ROUTE_ONLY = 1,
    MESH_SMOKE_FAST_CH9_TIMING_STALE = 2,
    MESH_SMOKE_FAST_CH9_TIMING_FRESH = 3,
};

enum mesh_smoke_fast_defer_reason {
    MESH_SMOKE_FAST_DEFER_NONE = 0,
    MESH_SMOKE_FAST_DEFER_RELAY_TX = 1,
    MESH_SMOKE_FAST_DEFER_ROUTE_WAIT = 2,
    MESH_SMOKE_FAST_DEFER_ACK_WAIT = 3,
    MESH_SMOKE_FAST_DEFER_QUEUE_FULL = 4,
};

struct mesh_smoke_fast_payload_input {
    uint32_t packet_id;
    uint32_t build_uptime_ms;
    uint32_t packet_age_ms;
    uint32_t drop_count;
    uint64_t origin_id;
    uint64_t target_id;
    uint64_t selected_parent_id;
    uint16_t attempt;
    uint8_t device_role;
    uint8_t mesh_channel;
    uint8_t ch9_timing_state;
    uint32_t flags;
};

struct mesh_smoke_fast_payload {
    uint32_t packet_id;
    uint32_t build_uptime_ms;
    uint32_t packet_age_ms;
    uint32_t drop_count;
    uint64_t origin_id;
    uint64_t target_id;
    uint64_t selected_parent_id;
    uint16_t attempt;
    uint8_t retry_count;
    uint8_t device_role;
    uint8_t mesh_channel;
    uint8_t ch9_timing_state;
    uint32_t flags;
    uint16_t payload_crc;
};

struct mesh_smoke_fast_tx_gate {
    bool relay_tx_active;
    bool route_waiting_active;
    bool ack_wait_active;
    uint32_t queue_used;
    uint32_t queue_depth;
    uint32_t configured_interval_ms;
    bool fast_mode;
};

struct mesh_smoke_fast_tx_decision {
    enum mesh_smoke_fast_defer_reason reason;
    uint32_t queue_headroom;
    uint32_t delay_ms;
    bool can_queue;
};

struct mesh_smoke_fast_summary {
    uint32_t delivered_count;
    uint32_t duplicate_count;
    uint32_t gap_count;
    uint32_t missing_count;
    uint32_t later_delivered_missing_count;
    uint32_t attributed_missing_count;
    uint32_t crc_fail_count;
    uint32_t age_invalid_count;
    uint32_t retry_total;
    uint32_t retry_max;
    uint32_t missed_ch9_events;
    uint32_t c5_refreshes;
    uint32_t queue_depth_max;
    uint32_t gateway_ack_latency_p50_ms;
    uint32_t gateway_ack_latency_p95_ms;
    uint32_t gateway_ack_latency_max_ms;
    uint32_t first_delivered_ms;
    uint32_t last_delivered_ms;
    uint32_t last_packet_id;
    uint32_t last_gap_start;
    uint32_t last_gap_end;
    uint32_t last_drop_or_defer_reason;
};

struct mesh_smoke_fast_state {
    struct mesh_smoke_fast_summary summary;
    uint32_t latency_samples[MESH_SMOKE_FAST_LATENCY_SAMPLES];
    uint32_t missing[MESH_SMOKE_FAST_MISSING_TRACKED];
    uint8_t latency_next;
    uint8_t latency_count;
};

int mesh_smoke_fast_payload_append(uint8_t *payload,
                                   size_t payload_cap,
                                   size_t *payload_len,
                                   const struct mesh_smoke_fast_payload_input *input,
                                   size_t target_payload_len);
int mesh_smoke_fast_payload_decode(const uint8_t *payload,
                                   size_t payload_len,
                                   struct mesh_smoke_fast_payload *out);
void mesh_smoke_fast_tx_decide(const struct mesh_smoke_fast_tx_gate *gate,
                               struct mesh_smoke_fast_tx_decision *decision);
void mesh_smoke_fast_init(struct mesh_smoke_fast_state *state);
int mesh_smoke_fast_note_delivery(struct mesh_smoke_fast_state *state,
                                  const uint8_t *payload,
                                  size_t payload_len,
                                  uint32_t gateway_now_ms,
                                  uint32_t gateway_ack_latency_ms,
                                  uint32_t queue_depth);
void mesh_smoke_fast_note_missing_reason(struct mesh_smoke_fast_state *state,
                                         uint32_t packet_id,
                                         uint32_t reason);
void mesh_smoke_fast_note_ch9_missed(struct mesh_smoke_fast_state *state);
void mesh_smoke_fast_note_c5_refresh(struct mesh_smoke_fast_state *state);
void mesh_smoke_fast_get_summary(const struct mesh_smoke_fast_state *state,
                                 struct mesh_smoke_fast_summary *out);

#endif
