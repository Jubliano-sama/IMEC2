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

static void test_lower_hop_count_wins(void)
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

static void test_failures_try_alternate_then_discovery(void)
{
    struct route_table table;
    struct route_candidate route_a = candidate(0x02u, 1u, 1u, 90u, 1000u);
    struct route_candidate route_b = candidate(0x03u, 1u, 2u, 60u, 1000u);
    const struct route_candidate *selected;

    route_table_init(&table, 1u);
    assert(route_upsert_candidate(&table, &route_a) == PROTO_OK);
    assert(route_upsert_candidate(&table, &route_b) == PROTO_OK);

    assert(route_record_failure(&table, ROUTE_FAILURE_HOP_ACK) == ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure(&table, ROUTE_FAILURE_HOP_ACK) == ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure(&table, ROUTE_FAILURE_HOP_ACK) == ROUTE_DELIVERY_TRY_ALTERNATE);

    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == 0x03u);

    assert(route_record_failure(&table, ROUTE_FAILURE_GATEWAY_ACK) == ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure(&table, ROUTE_FAILURE_GATEWAY_ACK) == ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure(&table, ROUTE_FAILURE_GATEWAY_ACK) == ROUTE_DELIVERY_DISCOVER);
    assert(route_selected(&table) == NULL);
}

static void test_retry_backoff_values(void)
{
    assert(route_retry_backoff_ms(1u) == 100u);
    assert(route_retry_backoff_ms(2u) == 250u);
    assert(route_retry_backoff_ms(3u) == 500u);
}

int main(void)
{
    test_lower_hop_count_wins();
    test_same_hop_uses_link_quality();
    test_epoch_change_invalidates_old_routes();
    test_failures_try_alternate_then_discovery();
    test_retry_backoff_values();
    return 0;
}
