#include "route.h"

#include <string.h>

static bool candidate_valid_for_epoch(const struct route_candidate *candidate, uint32_t epoch)
{
    return candidate->valid && candidate->route_epoch == epoch;
}

static uint16_t candidate_effective_cost(const struct route_candidate *candidate)
{
    return (uint16_t)((uint16_t)candidate->hop_count * 100u +
                      (uint16_t)(100u - candidate->link_quality));
}

static bool candidate_is_better(const struct route_candidate *candidate,
                                const struct route_candidate *selected)
{
    uint16_t candidate_cost;
    uint16_t selected_cost;

    if (selected == NULL) {
        return true;
    }
    candidate_cost = candidate_effective_cost(candidate);
    selected_cost = candidate_effective_cost(selected);
    if (candidate_cost != selected_cost) {
        return candidate_cost < selected_cost;
    }
    if (candidate->link_quality != selected->link_quality) {
        return candidate->link_quality > selected->link_quality;
    }
    if (candidate->hop_count != selected->hop_count) {
        return candidate->hop_count < selected->hop_count;
    }
    if (candidate->last_seen_ms != selected->last_seen_ms) {
        return candidate->last_seen_ms > selected->last_seen_ms;
    }
    return candidate->next_hop_id < selected->next_hop_id;
}

static int find_candidate_index(const struct route_table *table,
                                uint64_t next_hop_id,
                                uint64_t gateway_id)
{
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        const struct route_candidate *candidate = &table->candidates[i];
        if (candidate->valid &&
            candidate->next_hop_id == next_hop_id &&
            candidate->gateway_id == gateway_id) {
            return (int)i;
        }
    }
    return -1;
}

static int find_free_candidate_index(const struct route_table *table)
{
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        if (!table->candidates[i].valid) {
            return (int)i;
        }
    }
    return -1;
}

static void invalidate_candidates(struct route_table *table)
{
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        table->candidates[i].valid = false;
    }
    table->selected_index = ROUTE_NO_SELECTION;
}

void route_table_init(struct route_table *table, uint32_t current_epoch)
{
    if (table != NULL) {
        memset(table, 0, sizeof(*table));
        table->current_epoch = current_epoch;
        table->selected_index = ROUTE_NO_SELECTION;
    }
}

int route_select_best(struct route_table *table)
{
    uint8_t selected_index = ROUTE_NO_SELECTION;
    const struct route_candidate *selected = NULL;

    if (table == NULL) {
        return PROTO_ERR_ARG;
    }

    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        const struct route_candidate *candidate = &table->candidates[i];
        if (!candidate_valid_for_epoch(candidate, table->current_epoch)) {
            continue;
        }
        if (candidate_is_better(candidate, selected)) {
            selected = candidate;
            selected_index = i;
        }
    }

    table->selected_index = selected_index;
    return selected == NULL ? PROTO_ERR_NOT_FOUND : PROTO_OK;
}

uint8_t route_expire_stale(struct route_table *table, uint32_t now_ms, uint32_t max_age_ms)
{
    (void)table;
    (void)now_ms;
    (void)max_age_ms;
    return 0u;
}

int route_upsert_candidate(struct route_table *table,
                                const struct route_candidate *candidate)
{
    int index;

    if (table == NULL || candidate == NULL) {
        return PROTO_ERR_ARG;
    }
    if (candidate->next_hop_id == 0u ||
        candidate->gateway_id == 0u ||
        candidate->hop_count == UINT8_MAX ||
        candidate->link_quality > 100u) {
        return PROTO_ERR_ARG;
    }
    if (candidate->route_epoch < table->current_epoch) {
        return PROTO_ERR_STALE;
    }
    if (candidate->route_epoch > table->current_epoch) {
        table->current_epoch = candidate->route_epoch;
        invalidate_candidates(table);
    }

    index = find_candidate_index(table, candidate->next_hop_id, candidate->gateway_id);
    if (index < 0) {
        index = find_free_candidate_index(table);
    }
    if (index < 0) {
        return PROTO_ERR_NO_SPACE;
    }

    table->candidates[index] = *candidate;
    table->candidates[index].valid = true;
    table->candidates[index].failure_count = 0u;
    return route_select_best(table);
}

const struct route_candidate *route_selected(const struct route_table *table)
{
    if (table == NULL || table->selected_index == ROUTE_NO_SELECTION ||
        table->selected_index >= ROUTE_MAX_CANDIDATES) {
        return NULL;
    }
    if (!candidate_valid_for_epoch(&table->candidates[table->selected_index], table->current_epoch)) {
        return NULL;
    }
    return &table->candidates[table->selected_index];
}

void route_record_success(struct route_table *table)
{
    struct route_candidate *candidate;

    if (table == NULL || table->selected_index == ROUTE_NO_SELECTION ||
        table->selected_index >= ROUTE_MAX_CANDIDATES) {
        return;
    }

    candidate = &table->candidates[table->selected_index];
    if (candidate_valid_for_epoch(candidate, table->current_epoch)) {
        candidate->failure_count = 0u;
    }
}

void route_record_success_at(struct route_table *table, uint32_t now_ms)
{
    struct route_candidate *candidate;

    if (table == NULL || table->selected_index == ROUTE_NO_SELECTION ||
        table->selected_index >= ROUTE_MAX_CANDIDATES) {
        return;
    }

    candidate = &table->candidates[table->selected_index];
    if (candidate_valid_for_epoch(candidate, table->current_epoch)) {
        candidate->failure_count = 0u;
        candidate->last_seen_ms = now_ms;
    }
}

void route_refresh_selected_at(struct route_table *table, uint32_t now_ms)
{
    struct route_candidate *candidate;

    if (table == NULL || table->selected_index == ROUTE_NO_SELECTION ||
        table->selected_index >= ROUTE_MAX_CANDIDATES) {
        return;
    }

    candidate = &table->candidates[table->selected_index];
    if (candidate_valid_for_epoch(candidate, table->current_epoch)) {
        candidate->last_seen_ms = now_ms;
    }
}

enum route_delivery_action route_record_failure(struct route_table *table,
                                                          enum route_failure_kind kind)
{
    struct route_candidate *candidate;

    if (kind != ROUTE_FAILURE_GATEWAY_ACK) {
        return ROUTE_DELIVERY_DISCOVER;
    }
    if (table == NULL || table->selected_index == ROUTE_NO_SELECTION ||
        table->selected_index >= ROUTE_MAX_CANDIDATES) {
        return ROUTE_DELIVERY_DISCOVER;
    }

    candidate = &table->candidates[table->selected_index];
    if (!candidate_valid_for_epoch(candidate, table->current_epoch)) {
        table->selected_index = ROUTE_NO_SELECTION;
        return ROUTE_DELIVERY_DISCOVER;
    }

    if (candidate->failure_count < UINT8_MAX) {
        candidate->failure_count++;
    }
    if (candidate->failure_count < ROUTE_MAX_FAILURES) {
        return ROUTE_DELIVERY_RETRY_CURRENT;
    }

    candidate->valid = false;
    if (route_select_best(table) == PROTO_OK) {
        return ROUTE_DELIVERY_TRY_ALTERNATE;
    }
    return ROUTE_DELIVERY_DISCOVER;
}

uint32_t route_retry_backoff_ms(uint8_t failure_count)
{
    if (failure_count <= 1u) {
        return 100u;
    }
    if (failure_count == 2u) {
        return 250u;
    }
    return 500u;
}
