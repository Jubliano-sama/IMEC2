#include "route.h"

#include <assert.h>

static struct route_candidate candidate(uint64_t next_hop_id,
                                             uint32_t epoch,
                                             uint8_t hop_count,
                                             uint8_t quality,
                                             uint32_t last_seen_ms)
{
    struct route_candidate route = {
        .next_hop_id = next_hop_id,
        .gateway_id = 0xAA55AA55AA55AA55ull,
        .route_epoch = epoch,
        .last_seen_ms = last_seen_ms,
        .hop_count = hop_count,
        .link_quality = quality,
    };
    return route;
}

static uint8_t valid_candidate_count(const struct route_table *table)
{
    uint8_t count = 0u;

    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        if (table->candidates[i].valid) {
            count++;
        }
    }
    return count;
}

static void test_weighted_cost_prefers_useful_direct_route(void)
{
    struct route_table table;
    struct route_candidate route_a = candidate(0x02u, 1u, 2u, 100u, 1000u);
    struct route_candidate route_b = candidate(0x03u, 1u, 1u, 20u, 1000u);
    const struct route_candidate *selected;

    route_table_init(&table, 1u);
    assert(route_upsert_candidate(&table, &route_a) == PROTO_OK);
    assert(route_upsert_candidate(&table, &route_b) == PROTO_OK);

    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == 0x03u);
}

static void test_weighted_cost_avoids_unusable_direct_route(void)
{
    struct route_table table;
    struct route_candidate route_a = candidate(0x02u, 1u, 1u, 0u, 1000u);
    struct route_candidate route_b = candidate(0x03u, 1u, 2u, 100u, 900u);
    const struct route_candidate *selected;

    route_table_init(&table, 1u);
    assert(route_upsert_candidate(&table, &route_a) == PROTO_OK);
    assert(route_upsert_candidate(&table, &route_b) == PROTO_OK);

    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == 0x03u);
}

static void test_same_hop_uses_link_quality(void)
{
    struct route_table table;
    struct route_candidate route_a = candidate(0x02u, 7u, 2u, 40u, 1000u);
    struct route_candidate route_b = candidate(0x03u, 7u, 2u, 90u, 900u);
    const struct route_candidate *selected;

    route_table_init(&table, 7u);
    assert(route_upsert_candidate(&table, &route_a) == PROTO_OK);
    assert(route_upsert_candidate(&table, &route_b) == PROTO_OK);

    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == 0x03u);
}

static void test_epoch_change_invalidates_old_routes(void)
{
    struct route_table table;
    const struct route_candidate *selected;
    struct route_candidate old = candidate(0x02u, 1u, 1u, 100u, 1000u);
    struct route_candidate fresh = candidate(0x03u, 2u, 3u, 10u, 1100u);
    struct route_candidate stale = candidate(0x04u, 1u, 1u, 100u, 1200u);

    route_table_init(&table, 1u);
    assert(route_upsert_candidate(&table, &old) == PROTO_OK);
    assert(route_upsert_candidate(&table, &fresh) == PROTO_OK);
    assert(route_upsert_candidate(&table, &stale) == PROTO_ERR_STALE);

    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == 0x03u);
    assert(selected->route_epoch == 2u);
}

static void test_age_alone_keeps_selected_route(void)
{
    struct route_table table;
    struct route_candidate old_direct = candidate(0x02u, 3u, 1u, 90u, 1000u);
    struct route_candidate newer_relay = candidate(0x03u, 3u, 2u, 80u, 7000u);
    const struct route_candidate *selected;

    route_table_init(&table, 3u);
    assert(route_upsert_candidate(&table, &old_direct) == PROTO_OK);
    assert(route_upsert_candidate(&table, &newer_relay) == PROTO_OK);

    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == 0x02u);

    assert(route_expire_stale(&table, 31001u, ROUTE_CANDIDATE_MAX_AGE_MS) == 0u);
    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == 0x02u);
}

