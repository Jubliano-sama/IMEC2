#include "mesh_relay.h"

#include "mesh.h"
#include "mesh_radio_timing.h"
#include "route.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PRODUCER_ID UINT64_C(0xB001)
#define PRIMARY_PARENT_ID UINT64_C(0xA101)
#define ALTERNATE_PARENT_ID UINT64_C(0xA102)
#define DEPENDENT_CHILD_ID UINT64_C(0xB002)
#define GATEWAY_ID UINT64_C(0x9000)
#define ROUTE_EPOCH UINT32_C(17)
#define PAYLOAD_LEN 12u

struct test_context {
    const char *scenario;
    const char *phase;
    uint32_t seed;
};

struct pending_identity {
    uint8_t msg_type;
    uint64_t src_id;
    uint64_t dst_id;
    uint32_t session_id;
    uint16_t seq;
    uint16_t payload_len;
    uint8_t payload[PAYLOAD_LEN];
};

static struct test_context test_ctx;

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, \
                "scenario=%s seed=0x%08x phase=%s line=%d assertion=%s\n", \
                test_ctx.scenario, (unsigned int)test_ctx.seed, test_ctx.phase, \
                __LINE__, #expression); \
        return 1; \
    } \
} while (0)

static bool has_action(const struct mesh_relay_result *result,
                       enum mesh_relay_action action)
{
    return result != NULL && (result->actions & action) != 0u;
}

static struct route_candidate route_candidate(uint64_t next_hop_id,
                                              uint8_t hop_count,
                                              uint8_t quality,
                                              uint32_t now_ms)
{
    return (struct route_candidate) {
        .next_hop_id = next_hop_id,
        .gateway_id = GATEWAY_ID,
        .route_epoch = ROUTE_EPOCH,
        .last_seen_ms = now_ms,
        .last_success_ms = now_ms,
        .route_cost = route_candidate_cost(hop_count, quality),
        .hop_count = hop_count,
        .link_quality = quality,
        .failure_count = 0u,
        .channel9_timing_valid = true,
        .valid = true,
    };
}

static struct mesh_event_params event_params(uint32_t first_event_ms)
{
    return (struct mesh_event_params) {
        .event_interval_ms = MESH_RADIO_EVENT_INTERVAL_MS,
        .event_window_ms = MESH_RADIO_EVENT_WINDOW_MS,
        .first_event_time_ms = first_event_ms,
        .guard_ms = 4u,
        .peer_clock_skew_estimate_ppm = 20,
        .max_missed_events = MESH_RADIO_EVENT_MAX_MISSES,
        .supervision_timeout_ms = 20000u,
    };
}

static const struct mesh_relay_event_timing_entry *find_event_timing(
    const struct mesh_relay *relay,
    uint64_t peer_id)
{
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        if (relay->event_timings[i].valid &&
            relay->event_timings[i].next_hop_id == peer_id) {
            return &relay->event_timings[i];
        }
    }
    return NULL;
}

static const struct route_candidate *find_candidate(
    const struct mesh_relay *relay,
    uint64_t parent_id)
{
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        if (relay->upstream.candidates[i].valid &&
            relay->upstream.candidates[i].next_hop_id == parent_id) {
            return &relay->upstream.candidates[i];
        }
    }
    return NULL;
}

static int model_anchor_abandonment_and_reverse_loss(uint32_t now_ms)
{
    struct mesh_relay parent;
    struct route_candidate gateway_route = route_candidate(
        GATEWAY_ID, 0u, 100u, now_ms);
    struct mesh_event_timing upstream_timing;
    struct mesh_event_timing downstream_timing;
    struct mesh_event_params upstream_params = event_params(now_ms + 100u);
    struct mesh_event_params downstream_params = event_params(now_ms + 200u);

    test_ctx.phase = "anchor_setup";
    mesh_relay_init(&parent, MESH_RELAY_ROLE_ANCHOR, PRIMARY_PARENT_ID,
                    GATEWAY_ID, ROUTE_EPOCH);
    CHECK(route_upsert_candidate(&parent.upstream, &gateway_route) == PROTO_OK);
    parent.downlinks[0] = (struct mesh_downlink_entry) {
        .target_id = PRODUCER_ID,
        .next_hop_id = PRODUCER_ID,
        .gateway_id = GATEWAY_ID,
        .route_epoch = ROUTE_EPOCH,
        .last_seen_ms = now_ms,
        .hop_count = 1u,
        .quality = 90u,
        .valid = true,
    };
    CHECK(mesh_event_timing_negotiate(&upstream_timing, &upstream_params,
                                      true) == PROTO_OK);
    CHECK(mesh_event_timing_negotiate(&downstream_timing, &downstream_params,
                                      true) == PROTO_OK);
    CHECK(mesh_relay_set_channel9_timing(&parent, GATEWAY_ID,
                                         &upstream_timing) == PROTO_OK);
    CHECK(mesh_relay_set_channel9_timing(&parent, PRODUCER_ID,
                                         &downstream_timing) == PROTO_OK);
    CHECK(find_event_timing(&parent, GATEWAY_ID) != NULL);
    CHECK(find_event_timing(&parent, PRODUCER_ID) != NULL);

    test_ctx.phase = "post_click_abandon";
    mesh_relay_abandon_transit_reservations(&parent);
    CHECK(find_event_timing(&parent, GATEWAY_ID) != NULL);
    CHECK(find_event_timing(&parent, PRODUCER_ID) == NULL);
    CHECK(mesh_relay_find_downlink(&parent, PRODUCER_ID) != NULL);

    test_ctx.phase = "reverse_identity_loss";
    mesh_relay_clear_routes_preserve_epoch(&parent);
    CHECK(mesh_relay_find_downlink(&parent, PRODUCER_ID) == NULL);
    CHECK(find_event_timing(&parent, PRODUCER_ID) == NULL);
    CHECK(route_selected(&parent.upstream) == NULL);
    return 0;
}

