#include "route.h"

#include <string.h>

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static bool time_after(uint32_t now_ms, uint32_t timestamp_ms)
{
    return (int32_t)(now_ms - timestamp_ms) > 0;
}

bool route_epoch_strictly_newer(uint32_t candidate, uint32_t current)
{
    uint32_t delta = candidate - current;

    return delta != 0u && delta < UINT32_C(0x80000000);
}

static bool candidate_valid_for_epoch(const struct route_candidate *candidate, uint32_t epoch)
{
    return candidate->valid && candidate->route_epoch == epoch;
}

uint16_t route_candidate_cost(uint8_t hop_count, uint8_t link_quality)
{
    if (link_quality > 100u) {
        link_quality = 100u;
    }
    return (uint16_t)((uint16_t)hop_count * 100u +
                      (uint16_t)(100u - link_quality));
}

static uint16_t candidate_effective_cost(const struct route_candidate *candidate)
{
    return candidate->route_cost != 0u ?
           candidate->route_cost :
           route_candidate_cost(candidate->hop_count, candidate->link_quality);
}

static bool candidate_in_hold_down(const struct route_candidate *candidate, uint32_t now_ms)
{
    return candidate->hold_down_valid &&
           !deadline_reached(now_ms, candidate->hold_down_until_ms);
}

static void normalize_candidate_hold_down(struct route_candidate *candidate,
                                          uint32_t now_ms)
{
    if (candidate->hold_down_valid &&
        deadline_reached(now_ms, candidate->hold_down_until_ms)) {
        candidate->hold_down_until_ms = 0u;
        candidate->hold_down_valid = false;
    }
}

static uint8_t normalized_capacity_state(uint8_t capacity_state)
{
    return capacity_state <= RELAY_CAP_BLACK ? capacity_state : RELAY_CAP_UNKNOWN;
}

static uint8_t candidate_effective_capacity(const struct route_candidate *candidate,
                                            uint32_t now_ms)
{
    if (!candidate->capacity_hint_valid ||
        time_after(now_ms, candidate->capacity_valid_until_ms)) {
        return RELAY_CAP_UNKNOWN;
    }
    return normalized_capacity_state(candidate->relay_capacity_state);
}

static uint8_t capacity_preference_rank(uint8_t capacity_state)
{
    switch (capacity_state) {
    case RELAY_CAP_GREEN:
        return 0u;
    case RELAY_CAP_UNKNOWN:
        return 1u;
    case RELAY_CAP_YELLOW:
        return 2u;
    case RELAY_CAP_RED:
        return 3u;
    case RELAY_CAP_BLACK:
        return 4u;
    default:
        return 1u;
    }
}

static void normalize_candidate_capacity_hint(struct route_candidate *candidate,
                                              uint32_t now_ms)
{
    candidate->relay_capacity_state =
        normalized_capacity_state(candidate->relay_capacity_state);
    if (candidate->relay_capacity_state == RELAY_CAP_UNKNOWN ||
        !candidate->capacity_hint_valid ||
        time_after(now_ms, candidate->capacity_valid_until_ms)) {
        candidate->relay_capacity_state = RELAY_CAP_UNKNOWN;
        candidate->queue_free_hint = 0u;
        candidate->channel9_busy_hint = 0u;
        candidate->capacity_observed_at_ms = 0u;
        candidate->capacity_valid_until_ms = 0u;
        candidate->capacity_hint_valid = false;
    }
}

