#ifndef ROUTE_H
#define ROUTE_H

#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PARENT_CANDIDATE_COUNT 3u
#define ROUTE_MAX_CANDIDATES PARENT_CANDIDATE_COUNT
#define ROUTE_NO_SELECTION 0xFFu
#define ROUTE_RETRIES_PER_CANDIDATE 3u
#define ROUTE_MAX_FAILURES (ROUTE_RETRIES_PER_CANDIDATE + 1u)
#define ROUTE_GATEWAY_ACK_TIMEOUT_MS 2000u
#define ROUTE_RETRY_BACKOFF_FIRST_MS 1500u
#define ROUTE_RETRY_BACKOFF_SECOND_MS 3000u
#define ROUTE_RETRY_BACKOFF_MAX_MS 6000u
#define ROUTE_PARENT_HOLDDOWN_S 60u
#define ROUTE_PARENT_HOLDDOWN_MS (ROUTE_PARENT_HOLDDOWN_S * 1000u)
/* Kept for compatibility; route age alone no longer invalidates candidates. */
#define ROUTE_CANDIDATE_MAX_AGE_MS 30000u
#define ROUTE_DEDUP_WINDOW_MS 60000u

enum route_failure_kind {
    ROUTE_FAILURE_GATEWAY_ACK = 1,
};

enum route_delivery_action {
    ROUTE_DELIVERY_RETRY_CURRENT = 0,
    ROUTE_DELIVERY_TRY_ALTERNATE = 1,
    ROUTE_DELIVERY_DISCOVER = 2,
};

enum relay_capacity_state {
    RELAY_CAP_UNKNOWN = 0u,
    RELAY_CAP_GREEN = 1u,
    RELAY_CAP_YELLOW = 2u,
    RELAY_CAP_RED = 3u,
    RELAY_CAP_BLACK = 4u,
};

struct route_candidate {
    uint64_t next_hop_id;
    uint64_t gateway_id;
    uint32_t route_epoch;
    uint32_t last_seen_ms;
    uint32_t last_success_ms;
    uint32_t hold_down_until_ms;
    uint16_t route_cost;
    uint16_t queue_free_hint;
    uint8_t hop_count;
    uint8_t link_quality;
    uint8_t failure_count;
    uint8_t relay_capacity_state;
    uint8_t channel9_busy_hint;
    uint32_t capacity_observed_at_ms;
    uint32_t capacity_valid_until_ms;
    bool capacity_hint_valid;
    bool hold_down_valid;
    bool channel9_timing_valid;
    bool valid;
};

struct route_table {
    struct route_candidate candidates[ROUTE_MAX_CANDIDATES];
    uint32_t current_epoch;
    uint8_t selected_index;
};

void route_table_init(struct route_table *table, uint32_t current_epoch);
bool route_epoch_strictly_newer(uint32_t candidate, uint32_t current);
uint16_t route_candidate_cost(uint8_t hop_count, uint8_t link_quality);
int route_upsert_candidate(struct route_table *table,
                                const struct route_candidate *candidate);
int route_select_best(struct route_table *table);
int route_select_best_at(struct route_table *table, uint32_t now_ms);
uint8_t route_expire_stale(struct route_table *table, uint32_t now_ms, uint32_t max_age_ms);
const struct route_candidate *route_selected(const struct route_table *table);
void route_set_channel9_timing_valid(struct route_table *table,
                                     uint64_t next_hop_id,
                                     uint64_t gateway_id,
                                     bool valid,
                                     uint32_t now_ms);
void route_update_capacity_hint(struct route_table *table,
                                uint64_t next_hop_id,
                                uint64_t gateway_id,
                                uint8_t relay_capacity_state,
                                uint16_t queue_free_hint,
                                uint8_t channel9_busy_hint,
                                bool capacity_hint_valid,
                                uint32_t observed_at_ms,
                                uint32_t valid_until_ms,
                                uint32_t now_ms);
void route_record_success(struct route_table *table);
void route_record_success_at(struct route_table *table, uint32_t now_ms);
int route_record_candidate_success_at(struct route_table *table,
                                      uint64_t next_hop_id,
                                      uint64_t gateway_id,
                                      uint32_t now_ms);
void route_refresh_selected_at(struct route_table *table, uint32_t now_ms);
enum route_delivery_action route_record_failure_at(struct route_table *table,
                                                   enum route_failure_kind kind,
                                                   uint32_t now_ms);
uint32_t route_retry_backoff_ms(uint8_t failure_count);

#ifdef __cplusplus
}
#endif

#endif
