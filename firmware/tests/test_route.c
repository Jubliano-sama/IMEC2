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

static const struct route_candidate *candidate_for_next_hop(
    const struct route_table *table,
    uint64_t next_hop_id)
{
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        if (table->candidates[i].valid &&
            table->candidates[i].next_hop_id == next_hop_id) {
            return &table->candidates[i];
        }
    }
    return NULL;
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

static void test_borderline_direct_waits_for_stronger_relay(void)
{
    struct route_table table;
    struct route_candidate weak_direct =
        candidate(0x02u, 9u, 0u,
                  route_link_quality_from_rsl(-99), 1000u);
    struct route_candidate strong_relay =
        candidate(0x03u, 9u, 1u,
                  route_link_quality_from_rsl(-90), 5000u);
    const struct route_candidate *selected;

    weak_direct.link_rsl_dbm = -99;
    weak_direct.link_rsl_valid = true;
    weak_direct.provisional_until_ms = 9000u;
    weak_direct.provisional_valid = true;
    strong_relay.link_rsl_dbm = -90;
    strong_relay.link_rsl_valid = true;

    route_table_init(&table, 9u);
    assert(route_upsert_candidate(&table, &weak_direct) == PROTO_OK);
    assert(route_selected(&table) == NULL);
    assert(route_upsert_candidate(&table, &strong_relay) == PROTO_OK);
    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == strong_relay.next_hop_id);

    assert(route_select_best_at(&table, 9000u) == PROTO_OK);
    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == strong_relay.next_hop_id);
}

static void test_borderline_relay_becomes_fallback_after_next_depth(void)
{
    struct route_table table;
    struct route_candidate weak_relay =
        candidate(0x04u, 11u, 2u,
                  route_link_quality_from_rsl(-99), UINT32_MAX - 1000u);
    const struct route_candidate *selected;

    weak_relay.link_rsl_dbm = -99;
    weak_relay.link_rsl_valid = true;
    weak_relay.provisional_until_ms = 100u;
    weak_relay.provisional_valid = true;

    route_table_init(&table, 11u);
    assert(route_upsert_candidate(&table, &weak_relay) == PROTO_OK);
    assert(route_selected(&table) == NULL);
    assert(route_select_best_at(&table, 99u) == PROTO_ERR_NOT_FOUND);
    assert(route_select_best_at(&table, 100u) == PROTO_OK);
    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == weak_relay.next_hop_id);
    assert(!selected->provisional_valid);
}

static void test_strong_direct_still_beats_strong_relay(void)
{
    struct route_table table;
    struct route_candidate direct =
        candidate(0x05u, 12u, 0u,
                  route_link_quality_from_rsl(-94), 1000u);
    struct route_candidate relay =
        candidate(0x06u, 12u, 1u,
                  route_link_quality_from_rsl(-85), 1000u);
    const struct route_candidate *selected;

    direct.link_rsl_dbm = -94;
    direct.link_rsl_valid = true;
    relay.link_rsl_dbm = -85;
    relay.link_rsl_valid = true;

    route_table_init(&table, 12u);
    assert(route_upsert_candidate(&table, &relay) == PROTO_OK);
    assert(route_upsert_candidate(&table, &direct) == PROTO_OK);
    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == direct.next_hop_id);
}