static void test_age_alone_keeps_all_routes_selectable(void)
{
    struct route_table table;
    struct route_candidate route_a = candidate(0x02u, 3u, 1u, 90u, 1000u);
    struct route_candidate route_b = candidate(0x03u, 3u, 2u, 80u, 1500u);
    const struct route_candidate *selected;

    route_table_init(&table, 3u);
    assert(route_upsert_candidate(&table, &route_a) == PROTO_OK);
    assert(route_upsert_candidate(&table, &route_b) == PROTO_OK);

    assert(route_expire_stale(&table, 31501u, ROUTE_CANDIDATE_MAX_AGE_MS) == 0u);
    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == 0x02u);
}

static void test_success_refreshes_selected_route_age(void)
{
    struct route_table table;
    struct route_candidate route = candidate(0x02u, 3u, 1u, 90u, 1000u);
    const struct route_candidate *selected;

    route_table_init(&table, 3u);
    assert(route_upsert_candidate(&table, &route) == PROTO_OK);

    route_record_success_at(&table, 7500u);
    assert(route_expire_stale(&table, 8001u, ROUTE_CANDIDATE_MAX_AGE_MS) == 0u);

    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == 0x02u);
    assert(selected->last_seen_ms == 7500u);
}

static void test_failures_try_alternate_then_discovery(void)
{
    struct route_table table;
    struct route_candidate route_a = candidate(0x02u, 1u, 1u, 90u, 1000u);
    struct route_candidate route_b = candidate(0x03u, 1u, 2u, 60u, 1000u);
    const struct route_candidate *selected;

    route_table_init(&table, 1u);
    assert(route_upsert_candidate(&table, &route_a) == PROTO_OK);
    assert(route_upsert_candidate(&table, &route_b) == PROTO_OK);

    assert(route_record_failure_at(&table, ROUTE_FAILURE_GATEWAY_ACK, 2000u) ==
           ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure_at(&table, ROUTE_FAILURE_GATEWAY_ACK, 2100u) ==
           ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure_at(&table, ROUTE_FAILURE_GATEWAY_ACK, 2200u) ==
           ROUTE_DELIVERY_TRY_ALTERNATE);

    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == 0x03u);

    assert(route_record_failure_at(&table, ROUTE_FAILURE_GATEWAY_ACK, 2300u) ==
           ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure_at(&table, ROUTE_FAILURE_GATEWAY_ACK, 2400u) ==
           ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure_at(&table, ROUTE_FAILURE_GATEWAY_ACK, 2500u) ==
           ROUTE_DELIVERY_DISCOVER);
    assert(route_selected(&table) == NULL);
}

static void test_parent_candidate_count_replaces_worst_route(void)
{
    struct route_table table;
    struct route_candidate route_a = candidate(0x02u, 1u, 3u, 20u, 1000u);
    struct route_candidate route_b = candidate(0x03u, 1u, 2u, 60u, 1000u);
    struct route_candidate route_c = candidate(0x04u, 1u, 1u, 90u, 1000u);
    struct route_candidate route_d = candidate(0x05u, 1u, 1u, 95u, 1000u);
    const struct route_candidate *selected;

    route_table_init(&table, 1u);
    assert(route_upsert_candidate(&table, &route_a) == PROTO_OK);
    assert(route_upsert_candidate(&table, &route_b) == PROTO_OK);
    assert(route_upsert_candidate(&table, &route_c) == PROTO_OK);
    assert(valid_candidate_count(&table) == PARENT_CANDIDATE_COUNT);

    assert(route_upsert_candidate(&table, &route_d) == PROTO_OK);
    assert(valid_candidate_count(&table) == PARENT_CANDIDATE_COUNT);

    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == 0x05u);
}

static void test_parent_hold_down_recovers_without_age_expiry(void)
{
    struct route_table table;
    struct route_candidate route = candidate(0x02u, 1u, 1u, 90u, 1000u);
    const struct route_candidate *selected;

    route_table_init(&table, 1u);
    assert(route_upsert_candidate(&table, &route) == PROTO_OK);

    assert(route_record_failure_at(&table, ROUTE_FAILURE_GATEWAY_ACK, 2000u) ==
           ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure_at(&table, ROUTE_FAILURE_GATEWAY_ACK, 2100u) ==
           ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure_at(&table, ROUTE_FAILURE_GATEWAY_ACK, 2200u) ==
           ROUTE_DELIVERY_DISCOVER);
    assert(route_selected(&table) == NULL);

    assert(route_expire_stale(&table,
                              2200u + ROUTE_PARENT_HOLDDOWN_MS + 1u,
                              ROUTE_CANDIDATE_MAX_AGE_MS) == 0u);
    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == 0x02u);
}