static int start_pending_producer(struct mesh_relay *producer,
                                  bool with_alternate,
                                  uint32_t now_ms,
                                  struct mesh_outbound *initial,
                                  struct pending_identity *identity)
{
    struct route_candidate primary = route_candidate(
        PRIMARY_PARENT_ID, 1u, 95u, now_ms);
    struct route_candidate alternate = route_candidate(
        ALTERNATE_PARENT_ID, 2u, 75u, now_ms);
    struct mesh_event_timing timing;
    struct mesh_event_params params = event_params(now_ms + 100u);
    struct proto_packet packet = {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        .src_id = PRODUCER_ID,
        .dst_id = GATEWAY_ID,
        .session_id = UINT32_C(0x71000000) ^ now_ms,
        .seq = (uint16_t)(1u + (now_ms % UINT16_MAX)),
        .ttl = 12u,
        .payload_len = PAYLOAD_LEN,
    };
    uint8_t payload[PAYLOAD_LEN];

    for (size_t i = 0u; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(test_ctx.seed + now_ms + (uint32_t)i);
    }
    mesh_relay_init(producer, MESH_RELAY_ROLE_ANCHOR, PRODUCER_ID,
                    GATEWAY_ID, ROUTE_EPOCH);
    CHECK(route_upsert_candidate(&producer->upstream, &primary) == PROTO_OK);
    if (with_alternate) {
        CHECK(route_upsert_candidate(&producer->upstream, &alternate) == PROTO_OK);
    }
    CHECK(route_selected(&producer->upstream) != NULL);
    CHECK(route_selected(&producer->upstream)->next_hop_id == PRIMARY_PARENT_ID);
    CHECK(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    CHECK(mesh_relay_set_channel9_timing(producer, PRIMARY_PARENT_ID,
                                         &timing) == PROTO_OK);
    CHECK(mesh_relay_start_tx(producer, &packet, payload, sizeof(payload),
                              now_ms, initial) == PROTO_OK);
    CHECK(initial->next_hop_id == PRIMARY_PARENT_ID);
    mesh_relay_note_tx_sent(producer, initial, now_ms + 1u);
    CHECK(producer->pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    CHECK(producer->outbox_record.valid);

    identity->msg_type = packet.msg_type;
    identity->src_id = packet.src_id;
    identity->dst_id = packet.dst_id;
    identity->session_id = packet.session_id;
    identity->seq = packet.seq;
    identity->payload_len = sizeof(payload);
    memcpy(identity->payload, payload, sizeof(payload));

    test_ctx.phase = "producer_retains_parent";
    mesh_relay_clear_channel9_timing(producer, PRIMARY_PARENT_ID);
    CHECK(find_event_timing(producer, PRIMARY_PARENT_ID) == NULL);
    CHECK(route_selected(&producer->upstream) != NULL);
    CHECK(route_selected(&producer->upstream)->next_hop_id == PRIMARY_PARENT_ID);
    CHECK(producer->pending.next_hop_id == PRIMARY_PARENT_ID);

    producer->downlinks[0] = (struct mesh_downlink_entry) {
        .target_id = DEPENDENT_CHILD_ID,
        .next_hop_id = DEPENDENT_CHILD_ID,
        .gateway_id = GATEWAY_ID,
        .route_epoch = ROUTE_EPOCH,
        .last_seen_ms = now_ms,
        .hop_count = 1u,
        .quality = 90u,
        .valid = true,
    };
    params = event_params(now_ms + 200u);
    CHECK(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    CHECK(mesh_relay_set_channel9_timing_guarded_direction(
              producer,
              DEPENDENT_CHILD_ID,
              &timing,
              MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM,
              MESH_RELAY_EVENT_TIMINGS,
              NULL) == PROTO_OK);
    CHECK(mesh_relay_find_downlink(producer, DEPENDENT_CHILD_ID) != NULL);
    CHECK(find_event_timing(producer, DEPENDENT_CHILD_ID) != NULL);
    return 0;
}

static int pending_payload_matches(const struct mesh_relay *relay,
                                   const struct pending_identity *identity)
{
    return relay->pending.packet.msg_type == identity->msg_type &&
           relay->pending.packet.src_id == identity->src_id &&
           relay->pending.packet.dst_id == identity->dst_id &&
           relay->pending.packet.session_id == identity->session_id &&
           relay->pending.packet.seq == identity->seq &&
           relay->pending.payload_len == identity->payload_len &&
           memcmp(relay->pending.payload, identity->payload,
                  identity->payload_len) == 0;
}

static int note_parent_failure_with_lightweight_parity(
    struct mesh_relay *relay,
    uint32_t now_ms,
    uint32_t random_value,
    struct mesh_relay_result *result)
{
    struct mesh_relay lightweight_relay = *relay;
    uint32_t lightweight_actions = UINT32_MAX;
    int lightweight_status = PROTO_ERR_MALFORMED;
    int full_ret;
    int lightweight_ret;

    full_ret = mesh_relay_note_pending_parent_failure(relay,
                                                       now_ms,
                                                       random_value,
                                                       result);
    lightweight_ret = mesh_relay_note_pending_parent_failure_status(
        &lightweight_relay,
        now_ms,
        random_value,
        &lightweight_actions,
        &lightweight_status);

    CHECK(lightweight_ret == full_ret);
    CHECK(lightweight_actions == result->actions);
    CHECK(lightweight_status == result->status);
    CHECK(memcmp(&lightweight_relay, relay, sizeof(*relay)) == 0);
    return full_ret;
}
static int exercise_first_three_parent_hop_ack_misses(
    struct mesh_relay *producer,
    const struct pending_identity *identity,
    uint32_t *now_ms)
{
    for (uint8_t failure = 1u; failure <= ROUTE_RETRIES_PER_CANDIDATE;
         failure++) {
        struct mesh_relay_result failure_result;
        struct mesh_relay_result tick_result;
        const struct route_candidate *selected;
        uint32_t random_value = test_ctx.seed ^
                                ((uint32_t)failure * UINT32_C(0x9e3779b9));
        uint32_t expected_delay = mesh_relay_retry_backoff_ms(
            failure, random_value);
        uint32_t retry_at_ms;

        test_ctx.phase = failure == 1u ? "parent_hop_ack_miss_1" :
                         (failure == 2u ? "parent_hop_ack_miss_2" :
                          "parent_hop_ack_miss_3");
        CHECK(note_parent_failure_with_lightweight_parity(producer,
                                                          *now_ms,
                                                          random_value,
                                                          &failure_result) == PROTO_OK);
        CHECK(failure_result.actions == MESH_RELAY_ACTION_NONE);
        selected = route_selected(&producer->upstream);
        CHECK(selected != NULL);
        CHECK(selected->next_hop_id == PRIMARY_PARENT_ID);
        CHECK(selected->failure_count == 0u);
        CHECK(producer->pending.parent_hop_ack_miss_count == failure);
        CHECK(producer->pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
        CHECK(producer->pending.next_hop_id == PRIMARY_PARENT_ID);
        CHECK(producer->pending.retry_after_ms == *now_ms + expected_delay);
        CHECK(expected_delay >= MESH_RELAY_ACK_RETRY_BACKOFF_MIN_MS);
        CHECK(expected_delay <= MESH_RELAY_ACK_RETRY_BACKOFF_CAP_MS);
        if (failure == 1u) {
            CHECK(expected_delay <=
                  MESH_RELAY_ACK_RETRY_BACKOFF_FIRST_MAX_MS);
        }
        CHECK(producer->pending.gateway_ack_deadline_ms == 0u);
        CHECK(producer->outbox_record.valid);
        CHECK(pending_payload_matches(producer, identity));

        retry_at_ms = producer->pending.retry_after_ms;
        test_ctx.phase = failure == 1u ? "retry_boundary_1" :
                         (failure == 2u ? "retry_boundary_2" :
                          "retry_boundary_3");
        CHECK(mesh_relay_tick_with_random(producer, retry_at_ms - 1u,
                                          random_value,
                                          &tick_result) == PROTO_OK);
        CHECK(tick_result.actions == MESH_RELAY_ACTION_NONE);
        CHECK(mesh_relay_tick_with_random(producer, retry_at_ms,
                                          random_value,
                                          &tick_result) == PROTO_OK);
        CHECK(has_action(&tick_result, MESH_RELAY_ACTION_RETRANSMIT));
        CHECK(!has_action(&tick_result,
                          MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));
        CHECK(tick_result.retransmit.next_hop_id == PRIMARY_PARENT_ID);
        CHECK(tick_result.retransmit.packet.session_id == identity->session_id);
        CHECK(tick_result.retransmit.packet.seq == identity->seq);
        CHECK(tick_result.retransmit.payload_len == identity->payload_len);
        CHECK(memcmp(tick_result.retransmit.payload, identity->payload,
                     identity->payload_len) == 0);
        CHECK(pending_payload_matches(producer, identity));
        *now_ms = retry_at_ms + 17u + failure;
    }
    return 0;
}

static int test_no_alternate_fourth_failure(uint32_t seed, uint32_t start_ms)
{
    struct mesh_relay producer;
    struct mesh_outbound initial;
    struct pending_identity identity;
    struct mesh_relay_result result;
    const struct route_candidate *primary;
    uint32_t now_ms = start_ms;

    test_ctx = (struct test_context) {
        .scenario = "no_alternate",
        .phase = "setup",
        .seed = seed,
    };
    CHECK(model_anchor_abandonment_and_reverse_loss(now_ms) == 0);
    test_ctx.phase = "pending_setup";
    CHECK(start_pending_producer(&producer, false, now_ms, &initial,
                                 &identity) == 0);
    now_ms += 101u;
    CHECK(exercise_first_three_parent_hop_ack_misses(
              &producer, &identity, &now_ms) == 0);
    CHECK(mesh_relay_find_downlink(&producer, DEPENDENT_CHILD_ID) != NULL);
    CHECK(find_event_timing(&producer, DEPENDENT_CHILD_ID) != NULL);

    test_ctx.phase = "parent_failure_4_discovery";
    CHECK(note_parent_failure_with_lightweight_parity(
              &producer,
              now_ms,
              seed ^ UINT32_C(0xdeadbeef),
              &result) == PROTO_OK);
    CHECK(has_action(&result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));
    CHECK(result.status == PROTO_ERR_NOT_FOUND);
    CHECK(route_selected(&producer.upstream) == NULL);
    primary = find_candidate(&producer, PRIMARY_PARENT_ID);
    CHECK(primary != NULL);
    CHECK(primary->failure_count == 0u);
    CHECK(primary->hold_down_until_ms == now_ms + ROUTE_PARENT_HOLDDOWN_MS);
    CHECK(primary->hold_down_valid);
    CHECK(!primary->channel9_timing_valid);
    CHECK(producer.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    CHECK(producer.pending.retry_after_ms > now_ms);
    CHECK(producer.outbox_record.valid);
    CHECK(pending_payload_matches(&producer, &identity));
    CHECK(mesh_relay_find_downlink(&producer, DEPENDENT_CHILD_ID) != NULL);
    CHECK(find_event_timing(&producer, DEPENDENT_CHILD_ID) == NULL);
    return 0;
}

static int test_alternate_parent_fourth_failure(uint32_t seed,
                                                uint32_t start_ms)
{
    struct mesh_relay producer;
    struct mesh_outbound initial;
    struct pending_identity identity;
    struct mesh_relay_result result;
    struct mesh_relay_result tick_result;
    const struct route_candidate *selected;
    const struct route_candidate *primary;
    uint32_t now_ms = start_ms;

    test_ctx = (struct test_context) {
        .scenario = "alternate_parent",
        .phase = "setup",
        .seed = seed,
    };
    CHECK(model_anchor_abandonment_and_reverse_loss(now_ms) == 0);
    test_ctx.phase = "pending_setup";
    CHECK(start_pending_producer(&producer, true, now_ms, &initial,
                                 &identity) == 0);
    now_ms += 101u;
    CHECK(exercise_first_three_parent_hop_ack_misses(
              &producer, &identity, &now_ms) == 0);
    CHECK(mesh_relay_find_downlink(&producer, DEPENDENT_CHILD_ID) != NULL);
    CHECK(find_event_timing(&producer, DEPENDENT_CHILD_ID) != NULL);

    test_ctx.phase = "parent_failure_4_alternate";
    CHECK(note_parent_failure_with_lightweight_parity(
              &producer,
              now_ms,
              seed ^ UINT32_C(0x13579bdf),
              &result) == PROTO_OK);
    CHECK(!has_action(&result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));
    CHECK(result.status == PROTO_OK);
    selected = route_selected(&producer.upstream);
    CHECK(selected != NULL);
    CHECK(selected->next_hop_id == ALTERNATE_PARENT_ID);
    primary = find_candidate(&producer, PRIMARY_PARENT_ID);
    CHECK(primary != NULL);
    CHECK(primary->hold_down_until_ms == now_ms + ROUTE_PARENT_HOLDDOWN_MS);
    CHECK(primary->hold_down_valid);
    CHECK(producer.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    CHECK(producer.pending.next_hop_id == ALTERNATE_PARENT_ID);
    CHECK(producer.pending.retry_after_ms == now_ms);
    CHECK(producer.outbox_record.valid);
    CHECK(pending_payload_matches(&producer, &identity));
    CHECK(mesh_relay_find_downlink(&producer, DEPENDENT_CHILD_ID) != NULL);
    CHECK(find_event_timing(&producer, DEPENDENT_CHILD_ID) == NULL);

    test_ctx.phase = "alternate_retry_boundary";
    CHECK(mesh_relay_tick_with_random(&producer, now_ms, seed,
                                      &tick_result) == PROTO_OK);
    CHECK(has_action(&tick_result, MESH_RELAY_ACTION_RETRANSMIT));
    CHECK(!has_action(&tick_result,
                      MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));
    CHECK(tick_result.retransmit.next_hop_id == ALTERNATE_PARENT_ID);
    CHECK(tick_result.retransmit.packet.session_id == identity.session_id);
    CHECK(memcmp(tick_result.retransmit.payload, identity.payload,
                 identity.payload_len) == 0);
    return 0;
}

static int test_temporary_deferral_is_not_parent_failure(uint32_t seed,
                                                         uint32_t start_ms)
{
    struct mesh_relay producer;
    struct mesh_outbound initial;
    struct pending_identity identity;
    struct mesh_relay_result tick_result;
    const struct route_candidate *selected;
    uint32_t retry_at_ms = start_ms + 400u;

    test_ctx = (struct test_context) {
        .scenario = "temporary_deferral",
        .phase = "setup",
        .seed = seed,
    };
    CHECK(start_pending_producer(&producer, false, start_ms, &initial,
                                 &identity) == 0);
    test_ctx.phase = "deferral_1";
    CHECK(mesh_relay_note_retransmit_deferred(&producer, &initial,
                                              retry_at_ms) == PROTO_OK);
    selected = route_selected(&producer.upstream);
    CHECK(selected != NULL);
    CHECK(selected->failure_count == 0u);
    CHECK(producer.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    CHECK(producer.pending.retry_after_ms == retry_at_ms);
    CHECK(pending_payload_matches(&producer, &identity));

    test_ctx.phase = "deferral_retry";
    CHECK(mesh_relay_tick_with_random(&producer, retry_at_ms, seed,
                                      &tick_result) == PROTO_OK);
    CHECK(has_action(&tick_result, MESH_RELAY_ACTION_RETRANSMIT));
    CHECK(route_selected(&producer.upstream)->failure_count == 0u);
    retry_at_ms += 275u;
    CHECK(mesh_relay_note_retransmit_deferred(&producer,
                                              &tick_result.retransmit,
                                              retry_at_ms) == PROTO_OK);
    CHECK(route_selected(&producer.upstream)->failure_count == 0u);
    CHECK(!has_action(&tick_result,
                      MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));
    CHECK(pending_payload_matches(&producer, &identity));
    return 0;
}

static int test_parent_hold_down_survives_wrapped_zero_deadline(void)
{
    const uint32_t fourth_failure_ms =
        UINT32_MAX - ROUTE_PARENT_HOLDDOWN_MS + 1u;
    struct route_table table;
    struct route_candidate route = route_candidate(
        PRIMARY_PARENT_ID, 1u, 90u, fourth_failure_ms - 3u);
    const struct route_candidate *candidate;

    test_ctx = (struct test_context) {
        .scenario = "hold_down_wrap",
        .phase = "failure_boundary",
        .seed = 0u,
    };
    route_table_init(&table, ROUTE_EPOCH);
    CHECK(route_upsert_candidate(&table, &route) == PROTO_OK);
    CHECK(route_record_failure_at(&table,
                                  ROUTE_FAILURE_GATEWAY_ACK,
                                  fourth_failure_ms - 3u) ==
          ROUTE_DELIVERY_RETRY_CURRENT);
    CHECK(route_record_failure_at(&table,
                                  ROUTE_FAILURE_GATEWAY_ACK,
                                  fourth_failure_ms - 2u) ==
          ROUTE_DELIVERY_RETRY_CURRENT);
    CHECK(route_record_failure_at(&table,
                                  ROUTE_FAILURE_GATEWAY_ACK,
                                  fourth_failure_ms - 1u) ==
          ROUTE_DELIVERY_RETRY_CURRENT);
    CHECK(route_record_failure_at(&table,
                                  ROUTE_FAILURE_GATEWAY_ACK,
                                  fourth_failure_ms) ==
          ROUTE_DELIVERY_DISCOVER);

    candidate = &table.candidates[0];
    CHECK(candidate->hold_down_valid);
    CHECK(candidate->hold_down_until_ms == 0u);
    CHECK(route_selected(&table) == NULL);
    CHECK(route_select_best_at(&table, UINT32_MAX) == PROTO_ERR_NOT_FOUND);
    CHECK(candidate->hold_down_valid);

    test_ctx.phase = "deadline_boundary";
    CHECK(route_select_best_at(&table, 0u) == PROTO_OK);
    candidate = route_selected(&table);
    CHECK(candidate != NULL);
    CHECK(candidate->next_hop_id == PRIMARY_PARENT_ID);
    CHECK(!candidate->hold_down_valid);
    CHECK(candidate->hold_down_until_ms == 0u);
    return 0;
}

static int test_fresh_same_epoch_route_advertisement_reenables_parent(void)
{
    struct mesh_relay gateway;
    struct mesh_relay anchor;
    struct mesh_outbound advertisement;
    struct mesh_relay_result result;
    const struct route_candidate *candidate;
    uint32_t hold_down_until_ms;

    test_ctx = (struct test_context) {
        .scenario = "fresh_same_epoch_route_advertisement",
        .phase = "initial_observation",
        .seed = 0u,
    };
    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY_ID,
                    GATEWAY_ID, ROUTE_EPOCH);
    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, PRODUCER_ID,
                    GATEWAY_ID, ROUTE_EPOCH);
    CHECK(mesh_relay_build_gateway_route_adv(&gateway, 77u, 1000u,
                                              &advertisement) == PROTO_OK);
    CHECK(mesh_relay_handle_rx(&anchor,
                               &advertisement.packet,
                               advertisement.payload,
                               advertisement.payload_len,
                               GATEWAY_ID,
                               90u,
                               1010u,
                               &result) == PROTO_OK);
    candidate = find_candidate(&anchor, GATEWAY_ID);
    CHECK(candidate != NULL);
    CHECK(route_selected(&anchor.upstream) == candidate);

    test_ctx.phase = "accumulated_failures";
    CHECK(route_record_failure_at(&anchor.upstream,
                                  ROUTE_FAILURE_GATEWAY_ACK,
                                  1100u) == ROUTE_DELIVERY_RETRY_CURRENT);
    CHECK(route_record_failure_at(&anchor.upstream,
                                  ROUTE_FAILURE_GATEWAY_ACK,
                                  1101u) == ROUTE_DELIVERY_RETRY_CURRENT);
    candidate = find_candidate(&anchor, GATEWAY_ID);
    CHECK(candidate != NULL);
    CHECK(candidate->failure_count == 2u);

    test_ctx.phase = "fresh_sequence_clears_failures";
    CHECK(mesh_relay_build_gateway_route_adv(&gateway, 78u, 1200u,
                                              &advertisement) == PROTO_OK);
    CHECK(mesh_relay_handle_rx(&anchor,
                               &advertisement.packet,
                               advertisement.payload,
                               advertisement.payload_len,
                               GATEWAY_ID,
                               85u,
                               1210u,
                               &result) == PROTO_OK);
    candidate = find_candidate(&anchor, GATEWAY_ID);
    CHECK(candidate != NULL);
    CHECK(candidate->failure_count == 0u);
    CHECK(!candidate->hold_down_valid);
    CHECK(route_selected(&anchor.upstream) == candidate);

    test_ctx.phase = "fourth_failure_starts_hold_down";
    for (uint8_t failure = 0u;
         failure < ROUTE_RETRIES_PER_CANDIDATE;
         failure++) {
        CHECK(route_record_failure_at(&anchor.upstream,
                                      ROUTE_FAILURE_GATEWAY_ACK,
                                      1300u + failure) ==
              ROUTE_DELIVERY_RETRY_CURRENT);
    }
    CHECK(route_record_failure_at(&anchor.upstream,
                                  ROUTE_FAILURE_GATEWAY_ACK,
                                  1310u) == ROUTE_DELIVERY_DISCOVER);
    candidate = find_candidate(&anchor, GATEWAY_ID);
    CHECK(candidate != NULL);
    CHECK(candidate->failure_count == 0u);
    CHECK(candidate->hold_down_valid);
    hold_down_until_ms = 1310u + ROUTE_PARENT_HOLDDOWN_MS;
    CHECK(candidate->hold_down_until_ms == hold_down_until_ms);
    CHECK(route_selected(&anchor.upstream) == NULL);

    test_ctx.phase = "exact_duplicate_remains_stale";
    CHECK(mesh_relay_handle_rx(&anchor,
                               &advertisement.packet,
                               advertisement.payload,
                               advertisement.payload_len,
                               GATEWAY_ID,
                               100u,
                               1320u,
                               &result) == PROTO_OK);
    CHECK(result.status == PROTO_ERR_STALE);
    CHECK(has_action(&result, MESH_RELAY_ACTION_DROP));
    candidate = find_candidate(&anchor, GATEWAY_ID);
    CHECK(candidate != NULL);
    CHECK(candidate->hold_down_valid);
    CHECK(candidate->hold_down_until_ms == hold_down_until_ms);
    CHECK(candidate->last_seen_ms == 1210u);
    CHECK(candidate->link_quality == 85u);
    CHECK(route_selected(&anchor.upstream) == NULL);

    test_ctx.phase = "fresh_sequence_clears_hold_down";
    CHECK(mesh_relay_build_gateway_route_adv(&gateway, 79u, 1400u,
                                              &advertisement) == PROTO_OK);
    CHECK(mesh_relay_handle_rx(&anchor,
                               &advertisement.packet,
                               advertisement.payload,
                               advertisement.payload_len,
                               GATEWAY_ID,
                               75u,
                               1410u,
                               &result) == PROTO_OK);
    candidate = find_candidate(&anchor, GATEWAY_ID);
    CHECK(candidate != NULL);
    CHECK(candidate->last_seen_ms == 1410u);
    CHECK(candidate->link_quality == 75u);
    CHECK(candidate->failure_count == 0u);
    CHECK(!candidate->hold_down_valid);
    CHECK(candidate->hold_down_until_ms == 0u);
    CHECK(route_selected(&anchor.upstream) == candidate);
    CHECK(1410u < hold_down_until_ms);
    return 0;
}

/*
 * Build the explicit refusal a parent owes a sender it cannot serve: an ACK
 * that names no accepted identity, states the parent's real depth and its
 * remaining credit, and optionally says when to come back.
 */
static size_t build_refusal_hop_ack(struct proto_packet *ack,
                                    uint8_t *payload,
                                    size_t payload_cap,
                                    uint64_t responder_id,
                                    const struct pending_identity *identity,
                                    uint8_t depth,
                                    uint16_t retry_after_ms)
{
    struct mesh_ack_flow_control flow = {
        .depth = depth,
        .credit = 0u,
        .retry_after_ms = retry_after_ms,
        .depth_valid = true,
        .credit_valid = true,
        .retry_after_valid = retry_after_ms != 0u,
    };
    size_t offset = 0u;

    if (mesh_append_requested_seq(payload,
                                  payload_cap,
                                  &offset,
                                  identity->seq) != PROTO_OK ||
        mesh_append_ack_flow_control(payload,
                                     payload_cap,
                                     &offset,
                                     &flow) != PROTO_OK) {
        return 0u;
    }
    memset(ack, 0, sizeof(*ack));
    ack->msg_type = MSG_MESH_HOP_ACK;
    ack->flags = 0u;
    ack->src_id = responder_id;
    ack->dst_id = identity->src_id;
    ack->session_id = identity->session_id;
    ack->seq = 3u;
    ack->ttl = MESH_GATEWAY_ACK_TTL;
    ack->payload_len = (uint16_t)offset;
    return offset;
}

/*
 * A parent that answers UNREACHABLE heard the frame and refused it.  The
 * producer must keep the bytes, park that parent, move to the alternate, and
 * spend none of its bounded RF retries on somebody else's routing problem.
 */
static int test_dead_end_ack_reselects_alternate(uint32_t seed,
                                                 uint32_t now_ms)
{
    struct mesh_relay producer;
    struct mesh_outbound initial;
    struct pending_identity identity;
    struct mesh_relay_result result;
    struct proto_packet ack;
    const struct route_candidate *primary;
    uint8_t ack_payload[64];
    size_t ack_payload_len;

    test_ctx.scenario = "dead_end_reselects_alternate";
    test_ctx.seed = seed;
    test_ctx.phase = "setup";
    if (start_pending_producer(&producer, true, now_ms, &initial,
                               &identity) != 0) {
        return 1;
    }

    test_ctx.phase = "dead_end_ack";
    ack_payload_len = build_refusal_hop_ack(&ack,
                                            ack_payload,
                                            sizeof(ack_payload),
                                            PRIMARY_PARENT_ID,
                                            &identity,
                                            MESH_ROUTE_DEPTH_UNREACHABLE,
                                            0u);
    CHECK(ack_payload_len > 0u);
    CHECK(mesh_relay_handle_rx_with_random(&producer,
                                           &ack,
                                           ack_payload,
                                           ack_payload_len,
                                           PRIMARY_PARENT_ID,
                                           90u,
                                           now_ms + 40u,
                                           seed,
                                           &result) == PROTO_OK);
    CHECK(has_action(&result, MESH_RELAY_ACTION_TX_HOP_DEFERRED));

    test_ctx.phase = "custody_and_reselection";
    CHECK(mesh_relay_tx_active(&producer));
    CHECK(pending_payload_matches(&producer, &identity));
    CHECK(producer.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    CHECK(producer.pending.next_hop_id == ALTERNATE_PARENT_ID);
    CHECK(producer.pending.retry_after_ms == now_ms + 40u);
    CHECK(!mesh_relay_upstream_poisoned(&producer));

    test_ctx.phase = "no_rf_penalty";
    primary = find_candidate(&producer, PRIMARY_PARENT_ID);
    CHECK(primary != NULL);
    CHECK(primary->failure_count == 0u);
    CHECK(primary->hold_down_valid);
    CHECK(primary->hold_down_until_ms ==
          now_ms + 40u + MESH_PARENT_DEAD_END_HOLD_MS);
    CHECK(primary->hop_count == MESH_ROUTE_DEPTH_UNREACHABLE);
    return 0;
}

/*
 * A parent that is routed but out of room says so.  Same parent, same route
 * quality, no hold-down: only the schedule moves, by the amount it asked for.
 */
static int test_backpressure_ack_keeps_parent(uint32_t seed, uint32_t now_ms)
{
    struct mesh_relay producer;
    struct mesh_outbound initial;
    struct pending_identity identity;
    struct mesh_relay_result result;
    struct proto_packet ack;
    const struct route_candidate *primary;
    uint8_t ack_payload[64];
    size_t ack_payload_len;
    uint32_t delay_ms;

    test_ctx.scenario = "backpressure_keeps_parent";
    test_ctx.seed = seed;
    test_ctx.phase = "setup";
    if (start_pending_producer(&producer, true, now_ms, &initial,
                               &identity) != 0) {
        return 1;
    }

    test_ctx.phase = "backpressure_ack";
    ack_payload_len = build_refusal_hop_ack(&ack,
                                            ack_payload,
                                            sizeof(ack_payload),
                                            PRIMARY_PARENT_ID,
                                            &identity,
                                            1u,
                                            300u);
    CHECK(ack_payload_len > 0u);
    CHECK(mesh_relay_handle_rx_with_random(&producer,
                                           &ack,
                                           ack_payload,
                                           ack_payload_len,
                                           PRIMARY_PARENT_ID,
                                           90u,
                                           now_ms + 40u,
                                           seed,
                                           &result) == PROTO_OK);
    CHECK(has_action(&result, MESH_RELAY_ACTION_TX_HOP_DEFERRED));
    CHECK(!has_action(&result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));

    test_ctx.phase = "parent_unpenalised";
    CHECK(mesh_relay_tx_active(&producer));
    CHECK(pending_payload_matches(&producer, &identity));
    CHECK(producer.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    CHECK(producer.pending.next_hop_id == PRIMARY_PARENT_ID);
    CHECK(!mesh_relay_upstream_poisoned(&producer));
    primary = find_candidate(&producer, PRIMARY_PARENT_ID);
    CHECK(primary != NULL);
    CHECK(primary->failure_count == 0u);
    CHECK(!primary->hold_down_valid);

    test_ctx.phase = "retry_after_honoured";
    delay_ms = producer.pending.retry_after_ms - (now_ms + 40u);
    CHECK(delay_ms >= 225u && delay_ms <= 375u);
    return 0;
}

static int run_seed_set(const char *selection)
{
    static const uint32_t seeds[] = {
        UINT32_C(0x00000000),
        UINT32_C(0x13572468),
        UINT32_C(0xf00dcafe),
    };
    static const uint32_t start_times_ms[] = {
        1000u,
        17000u,
        83000u,
    };
    int failed = 0;

    for (size_t i = 0u; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
        if (selection == NULL || strcmp(selection, "no_alternate") == 0) {
            failed |= test_no_alternate_fourth_failure(seeds[i],
                                                       start_times_ms[i]);
        }
        if (selection == NULL || strcmp(selection, "alternate") == 0) {
            failed |= test_alternate_parent_fourth_failure(seeds[i],
                                                           start_times_ms[i]);
        }
        if (selection == NULL || strcmp(selection, "deferral") == 0) {
            failed |= test_temporary_deferral_is_not_parent_failure(
                seeds[i], start_times_ms[i]);
        }
        if (selection == NULL || strcmp(selection, "dead_end") == 0) {
            failed |= test_dead_end_ack_reselects_alternate(
                seeds[i], start_times_ms[i]);
        }
        if (selection == NULL || strcmp(selection, "backpressure") == 0) {
            failed |= test_backpressure_ack_keeps_parent(
                seeds[i], start_times_ms[i]);
        }
    }
    return failed;
}

int main(int argc, char **argv)
{
    const char *selection = argc == 2 ? argv[1] : NULL;

    if (argc > 2 ||
        (selection != NULL && strcmp(selection, "no_alternate") != 0 &&
         strcmp(selection, "alternate") != 0 &&
         strcmp(selection, "dead_end") != 0 &&
         strcmp(selection, "backpressure") != 0 &&
         strcmp(selection, "deferral") != 0)) {
        fprintf(stderr,
                "usage: %s "
                "[no_alternate|alternate|deferral|dead_end|backpressure]\n",
                argv[0]);
        return 2;
    }
    if (run_seed_set(selection) != 0) {
        return 1;
    }
    if (selection == NULL &&
        test_parent_hold_down_survives_wrapped_zero_deadline() != 0) {
        return 1;
    }
    if (selection == NULL &&
        test_fresh_same_epoch_route_advertisement_reenables_parent() != 0) {
        return 1;
    }
    printf("mesh route recovery scenarios passed\n");
    return 0;
}