static bool candidate_is_better(const struct route_candidate *candidate,
                                const struct route_candidate *selected,
                                uint32_t now_ms)
{
    uint16_t candidate_cost;
    uint16_t selected_cost;
    bool candidate_held;
    bool selected_held;
    uint8_t candidate_capacity;
    uint8_t selected_capacity;

    if (selected == NULL) {
        return true;
    }
    candidate_cost = candidate_effective_cost(candidate);
    selected_cost = candidate_effective_cost(selected);
    if (candidate_cost != selected_cost) {
        return candidate_cost < selected_cost;
    }
    candidate_held = candidate_in_hold_down(candidate, now_ms);
    selected_held = candidate_in_hold_down(selected, now_ms);
    if (candidate_held != selected_held) {
        return !candidate_held;
    }
    if (candidate->link_quality != selected->link_quality) {
        return candidate->link_quality > selected->link_quality;
    }
    if (candidate->hop_count != selected->hop_count) {
        return candidate->hop_count < selected->hop_count;
    }
    if (candidate->channel9_timing_valid != selected->channel9_timing_valid) {
        return candidate->channel9_timing_valid;
    }
    candidate_capacity = capacity_preference_rank(
        candidate_effective_capacity(candidate, now_ms));
    selected_capacity = capacity_preference_rank(
        candidate_effective_capacity(selected, now_ms));
    if (candidate_capacity != selected_capacity) {
        return candidate_capacity < selected_capacity;
    }
    if (candidate->last_success_ms != selected->last_success_ms) {
        return time_after(candidate->last_success_ms,
                          selected->last_success_ms);
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

static int find_replacement_candidate_index(const struct route_table *table,
                                            const struct route_candidate *candidate,
                                            uint32_t now_ms)
{
    const struct route_candidate *worst = NULL;
    uint8_t worst_index = ROUTE_NO_SELECTION;

    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        const struct route_candidate *current = &table->candidates[i];

        if (!candidate_valid_for_epoch(current, table->current_epoch)) {
            return (int)i;
        }
        if (worst == NULL || candidate_is_better(worst, current, now_ms)) {
            worst = current;
            worst_index = i;
        }
    }

    if (worst != NULL && candidate_is_better(candidate, worst, now_ms)) {
        return (int)worst_index;
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
    return route_select_best_at(table, 0u);
}

int route_select_best_at(struct route_table *table, uint32_t now_ms)
{
    uint8_t selected_index = ROUTE_NO_SELECTION;
    const struct route_candidate *selected = NULL;

    if (table == NULL) {
        return PROTO_ERR_ARG;
    }

    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        struct route_candidate *candidate = &table->candidates[i];

        normalize_candidate_hold_down(candidate, now_ms);
        if (!candidate_valid_for_epoch(candidate, table->current_epoch)) {
            continue;
        }
        if (candidate_in_hold_down(candidate, now_ms)) {
            continue;
        }
        if (candidate_is_better(candidate, selected, now_ms)) {
            selected = candidate;
            selected_index = i;
        }
    }

    table->selected_index = selected_index;
    return selected == NULL ? PROTO_ERR_NOT_FOUND : PROTO_OK;
}

uint8_t route_expire_stale(struct route_table *table, uint32_t now_ms, uint32_t max_age_ms)
{
    (void)max_age_ms;
    (void)route_select_best_at(table, now_ms);
    return 0u;
}

int route_upsert_candidate(struct route_table *table,
                                const struct route_candidate *candidate)
{
    struct route_candidate stored;
    struct route_candidate previous = {0};
    int index;
    bool updating_existing = false;
    uint32_t now_ms;

    if (table == NULL || candidate == NULL) {
        return PROTO_ERR_ARG;
    }
    if (candidate->next_hop_id == 0u ||
        candidate->gateway_id == 0u ||
        (uint16_t)candidate->route_epoch == 0u ||
        candidate->hop_count == UINT8_MAX ||
        candidate->link_quality > 100u) {
        return PROTO_ERR_ARG;
    }
    if (candidate->route_epoch != table->current_epoch &&
        !route_epoch_strictly_newer(candidate->route_epoch,
                                    table->current_epoch)) {
        return PROTO_ERR_STALE;
    }
    if (candidate->route_epoch != table->current_epoch) {
        table->current_epoch = candidate->route_epoch;
        invalidate_candidates(table);
    }

    stored = *candidate;
    stored.valid = true;
    stored.route_cost = route_candidate_cost(stored.hop_count, stored.link_quality);
    now_ms = stored.last_seen_ms;
    normalize_candidate_capacity_hint(&stored, now_ms);

    index = find_candidate_index(table, candidate->next_hop_id, candidate->gateway_id);
    if (index >= 0) {
        previous = table->candidates[index];
        updating_existing = true;
    }
    if (index < 0) {
        index = find_free_candidate_index(table);
    }
    if (index < 0) {
        index = find_replacement_candidate_index(table, &stored, now_ms);
    }
    if (index < 0) {
        return PROTO_ERR_NO_SPACE;
    }

    if (updating_existing) {
        stored.last_success_ms = previous.last_success_ms;
        stored.failure_count = 0u;
        stored.hold_down_until_ms = 0u;
        stored.hold_down_valid = false;
        stored.channel9_timing_valid = stored.channel9_timing_valid ||
                                       previous.channel9_timing_valid;
    } else {
        stored.failure_count = 0u;
        stored.last_success_ms = 0u;
        stored.hold_down_until_ms = 0u;
        stored.hold_down_valid = false;
    }

    table->candidates[index] = stored;
    return route_select_best_at(table, now_ms);
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

void route_set_channel9_timing_valid(struct route_table *table,
                                     uint64_t next_hop_id,
                                     uint64_t gateway_id,
                                     bool valid,
                                     uint32_t now_ms)
{
    int index;

    if (table == NULL || next_hop_id == 0u || gateway_id == 0u) {
        return;
    }

    index = find_candidate_index(table, next_hop_id, gateway_id);
    if (index < 0) {
        return;
    }
    table->candidates[index].channel9_timing_valid = valid;
    (void)route_select_best_at(table, now_ms);
}

void route_update_capacity_hint(struct route_table *table,
                                uint64_t next_hop_id,
                                uint64_t gateway_id,
                                uint8_t relay_capacity_state,
                                uint16_t queue_free_hint,
                                uint8_t channel9_busy_hint,
                                bool capacity_hint_valid,
                                uint32_t observed_at_ms,
                                uint32_t valid_until_ms,
                                uint32_t now_ms)
{
    int index;
    struct route_candidate *candidate;

    if (table == NULL || next_hop_id == 0u || gateway_id == 0u) {
        return;
    }

    index = find_candidate_index(table, next_hop_id, gateway_id);
    if (index < 0) {
        return;
    }

    candidate = &table->candidates[index];
    candidate->relay_capacity_state = relay_capacity_state;
    candidate->queue_free_hint = queue_free_hint;
    candidate->channel9_busy_hint = channel9_busy_hint;
    candidate->capacity_hint_valid = capacity_hint_valid;
    candidate->capacity_observed_at_ms = observed_at_ms;
    candidate->capacity_valid_until_ms = valid_until_ms;
    normalize_candidate_capacity_hint(candidate, now_ms);
    (void)route_select_best_at(table, now_ms);
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
        candidate->hold_down_until_ms = 0u;
        candidate->hold_down_valid = false;
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
        candidate->last_success_ms = now_ms;
        candidate->hold_down_until_ms = 0u;
        candidate->hold_down_valid = false;
    }
}

int route_record_candidate_success_at(struct route_table *table,
                                      uint64_t next_hop_id,
                                      uint64_t gateway_id,
                                      uint32_t now_ms)
{
    struct route_candidate *candidate;
    int index;

    if (table == NULL || next_hop_id == 0u || gateway_id == 0u) {
        return PROTO_ERR_ARG;
    }

    index = find_candidate_index(table, next_hop_id, gateway_id);
    if (index < 0) {
        return PROTO_ERR_NOT_FOUND;
    }

    candidate = &table->candidates[index];
    if (!candidate_valid_for_epoch(candidate, table->current_epoch)) {
        return PROTO_ERR_STALE;
    }

    candidate->failure_count = 0u;
    candidate->last_seen_ms = now_ms;
    candidate->last_success_ms = now_ms;
    candidate->hold_down_until_ms = 0u;
    candidate->hold_down_valid = false;
    return route_select_best_at(table, now_ms);
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

enum route_delivery_action route_record_failure_at(struct route_table *table,
                                                   enum route_failure_kind kind,
                                                   uint32_t now_ms)
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
    if (candidate->failure_count <= ROUTE_RETRIES_PER_CANDIDATE) {
        return ROUTE_DELIVERY_RETRY_CURRENT;
    }

    candidate->failure_count = 0u;
    candidate->hold_down_until_ms = now_ms + ROUTE_PARENT_HOLDDOWN_MS;
    candidate->hold_down_valid = true;
    if (route_select_best_at(table, now_ms) == PROTO_OK) {
        return ROUTE_DELIVERY_TRY_ALTERNATE;
    }
    return ROUTE_DELIVERY_DISCOVER;
}

enum route_delivery_action route_abandon_selected_at(
    struct route_table *table,
    uint32_t now_ms)
{
    const struct route_candidate *candidate;
    uint64_t next_hop_id;
    uint64_t gateway_id;
    uint32_t route_epoch;

    if (table == NULL || table->selected_index == ROUTE_NO_SELECTION ||
        table->selected_index >= ROUTE_MAX_CANDIDATES) {
        return ROUTE_DELIVERY_DISCOVER;
    }
    candidate = &table->candidates[table->selected_index];
    if (!candidate_valid_for_epoch(candidate, table->current_epoch)) {
        table->selected_index = ROUTE_NO_SELECTION;
        return ROUTE_DELIVERY_DISCOVER;
    }

    next_hop_id = candidate->next_hop_id;
    gateway_id = candidate->gateway_id;
    route_epoch = candidate->route_epoch;
    return route_abandon_candidate_at(table,
                                      next_hop_id,
                                      gateway_id,
                                      route_epoch,
                                      now_ms);
}

enum route_delivery_action route_abandon_candidate_at(
    struct route_table *table,
    uint64_t next_hop_id,
    uint64_t gateway_id,
    uint32_t route_epoch,
    uint32_t now_ms)
{
    struct route_candidate *candidate;
    int index;

    if (table == NULL || next_hop_id == 0u || gateway_id == 0u ||
        route_epoch != table->current_epoch) {
        return ROUTE_DELIVERY_DISCOVER;
    }
    index = find_candidate_index(table, next_hop_id, gateway_id);
    if (index < 0) {
        return ROUTE_DELIVERY_DISCOVER;
    }
    candidate = &table->candidates[index];
    if (!candidate_valid_for_epoch(candidate, route_epoch)) {
        return ROUTE_DELIVERY_DISCOVER;
    }

    candidate->failure_count = 0u;
    candidate->hold_down_until_ms = now_ms + ROUTE_PARENT_HOLDDOWN_MS;
    candidate->hold_down_valid = true;
    return route_select_best_at(table, now_ms) == PROTO_OK ?
           ROUTE_DELIVERY_TRY_ALTERNATE : ROUTE_DELIVERY_DISCOVER;
}

uint32_t route_retry_backoff_ms(uint8_t failure_count)
{
    if (failure_count <= 1u) {
        return ROUTE_RETRY_BACKOFF_FIRST_MS;
    }
    if (failure_count == 2u) {
        return ROUTE_RETRY_BACKOFF_SECOND_MS;
    }
    return ROUTE_RETRY_BACKOFF_MAX_MS;
}