static void test_channel9_timing_breaks_equal_cost_tie(void)
{
    struct route_table table;
    struct route_candidate route_a = candidate(0x02u, 1u, 1u, 80u, 1000u);
    struct route_candidate route_b = candidate(0x03u, 1u, 1u, 80u, 1000u);
    const struct route_candidate *selected;

    route_table_init(&table, 1u);
    assert(route_upsert_candidate(&table, &route_a) == PROTO_OK);
    assert(route_upsert_candidate(&table, &route_b) == PROTO_OK);

    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == 0x02u);

    route_set_channel9_timing_valid(&table, 0x03u, route_b.gateway_id, true, 1200u);
    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == 0x03u);
}

static void test_capacity_breaks_equal_cost_tie(void)
{
    struct route_table table;
    struct route_candidate green = candidate(0x02u, 1u, 1u, 80u, 1000u);
    struct route_candidate yellow = candidate(0x03u, 1u, 1u, 80u, 1000u);
    const struct route_candidate *selected;

    green.relay_capacity_state = RELAY_CAP_GREEN;
    green.capacity_observed_at_ms = 1000u;
    green.capacity_valid_until_ms = 2000u;
    yellow.relay_capacity_state = RELAY_CAP_YELLOW;
    yellow.capacity_observed_at_ms = 1000u;
    yellow.capacity_valid_until_ms = 2000u;

    route_table_init(&table, 1u);
    assert(route_upsert_candidate(&table, &yellow) == PROTO_OK);
    assert(route_upsert_candidate(&table, &green) == PROTO_OK);

    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == 0x02u);
}

static void test_expired_capacity_hint_does_not_invalidate_route(void)
{
    struct route_table table;
    struct route_candidate expired_green = candidate(0x03u, 1u, 1u, 80u, 1000u);
    struct route_candidate unknown = candidate(0x02u, 1u, 1u, 80u, 1000u);
    const struct route_candidate *selected;

    expired_green.relay_capacity_state = RELAY_CAP_GREEN;
    expired_green.capacity_observed_at_ms = 1000u;
    expired_green.capacity_valid_until_ms = 1500u;
    expired_green.channel9_timing_valid = true;
    unknown.relay_capacity_state = RELAY_CAP_UNKNOWN;
    unknown.channel9_timing_valid = true;

    route_table_init(&table, 1u);
    assert(route_upsert_candidate(&table, &expired_green) == PROTO_OK);
    assert(route_upsert_candidate(&table, &unknown) == PROTO_OK);

    assert(route_select_best_at(&table, 1500u) == PROTO_OK);
    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == 0x03u);

    assert(route_select_best_at(&table, 1501u) == PROTO_OK);
    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == 0x02u);
    assert(table.candidates[0].next_hop_id == 0x03u);
    assert(table.candidates[0].valid);
    assert(table.candidates[0].channel9_timing_valid);
    assert(table.candidates[0].hold_down_until_ms == 0u);
}

static void test_retry_backoff_values(void)
{
    assert(route_retry_backoff_ms(1u) == 100u);
    assert(route_retry_backoff_ms(2u) == 250u);
    assert(route_retry_backoff_ms(3u) == 500u);
}

int main(void)
{
    test_weighted_cost_prefers_useful_direct_route();
    test_weighted_cost_avoids_unusable_direct_route();
    test_same_hop_uses_link_quality();
    test_epoch_change_invalidates_old_routes();
    test_age_alone_keeps_selected_route();
    test_age_alone_keeps_all_routes_selectable();
    test_success_refreshes_selected_route_age();
    test_failures_try_alternate_then_discovery();
    test_parent_candidate_count_replaces_worst_route();
    test_parent_hold_down_recovers_without_age_expiry();
    test_channel9_timing_breaks_equal_cost_tie();
    test_capacity_breaks_equal_cost_tie();
    test_expired_capacity_hint_does_not_invalidate_route();
    test_retry_backoff_values();
    return 0;
}