static void test_rsl_margin_boundary_is_inclusive(void)
{
    assert(ROUTE_LINK_MIN_IMMEDIATE_RSL_DBM == -98);
    assert(route_link_rsl_immediately_usable(-98));
    assert(!route_link_rsl_immediately_usable(-99));
    assert(route_link_quality_from_rsl(-108) == 0u);
    assert(route_link_quality_from_rsl(-98) == 10u);
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

static void test_epoch_serial_order_accepts_wrap_and_rejects_ambiguity(void)
{
    struct route_table table;
    struct route_candidate before_wrap =
        candidate(0x02u, UINT32_MAX, 1u, 90u, 1000u);
    struct route_candidate after_wrap =
        candidate(0x03u, 1u, 1u, 90u, 1001u);
    struct route_candidate ambiguous =
        candidate(0x04u, UINT32_C(0x80000001), 1u, 90u, 1002u);
    struct route_candidate zero_wire_epoch =
        candidate(0x05u, UINT32_C(0x00010000), 1u, 90u, 1003u);

    route_table_init(&table, UINT32_MAX);
    assert(route_upsert_candidate(&table, &before_wrap) == PROTO_OK);
    assert(route_upsert_candidate(&table, &after_wrap) == PROTO_OK);
    assert(table.current_epoch == 1u);
    assert(valid_candidate_count(&table) == 1u);
    assert(route_upsert_candidate(&table, &before_wrap) == PROTO_ERR_STALE);
    assert(route_upsert_candidate(&table, &ambiguous) == PROTO_ERR_STALE);
    assert(route_upsert_candidate(&table, &zero_wire_epoch) == PROTO_ERR_ARG);
}

static void test_last_success_tie_break_survives_uptime_wrap(void)
{
    struct route_table table;
    struct route_candidate before_wrap =
        candidate(0x02u, 1u, 1u, 90u, UINT32_MAX - 4u);
    struct route_candidate after_wrap =
        candidate(0x03u, 1u, 1u, 90u, 3u);
    const struct route_candidate *selected;

    route_table_init(&table, 1u);
    assert(route_upsert_candidate(&table, &before_wrap) == PROTO_OK);
    assert(route_upsert_candidate(&table, &after_wrap) == PROTO_OK);
    table.candidates[0].last_success_ms = UINT32_MAX - 4u;
    table.candidates[1].last_success_ms = 3u;
    assert(route_select_best_at(&table, 3u) == PROTO_OK);
    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == after_wrap.next_hop_id);
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
           ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure_at(&table, ROUTE_FAILURE_GATEWAY_ACK, 2300u) ==
           ROUTE_DELIVERY_TRY_ALTERNATE);

    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == 0x03u);

    assert(route_record_failure_at(&table, ROUTE_FAILURE_GATEWAY_ACK, 2400u) ==
           ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure_at(&table, ROUTE_FAILURE_GATEWAY_ACK, 2500u) ==
           ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure_at(&table, ROUTE_FAILURE_GATEWAY_ACK, 2600u) ==
           ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure_at(&table, ROUTE_FAILURE_GATEWAY_ACK, 2700u) ==
           ROUTE_DELIVERY_DISCOVER);
    assert(route_selected(&table) == NULL);
}

static void test_bounded_contact_abandons_parent_before_discovery(void)
{
    struct route_table table;
    struct route_candidate primary = candidate(0x02u, 1u, 1u, 90u, 1000u);
    struct route_candidate alternate = candidate(0x03u, 1u, 2u, 60u, 1000u);
    const struct route_candidate *selected;

    route_table_init(&table, 1u);
    assert(route_upsert_candidate(&table, &primary) == PROTO_OK);
    assert(route_upsert_candidate(&table, &alternate) == PROTO_OK);

    assert(route_abandon_selected_at(&table, 2000u) ==
           ROUTE_DELIVERY_TRY_ALTERNATE);
    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == alternate.next_hop_id);
    assert(table.candidates[0].hold_down_valid);
    assert(table.candidates[0].hold_down_until_ms ==
           2000u + ROUTE_PARENT_HOLDDOWN_MS);

    assert(route_abandon_selected_at(&table, 2100u) ==
           ROUTE_DELIVERY_DISCOVER);
    assert(route_selected(&table) == NULL);
}

static void test_bounded_contact_abandons_failed_peer_after_selection_changes(void)
{
    struct route_table table;
    struct route_candidate failed = candidate(0x02u, 7u, 1u, 90u, 1000u);
    struct route_candidate alternate = candidate(0x03u, 7u, 2u, 60u, 1000u);
    const struct route_candidate *failed_state;
    const struct route_candidate *selected;

    route_table_init(&table, 7u);
    assert(route_upsert_candidate(&table, &failed) == PROTO_OK);
    assert(route_upsert_candidate(&table, &alternate) == PROTO_OK);
    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == failed.next_hop_id);

    /* A concurrent route reply can change selection while the bounded contact
     * attempt still belongs to the original peer. */
    alternate.hop_count = 1u;
    alternate.link_quality = 100u;
    alternate.last_seen_ms = 1500u;
    assert(route_upsert_candidate(&table, &alternate) == PROTO_OK);
    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == alternate.next_hop_id);

    (void)route_abandon_candidate_at(&table,
                                     failed.next_hop_id,
                                     failed.gateway_id,
                                     failed.route_epoch + 1u,
                                     2000u);
    failed_state = candidate_for_next_hop(&table, failed.next_hop_id);
    assert(failed_state != NULL);
    assert(!failed_state->hold_down_valid);
    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == alternate.next_hop_id);

    assert(route_abandon_candidate_at(&table,
                                      failed.next_hop_id,
                                      failed.gateway_id,
                                      failed.route_epoch,
                                      2000u) ==
           ROUTE_DELIVERY_TRY_ALTERNATE);
    failed_state = candidate_for_next_hop(&table, failed.next_hop_id);
    assert(failed_state != NULL);
    assert(failed_state->hold_down_valid);
    assert(failed_state->hold_down_until_ms ==
           2000u + ROUTE_PARENT_HOLDDOWN_MS);
    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == alternate.next_hop_id);
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
           ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure_at(&table, ROUTE_FAILURE_GATEWAY_ACK, 2300u) ==
           ROUTE_DELIVERY_DISCOVER);
    assert(route_selected(&table) == NULL);

    assert(route_expire_stale(&table,
                              2300u + ROUTE_PARENT_HOLDDOWN_MS + 1u,
                              ROUTE_CANDIDATE_MAX_AGE_MS) == 0u);
    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == 0x02u);
}

