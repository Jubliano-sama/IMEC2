#ifndef ROUTE_H
#define ROUTE_H

#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROUTE_MAX_CANDIDATES 8u
#define ROUTE_NO_SELECTION 0xFFu
#define ROUTE_MAX_FAILURES 3u
#define ROUTE_GATEWAY_ACK_TIMEOUT_MS 2000u
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

struct route_candidate {
    uint64_t next_hop_id;
    uint64_t gateway_id;
    uint32_t route_epoch;
    uint32_t last_seen_ms;
    uint8_t hop_count;
    uint8_t link_quality;
    uint8_t failure_count;
    bool valid;
};

struct route_table {
    struct route_candidate candidates[ROUTE_MAX_CANDIDATES];
    uint32_t current_epoch;
    uint8_t selected_index;
};

void route_table_init(struct route_table *table, uint32_t current_epoch);
int route_upsert_candidate(struct route_table *table,
                                const struct route_candidate *candidate);
int route_select_best(struct route_table *table);
uint8_t route_expire_stale(struct route_table *table, uint32_t now_ms, uint32_t max_age_ms);
const struct route_candidate *route_selected(const struct route_table *table);
void route_record_success(struct route_table *table);
void route_record_success_at(struct route_table *table, uint32_t now_ms);
void route_refresh_selected_at(struct route_table *table, uint32_t now_ms);
enum route_delivery_action route_record_failure(struct route_table *table,
                                                          enum route_failure_kind kind);
uint32_t route_retry_backoff_ms(uint8_t failure_count);

#ifdef __cplusplus
}
#endif

#endif