static void test_candidate_success_clears_hold_down(void)
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
           ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure_at(&table, ROUTE_FAILURE_GATEWAY_ACK, 2300u) ==
           ROUTE_DELIVERY_DISCOVER);
    assert(route_selected(&table) == NULL);

    assert(route_record_candidate_success_at(&table,
                                             route.next_hop_id,
                                             route.gateway_id,
                                             2400u) == PROTO_OK);
    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == route.next_hop_id);
    assert(selected->failure_count == 0u);
    assert(selected->hold_down_until_ms == 0u);
    assert(!selected->hold_down_valid);
    assert(selected->last_success_ms == 2400u);
}

static void test_rediscovered_candidate_clears_hold_down(void)
{
    struct route_table table;
    struct route_candidate route = candidate(0x02u, 1u, 1u, 90u, 1000u);
    struct route_candidate rediscovered = candidate(0x02u, 1u, 1u, 95u, 2400u);
    const struct route_candidate *selected;

    route_table_init(&table, 1u);
    assert(route_upsert_candidate(&table, &route) == PROTO_OK);

    assert(route_record_failure_at(&table, ROUTE_FAILURE_GATEWAY_ACK, 2000u) ==
           ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure_at(&table, ROUTE_FAILURE_GATEWAY_ACK, 2100u) ==
           ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure_at(&table, ROUTE_FAILURE_GATEWAY_ACK, 2200u) ==
           ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure_at(&table, ROUTE_FAILURE_GATEWAY_ACK, 2300u) ==
           ROUTE_DELIVERY_DISCOVER);
    assert(route_selected(&table) == NULL);

    assert(route_upsert_candidate(&table, &rediscovered) == PROTO_OK);
    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == route.next_hop_id);
    assert(selected->failure_count == 0u);
    assert(selected->hold_down_until_ms == 0u);
    assert(!selected->hold_down_valid);
    assert(selected->last_seen_ms == 2400u);
    assert(selected->link_quality == 95u);
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
    green.capacity_hint_valid = true;
    yellow.relay_capacity_state = RELAY_CAP_YELLOW;
    yellow.capacity_observed_at_ms = 1000u;
    yellow.capacity_valid_until_ms = 2000u;
    yellow.capacity_hint_valid = true;

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
    expired_green.capacity_hint_valid = true;
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
    assert(route_expire_stale(&table, 31501u, ROUTE_CANDIDATE_MAX_AGE_MS) == 0u);
    assert(route_selected(&table) != NULL);
    assert(table.candidates[0].valid);
    assert(table.candidates[0].channel9_timing_valid);
    assert(table.candidates[0].hold_down_until_ms == 0u);
}

static void test_expired_capacity_update_only_clears_capacity_hint(void)
{
    struct route_table table;
    struct route_candidate route = candidate(0x02u, 1u, 1u, 90u, 1000u);
    const struct route_candidate *selected;

    route.channel9_timing_valid = true;

    route_table_init(&table, 1u);
    assert(route_upsert_candidate(&table, &route) == PROTO_OK);

    route_update_capacity_hint(&table,
                               route.next_hop_id,
                               route.gateway_id,
                               RELAY_CAP_GREEN,
                               4u,
                               1u,
                               true,
                               2000u,
                               2500u,
                               2501u);

    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == route.next_hop_id);
    assert(selected->relay_capacity_state == RELAY_CAP_UNKNOWN);
    assert(selected->queue_free_hint == 0u);
    assert(selected->channel9_busy_hint == 0u);
    assert(selected->capacity_observed_at_ms == 0u);
    assert(selected->capacity_valid_until_ms == 0u);
    assert(!selected->capacity_hint_valid);
    assert(selected->valid);
    assert(selected->channel9_timing_valid);
    assert(selected->failure_count == 0u);
    assert(selected->hold_down_until_ms == 0u);
    assert(route_expire_stale(&table, 60000u, ROUTE_CANDIDATE_MAX_AGE_MS) == 0u);
    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == route.next_hop_id);
    assert(selected->valid);
    assert(selected->channel9_timing_valid);
    assert(selected->failure_count == 0u);
    assert(selected->hold_down_until_ms == 0u);
}

static void test_capacity_hint_deadline_zero_is_valid_then_expires(void)
{
    const uint32_t observed_at_ms = UINT32_MAX - 9u;
    struct route_table table;
    struct route_candidate green =
        candidate(0x03u, 1u, 1u, 80u, observed_at_ms);
    struct route_candidate unknown =
        candidate(0x02u, 1u, 1u, 80u, observed_at_ms);
    const struct route_candidate *selected;

    green.relay_capacity_state = RELAY_CAP_GREEN;
    green.capacity_observed_at_ms = observed_at_ms;
    green.capacity_valid_until_ms = 0u;
    green.capacity_hint_valid = true;

    route_table_init(&table, 1u);
    assert(route_upsert_candidate(&table, &unknown) == PROTO_OK);
    assert(route_upsert_candidate(&table, &green) == PROTO_OK);

    assert(route_select_best_at(&table, 0u) == PROTO_OK);
    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == green.next_hop_id);
    assert(selected->capacity_hint_valid);
    assert(selected->capacity_valid_until_ms == 0u);

    assert(route_select_best_at(&table, 1u) == PROTO_OK);
    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == unknown.next_hop_id);
}

static void test_parent_hold_down_deadline_zero_survives_uptime_wrap(void)
{
    const uint32_t final_failure_ms =
        UINT32_MAX - ROUTE_PARENT_HOLDDOWN_MS + 1u;
    struct route_table table;
    struct route_candidate route =
        candidate(0x02u, 1u, 1u, 90u,
                  final_failure_ms - ROUTE_RETRIES_PER_CANDIDATE);
    const struct route_candidate *held;
    const struct route_candidate *selected;

    route_table_init(&table, 1u);
    assert(route_upsert_candidate(&table, &route) == PROTO_OK);

    for (uint8_t retry = 0u;
         retry < ROUTE_RETRIES_PER_CANDIDATE;
         retry++) {
        assert(route_record_failure_at(
                   &table,
                   ROUTE_FAILURE_GATEWAY_ACK,
                   final_failure_ms - ROUTE_RETRIES_PER_CANDIDATE + retry) ==
               ROUTE_DELIVERY_RETRY_CURRENT);
    }
    assert(route_record_failure_at(&table,
                                   ROUTE_FAILURE_GATEWAY_ACK,
                                   final_failure_ms) ==
           ROUTE_DELIVERY_DISCOVER);

    held = &table.candidates[0];
    assert(held->hold_down_valid);
    assert(held->hold_down_until_ms == 0u);
    assert(route_selected(&table) == NULL);
    assert(route_select_best_at(&table, UINT32_MAX) == PROTO_ERR_NOT_FOUND);
    assert(held->hold_down_valid);

    assert(route_select_best_at(&table, 0u) == PROTO_OK);
    selected = route_selected(&table);
    assert(selected != NULL);
    assert(selected->next_hop_id == route.next_hop_id);
    assert(!selected->hold_down_valid);
    assert(selected->hold_down_until_ms == 0u);
}

static void test_retry_backoff_values(void)
{
    assert(route_retry_backoff_ms(1u) == 1500u);
    assert(route_retry_backoff_ms(2u) == 3000u);
    assert(route_retry_backoff_ms(3u) == 6000u);
    assert(route_retry_backoff_ms(4u) == 6000u);
}

int main(void)
{
    test_weighted_cost_prefers_useful_direct_route();
    test_weighted_cost_avoids_unusable_direct_route();
    test_same_hop_uses_link_quality();
    test_borderline_direct_waits_for_stronger_relay();
    test_borderline_relay_becomes_fallback_after_next_depth();
    test_strong_direct_still_beats_strong_relay();
    test_rsl_margin_boundary_is_inclusive();
    test_epoch_change_invalidates_old_routes();
    test_epoch_serial_order_accepts_wrap_and_rejects_ambiguity();
    test_last_success_tie_break_survives_uptime_wrap();
    test_age_alone_keeps_selected_route();
    test_age_alone_keeps_all_routes_selectable();
    test_success_refreshes_selected_route_age();
    test_failures_try_alternate_then_discovery();
    test_bounded_contact_abandons_parent_before_discovery();
    test_bounded_contact_abandons_failed_peer_after_selection_changes();
    test_parent_candidate_count_replaces_worst_route();
    test_parent_hold_down_recovers_without_age_expiry();
    test_candidate_success_clears_hold_down();
    test_rediscovered_candidate_clears_hold_down();
    test_channel9_timing_breaks_equal_cost_tie();
    test_capacity_breaks_equal_cost_tie();
    test_expired_capacity_hint_does_not_invalidate_route();
    test_expired_capacity_update_only_clears_capacity_hint();
    test_capacity_hint_deadline_zero_is_valid_then_expires();
    test_parent_hold_down_deadline_zero_survives_uptime_wrap();
    test_retry_backoff_values();
    return 0;
}
